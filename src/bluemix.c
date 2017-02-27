/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Much of the initial code here was pulled from
 * samples/net/mqtt_publisher
 */

#include "config.h"

#if (CONFIG_DM_BACKEND == BACKEND_BLUEMIX)

#include <zephyr.h>

#include <net/net_context.h>
#include <net/nbuf.h>

#include <string.h>
#include <errno.h>
#include <stdio.h>

#include "ota_debug.h"
#include "device.h"
#include "tcp.h"
#include "bluemix.h"

#define BLUEMIX_USERNAME 	"use-token-auth"
#define MQTT_SUBSCRIBE_WAIT 	K_MSEC(1000)
#define BLUEMIX_MGMT_WAIT	K_MSEC(1200) /* 400 msec has been observed */

/*
 * Various topics are of the form:
 * "foo/type/<device_type>/id/<device_id>/bar".
 * This is a convenience macro for those cases.
 */
#define INIT_DEVICE_TOPIC(ctx, fmt)					\
	snprintf(ctx->bm_topic, sizeof(ctx->bm_topic), fmt,		\
		 CONFIG_BLUEMIX_DEVICE_TYPE, ctx->bm_id)

static inline struct bluemix_ctx* mqtt_to_bluemix(struct mqtt_ctx *mqtt)
{
	return CONTAINER_OF(mqtt, struct bluemix_ctx, mqtt_ctx);
}

static inline int wait_for_mqtt(struct bluemix_ctx *ctx, int32_t timeout)
{
	return k_sem_take(&ctx->reply_sem, timeout);
}

/*
 * MQTT helpers
 */

static void connect_cb(struct mqtt_ctx *ctx)
{
	OTA_DBG("MQTT connect CB\n");
}

static void disconnect_cb(struct mqtt_ctx *ctx)
{
	OTA_DBG("MQTT disconnect CB\n");
}

static int publish_tx_cb(struct mqtt_ctx *ctx, uint16_t pkt_id,
			 enum mqtt_packet type)
{
	OTA_DBG("MQTT publish TX CB\n");
	return 0;
}

static int publish_rx_cb(struct mqtt_ctx *ctx, struct mqtt_publish_msg *msg,
			 uint16_t pkt_id, enum mqtt_packet type)
{
	struct bluemix_ctx *bm_ctx;

	OTA_DBG("MQTT publish RX CB\n");
	bm_ctx = mqtt_to_bluemix(ctx);

	if (msg->topic_len + 1 > sizeof(bm_ctx->bm_topic)) {
		bm_ctx->bm_fatal_err = -ENOMEM;
		OTA_ERR("Bluemix topic buffer size %u overflowed by %u B\n",
			sizeof(bm_ctx->bm_topic),
			msg->topic_len + 1 - sizeof(bm_ctx->bm_topic));

	} else if (msg->msg_len + 1 > sizeof(bm_ctx->bm_message)) {
		bm_ctx->bm_fatal_err = -ENOMEM;
		OTA_ERR("Bluemix message buffer size %u overflowed by %u B\n",
			sizeof(bm_ctx->bm_message),
			msg->msg_len + 1 - sizeof(bm_ctx->bm_message));
	}
	if (bm_ctx->bm_fatal_err) {
		/* Propagate fatal error to waiter. */
		k_sem_give(&bm_ctx->reply_sem);
		return 0;
	}

	memcpy(bm_ctx->bm_topic, msg->topic, msg->topic_len);
	bm_ctx->bm_topic[msg->topic_len] = '\0';
	memcpy(bm_ctx->bm_message, msg->msg, msg->msg_len);
	bm_ctx->bm_message[msg->msg_len] = '\0';
	k_sem_give(&bm_ctx->reply_sem);

	/* FIXME: parse JSON and validate any pending reqId. */
	OTA_DBG("topic: %s\n", bm_ctx->bm_topic);
	OTA_DBG("msg: %s\n", bm_ctx->bm_message);
	return 0;
}

static int subscribe_cb(struct mqtt_ctx *ctx, uint16_t pkt_id,
			uint8_t items, enum mqtt_qos qos[])
{
	/* FIXME: validate this is the suback we were waiting for. */
	OTA_DBG("MQTT subscribe CB\n");

	k_sem_give(&mqtt_to_bluemix(ctx)->reply_sem);
	return 0;
}

static int unsubscribe_cb(struct mqtt_ctx *ctx, uint16_t pkt_id)
{
	OTA_DBG("MQTT unsubscribe CB\n");
	return 0;
}

static void malformed_cb(struct mqtt_ctx *ctx, uint16_t pkt_type)
{
	OTA_DBG("MQTT malformed CB\n");
}

/* In this routine we block until the connected variable is 1 */
static int try_to_connect(struct mqtt_ctx *ctx, struct mqtt_connect_msg *msg)
{
	int i = 0;
	int ret;

	while (i++ < APP_CONNECT_TRIES && !ctx->connected) {
		OTA_DBG(">>> connect: client_id=[%s/%d] user_name=[%s/%d]\n",
			msg->client_id, msg->client_id_len,
			msg->user_name, msg->user_name_len);
		ret = mqtt_tx_connect(ctx, msg);
		k_sleep(APP_SLEEP_MSECS);
		OTA_DBG("mqtt_tx_connect %d\n", ret);
		if (ret) {
			continue;
		}
	}

	if (ctx->connected) {
		return 0;
	}

	return -EINVAL;
}

/*
 * Bluemix
 */

static void build_req_uuid(struct bluemix_ctx *ctx) /* TODO: improve this. */
{
	snprintf(ctx->bm_req_id, sizeof(ctx->bm_req_id),
		 "%08x-401c-453c-b8f5-%012x",
		 product_id.number, ctx->bm_next_req_id++);
}

static int subscribe_to_topic(struct bluemix_ctx *ctx)
{
	const char* topics[] = { ctx->bm_topic };
	const enum mqtt_qos qos0[] = { MQTT_QoS0 };
	int ret;

	ret = mqtt_tx_subscribe(&ctx->mqtt_ctx, sys_rand32_get() & 0xffff,
				1, topics, qos0);
	if (ret) {
		OTA_ERR("mqtt_tx_subscribe: %d\n", ret);
		return ret;
	}
	ret = wait_for_mqtt(ctx, MQTT_SUBSCRIBE_WAIT);
	OTA_DBG("wait_for_mqtt: %d\n", ret);
	return ret;
}

static int publish_message(struct bluemix_ctx *ctx)
{
	OTA_DBG("topic:%s\n", ctx->pub_msg.topic);
	OTA_DBG("message:%s\n", ctx->pub_msg.msg);
	return mqtt_tx_publish(&ctx->mqtt_ctx, &ctx->pub_msg);
}

static void build_manage_request(struct bluemix_ctx *ctx)
{
	struct mqtt_publish_msg *pub_msg = &ctx->pub_msg;
	uint8_t *buffer = ctx->bm_message;
	size_t size = sizeof(ctx->bm_message);
	char *helper;

	memset(buffer, 0, size);
	helper = buffer;

	INIT_DEVICE_TOPIC(ctx, "iotdevice-1/type/%s/id/%s/mgmt/manage");
	build_req_uuid(ctx);
	snprintf(helper, size,
	"{"
		"\"d\":{"
			"\"supports\":{"
			    "\"deviceActions\":true,"
			    "\"firmwareActions\":true"
			"},"
			"\"deviceInfo\":{"
			    "\"serialNumber\":\"%08x\","
			    "\"fwVersion\":\"1.0\""
			"}"
		"},"
		"\"reqId\":\"%s\""
		 "}", product_id.number, ctx->bm_req_id);

	pub_msg->msg = buffer;
	pub_msg->msg_len = strlen(pub_msg->msg);
	pub_msg->qos = MQTT_QoS0;
	pub_msg->topic = ctx->bm_topic;
	pub_msg->topic_len = strlen(pub_msg->topic);
}

static int become_managed_device(struct bluemix_ctx *ctx)
{
	int ret;
	OTA_DBG("becoming a managed device\n");
	build_manage_request(ctx);
	ret = publish_message(ctx);
	if (ret) {
		return ret;
	}
	ret = wait_for_mqtt(ctx, BLUEMIX_MGMT_WAIT);
	if (ret == -EAGAIN) {
		OTA_ERR("timed out\n");
		return ret;
	} else if (ctx->bm_fatal_err) {
		OTA_DBG("fatal error %d", ctx->bm_fatal_err);
		return ctx->bm_fatal_err;
	}
	OTA_DBG("wait_for_mqtt: %d\n", ret);
	return ret;
}

int bluemix_init(struct bluemix_ctx *ctx)
{
	int ret = 0;

	/* Set everything to 0 before assigning the required fields. */
	memset(ctx, 0x00, sizeof(*ctx));

	/*
	 * Initialize the IDs etc. before doing anything else.
	 */
	snprintf(ctx->bm_id, sizeof(ctx->bm_id), "%s-%08x",
		 CONFIG_BLUEMIX_DEVICE_TYPE, product_id.number);
	snprintf(ctx->client_id, sizeof(ctx->client_id),
		"d:%s:%s:%s", CONFIG_BLUEMIX_ORG, CONFIG_BLUEMIX_DEVICE_TYPE,
		ctx->bm_id);
	snprintf(ctx->bm_auth_token, sizeof(ctx->bm_auth_token),
		 "%08x", product_id.number);

	k_sem_init(&ctx->reply_sem, 0, 1);

	/*
	 * try connecting here so that tcp_get_context()
	 * will return a valid net_ctx later
	 */
	ret = tcp_connect();
	if (ret < 0) {
		return ret;
	}

	ctx->mqtt_ctx.connect = connect_cb;
	ctx->mqtt_ctx.disconnect = disconnect_cb;
	ctx->mqtt_ctx.malformed = malformed_cb;
	ctx->mqtt_ctx.publish_tx = publish_tx_cb;
	ctx->mqtt_ctx.publish_rx = publish_rx_cb;
	ctx->mqtt_ctx.subscribe = subscribe_cb;
	ctx->mqtt_ctx.unsubscribe = unsubscribe_cb;
	ctx->mqtt_ctx.net_timeout = APP_TX_RX_TIMEOUT;
	ctx->mqtt_ctx.net_ctx = tcp_get_context();

	ret = mqtt_init(&ctx->mqtt_ctx, MQTT_APP_PUBLISHER_SUBSCRIBER);
	OTA_DBG("mqtt_init %d\n", ret);
	if (ret) {
		goto out;
	}

	/* The connect message will be sent to the MQTT server (broker).
	 * If clean_session here is 0, the mqtt_ctx clean_session variable
	 * will be set to 0 also. Please don't do that, set always to 1.
	 * Clean session = 0 is not yet supported.
	 */
	ctx->connect_msg.client_id = ctx->client_id;
	ctx->connect_msg.client_id_len = strlen(ctx->connect_msg.client_id);
	/*
	 * FIXME: HACK
	 *
	 * This is part of a work-around for problems with the Zephyr
	 * MQTT stack. That stack doesn't correctly parse multiple
	 * MQTT packets within a single struct net_buf. Working around
	 * that implies trying to never receive multiple MQTT packets
	 * in the same net_buf.
	 *
	 * In order to avoid having to worry about scheduling PINGREQ
	 * packets (whose PINGRESP packets might come at an
	 * inconvenient time relative to other MQTT traffic), let's
	 * just disable the keep alive feature entirely for now. We
	 * should turn it on later.
	 */
	ctx->connect_msg.keep_alive = 0;
	ctx->connect_msg.user_name = BLUEMIX_USERNAME;
	ctx->connect_msg.user_name_len = strlen(ctx->connect_msg.user_name);
	ctx->connect_msg.password = ctx->bm_auth_token;
	ctx->connect_msg.password_len = strlen(ctx->connect_msg.password);
	ctx->connect_msg.clean_session = 1;

	ctx->connect_data = "CONNECTED";
	ctx->disconnect_data = "DISCONNECTED";
	ctx->publish_data = "PUBLISH";

	ret = try_to_connect(&ctx->mqtt_ctx, &ctx->connect_msg);
	OTA_DBG("try_to_connect %d\n", ret);
	if (ret) {
		goto out;
	}

	OTA_DBG("subscribing to command and DM topics\n");
	INIT_DEVICE_TOPIC(ctx, "iot-2/type/%s/id/%s/cmd/+/fmt/+");
	ret = subscribe_to_topic(ctx);
	if (ret) {
		OTA_ERR("can't subscribe to command topics: %d\n", ret);
		goto out;
	}
	INIT_DEVICE_TOPIC(ctx, "iotdm-1/type/%s/id/%s/#");
	ret = subscribe_to_topic(ctx);
	if (ret) {
		OTA_ERR("can't subscribe to device management topics: %d\n",
			ret);
		goto out;
	}

	ret = become_managed_device(ctx);
	if (ret) {
		goto out;
	}

	return 0;
 out:
	tcp_cleanup(true);
	return ret;
}

int bluemix_pub_temp_c(struct bluemix_ctx *ctx, int temperature)
{
	struct mqtt_publish_msg *pub_msg = &ctx->pub_msg;
	INIT_DEVICE_TOPIC(ctx, "iot-2/type/%s/id/%s/evt/status/fmt/json");
	snprintf(ctx->bm_message, sizeof(ctx->bm_message),
		"{"
			"\"d\":{"
				"\"temperature\":%d"
			"}"
		"}",
		temperature);
	pub_msg->msg = ctx->bm_message;
	pub_msg->msg_len = strlen(pub_msg->msg);
	pub_msg->qos = MQTT_QoS0;
	pub_msg->topic = ctx->bm_topic;
	pub_msg->topic_len = strlen(pub_msg->topic);
	return publish_message(ctx);
}

#endif /* (CONFIG_DM_BACKEND == BACKEND_BLUEMIX) */
