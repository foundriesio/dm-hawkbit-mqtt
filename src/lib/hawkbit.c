/*
 * Copyright (c) 2016-2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "fota/hawkbit"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_FOTA_LEVEL
#include <logging/sys_log.h>

#include <zephyr/types.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <misc/byteorder.h>
#include <flash.h>
#include <zephyr.h>
#include <misc/reboot.h>
#include <misc/stack.h>
#include <net/http.h>
#include <net/net_app.h>
#include <net/net_event.h>
#include <net/net_mgmt.h>

#include <soc.h>

#include "jsmn.h"
#include "hawkbit.h"
#include "mcuboot.h"
#include "flash_block.h"
#include "product_id.h"
#include "tcp.h"

#define HAWKBIT_MAX_SERVER_FAIL	5

/* Network configuration checks */
#if defined(CONFIG_NET_IPV6)
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_PEER_IPV6_ADDR) > 1,
		"CONFIG_NET_APP_PEER_IPV6_ADDR must be defined in boards/$(BOARD)-local.conf");
#define HAWKBIT_SERVER_ADDR    CONFIG_NET_APP_PEER_IPV6_ADDR
#elif defined(CONFIG_NET_IPV4)
#if !defined(CONFIG_NET_DHCPV4)
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_MY_IPV4_ADDR) > 1,
		"DHCPv4 must be enabled, or CONFIG_NET_APP_MY_IPV4_ADDR must be defined, in boards/$(BOARD)-local.conf");
#endif
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_PEER_IPV4_ADDR) > 1,
		"CONFIG_NET_APP_PEER_IPV4_ADDR must be defined in boards/$(BOARD)-local.conf");
#define HAWKBIT_SERVER_ADDR    CONFIG_NET_APP_PEER_IPV4_ADDR
#endif

/*
 * TODO:
 * create a transfer lifecycle structure
 * to contain the following vars:
 * TCP receive buffer
 * tracking indexes
 * status
 *
 */

/*
 * Buffer sizes were calculated by watching the actual usage on HW and then
 * adding a "safe" cushion (or round up to a nice big number).
 */
/* TODO: optimize these values */
#define TCP_RECV_BUFFER_SIZE	2048
#define URL_BUFFER_SIZE		128
#define STATUS_BUFFER_SIZE	128
#define HTTP_HEADER_BUFFER_SIZE	512

struct hawkbit_context {
	struct http_client_ctx http_ctx;
	struct http_client_request http_req;
	u8_t tcp_buffer[TCP_RECV_BUFFER_SIZE];
	size_t tcp_buffer_size;
	u8_t url_buffer[URL_BUFFER_SIZE];
	size_t url_buffer_size;
	u8_t status_buffer[STATUS_BUFFER_SIZE];
	size_t status_buffer_size;
};

struct hawkbit_download {
	size_t http_content_size;
	size_t downloaded_size;
	int download_progress;
	int download_status;
	struct k_sem *download_waitp;
};

struct json_data_t {
	char *data;
	size_t len;
};

struct http_download_t {
	size_t header_size;
	size_t content_length;
};

typedef enum {
	HAWKBIT_UPDATE_SKIP = 0,
	HAWKBIT_UPDATE_ATTEMPT,
	HAWKBIT_UPDATE_FORCED
} hawkbit_update_action_t;

typedef enum {
	HAWKBIT_RESULT_SUCCESS = 0,
	HAWKBIT_RESULT_FAILURE,
	HAWKBIT_RESULT_NONE,
} hawkbit_result_status_t;

typedef enum {
	HAWKBIT_EXEC_CLOSED = 0,
	HAWKBIT_EXEC_PROCEEDING,
	HAWKBIT_EXEC_CANCELED,
	HAWKBIT_EXEC_SCHEDULED,
	HAWKBIT_EXEC_REJECTED,
	HAWKBIT_EXEC_RESUMED,
} hawkbit_exec_status_t;

typedef enum {
	HAWKBIT_ACID_CURRENT = 0,
	HAWKBIT_ACID_UPDATE,
} hawkbit_dev_acid_t;

#define HAWKBIT_RX_TIMEOUT	K_SECONDS(3)

#define HAWKBIT_STACK_SIZE 3840
static K_THREAD_STACK_DEFINE(hawkbit_thread_stack, HAWKBIT_STACK_SIZE);
static struct k_thread hawkbit_thread_data;

int poll_sleep = K_SECONDS(30);
static bool connection_ready;
#if defined(CONFIG_NET_MGMT_EVENT)
static struct net_mgmt_event_callback cb;
#endif

#define HAWKBIT_DOWNLOAD_TIMEOUT	K_SECONDS(10)

#define HTTP_HEADER_CONTENT_TYPE_JSON		"application/json"
#define HTTP_HEADER_CONNECTION_CLOSE_CRLF	"Connection: close\r\n"

static struct hawkbit_context hbc;
static struct k_sem download_wait_sem;

#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
NET_PKT_TX_SLAB_DEFINE(http_client_tx, 15);
NET_PKT_DATA_POOL_DEFINE(http_client_data, 30);

static struct k_mem_slab *tx_slab(void)
{
	return &http_client_tx;
}

static struct net_buf_pool *data_pool(void)
{
	return &http_client_data;
}
#else
#if defined(CONFIG_NET_L2_BT)
#error "TCP connections over Bluetooth need CONFIG_NET_CONTEXT_NET_PKT_POOL "\
	"defined."
#endif /* CONFIG_NET_L2_BT */

#define tx_slab NULL
#define data_pool NULL
#endif /* CONFIG_NET_CONTEXT_NET_PKT_POOL */

/* Utils */
static int atoi_n(const char *s, int len)
{
        int i, val = 0;

	for (i = 0; i < len; i++) {
		if (*s < '0' || *s > '9')
			return val;
		val = (val * 10) + (*s - '0');
		s++;
	}

        return val;
}

static int jsoneq(const char *json, jsmntok_t *tok, const char *s)
{
	if (tok->type == JSMN_STRING &&
		(int) strlen(s) == tok->end - tok->start &&
		strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
		return 1;
	}
	return 0;
}

static int json_parser(struct json_data_t *json, jsmn_parser *parser,
		       jsmntok_t *tks, u16_t num_tokens)
{
	int ret = 0;

	SYS_LOG_DBG("JSON: max tokens supported %d", num_tokens);

	jsmn_init(parser);
	ret = jsmn_parse(parser, json->data, json->len, tks, num_tokens);
	if (ret < 0) {
		switch (ret) {
		case JSMN_ERROR_NOMEM:
			SYS_LOG_ERR("JSON: Not enough tokens");
			break;
		case JSMN_ERROR_INVAL:
			SYS_LOG_ERR("JSON: Invalid character found");
			break;
		case JSMN_ERROR_PART:
			SYS_LOG_ERR("JSON: Incomplete JSON");
			break;
		}
		return ret;
	} else if (ret == 0 || tks[0].type != JSMN_OBJECT) {
		SYS_LOG_ERR("JSON: First token is not an object");
		return 0;
	}

	SYS_LOG_DBG("JSON: %d tokens found", ret);

	return ret;
}

static int hawkbit_time2sec(const char *s)
{
        int sec = 0;

	/* Time: HH:MM:SS */
	sec = atoi_n(s, 2) * 60 * 60;
	sec += atoi_n(s + 3, 2) * 60;
	sec += atoi_n(s + 6, 2);

	if (sec < 0) {
		return -1;
	} else {
		return sec;
	}
}

void hawkbit_device_acid_read(struct hawkbit_device_acid *device_acid)
{
	flash_read(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET, device_acid,
		   sizeof(*device_acid));
}

/**
 * @brief Update an ACID of a given type on flash.
 *
 * @param type ACID type to update
 * @param acid New ACID value
 * @return 0 on success, negative on error.
 */
static int hawkbit_device_acid_update(hawkbit_dev_acid_t type,
				      u32_t new_value)
{
	struct hawkbit_device_acid device_acid;
	int ret;

	flash_read(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET, &device_acid,
		   sizeof(device_acid));
	if (type == HAWKBIT_ACID_UPDATE) {
		device_acid.update = new_value;
	} else {
		device_acid.current = new_value;
	}

	flash_write_protection_set(flash_dev, false);
	ret = flash_erase(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET,
			  FLASH_AREA_APPLICATION_STATE_SIZE);
	flash_write_protection_set(flash_dev, true);
	if (ret) {
		return ret;
	}

	flash_write_protection_set(flash_dev, false);
	ret = flash_write(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET,
			  &device_acid, sizeof(device_acid));
	flash_write_protection_set(flash_dev, true);
	return ret;
}

static int hawkbit_start(void)
{
	int ret = 0;
	struct hawkbit_device_acid init_acid;
	u8_t boot_status;

	/* Update boot status and acid */
	hawkbit_device_acid_read(&init_acid);
	SYS_LOG_INF("ACID: current %d, update %d",
		    init_acid.current, init_acid.update);
	boot_status = boot_status_read();
	SYS_LOG_INF("Current boot status %x", boot_status);
	if (boot_status == BOOT_STATUS_ONGOING) {
		boot_status_update();
		SYS_LOG_INF("Updated boot status to %x", boot_status_read());
		ret = boot_erase_flash_bank(FLASH_AREA_IMAGE_1_OFFSET);
		if (ret) {
			SYS_LOG_ERR("Flash bank erase at offset %x: error %d",
				    FLASH_AREA_IMAGE_1_OFFSET, ret);
			return ret;
		} else {
			SYS_LOG_DBG("Erased flash bank at offset %x",
				    FLASH_AREA_IMAGE_1_OFFSET);
		}
		if (init_acid.update != -1) {
			ret = hawkbit_device_acid_update(HAWKBIT_ACID_CURRENT,
						  init_acid.update);
		}
		if (!ret) {
			hawkbit_device_acid_read(&init_acid);
			SYS_LOG_INF("ACID updated, current %d, update %d",
				    init_acid.current, init_acid.update);
		} else {
			SYS_LOG_ERR("Failed to update ACID: %d", ret);
		}
	}
	return ret;
}

/* http_client doesn't callback until the HTTP body has started */
static void install_update_cb(struct http_client_ctx *ctx,
			      u8_t *data, size_t data_size,
			      size_t data_len,
			      enum http_final_call final_data,
			      void *user_data)
{
	struct hawkbit_download *hbd = user_data;
	int downloaded, ret = 0;
	u8_t *body_data = NULL;
	size_t body_len = 0;

	/* HTTP error */
	if (strncmp(ctx->rsp.http_status, "OK", 2) != 0) {
		SYS_LOG_ERR("HTTP error: %s!", ctx->rsp.http_status);
		goto error;
	}

	/* header hasn't been read yet */
	if (hbd->http_content_size == 0) {
		if (ctx->rsp.body_found == 0) {
			SYS_LOG_ERR("Callback called w/o HTTP header found!");
			goto error;
		}

		body_data = ctx->rsp.body_start;
		body_len = data_len;
		body_len -= (ctx->rsp.body_start - ctx->rsp.response_buf);
		hbd->http_content_size = ctx->rsp.content_length;
	}

	if (body_data == NULL) {
		body_data = ctx->rsp.response_buf;
		body_len = data_len;
	}

	/* everything looks good: flash */
	ret = flash_block_write(flash_dev,
				FLASH_AREA_IMAGE_1_OFFSET,
				&hbd->downloaded_size,
				body_data, body_len,
				final_data == HTTP_DATA_FINAL);
	if (ret < 0) {
		SYS_LOG_ERR("Flash write error: %d", ret);
		goto error;
	}

	downloaded = hbd->downloaded_size * 100 /
		     hbd->http_content_size;
	if (downloaded > hbd->download_progress) {
		hbd->download_progress = downloaded;
		SYS_LOG_DBG("%d%%", hbd->download_progress);
	}

	if (final_data == HTTP_DATA_FINAL) {
		hbd->download_status = 1;
		k_sem_give(hbd->download_waitp);
	}

	return;

error:
	hbd->download_status = -1;
	k_sem_give(hbd->download_waitp);
}

static int hawkbit_install_update(struct hawkbit_context *hb_ctx,
				  const char *download_http,
				  size_t file_size)
{
	struct hawkbit_download hbd;
	int ret = 0;
	size_t last_downloaded_size = 0;

	if (!download_http || !file_size) {
		return -EINVAL;
	}

	flash_write_protection_set(flash_dev, false);
	ret = flash_erase(flash_dev, FLASH_AREA_IMAGE_1_OFFSET,
			  FLASH_BANK_SIZE);
	flash_write_protection_set(flash_dev, true);
	if (ret != 0) {
		SYS_LOG_ERR("Failed to erase flash at offset %x, size %d",
			    FLASH_AREA_IMAGE_1_OFFSET, FLASH_BANK_SIZE);
		return -EIO;
	}

	SYS_LOG_INF("Starting the download and flash process");

	/* Receive is special for download, since it writes to flash */
	memset(hb_ctx->tcp_buffer, 0, hb_ctx->tcp_buffer_size);
	memset(&hbd, 0, sizeof(struct hawkbit_download));
	hbd.download_waitp = &download_wait_sem;
	/* reset download semaphore */
	k_sem_init(hbd.download_waitp, 0, 1);

	ret = http_client_init(&hbc.http_ctx,
			       HAWKBIT_SERVER_ADDR, HAWKBIT_PORT);
	if (ret < 0) {
		SYS_LOG_ERR("Failed to init http ctx, err %d", ret);
		return ret;
	}

#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
	http_client_set_net_pkt_pool(&hbc.http_ctx, tx_slab, data_pool);
#endif

	ret = http_client_send_get_req(&hb_ctx->http_ctx, download_http,
				       HAWKBIT_HOST,
				       HTTP_HEADER_CONNECTION_CLOSE_CRLF,
				       install_update_cb,
				       hb_ctx->tcp_buffer,
				       hb_ctx->tcp_buffer_size,
				       (void *)&hbd, K_NO_WAIT);
	/* http_client returns EINPROGRESS for get_req w/ K_NO_WAIT */
	if (ret < 0 && ret != -EINPROGRESS) {
		SYS_LOG_ERR("Failed to send request, err %d", ret);
		return ret;
	}

	while (k_sem_take(hbd.download_waitp, HAWKBIT_DOWNLOAD_TIMEOUT)) {
		/* wait timeout: check for download activity */
		if (last_downloaded_size == hbd.downloaded_size) {
			/* no activity: break loop */
			break;
		} else {
			last_downloaded_size = hbd.downloaded_size;
		}
	}

	/* clean up TCP context */
	http_client_release(&hb_ctx->http_ctx);

	if (hbd.download_status < 0) {
		SYS_LOG_ERR("Unable to finish the download process %d",
			    hbd.download_status);
		return -1;
	}

	if (hbd.downloaded_size != hbd.http_content_size) {
		SYS_LOG_ERR("Download: downloaded image size mismatch, "
			    "downloaded %zu, expecting %zu",
			    hbd.downloaded_size, hbd.http_content_size);
		return -1;
	}

	if (hbd.downloaded_size != file_size) {
		SYS_LOG_ERR("Download: downloaded image size mismatch, "
			    "downloaded %zu, expecting from JSON %zu",
			    hbd.downloaded_size, file_size);
		return -1;
	}

	SYS_LOG_INF("Download: downloaded bytes %zu", hbd.downloaded_size);
	return 0;
}

static int hawkbit_query(struct hawkbit_context *hb_ctx,
			 struct json_data_t *json)
{
	int ret = 0;

	SYS_LOG_DBG("[%s] HOST:%s URL:%s",
		    http_method_str(hb_ctx->http_req.method),
		    hb_ctx->http_req.host, hb_ctx->http_req.url);

	memset(hb_ctx->tcp_buffer, 0, hb_ctx->tcp_buffer_size);

	ret = http_client_init(&hbc.http_ctx,
			       HAWKBIT_SERVER_ADDR, HAWKBIT_PORT);
	if (ret < 0) {
		SYS_LOG_ERR("Failed to init http ctx, err %d", ret);
		return ret;
	}

#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
	http_client_set_net_pkt_pool(&hbc.http_ctx, tx_slab, data_pool);
#endif

	ret = http_client_send_req(&hb_ctx->http_ctx, &hb_ctx->http_req, NULL,
				   hb_ctx->tcp_buffer, hb_ctx->tcp_buffer_size,
				   NULL, HAWKBIT_RX_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("Failed to send buffer, err %d", ret);
		goto cleanup;
	}

	if (hb_ctx->http_ctx.rsp.data_len == 0) {
		SYS_LOG_ERR("No received data (rsp.data_len: %zu)",
			    hb_ctx->http_ctx.rsp.data_len);
		ret = -EIO;
		goto cleanup;
	}

	if (strncmp(hb_ctx->http_ctx.rsp.http_status, "OK", 2)) {
		SYS_LOG_ERR("Invalid HTTP status code [%s]",
			    hb_ctx->http_ctx.rsp.http_status);
		ret = -1;
		goto cleanup;
	}

	if (json) {
		json->data = hb_ctx->http_ctx.rsp.body_start;
		json->len = strlen(hb_ctx->http_ctx.rsp.response_buf);
		json->len -= hb_ctx->http_ctx.rsp.body_start -
			     hb_ctx->http_ctx.rsp.response_buf;

		/* FIXME: Each poll needs a new connection, this saves
		 * us from using content from a previous package.
		 */
		json->data[json->len] = '\0';
		SYS_LOG_DBG("JSON DATA:\n%s", json->data);
	}

	SYS_LOG_DBG("Hawkbit query completed");

cleanup:
	/* clean up TCP context */
	http_client_release(&hb_ctx->http_ctx);
	return ret;
}

static int hawkbit_report_config_data(struct hawkbit_context *hb_ctx)
{
	const struct product_id_t *product_id = product_id_get();

	SYS_LOG_INF("Reporting target config data to Hawkbit");

	/* Build URL */
	snprintf(hb_ctx->url_buffer, hb_ctx->url_buffer_size,
		 "%s/%s-%x/configData", HAWKBIT_JSON_URL,
		 product_id->name, product_id->number);

	/* Build JSON */
	snprintf(hb_ctx->status_buffer, hb_ctx->status_buffer_size, "{"
			"\"data\":{"
				"\"board\":\"%s\","
				"\"serial\":\"%x\"},"
			"\"status\":{"
				"\"result\":{\"finished\":\"success\"},"
				"\"execution\":\"closed\"}"
			"}", product_id->name, product_id->number);

	memset(&hb_ctx->http_req, 0, sizeof(hb_ctx->http_req));
	hb_ctx->http_req.method = HTTP_PUT;
	hb_ctx->http_req.url = hb_ctx->url_buffer;
	hb_ctx->http_req.host = HAWKBIT_HOST;
	hb_ctx->http_req.protocol = " " HTTP_PROTOCOL HTTP_CRLF;
	hb_ctx->http_req.header_fields = HTTP_HEADER_CONNECTION_CLOSE_CRLF;
	hb_ctx->http_req.content_type_value = "application/json";
	hb_ctx->http_req.payload = hb_ctx->status_buffer;
	hb_ctx->http_req.payload_size = strlen(hb_ctx->status_buffer);

	if (hawkbit_query(hb_ctx, NULL)) {
		SYS_LOG_ERR("Error when reporting config data to Hawkbit");
		return -1;
	}

	return 0;
}

static int hawkbit_report_update_status(struct hawkbit_context *hb_ctx,
					int acid,
					hawkbit_result_status_t status,
					hawkbit_exec_status_t exec)
{
	const struct product_id_t *product_id = product_id_get();
	char finished[8];	/* 'success', 'failure', 'none' */
	char execution[11];

	switch (status) {
	case HAWKBIT_RESULT_SUCCESS:
		snprintf(finished, sizeof(finished), "success");
		break;
	case HAWKBIT_RESULT_FAILURE:
		snprintf(finished, sizeof(finished), "failure");
		break;
	case HAWKBIT_RESULT_NONE:
		snprintf(finished, sizeof(finished), "none");
		break;
	}

	/* 'closed', 'proceeding', 'canceled', 'scheduled',
	 * 'rejected', 'resumed'
	 */
	switch (exec) {
	case HAWKBIT_EXEC_CLOSED:
		snprintf(execution, sizeof(execution), "closed");
		break;
	case HAWKBIT_EXEC_PROCEEDING:
		snprintf(execution, sizeof(execution), "proceeding");
		break;
	case HAWKBIT_EXEC_CANCELED:
		snprintf(execution, sizeof(execution), "canceled");
		break;
	case HAWKBIT_EXEC_SCHEDULED:
		snprintf(execution, sizeof(execution), "scheduled");
		break;
	case HAWKBIT_EXEC_REJECTED:
		snprintf(execution, sizeof(execution), "rejected");
		break;
	case HAWKBIT_EXEC_RESUMED:
		snprintf(execution, sizeof(execution), "resumed");
		break;
	}

	SYS_LOG_INF("Reporting action ID feedback: %s", finished);

	/* Build URL */
	snprintf(hb_ctx->url_buffer, hb_ctx->url_buffer_size,
		 "%s/%s-%x/deploymentBase/%d/feedback",
		 HAWKBIT_JSON_URL, product_id->name, product_id->number, acid);

	/* Build JSON */
	snprintf(hb_ctx->status_buffer, hb_ctx->status_buffer_size, "{"
			"\"id\":\"%d\","
			"\"status\":{"
				"\"result\":{\"finished\":\"%s\"},"
				"\"execution\":\"%s\"}"
			"}", acid, finished, execution);

	memset(&hb_ctx->http_req, 0, sizeof(hb_ctx->http_req));
	hb_ctx->http_req.method = HTTP_POST;
	hb_ctx->http_req.url = hb_ctx->url_buffer;
	hb_ctx->http_req.host = HAWKBIT_HOST;
	hb_ctx->http_req.protocol = " " HTTP_PROTOCOL HTTP_CRLF;
	hb_ctx->http_req.header_fields = HTTP_HEADER_CONNECTION_CLOSE_CRLF;
	hb_ctx->http_req.content_type_value = "application/json";
	hb_ctx->http_req.payload = hb_ctx->status_buffer;
	hb_ctx->http_req.payload_size = strlen(hb_ctx->status_buffer);

	if (hawkbit_query(hb_ctx, NULL) < 0) {
		SYS_LOG_ERR("Error when reporting acId feedback to Hawkbit");
		return -1;
	}

	return 0;
}

static int hawkbit_ddi_poll(struct hawkbit_context *hb_ctx)
{
	jsmn_parser jsmnp;
	jsmntok_t jtks[60];	/* Enough for one artifact per SM */
	int i, ret, len, ntk;
	static hawkbit_update_action_t hawkbit_update_action;
	static int json_acid;
	struct hawkbit_device_acid device_acid;
	struct json_data_t json = { NULL, 0 };
	char deployment_base[40];	/* TODO: Find a better value */
	char download_http[200];	/* TODO: Find a better value */
	bool update_config_data = false;
	int file_size = 0;
	char *helper;
	const struct product_id_t *product_id = product_id_get();

	SYS_LOG_DBG("Polling target data from Hawkbit");

	/* Build URL */
	snprintf(hb_ctx->url_buffer, hb_ctx->url_buffer_size, "%s/%s-%x",
		 HAWKBIT_JSON_URL, product_id->name, product_id->number);

	memset(&hb_ctx->http_req, 0, sizeof(hb_ctx->http_req));
	hb_ctx->http_req.method = HTTP_GET;
	hb_ctx->http_req.url = hb_ctx->url_buffer;
	hb_ctx->http_req.host = HAWKBIT_HOST;
	hb_ctx->http_req.protocol = " " HTTP_PROTOCOL HTTP_CRLF;
	hb_ctx->http_req.header_fields = HTTP_HEADER_CONNECTION_CLOSE_CRLF;

	ret = hawkbit_query(hb_ctx, &json);
	if (ret < 0) {
		SYS_LOG_ERR("Error when polling from Hawkbit");
		return ret;
	}

	ntk = json_parser(&json, &jsmnp, jtks,
			  sizeof(jtks) / sizeof(jsmntok_t));
	if (ntk <= 0) {
		SYS_LOG_ERR("Error when parsing JSON from target");
		return -1;
	}

	/* Hawkbit DDI v1 targetid */
	memset(deployment_base, 0, sizeof(deployment_base));
	/* TODO: Implement cancel action logic */
	for (i = 1; i < ntk - 1; i++) {
		/* config -> polling -> sleep */
		if (jsoneq(json.data, &jtks[i], "config") &&
				(i + 5 < ntk) &&
				(jsoneq(json.data, &jtks[i + 4], "sleep"))) {
			/* Sleep format: HH:MM:SS */
			if (jtks[i + 5].end - jtks[i + 5].start > 8) {
				SYS_LOG_ERR("Invalid poll sleep string");
				continue;
			}
			len = hawkbit_time2sec(json.data + jtks[i + 5].start);
			if (len > 0 &&
				poll_sleep != K_SECONDS(len)) {
				SYS_LOG_INF("New poll sleep %d seconds", len);
				poll_sleep = K_SECONDS(len);
				i += 5;
			}
		} else if (jsoneq(json.data, &jtks[i], "deploymentBase") &&
				(i + 3 < ntk) &&
				(jsoneq(json.data, &jtks[i + 2], "href"))) {
			/* Just extract the deploymentBase piece */
			helper = strstr(json.data + jtks[i + 3].start,
							"deploymentBase/");
			if (helper == NULL ||
					helper > json.data + jtks[i + 3].end) {
				continue;
			}
			len = json.data + jtks[i + 3].end - helper;
			memcpy(&deployment_base, helper, len);
			deployment_base[len] = '\0';
			SYS_LOG_DBG("Deployment base %s", deployment_base);
			i += 3;
		} else if (jsoneq(json.data, &jtks[i], "configData") &&
				(i + 3 < ntk) &&
				(jsoneq(json.data, &jtks[i + 2], "href"))) {
			update_config_data = true;
			i += 3;
		}
	}

	/* Update config data if the server asked for it */
	if (update_config_data) {
		hawkbit_report_config_data(hb_ctx);
	}

	if (strlen(deployment_base) == 0) {
		SYS_LOG_DBG("No deployment base found, no actions to take");
		return 0;
	}

	memset(&json, 0, sizeof(struct json_data_t));

	/* Build URL: Hawkbit DDI v1 deploymentBase */
	snprintf(hb_ctx->url_buffer, hb_ctx->url_buffer_size, "%s/%s-%x/%s",
		 HAWKBIT_JSON_URL, product_id->name, product_id->number,
		 deployment_base);

	memset(&hb_ctx->http_req, 0, sizeof(hb_ctx->http_req));
	hb_ctx->http_req.method = HTTP_GET;
	hb_ctx->http_req.url = hb_ctx->url_buffer;
	hb_ctx->http_req.host = HAWKBIT_HOST;
	hb_ctx->http_req.protocol = " " HTTP_PROTOCOL HTTP_CRLF;
	hb_ctx->http_req.header_fields = HTTP_HEADER_CONNECTION_CLOSE_CRLF;

	if (hawkbit_query(hb_ctx, &json) < 0) {
		SYS_LOG_ERR("Error when querying from Hawkbit");
		return -1;
	}

	/* We have our own limit here, which is directly affected by the
	 * number of artifacts available as part of the software module
	 * assigned, so needs coordination with the deployment process.
	 */
	ntk = json_parser(&json, &jsmnp, jtks,
			  sizeof(jtks) / sizeof(jsmntok_t));
	if (ntk <= 0) {
		SYS_LOG_ERR("Error when parsing JSON from deploymentBase");
		return -1;
	}

	ret = 0;
	memset(download_http, 0, sizeof(download_http));
	for (i = 1; i < ntk - 1; i++) {
		if (jsoneq(json.data, &jtks[i], "id")) {
			/* id -> id */
			json_acid = atoi_n(json.data + jtks[i + 1].start,
					jtks[i + 1].end - jtks[i + 1].start);
			SYS_LOG_DBG("Hawkbit ACTION ID %d", json_acid);
			i += 1;
		} else if (jsoneq(json.data, &jtks[i], "deployment")) {
			/* deployment -> download, update or chunks */
			if (i + 5 >= ntk) {
				continue;
			}
			/* Check just the first 2 keys, since chunk is [] */
			if (jsoneq(json.data, &jtks[i + 2], "update")) {
				i += 3;
			} else if (jsoneq(json.data, &jtks[i + 4], "update")) {
				i += 5;
			} else {
				continue;
			}
			/* Now just find the update action */
			if (jsoneq(json.data, &jtks[i], "skip")) {
				hawkbit_update_action = HAWKBIT_UPDATE_SKIP;
				SYS_LOG_DBG("Hawkbit update action: SKIP");
			} else if (jsoneq(json.data, &jtks[i], "attempt")) {
				hawkbit_update_action = HAWKBIT_UPDATE_ATTEMPT;
				SYS_LOG_DBG("Hawkbit update action: ATTEMPT");
			} else if (jsoneq(json.data, &jtks[i], "forced")) {
				hawkbit_update_action = HAWKBIT_UPDATE_FORCED;
				SYS_LOG_DBG("Hawkbit update action: FORCED");
			}
		} else if (jsoneq(json.data, &jtks[i], "chunks")) {
			if (jtks[i + 1].type != JSMN_ARRAY) {
				continue;
			}
			if (jtks[i + 1].size != 1) {
				SYS_LOG_ERR("Only one chunk is supported, %d",
							jtks[i + 1].size);
				ret = -1;
				break;
			}
			i += 1;
		} else if (jsoneq(json.data, &jtks[i], "part")) {
			if (!jsoneq(json.data, &jtks[i + 1], "os")) {
				SYS_LOG_ERR("Only part 'os' is supported");
				ret = -1;
				break;
			}
			i += 1;
		} else if (jsoneq(json.data, &jtks[i], "size")) {
			file_size = atoi_n(json.data + jtks[i + 1].start,
					jtks[i + 1].end - jtks[i + 1].start);
			SYS_LOG_DBG("Artifact file size: %d", file_size);
			i += 1;
		} else if (jsoneq(json.data, &jtks[i], "download-http")) {
			/* We just support DEFAULT tenant on the same server */
			if (i + 3 >= ntk ||
				!jsoneq(json.data, &jtks[i + 2], "href")) {
				SYS_LOG_ERR("No href entry for download-http");
				ret = -1;
				continue;
			}
			/* Extracting everying after server address */
			helper = strstr(json.data + jtks[i + 3].start,
						"/DEFAULT/controller/v1");
			if (helper == NULL ||
					helper > json.data + jtks[i + 3].end) {
				continue;
			}
			len = json.data + jtks[i + 3].end - helper;
			if (len >= sizeof(download_http)) {
				SYS_LOG_ERR("Download HREF too big (%d)", len);
				ret = - 1;
				continue;
			}
			memcpy(&download_http, helper, len);
			download_http[len] = '\0';
			SYS_LOG_DBG("Artifact address: %s", download_http);
			i += 3;
		}
	}

	hawkbit_device_acid_read(&device_acid);

	if (device_acid.current == json_acid) {
		/* We are coming from a successful flash, update the server */
		hawkbit_report_update_status(hb_ctx, json_acid,
					     HAWKBIT_RESULT_SUCCESS,
					     HAWKBIT_EXEC_CLOSED);
		return 0;
	} else if (device_acid.update == json_acid) {
		/* There was already an atempt, so announce a failure */
		hawkbit_report_update_status(hb_ctx, json_acid,
					     HAWKBIT_RESULT_FAILURE,
					     HAWKBIT_EXEC_CLOSED);
		return 0;
	}

	/* Perform the action */
	if (strlen(download_http) == 0) {
		SYS_LOG_DBG("No download http address found, no action");
		return 0;
	}
	/* Error detected when parsing the SM */
	if (ret == -1) {
		hawkbit_report_update_status(hb_ctx, json_acid,
					     HAWKBIT_RESULT_FAILURE,
					     HAWKBIT_EXEC_CLOSED);
		return -1;
	}
	if (file_size > FLASH_BANK_SIZE) {
		SYS_LOG_ERR("Artifact file size too big (%d)", file_size);
		hawkbit_report_update_status(hb_ctx, json_acid,
					     HAWKBIT_RESULT_FAILURE,
					     HAWKBIT_EXEC_CLOSED);
		return -1;
	}

	/* Here we should have everything we need to apply the action */
	SYS_LOG_INF("Valid action ID %d found, proceeding with the update",
					json_acid);
	hawkbit_report_update_status(hb_ctx, json_acid,
				     HAWKBIT_RESULT_SUCCESS,
				     HAWKBIT_EXEC_PROCEEDING);
	ret = hawkbit_install_update(hb_ctx, download_http, file_size);
	if (ret != 0) {
		SYS_LOG_ERR("Failed to install the update for action ID %d",
			    json_acid);
		return -1;
	}

	SYS_LOG_INF("Triggering OTA update.");
	boot_trigger_ota();
	ret = hawkbit_device_acid_update(HAWKBIT_ACID_UPDATE, json_acid);
	if (ret != 0) {
		SYS_LOG_ERR("Failed to update ACID: %d", ret);
		return -1;
	}
	SYS_LOG_INF("Image id %d flashed successfuly, rebooting now",
					json_acid);

	/* Reboot and let the bootloader take care of the swap process */
	sys_reboot(0);

	return 0;
}

/* Firmware OTA thread (Hawkbit) */
static void hawkbit_service(void)
{
	u32_t hawkbit_failures = 0;
	int ret;

	SYS_LOG_INF("Starting FOTA Service Thread");

	do {
		k_sleep(poll_sleep);

		if (!connection_ready) {
			SYS_LOG_DBG("Network interface is not ready");
			continue;
		}

		tcp_interface_lock();

		ret = hawkbit_ddi_poll(&hbc);
		if (ret < 0) {
			hawkbit_failures++;
			if (hawkbit_failures == HAWKBIT_MAX_SERVER_FAIL) {
				SYS_LOG_ERR("Too many unsuccessful poll"
					    " attempts, rebooting!");
				sys_reboot(0);
			}
		} else {
			/* restart the failed attempt counter */
			hawkbit_failures = 0;
		}

		tcp_interface_unlock();

		STACK_ANALYZE("Hawkbit Thread", hawkbit_thread_stack);
	} while (1);
}

static void event_iface_up(struct net_mgmt_event_callback *cb,
			   u32_t mgmt_event, struct net_if *iface)
{
	connection_ready = true;
}

int hawkbit_init(void)
{
	/* TODO: default interface may not always be the one we want */
	struct net_if *iface = net_if_get_default();
	int ret;

	ret = hawkbit_start();
	if (ret) {
		SYS_LOG_ERR("Hawkbit Client initialization generated "
			    "an error: %d", ret);
		return ret;
	}

	memset(&hbc, 0, sizeof(hbc));
	hbc.tcp_buffer_size = TCP_RECV_BUFFER_SIZE;
	hbc.url_buffer_size = URL_BUFFER_SIZE;
	hbc.status_buffer_size = STATUS_BUFFER_SIZE;

	k_thread_create(&hawkbit_thread_data, &hawkbit_thread_stack[0],
			K_THREAD_STACK_SIZEOF(hawkbit_thread_stack),
			(k_thread_entry_t) hawkbit_service,
			NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

#if defined(CONFIG_NET_MGMT_EVENT)
	/* Subscribe to NET_IF_UP if interface is not ready */
	if (!atomic_test_bit(iface->flags, NET_IF_UP)) {
		net_mgmt_init_event_callback(&cb, event_iface_up,
					     NET_EVENT_IF_UP);
		net_mgmt_add_event_callback(&cb);
		return 0;
	}
#endif

	event_iface_up(NULL, NET_EVENT_IF_UP, iface);

	return ret;
}
