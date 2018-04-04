/*
 * Copyright (c) 2016-2017 Linaro Limited
 * Copyright (c) 2018 Open Source Foundries Limited
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
#include <stdlib.h>
#include <errno.h>
#include <misc/byteorder.h>
#include <flash.h>
#include <zephyr.h>
#include <dfu/mcuboot.h>
#include <dfu/flash_img.h>
#include <misc/reboot.h>
#include <net/http.h>
#include <net/net_app.h>
#include <net/net_event.h>
#include <net/net_if.h>
#include <net/net_mgmt.h>
#include <json.h>

#include <soc.h>

#include "hawkbit.h"
#include "hawkbit_priv.h"
#include "product_id.h"

/*
 * Uncomment for extra debug printing.
 *
 * Currently, this dumps JSON responses received from the server.
 */
/* #define HAWKBIT_EXTRA_DEBUG */

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
#define STATUS_BUFFER_SIZE	200
#define HTTP_HEADER_BUFFER_SIZE	512

struct hawkbit_download {
	size_t http_content_size;
	size_t downloaded_size;
	int download_progress;
	int download_status;
};

struct hawkbit_context {
	int failures;
	struct http_ctx http_ctx;
	struct http_request http_req;
	u8_t tcp_buffer[TCP_RECV_BUFFER_SIZE];
	size_t tcp_buffer_size;
	u8_t url_buffer[URL_BUFFER_SIZE];
	size_t url_buffer_size;
	u8_t status_buffer[STATUS_BUFFER_SIZE];
	size_t status_buffer_size;
	struct hawkbit_download dl;
	struct k_work_q *work_q;
	struct k_delayed_work work;
	struct k_sem *sem;
};

struct hawkbit_device_acid {
	u32_t current;
	u32_t update;
};

struct json_data_t {
	char *data;
	size_t len;
};

typedef enum {
	HAWKBIT_ACID_CURRENT = 0,
	HAWKBIT_ACID_UPDATE,
} hawkbit_dev_acid_t;

#define HAWKBIT_RX_TIMEOUT	K_SECONDS(10)

static int poll_sleep = K_SECONDS(30);
#if defined(CONFIG_NET_MGMT_EVENT)
static struct net_mgmt_event_callback cb;
#endif

#define HAWKBIT_DOWNLOAD_TIMEOUT	K_SECONDS(10)

#define HTTP_HEADER_CONTENT_TYPE_JSON		"application/json"
#define HTTP_HEADER_CONNECTION_CLOSE_CRLF	"Connection: close\r\n"

static struct hawkbit_context hb_context;
static struct k_sem hb_sem;

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

static struct device *flash_dev;
static struct flash_img_context dfu_ctx;

#define FLASH_BANK_SIZE FLASH_AREA_IMAGE_1_SIZE

/*
 * Descriptors for mapping between JSON and structure representations.
 */

static const struct json_obj_descr json_href_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_href, href, JSON_TOK_STRING),
};

static const struct json_obj_descr json_status_result_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_status_result, finished,
			    JSON_TOK_STRING),
};

static const struct json_obj_descr json_status_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_status, execution, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_status, result,
			      json_status_result_descr),
};

static const struct json_obj_descr json_ctl_res_sleep_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_ctl_res_sleep, sleep,
			    JSON_TOK_STRING),
};

static const struct json_obj_descr json_ctl_res_polling_descr[] = {
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res_polling, polling,
			      json_ctl_res_sleep_descr),
};

static const struct json_obj_descr json_ctl_res_links_descr[] = {
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res_links, deploymentBase,
			      json_href_descr),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res_links, cancelAction,
			      json_href_descr),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res_links, configData,
			      json_href_descr),
};

static const struct json_obj_descr json_ctl_res_descr[] = {
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res, config,
			      json_ctl_res_polling_descr),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res, _links,
			      json_ctl_res_links_descr),
};

static const struct json_obj_descr json_cfg_data_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_cfg_data, board, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_cfg_data, serial, JSON_TOK_STRING),
};

static const struct json_obj_descr json_cfg_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_cfg, id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_cfg, status, json_status_descr),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_cfg, data, json_cfg_data_descr),
};

static const struct json_obj_descr json_dep_res_hashes_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_hashes, sha1,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_hashes, md5,
			    JSON_TOK_STRING),
};

static const struct json_obj_descr json_dep_res_links_descr[] = {
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_dep_res_links, download,
			      json_href_descr),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_dep_res_links, md5sum,
			      json_href_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct hawkbit_dep_res_links,
				    "download-http", download_http,
				    json_href_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct hawkbit_dep_res_links,
				    "md5sum-http", md5sum_http,
				    json_href_descr),
};

static const struct json_obj_descr json_dep_res_arts_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_arts, filename,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_arts, size,
			    JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_dep_res_arts, hashes,
			      json_dep_res_hashes_descr),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_dep_res_arts, _links,
			      json_dep_res_links_descr),
};

static const struct json_obj_descr json_dep_res_chunk_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_chunk, part,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_chunk, name,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_chunk, version,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJ_ARRAY(struct hawkbit_dep_res_chunk, artifacts,
				 HAWKBIT_DEP_MAX_CHUNK_ARTS, num_artifacts,
				 json_dep_res_arts_descr,
				 ARRAY_SIZE(json_dep_res_arts_descr)),
};

static const struct json_obj_descr json_dep_res_deploy_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_deploy, download,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_deploy, update,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJ_ARRAY(struct hawkbit_dep_res_deploy, chunks,
				 HAWKBIT_DEP_MAX_CHUNKS, num_chunks,
				 json_dep_res_chunk_descr,
				 ARRAY_SIZE(json_dep_res_chunk_descr)),
};

static const struct json_obj_descr json_dep_res_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res, id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_dep_res, deployment,
			      json_dep_res_deploy_descr),
};

static const struct json_obj_descr json_dep_fbk_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_fbk, id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_dep_fbk, status,
			      json_status_descr),
};

/*
 * JSON debug helpers; these may be unused.
 */

__unused
static const char *str_or_null(const char *str)
{
	if (str) {
		return str;
	} else {
		return "NULL";
	}
}

__unused
static void hawkbit_dump_base(struct hawkbit_ctl_res *r, const char *comment)
{
	SYS_LOG_DBG("Base polling resource results %s:\n\t"
		    "config.polling.sleep=%s\n\t"
		    "_links.deploymentBase.href=%s\n\t"
		    "_links.configData.href=%s\n\t"
		    "_links.cancelAction.href=%s\n\t",
		    comment,
		    str_or_null(r->config.polling.sleep),
		    str_or_null(r->_links.deploymentBase.href),
		    str_or_null(r->_links.configData.href),
		    str_or_null(r->_links.cancelAction.href));
}

__unused
static void hawkbit_dump_deployment(struct hawkbit_dep_res *d,
				    const char *comment)
{
	struct hawkbit_dep_res_chunk *c = &d->deployment.chunks[0];
	struct hawkbit_dep_res_arts *a = &c->artifacts[0];
	struct hawkbit_dep_res_links *l = &a->_links;

	SYS_LOG_DBG("Deployment base results %s:\n\t"
		    "id=%s\n\t"
		    "deployment =\n\t\t"
		    "download=%s\n\t\t"
		    "update=%s\n\t\t"
		    "chunks[0].part=%s\n\t\t"
		    "         .name=%s\n\t\t"
		    "         .version=%s\n\t\t"
		    "         .artifacts[0].filename=%s\n\t\t"
		    "                      .size=%d\n\t\t"
		    "                      .hashes = sha1=%s,md5=%s\n\t\t"
		    "                      ._links =\n\t\t\t"
		    "                                 download=%s\n\t\t\t"
		    "                                 md5sum=%s\n\t\t\t"
		    "                                 download_http=%s\n\t\t\t"
		    "                                 md5sum_http=%s\n\t\t\t",
		    comment,
		    str_or_null(d->id),
		    str_or_null(d->deployment.download),
		    str_or_null(d->deployment.update),
		    str_or_null(c->part),
		    str_or_null(c->name),
		    str_or_null(c->version),
		    str_or_null(a->filename),
		    a->size,
		    str_or_null(a->hashes.sha1),
		    str_or_null(a->hashes.md5),
		    str_or_null(l->download.href),
		    str_or_null(l->md5sum.href),
		    str_or_null(l->download_http.href),
		    str_or_null(l->md5sum_http.href));
}

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

static const char *hawkbit_status_finished(enum hawkbit_status_fini f)
{
	switch (f) {
	case HAWKBIT_STATUS_FINISHED_SUCCESS:
		return "success";
	case HAWKBIT_STATUS_FINISHED_FAILURE:
		return "failure";
	case HAWKBIT_STATUS_FINISHED_NONE:
		return "none";
	default:
		SYS_LOG_ERR("%d is invalid", (int)f);
		return NULL;
	}
}

static const char *hawkbit_status_execution(enum hawkbit_status_exec e)
{
	switch (e) {
	case HAWKBIT_STATUS_EXEC_CLOSED:
		return "closed";
	case HAWKBIT_STATUS_EXEC_PROCEEDING:
		return "proceeding";
	case HAWKBIT_STATUS_EXEC_CANCELED:
		return "canceled";
	case HAWKBIT_STATUS_EXEC_SCHEDULED:
		return "scheduled";
	case HAWKBIT_STATUS_EXEC_REJECTED:
		return "rejected";
	case HAWKBIT_STATUS_EXEC_RESUMED:
		return "resumed";
	default:
		SYS_LOG_ERR("%d is invalid", (int)e);
		return NULL;
	}
}

static void hawkbit_device_acid_read(struct hawkbit_device_acid *device_acid)
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

/* Log the semantic version number of the current image. */
static void log_img_ver(void)
{
	struct mcuboot_img_header header;
	struct mcuboot_img_sem_ver *ver;
	int ret;

	ret = boot_read_bank_header(FLASH_AREA_IMAGE_0_OFFSET,
				    &header, sizeof(header));
	if (ret) {
		SYS_LOG_ERR("can't read header: %d", ret);
		return;
	} else if (header.mcuboot_version != 1) {
		SYS_LOG_ERR("unsupported MCUboot version %u",
			    header.mcuboot_version);
		return;
	}

	ver = &header.h.v1.sem_ver;
	SYS_LOG_INF("image version %u.%u.%u build #%u",
		    ver->major, ver->minor, ver->revision, ver->build_num);
}

static int hawkbit_init_flash(void)
{
	int ret = 0;
	struct hawkbit_device_acid init_acid;
	bool image_ok;

	/*
	 * Initialize the DFU context.
	 */
	flash_dev = device_get_binding(FLASH_DEV_NAME);
	if (!flash_dev) {
		SYS_LOG_ERR("missing flash device %s", FLASH_DEV_NAME);
		return -ENODEV;
	}

	log_img_ver();

	/* Update boot status and acid */
	hawkbit_device_acid_read(&init_acid);
	SYS_LOG_INF("ACID: current %d, update %d",
		    init_acid.current, init_acid.update);
	image_ok = boot_is_img_confirmed();
	SYS_LOG_INF("Image is%s confirmed OK", image_ok ? "" : " not");
	if (!image_ok) {
		ret = boot_write_img_confirmed();
		if (ret) {
			SYS_LOG_ERR("Couldn't confirm this image: %d", ret);
			return ret;
		}
		SYS_LOG_INF("Marked image as OK");
		ret = boot_erase_img_bank(FLASH_AREA_IMAGE_1_OFFSET);
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
static void install_update_cb(struct http_ctx *ctx,
			      u8_t *data, size_t data_size,
			      size_t data_len,
			      enum http_final_call final_data,
			      void *user_data)
{
	struct hawkbit_context *hbc = user_data;
	int downloaded, ret = 0;
	u8_t *body_data = NULL;
	size_t body_len = 0;

	/* HTTP error */
	if (ctx->http.parser.status_code != 200) {
		SYS_LOG_ERR("HTTP error: %d!", ctx->http.parser.status_code);
		goto error;
	}

	/* header hasn't been read yet */
	if (hbc->dl.http_content_size == 0) {
		if (ctx->http.rsp.body_found == 0) {
			SYS_LOG_ERR("Callback called w/o HTTP header found!");
			goto error;
		}

		body_data = ctx->http.rsp.body_start;
		body_len = data_len;
		body_len -= (ctx->http.rsp.body_start -
			     ctx->http.rsp.response_buf);
		hbc->dl.http_content_size = ctx->http.rsp.content_length;
	}

	if (body_data == NULL) {
		body_data = ctx->http.rsp.response_buf;
		body_len = data_len;
	}

	/* everything looks good: flash */
	ret = flash_img_buffered_write(&dfu_ctx,
				       body_data, body_len,
				       final_data == HTTP_DATA_FINAL);
	if (ret < 0) {
		SYS_LOG_ERR("Flash write error: %d", ret);
		goto error;
	}
	hbc->dl.downloaded_size = flash_img_bytes_written(&dfu_ctx);

	downloaded = hbc->dl.downloaded_size * 100 /
		     hbc->dl.http_content_size;
	if (downloaded > hbc->dl.download_progress) {
		hbc->dl.download_progress = downloaded;
		SYS_LOG_DBG("%d%%", hbc->dl.download_progress);
	}

	if (final_data == HTTP_DATA_FINAL) {
		hbc->dl.download_status = 1;
		k_sem_give(hbc->sem);
	}

	return;

error:
	hbc->dl.download_status = -1;
	k_sem_give(hbc->sem);
}

static int hawkbit_install_update(struct hawkbit_context *hbc,
				  const char *download_http,
				  size_t file_size)
{
	struct hawkbit_download *dl = &hbc->dl;
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
	memset(hbc->tcp_buffer, 0, hbc->tcp_buffer_size);
	memset(&hbc->dl, 0, sizeof(struct hawkbit_download));
	/* reset download semaphore -- TODO is this really needed? */
	k_sem_init(hbc->sem, 0, 1);
	/* Re-initialize the flash writer state. */
	flash_img_init(&dfu_ctx, flash_dev);

	ret = http_client_init(&hbc->http_ctx,
			       HAWKBIT_SERVER_ADDR, HAWKBIT_PORT,
			       NULL, HAWKBIT_RX_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("Failed to init http ctx, err %d", ret);
		return ret;
	}

#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
	net_app_set_net_pkt_pool(&hbc->http_ctx.app_ctx, tx_slab, data_pool);
#endif

	ret = http_client_send_get_req(&hbc->http_ctx, download_http,
				       HAWKBIT_HOST,
				       HTTP_HEADER_CONNECTION_CLOSE_CRLF,
				       install_update_cb,
				       hbc->tcp_buffer,
				       hbc->tcp_buffer_size,
				       hbc, K_NO_WAIT);
	/* http_client returns EINPROGRESS for get_req w/ K_NO_WAIT */
	if (ret < 0 && ret != -EINPROGRESS) {
		SYS_LOG_ERR("Failed to send request, err %d", ret);
		return ret;
	}

	while (k_sem_take(hbc->sem, HAWKBIT_DOWNLOAD_TIMEOUT)) {
		/* wait timeout: check for download activity */
		if (last_downloaded_size == dl->downloaded_size) {
			/* no activity: break loop */
			break;
		} else {
			last_downloaded_size = dl->downloaded_size;
		}
	}

	/* clean up context */
	http_release(&hbc->http_ctx);

	if (dl->download_status < 0) {
		SYS_LOG_ERR("Unable to finish the download process %d",
			    dl->download_status);
		return -1;
	}

	if (dl->downloaded_size != dl->http_content_size) {
		SYS_LOG_ERR("Download: downloaded image size mismatch, "
			    "downloaded %zu, expecting %zu",
			    dl->downloaded_size, dl->http_content_size);
		return -1;
	}

	if (dl->downloaded_size != file_size) {
		SYS_LOG_ERR("Download: downloaded image size mismatch, "
			    "downloaded %zu, expecting from JSON %zu",
			    dl->downloaded_size, file_size);
		return -1;
	}

	SYS_LOG_INF("Download: downloaded bytes %zu", dl->downloaded_size);
	return 0;
}

static int hawkbit_query(struct hawkbit_context *hbc,
			 struct json_data_t *json)
{
	int ret = 0;

	SYS_LOG_DBG("[%s] HOST:%s URL:%s",
		    http_method_str(hbc->http_req.method),
		    hbc->http_req.host, hbc->http_req.url);

	memset(hbc->tcp_buffer, 0, hbc->tcp_buffer_size);

	ret = http_client_init(&hbc->http_ctx,
			       HAWKBIT_SERVER_ADDR, HAWKBIT_PORT,
			       NULL, HAWKBIT_RX_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("Failed to init http ctx, err %d", ret);
		return ret;
	}

#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
	net_app_set_net_pkt_pool(&hbc->http_ctx.app_ctx, tx_slab, data_pool);
#endif

	ret = http_client_send_req(&hbc->http_ctx, &hbc->http_req, NULL,
				   hbc->tcp_buffer, hbc->tcp_buffer_size,
				   NULL, HAWKBIT_RX_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("Failed to send buffer, err %d", ret);
		goto cleanup;
	}

	if (hbc->http_ctx.http.rsp.data_len == 0) {
		SYS_LOG_ERR("No received data (rsp.data_len: %zu)",
			    hbc->http_ctx.http.rsp.data_len);
		ret = -EIO;
		goto cleanup;
	}

	if (hbc->http_ctx.http.parser.status_code != 200) {
		SYS_LOG_ERR("Invalid HTTP status code [%d]",
			    hbc->http_ctx.http.parser.status_code);
		ret = -1;
		goto cleanup;
	}

	if (json) {
		json->data = hbc->http_ctx.http.rsp.body_start;
		json->len = strlen(hbc->http_ctx.http.rsp.response_buf);
		json->len -= hbc->http_ctx.http.rsp.body_start -
			     hbc->http_ctx.http.rsp.response_buf;

		/* FIXME: Each poll needs a new connection, this saves
		 * us from using content from a previous package.
		 */
		json->data[json->len] = '\0';
		SYS_LOG_DBG("JSON DATA:\n%s", json->data);
	}

	SYS_LOG_DBG("Hawkbit query completed");

cleanup:
	/* clean up context */
	http_release(&hbc->http_ctx);
	return ret;
}

/*
 * Update sleep interval, based on results from hawkBit base polling
 * resource.
 */
static void hawkbit_update_sleep(struct hawkbit_ctl_res *hawkbit_res)
{
	const char *sleep = hawkbit_res->config.polling.sleep;
	int len;

	if (strlen(sleep) != HAWKBIT_SLEEP_LENGTH) {
		SYS_LOG_ERR("invalid poll sleep: %s", sleep);
	} else {
		len = hawkbit_time2sec(sleep);
		if (len > 0 && poll_sleep != K_SECONDS(len)) {
			SYS_LOG_INF("New poll sleep %d seconds", len);
			poll_sleep = K_SECONDS(len);
		}
	}
}

static int hawkbit_report_config_data(struct hawkbit_context *hbc)
{
	const struct product_id_t *product_id = product_id_get();
	char product_id_number[11]; /* This is large enough for a u32_t. */
	struct hawkbit_cfg cfg;
	int ret;

	SYS_LOG_INF("Reporting target config data to Hawkbit");

	/* Build URL */
	snprintk(hbc->url_buffer, hbc->url_buffer_size,
		 "%s/%s-%x/configData", HAWKBIT_JSON_URL,
		 product_id->name, product_id->number);

	/* Build JSON */
	memset(&cfg, 0, sizeof(cfg));
	snprintk(product_id_number, sizeof(product_id_number), "%u",
		 product_id->number);
	cfg.id = "";
	cfg.status.execution =
		hawkbit_status_execution(HAWKBIT_STATUS_EXEC_CLOSED);
	cfg.status.result.finished =
		hawkbit_status_finished(HAWKBIT_STATUS_FINISHED_SUCCESS);
	cfg.data.board = product_id->name;
	cfg.data.serial = product_id_number;
	ret = json_obj_encode_buf(json_cfg_descr, ARRAY_SIZE(json_cfg_descr),
				  &cfg, hbc->status_buffer,
				  hbc->status_buffer_size - 1);
	if (ret) {
		SYS_LOG_ERR("can't encode response: %d", ret);
		return ret;
	}
	SYS_LOG_DBG("JSON response: %s", hbc->status_buffer);

	memset(&hbc->http_req, 0, sizeof(hbc->http_req));
	hbc->http_req.method = HTTP_PUT;
	hbc->http_req.url = hbc->url_buffer;
	hbc->http_req.host = HAWKBIT_HOST;
	hbc->http_req.protocol = " " HTTP_PROTOCOL;
	hbc->http_req.header_fields = HTTP_HEADER_CONNECTION_CLOSE_CRLF;
	hbc->http_req.content_type_value = "application/json";
	hbc->http_req.payload = hbc->status_buffer;
	hbc->http_req.payload_size = strlen(hbc->status_buffer);

	if (hawkbit_query(hbc, NULL)) {
		SYS_LOG_ERR("Error when reporting config data to Hawkbit");
		return -1;
	}

	return 0;
}

/*
 * Find URL component for this device's deployment operations
 * resource.
 */
static int hawkbit_find_deployment_base(struct hawkbit_ctl_res *res,
					char *deployment_base,
					size_t deployment_base_size)
{
	const char *href;
	const char *helper;
	size_t len;

	href = res->_links.deploymentBase.href;
	if (!href) {
		/* A missing deployment base is not an error. */
		*deployment_base = '\0';
		return 0;
	}
	helper = strstr(href, "deploymentBase/");
	if (!helper) {
		/* A badly formatted deployment base is a server error. */
		SYS_LOG_ERR("missing deploymentBase/ in href %s", href);
		return -EINVAL;
	}
	len = strlen(helper);
	if (len > deployment_base_size - 1) {
		/* Lack of memory is an application error. */
		SYS_LOG_ERR("deploymentBase %s is too big (len %zu, max %zu)",
			    helper, len, deployment_base_size - 1);
		return -ENOMEM;
	}
	strncpy(deployment_base, helper, deployment_base_size);
	return 0;
}

/*
 * Parse the results of polling the deployment operations resource.
 */
static int hawkbit_parse_deployment(struct hawkbit_dep_res *res,
				    int *json_acid,
				    char *download_http,
				    size_t download_http_size,
				    s32_t *file_size)
{
	const char *href;
	const char *helper;
	size_t len;
	struct hawkbit_dep_res_chunk *chunk;
	struct hawkbit_dep_res_arts *artifact;
	size_t num_chunks, num_artifacts;
	s32_t acid, size;

	acid = strtol(res->id, NULL, 10);
	if (acid < 0) {
		SYS_LOG_ERR("negative action ID %d", acid);
		return -EINVAL;
	}
	*json_acid = acid;
	num_chunks = res->deployment.num_chunks;
	if (num_chunks != 1) {
		SYS_LOG_ERR("expecting one chunk (got %d)", num_chunks);
		return -ENOSPC;
	}
	chunk = &res->deployment.chunks[0];
	if (strcmp("os", chunk->part)) {
		SYS_LOG_ERR("only part 'os' is supported; got %s", chunk->part);
		return -EINVAL;
	}
	num_artifacts = chunk->num_artifacts;
	if (num_artifacts != 1) {
		SYS_LOG_ERR("expecting one artifact (got %d)", num_artifacts);
		return -EINVAL;
	}
	artifact = &chunk->artifacts[0];
	size = artifact->size;
	if (size > FLASH_BANK_SIZE) {
		SYS_LOG_ERR("artifact file size too big (got %d, max is %d)",
			    size, FLASH_BANK_SIZE);
		return -ENOSPC;
	}
	/*
	 * Find the download-http href. We only support the DEFAULT
	 * tenant on the same hawkBit server.
	 */
	href = artifact->_links.download_http.href;
	if (!href) {
		SYS_LOG_ERR("missing expected download-http href");
		return -EINVAL;
	}
	helper = strstr(href, "/DEFAULT/controller/v1");
	if (!helper) {
		SYS_LOG_ERR("unexpected download-http href format: %s", helper);
		return -EINVAL;
	}
	len = strlen(helper);
	if (len == 0) {
		SYS_LOG_ERR("empty download-http");
		return -EINVAL;
	} else if (len > download_http_size - 1) {
		SYS_LOG_ERR("download-http %s is too big (len: %zu, max: %zu)",
			    helper, len, download_http_size - 1);
		return -ENOMEM;
	}
	/* Success. */
	strncpy(download_http, helper, download_http_size);
	*file_size = size;
	return 0;
}

static int hawkbit_report_dep_fbk(struct hawkbit_context *hbc,
				  s32_t action_id,
				  enum hawkbit_status_fini finished,
				  enum hawkbit_status_exec execution)
{
	const struct product_id_t *product_id = product_id_get();
	struct hawkbit_dep_fbk feedback;
	char acid[11]; /* This is large enough for a 32 bit integer. */
	const char *fini = hawkbit_status_finished(finished);
	const char *exec = hawkbit_status_execution(execution);
	int ret;

	if (!fini || !exec) {
		return -EINVAL;
	}

	SYS_LOG_INF("Reporting deployment feedback %s (%s) for action %d",
		    fini, exec, action_id);

	/* Build URL */
	snprintk(hbc->url_buffer, hbc->url_buffer_size,
		 "%s/%s-%x/deploymentBase/%d/feedback",
		 HAWKBIT_JSON_URL, product_id->name, product_id->number,
		 action_id);

	/* Build JSON */
	memset(&feedback, 0, sizeof(feedback));
	snprintk(acid, sizeof(acid), "%d", action_id);
	feedback.id = acid;
	feedback.status.result.finished = fini;
	feedback.status.execution = exec;
	ret = json_obj_encode_buf(json_dep_fbk_descr,
				  ARRAY_SIZE(json_dep_fbk_descr),
				  &feedback, hbc->status_buffer,
				  hbc->status_buffer_size - 1);
	if (ret) {
		SYS_LOG_ERR("Can't encode response: %d", ret);
		return ret;
	}
	SYS_LOG_DBG("JSON response: %s", hbc->status_buffer);

	memset(&hbc->http_req, 0, sizeof(hbc->http_req));
	hbc->http_req.method = HTTP_POST;
	hbc->http_req.url = hbc->url_buffer;
	hbc->http_req.host = HAWKBIT_HOST;
	hbc->http_req.protocol = " " HTTP_PROTOCOL;
	hbc->http_req.header_fields = HTTP_HEADER_CONNECTION_CLOSE_CRLF;
	hbc->http_req.content_type_value = "application/json";
	hbc->http_req.payload = hbc->status_buffer;
	hbc->http_req.payload_size = strlen(hbc->status_buffer);

	ret = hawkbit_query(hbc, NULL);
	if (ret) {
		SYS_LOG_ERR("Failed to report deployment feedback");
	}

	return ret;
}

static int hawkbit_ddi_poll(struct hawkbit_context *hbc)
{
	/*
	 * "Raw" decoded JSON objects.
	 */
	union {
		struct hawkbit_ctl_res base; /* Base resource. */
		struct hawkbit_dep_res dep;  /* Deployment operations. */
	} hawkbit_results;
	/*
	 * Cached hawkBit base resource results.
	 */
	char deployment_base[40];	/* TODO: Find a better value */
	char download_http[200];	/* TODO: Find a better value */
	static s32_t json_acid;
	s32_t file_size = 0;
	/*
	 * Etc.
	 */
	struct hawkbit_device_acid device_acid;
	struct json_data_t json = { NULL, 0 };
	const struct product_id_t *product_id = product_id_get();
	int ret;

	/*
	 * Query the hawkBit base polling resource.
	 */
	SYS_LOG_DBG("Polling target data from Hawkbit");

	/* Build URL */
	snprintk(hbc->url_buffer, hbc->url_buffer_size, "%s/%s-%x",
		 HAWKBIT_JSON_URL, product_id->name, product_id->number);

	memset(&hbc->http_req, 0, sizeof(hbc->http_req));
	hbc->http_req.method = HTTP_GET;
	hbc->http_req.url = hbc->url_buffer;
	hbc->http_req.host = HAWKBIT_HOST;
	hbc->http_req.protocol = " " HTTP_PROTOCOL;
	hbc->http_req.header_fields = HTTP_HEADER_CONNECTION_CLOSE_CRLF;

	ret = hawkbit_query(hbc, &json);
	if (ret < 0) {
		SYS_LOG_ERR("Error when polling from Hawkbit");
		return ret;
	}

	/*
	 * Decode the results from the base polling resource, finding
	 * the hawkBit DDI v1 deployment base in the returned result.
	 */
	memset(&hawkbit_results.base, 0, sizeof(hawkbit_results.base));
	ret = json_obj_parse(json.data, json.len, json_ctl_res_descr,
			     ARRAY_SIZE(json_ctl_res_descr),
			     &hawkbit_results.base);
	if (ret < 0) {
		SYS_LOG_ERR("JSON parse error %d polling base resource", ret);
		return ret;
	}

#ifdef HAWKBIT_EXTRA_DEBUG
	hawkbit_dump_base(&hawkbit_results.base, "");
#endif

	if (hawkbit_results.base.config.polling.sleep) {
		/* Update the sleep time. */
		hawkbit_update_sleep(&hawkbit_results.base);
	}
	if (hawkbit_results.base._links.cancelAction.href) {
		/* TODO: implement cancelAction logic. */
		SYS_LOG_WRN("Ignoring cancelAction (href %s)",
			    hawkbit_results.base._links.cancelAction.href);
	}
	ret = hawkbit_find_deployment_base(&hawkbit_results.base,
					   deployment_base,
					   sizeof(deployment_base));
	if (ret < 0) {
		return ret;
	}

	/* Provide this device's config data if the server asked for it. */
	if (hawkbit_results.base._links.configData.href) {
		hawkbit_report_config_data(hbc);
	}

	/*
	 * If one was found, poll the deployment base discovered
	 * earlier. If there was no deployment base, there is nothing
	 * else to do.
	 */
	if (strlen(deployment_base) == 0) {
		SYS_LOG_DBG("No deployment base found, no actions to take");
		return 0;
	}

	memset(&json, 0, sizeof(struct json_data_t));

	/* Build URL: Hawkbit DDI v1 deploymentBase */
	snprintk(hbc->url_buffer, hbc->url_buffer_size, "%s/%s-%x/%s",
		 HAWKBIT_JSON_URL, product_id->name, product_id->number,
		 deployment_base);

	memset(&hbc->http_req, 0, sizeof(hbc->http_req));
	hbc->http_req.method = HTTP_GET;
	hbc->http_req.url = hbc->url_buffer;
	hbc->http_req.host = HAWKBIT_HOST;
	hbc->http_req.protocol = " " HTTP_PROTOCOL;
	hbc->http_req.header_fields = HTTP_HEADER_CONNECTION_CLOSE_CRLF;

	if (hawkbit_query(hbc, &json) < 0) {
		SYS_LOG_ERR("Error when querying from Hawkbit");
		return -1;
	}

	/*
	 * Decode results from the deployment operations resource.
	 */

	memset(&hawkbit_results.dep, 0, sizeof(hawkbit_results.dep));
	ret = json_obj_parse(json.data, json.len, json_dep_res_descr,
			     ARRAY_SIZE(json_dep_res_descr),
			     &hawkbit_results.dep);
	if (ret < 0) {
		SYS_LOG_ERR("deploymentBase JSON parse error %d", ret);
		goto report_error;
	} else if (ret != (1 << ARRAY_SIZE(json_dep_res_descr)) - 1) {
		SYS_LOG_ERR("deploymentBase JSON mismatch"
			    " (expected 0x%x, got 0x%x)",
			    (1 << ARRAY_SIZE(json_dep_res_descr)) - 1, ret);
		ret = -EINVAL;
		goto report_error;
	}

#ifdef HAWKBIT_EXTRA_DEBUG
	hawkbit_dump_deployment(&hawkbit_results.dep, "");
#endif

	ret = hawkbit_parse_deployment(&hawkbit_results.dep, &json_acid,
				       download_http, sizeof(download_http),
				       &file_size);
	if (ret) {
		goto report_error;
	}

	/* TODO: handle download/update. */
	SYS_LOG_DBG("action ID: %d", json_acid);
	SYS_LOG_DBG("deployment: download %s, update %s (ignored)",
		    hawkbit_results.dep.deployment.download,
		    hawkbit_results.dep.deployment.update);
	SYS_LOG_DBG("artifact address: %s", download_http);
	SYS_LOG_DBG("artifact file size: %d", file_size);

	hawkbit_device_acid_read(&device_acid);
	if (device_acid.current == json_acid) {
		/* We are coming from a successful flash, update the server */
		ret = hawkbit_report_dep_fbk(hbc, json_acid,
					     HAWKBIT_STATUS_FINISHED_SUCCESS,
					     HAWKBIT_STATUS_EXEC_CLOSED);
		return ret;
	}

	/*
	 * Check for errors.
	 */
	if (device_acid.update == (u32_t)json_acid) {
		SYS_LOG_ERR("Preventing repeated attempt to install %d",
			    json_acid);
		ret = -EALREADY;
		goto report_error;
	}

	/* Here we should have everything we need to apply the action */
	SYS_LOG_INF("Valid action ID %d found, proceeding with the update",
					json_acid);
	ret = hawkbit_report_dep_fbk(hbc, json_acid,
				     HAWKBIT_STATUS_FINISHED_SUCCESS,
				     HAWKBIT_STATUS_EXEC_PROCEEDING);
	if (ret) {
		return ret;
	}
	ret = hawkbit_install_update(hbc, download_http, file_size);
	if (ret != 0) {
		SYS_LOG_ERR("Failed to install the update for action ID %d",
			    json_acid);
		goto report_error;
	}

	SYS_LOG_INF("Triggering OTA update.");
	boot_request_upgrade(false);
	ret = hawkbit_device_acid_update(HAWKBIT_ACID_UPDATE, json_acid);
	if (ret != 0) {
		SYS_LOG_ERR("Failed to update ACID: %d", ret);
		goto report_error;
	}
	SYS_LOG_INF("Image id %d flashed successfuly, rebooting now",
					json_acid);

	/* Reboot and let the bootloader take care of the swap process */
	sys_reboot(0);

	return 0;

 report_error:
	hawkbit_report_dep_fbk(hbc, json_acid,
			       HAWKBIT_STATUS_FINISHED_FAILURE,
			       HAWKBIT_STATUS_EXEC_CLOSED);
	return ret;
}

/* OTA worker */
static void hawkbit_work_fn(struct k_work *work)
{
	struct hawkbit_context *hbc = CONTAINER_OF(work, struct hawkbit_context,
						   work);
	int ret;

	ret = hawkbit_ddi_poll(hbc);
	if (ret < 0) {
		hbc->failures++;
	} else {
		/* restart the failed attempt counter */
		hbc->failures = 0;
	}
	if (hbc->failures == HAWKBIT_MAX_SERVER_FAIL) {
		SYS_LOG_ERR("Too many unsuccessful poll attempts, rebooting!");
		sys_reboot(0);
	}

	k_delayed_work_submit_to_queue(hbc->work_q, &hbc->work, poll_sleep);
}

static void event_iface_up(struct net_mgmt_event_callback *cb,
			   u32_t mgmt_event, struct net_if *iface)
{
	SYS_LOG_INF("Submitting FOTA Service work");
	k_delayed_work_submit_to_queue(hb_context.work_q, &hb_context.work,
				       poll_sleep);
}

int hawkbit_start(struct k_work_q *work_q)
{
	/* TODO: default interface may not always be the one we want */
	struct net_if *iface = net_if_get_default();
	int ret;

	ret = hawkbit_init_flash();
	if (ret) {
		SYS_LOG_ERR("Hawkbit Client initialization generated "
			    "an error: %d", ret);
		return ret;
	}

	k_sem_init(&hb_sem, 0, 1);
	memset(&hb_context, 0, sizeof(hb_context));
	hb_context.tcp_buffer_size = TCP_RECV_BUFFER_SIZE;
	hb_context.url_buffer_size = URL_BUFFER_SIZE;
	hb_context.status_buffer_size = STATUS_BUFFER_SIZE;
	hb_context.work_q = work_q;
	k_delayed_work_init(&hb_context.work, hawkbit_work_fn);
	hb_context.sem = &hb_sem;

#if defined(CONFIG_NET_MGMT_EVENT)
	/* Subscribe to NET_EVENT_IF_UP if interface is not ready */
	if (!net_if_is_up(iface)) {
		net_mgmt_init_event_callback(&cb, event_iface_up,
					     NET_EVENT_IF_UP);
		net_mgmt_add_event_callback(&cb);
		return 0;
	}
#endif

	event_iface_up(NULL, NET_EVENT_IF_UP, iface);

	return ret;
}
