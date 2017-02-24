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
#include <net/mqtt.h>

#include <net/net_context.h>
#include <net/nbuf.h>

#include <string.h>
#include <errno.h>
#include <stdio.h>

#include "ota_debug.h"
#include "device.h"
#include "tcp.h"
#include "bluemix.h"

#define BLUEMIX_USERNAME "use-token-auth"

static uint8_t my_bluemix_id[30];	/* Set in bluemix_init(). */
static uint8_t json_buf[1024];
static uint8_t topic[255];

/*
 * Various topics are of the form:
 * "foo/type/<device_type>/id/<device_id>/bar".
 * This is a convenience macro for those cases.
 */
#define INIT_DEVICE_TOPIC(fmt) \
	snprintf(topic, sizeof(topic), fmt, CONFIG_BLUEMIX_DEVICE_TYPE, my_bluemix_id)

/**
 * @brief mqtt_client_ctx	Container of some structures used by the
 *				publisher app.
 */
struct mqtt_client_ctx {
	struct mqtt_connect_msg connect_msg;
	struct mqtt_publish_msg pub_msg;

	struct mqtt_ctx mqtt_ctx;

	/**
	 * This variable will be passed to the connect callback, declared inside
	 * the mqtt context struct. If not used, it could be set to NULL.
	 */
	void *connect_data;

	/**
	 * This variable will be passed to the disconnect callback, declared
	 * inside the mqtt context struct. If not used, it could be set to NULL.
	 */
	void *disconnect_data;

	/**
	 * This variable will be passed to the publish_tx callback, declared
	 * inside the mqtt context struct. If not used, it could be set to NULL.
	 */
	void *publish_data;
};

/* The mqtt client struct */
static struct mqtt_client_ctx client_ctx;

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
	OTA_DBG("MQTT publish RX CB\n");
	return 0;
}

static int subscribe_cb(struct mqtt_ctx *ctx, uint16_t pkt_id,
			uint8_t items, enum mqtt_qos qos[])
{
	OTA_DBG("MQTT subscribe CB\n");
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

static char *build_clientid(void)
{
	static char clientid[50];

	snprintf(clientid, sizeof(clientid),
		"d:%s:%s:%s", CONFIG_BLUEMIX_ORG, CONFIG_BLUEMIX_DEVICE_TYPE,
		my_bluemix_id);

	return clientid;
}


static char *build_auth_token(void)
{
	static char auth_token[20];

	snprintf(auth_token, sizeof(auth_token), "%08x", product_id.number);

	return auth_token;
}

static int subscribe_to_topic(struct mqtt_ctx *ctx, const char *topic)
{
	const char* topics[] = { topic };
	const enum mqtt_qos qos0[] = { MQTT_QoS0 };
	return mqtt_tx_subscribe(ctx, sys_rand32_get() & 0xffff, 1, topics, qos0);
}

static void build_manage_request(struct mqtt_publish_msg *pub_msg,
				 uint8_t *buffer, size_t size)
{
	char *helper;

	memset(buffer, 0, size);
	helper = buffer;

	INIT_DEVICE_TOPIC("iotdevice-1/type/%s/id/%s/mgmt/manage");
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
		"\"reqId\":\"b53eb43e-401c-453c-b8f5-94b73290c056\""
	"}", product_id.number);

	pub_msg->msg = buffer;
	pub_msg->msg_len = strlen(pub_msg->msg);
	pub_msg->qos = MQTT_QoS0;
	pub_msg->topic = topic;
	pub_msg->topic_len = strlen(pub_msg->topic);
	pub_msg->pkt_id = sys_rand32_get();
}

int bluemix_init(void)
{
	int ret = 0;

	/*
	 * Initialize our device ID before doing anything else.
	 */
	snprintf(my_bluemix_id, sizeof(my_bluemix_id), "%s-%08x",
		 CONFIG_BLUEMIX_DEVICE_TYPE, product_id.number);

	/*
	 * try connecting here so that tcp_get_context()
	 * will return a valid net_ctx later
	 */
	ret = tcp_connect();
	if (ret < 0) {
		return ret;
	}

	/* Set everything to 0 and later just assign the required fields. */
	memset(&client_ctx, 0x00, sizeof(client_ctx));

	client_ctx.mqtt_ctx.connect = connect_cb;
	client_ctx.mqtt_ctx.disconnect = disconnect_cb;
	client_ctx.mqtt_ctx.malformed = malformed_cb;
	client_ctx.mqtt_ctx.publish_tx = publish_tx_cb;
	client_ctx.mqtt_ctx.publish_rx = publish_rx_cb;
	client_ctx.mqtt_ctx.subscribe = subscribe_cb;
	client_ctx.mqtt_ctx.unsubscribe = unsubscribe_cb;
	client_ctx.mqtt_ctx.net_timeout = APP_TX_RX_TIMEOUT;
	client_ctx.mqtt_ctx.net_ctx = tcp_get_context();

	ret = mqtt_init(&client_ctx.mqtt_ctx, MQTT_APP_PUBLISHER_SUBSCRIBER);
	OTA_DBG("mqtt_init %d\n", ret);
	if (ret) {
		goto out;
	}

	/* The connect message will be sent to the MQTT server (broker).
	 * If clean_session here is 0, the mqtt_ctx clean_session variable
	 * will be set to 0 also. Please don't do that, set always to 1.
	 * Clean session = 0 is not yet supported.
	 */
	client_ctx.connect_msg.client_id = build_clientid();
	client_ctx.connect_msg.client_id_len = strlen(client_ctx.connect_msg.client_id);
	client_ctx.connect_msg.user_name = BLUEMIX_USERNAME;
	client_ctx.connect_msg.user_name_len = strlen(client_ctx.connect_msg.user_name);
	client_ctx.connect_msg.password = build_auth_token();
	client_ctx.connect_msg.password_len = strlen(client_ctx.connect_msg.password);
	client_ctx.connect_msg.clean_session = 1;

	client_ctx.connect_data = "CONNECTED";
	client_ctx.disconnect_data = "DISCONNECTED";
	client_ctx.publish_data = "PUBLISH";

	ret = try_to_connect(&client_ctx.mqtt_ctx,
			     &client_ctx.connect_msg);
	OTA_DBG("try_to_connect %d\n", ret);
	if (ret) {
		goto out;
	}

	/*
	 * TODO: Now that we are connected:
	 * device registration
	 * push POWER event
	 * management subscription
	 */

	OTA_DBG("subscribing to command and DM topics\n");
	INIT_DEVICE_TOPIC("iot-2/type/%s/id/%s/cmd/+/fmt/+");
	ret = subscribe_to_topic(&client_ctx.mqtt_ctx, topic);
	if (ret) {
		OTA_ERR("can't subscribe to command topics: %d\n", ret);
		goto out;
	}
	INIT_DEVICE_TOPIC("iotdm-1/type/%s/id/%s/#");
	ret = subscribe_to_topic(&client_ctx.mqtt_ctx, topic);
	if (ret) {
		OTA_ERR("can't subscribe to device management topics: %d\n",
			ret);
		goto out;
	}

	OTA_DBG("becoming a managed device\n");
	build_manage_request(&client_ctx.pub_msg, json_buf, sizeof(json_buf));
	ret = mqtt_tx_publish(&client_ctx.mqtt_ctx, &client_ctx.pub_msg);
	if (ret) {
		OTA_ERR("failed becoming a managed device: %d\n", ret);
		goto out;
	}

	/* PING */
	OTA_DBG("Sending first ping\n");
	ret = mqtt_tx_pingreq(&client_ctx.mqtt_ctx);
	if (ret) {
		OTA_ERR("first ping failed: %d\n", ret);
		goto out;
	}

	return 0;
 out:
	tcp_cleanup(true);
	return ret;
}

int bluemix_pub_temp_c(int temperature, char *buffer, size_t size)
{
	struct mqtt_publish_msg *pub_msg = &client_ctx.pub_msg;
	INIT_DEVICE_TOPIC("iot-2/type/%s/id/%s/evt/status/fmt/json");
	snprintf(buffer, size,
		"{"
			"d:{"
				"temperature:%d"
			"}"
		"}",
		temperature);
	pub_msg->msg = buffer;
	pub_msg->msg_len = strlen(buffer);
	pub_msg->qos = MQTT_QoS0;
	pub_msg->topic = topic;
	pub_msg->topic_len = strlen(pub_msg->topic);
	pub_msg->pkt_id = sys_rand32_get();
	return mqtt_tx_publish(&client_ctx.mqtt_ctx, &client_ctx.pub_msg);
}

#endif /* (CONFIG_DM_BACKEND == BACKEND_BLUEMIX) */
