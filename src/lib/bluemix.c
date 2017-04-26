/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Much of the initial code here was pulled from
 * samples/net/mqtt_publisher
 */

#define SYS_LOG_DOMAIN "fota/bluemix"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_FOTA_LEVEL
#include <logging/sys_log.h>

#include <zephyr.h>

#include <net/net_context.h>
#include <net/nbuf.h>

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>

#include "product_id.h"
#include "tcp.h"
#include "bluemix.h"

#define BLUEMIX_USERNAME	"use-token-auth"
#define APP_CONNECT_TRIES	10
#define APP_SLEEP_MSECS		K_MSEC(500)
#define APP_TX_RX_TIMEOUT	K_MSEC(300)
#define MQTT_SUBSCRIBE_WAIT	K_MSEC(1000)
#define MQTT_DISCONNECT_WAIT	K_MSEC(1000)
#define BLUEMIX_MGMT_WAIT	K_MSEC(1200) /* 400 msec has been observed */

/*
 * Various topics are of the form:
 * "foo/type/<device_type>/id/<device_id>/bar".
 * This is a convenience macro for those cases.
 */
#define INIT_DEVICE_TOPIC(ctx, fmt)					\
	snprintf(ctx->bm_topic, sizeof(ctx->bm_topic), fmt,		\
		 CONFIG_FOTA_BLUEMIX_DEVICE_TYPE, ctx->bm_id)

static inline struct bluemix_ctx* mqtt_to_bluemix(struct mqtt_ctx *mqtt)
{
	return CONTAINER_OF(mqtt, struct bluemix_ctx, mqtt_ctx);
}

static inline int wait_for_mqtt(struct bluemix_ctx *ctx, s32_t timeout)
{
	return k_sem_take(&ctx->reply_sem, timeout);
}

/*
 * MQTT helpers
 */

static void connect_cb(struct mqtt_ctx *ctx)
{
}

static void disconnect_cb(struct mqtt_ctx *ctx)
{
	k_sem_give(&mqtt_to_bluemix(ctx)->reply_sem);
}

static int publish_tx_cb(struct mqtt_ctx *ctx, u16_t pkt_id,
			 enum mqtt_packet type)
{
	return 0;
}

static int publish_rx_cb(struct mqtt_ctx *ctx, struct mqtt_publish_msg *msg,
			 u16_t pkt_id, enum mqtt_packet type)
{
	struct bluemix_ctx *bm_ctx;

	bm_ctx = mqtt_to_bluemix(ctx);

	if (msg->topic_len + 1 > sizeof(bm_ctx->bm_topic)) {
		bm_ctx->bm_fatal_err = -ENOMEM;
		SYS_LOG_ERR("Bluemix topic buffer size %u overflowed by %u B",
			    sizeof(bm_ctx->bm_topic),
			    msg->topic_len + 1 - sizeof(bm_ctx->bm_topic));

	} else if (msg->msg_len + 1 > sizeof(bm_ctx->bm_message)) {
		bm_ctx->bm_fatal_err = -ENOMEM;
		SYS_LOG_ERR("Bluemix message buffer size %u overflowed by %u B",
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
	SYS_LOG_DBG("topic: %s", bm_ctx->bm_topic);
	SYS_LOG_DBG("msg: %s", bm_ctx->bm_message);
	return 0;
}

static int subscribe_cb(struct mqtt_ctx *ctx, u16_t pkt_id,
			u8_t items, enum mqtt_qos qos[])
{
	/* FIXME: validate this is the suback we were waiting for. */
	k_sem_give(&mqtt_to_bluemix(ctx)->reply_sem);
	return 0;
}

static int unsubscribe_cb(struct mqtt_ctx *ctx, u16_t pkt_id)
{
	SYS_LOG_DBG("MQTT unsubscribe CB");
	return 0;
}

static void malformed_cb(struct mqtt_ctx *ctx, u16_t pkt_type)
{
	SYS_LOG_DBG("MQTT malformed CB");
}

/* In this routine we block until the connected variable is 1 */
static int try_to_connect(struct mqtt_ctx *ctx, struct mqtt_connect_msg *msg)
{
	int i = 0;
	int ret;

	while (i++ < APP_CONNECT_TRIES && !ctx->connected) {
		ret = mqtt_tx_connect(ctx, msg);
		k_sleep(APP_SLEEP_MSECS);
		if (ret) {
			SYS_LOG_ERR("mqtt_tx_connect: %d", ret);
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

static int subscribe_to_topic(struct bluemix_ctx *ctx)
{
	const char* topics[] = { ctx->bm_topic };
	const enum mqtt_qos qos0[] = { MQTT_QoS0 };
	int ret;

	ret = mqtt_tx_subscribe(&ctx->mqtt_ctx, sys_rand32_get() & 0xffff,
				1, topics, qos0);
	if (ret) {
		SYS_LOG_ERR("mqtt_tx_subscribe: %d", ret);
		return ret;
	}
	ret = wait_for_mqtt(ctx, MQTT_SUBSCRIBE_WAIT);
	return ret;
}

static int publish_message(struct bluemix_ctx *ctx)
{
	SYS_LOG_DBG("topic:%s", ctx->pub_msg.topic);
	SYS_LOG_DBG("message:%s", ctx->pub_msg.msg);
	return mqtt_tx_publish(&ctx->mqtt_ctx, &ctx->pub_msg);
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
		 CONFIG_FOTA_BLUEMIX_DEVICE_TYPE, product_id_get()->number);
	snprintf(ctx->client_id, sizeof(ctx->client_id),
		"d:%s:%s:%s", CONFIG_FOTA_BLUEMIX_ORG,
		CONFIG_FOTA_BLUEMIX_DEVICE_TYPE,
		ctx->bm_id);
	snprintf(ctx->bm_auth_token, sizeof(ctx->bm_auth_token),
		 "%08x", product_id_get()->number);

	k_sem_init(&ctx->reply_sem, 0, 1);

	/*
	 * try connecting here so that tcp_get_net_context()
	 * will return a valid net_ctx later
	 */
	ret = tcp_connect(TCP_CTX_BLUEMIX);
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
	ctx->mqtt_ctx.net_ctx = tcp_get_net_context(TCP_CTX_BLUEMIX);

	ret = mqtt_init(&ctx->mqtt_ctx, MQTT_APP_PUBLISHER_SUBSCRIBER);
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
	if (ret) {
		goto out;
	}

	INIT_DEVICE_TOPIC(ctx, "iot-2/type/%s/id/%s/cmd/+/fmt/+");
	ret = subscribe_to_topic(ctx);
	if (ret) {
		SYS_LOG_ERR("can't subscribe to command topics: %d", ret);
		goto out;
	}

	return 0;
 out:
	tcp_cleanup(TCP_CTX_BLUEMIX, true);
	return ret;
}

int bluemix_fini(struct bluemix_ctx *ctx)
{
	int ret;

	ret = mqtt_tx_disconnect(&ctx->mqtt_ctx);
	if (ret) {
		SYS_LOG_ERR("%s: mqtt_tx_disconnect: %d", __func__, ret);
		goto cleanup;
	}
	ret = wait_for_mqtt(ctx, MQTT_DISCONNECT_WAIT);
 cleanup:
	tcp_cleanup(TCP_CTX_BLUEMIX, true);
	return ret;
}

int bluemix_pub_status_json(struct bluemix_ctx *ctx,
			    const char *fmt, ...)
{
	struct mqtt_publish_msg *pub_msg = &ctx->pub_msg;
	va_list vargs;
	int ret;

	INIT_DEVICE_TOPIC(ctx, "iot-2/type/%s/id/%s/evt/status/fmt/json");

	/* Fill in the initial '{"d":'. */
	ret = snprintf(ctx->bm_message, sizeof(ctx->bm_message), "{\"d\":");
	if (ret == sizeof(ctx->bm_message) - 1) {
		return -ENOMEM;
	}
	/* Add the user data. */
	va_start(vargs, fmt);
	ret += vsnprintf(ctx->bm_message + ret, sizeof(ctx->bm_message) - ret,
			 fmt, vargs);
	va_end(vargs);
	if (ret > sizeof(ctx->bm_message) - 2) {
		/* Overflow check: 2 = (1 for '\0') + (1 for "}") */
		return -ENOMEM;
	}
	/* Append the closing brace. */
	snprintf(ctx->bm_message + ret, sizeof(ctx->bm_message) - ret, "}");

	/* Fill out the MQTT publication, and ship it. */
	pub_msg->msg = ctx->bm_message;
	pub_msg->msg_len = strlen(pub_msg->msg);
	pub_msg->qos = MQTT_QoS0;
	pub_msg->topic = ctx->bm_topic;
	pub_msg->topic_len = strlen(pub_msg->topic);
	return publish_message(ctx);
}
