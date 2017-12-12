/*
 * Copyright (c) 2017 Linaro Limited
 * Copyright (c) 2017 Open Source Foundries Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Much of the initial code here was pulled from
 * samples/net/mqtt_publisher
 */

#define SYS_LOG_DOMAIN "fota/mqtt-helper"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_FOTA_LEVEL
#include <logging/sys_log.h>

#include <zephyr.h>

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <misc/stack.h>
#include <net/net_app.h>
#include <net/net_event.h>
#include <net/net_mgmt.h>

#include "product_id.h"
#include "tcp.h"
#include "mqtt-helper.h"

#define MQTT_USERNAME	"make-this-configurable"
#define MQTT_CONNECT_TRIES	10
#define APP_CONNECT_TRIES	10
#define APP_SLEEP_MSECS		K_MSEC(500)
#define APP_TX_RX_TIMEOUT	K_MSEC(300)
#define MQTT_DISCONNECT_WAIT	K_MSEC(1000)

/* Network configuration checks */
#if defined(CONFIG_NET_IPV6)
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_PEER_IPV6_ADDR) > 1,
		"CONFIG_NET_APP_PEER_IPV6_ADDR must be defined in boards/$(BOARD)-local.conf");
#define MQTT_HELPER_SERVER_ADDR    CONFIG_NET_APP_PEER_IPV6_ADDR
#elif defined(CONFIG_NET_IPV4)
#if !defined(CONFIG_NET_DHCPV4)
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_MY_IPV4_ADDR) > 1,
		"DHCPv4 must be enabled, or CONFIG_NET_APP_MY_IPV4_ADDR must be defined, in boards/$(BOARD)-local.conf");
#endif
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_PEER_IPV4_ADDR) > 1,
		"CONFIG_NET_APP_PEER_IPV4_ADDR must be defined in boards/$(BOARD)-local.conf");
#define MQTT_HELPER_SERVER_ADDR    CONFIG_NET_APP_PEER_IPV4_ADDR
#endif

#define MQTT_HELPER_STACK_SIZE 1024
static K_THREAD_STACK_DEFINE(mqtt_helper_thread_stack, MQTT_HELPER_STACK_SIZE);
static struct k_thread mqtt_helper_thread_data;

int mqtt_helper_poll_sleep = K_SECONDS(3);

static bool connection_ready;
#if defined(CONFIG_NET_MGMT_EVENT)
static struct net_mgmt_event_callback cb;
#endif

static inline struct mqtt_helper_ctx *mqtt_to_helper(struct mqtt_ctx *mqtt)
{
	return CONTAINER_OF(mqtt, struct mqtt_helper_ctx, mqtt_ctx);
}

static inline int wait_for_mqtt(struct mqtt_helper_ctx *ctx, s32_t timeout)
{
	return k_sem_take(&ctx->wait_sem, timeout);
}

static void connect_cb(struct mqtt_ctx *ctx)
{
	SYS_LOG_DBG("MQTT connected");
	k_sem_give(&mqtt_to_helper(ctx)->wait_sem);
}

static void disconnect_cb(struct mqtt_ctx *ctx)
{
	SYS_LOG_DBG("MQTT disconnected");
	k_sem_give(&mqtt_to_helper(ctx)->wait_sem);
}

static void malformed_cb(struct mqtt_ctx *ctx, u16_t pkt_type)
{
	SYS_LOG_DBG("MQTT malformed CB");
}

/*
 * Try to connect to the MQTT broker. The helper context must have
 * properly initialized mqtt_ctx and connect_msg fields.
 */
static int try_to_connect(struct mqtt_helper_ctx *ctx)
{
	struct mqtt_ctx *mqtt = &ctx->mqtt_ctx;
	struct mqtt_connect_msg *msg = &ctx->connect_msg;
	int i = 0;
	int ret;

	for (i = 0; i < APP_CONNECT_TRIES; i++) {
		ret = mqtt_tx_connect(mqtt, msg);
		if (ret) {
			SYS_LOG_ERR("mqtt_tx_connect: %d", ret);
			continue;
		}
		ret = wait_for_mqtt(ctx, APP_SLEEP_MSECS);
		if (mqtt->connected) {
			return 0;
		}
	}

	return -ETIMEDOUT;
}

static int publish_message(struct mqtt_helper_ctx *ctx)
{
	int ret;

	SYS_LOG_DBG("topic: %s", ctx->pub_msg.topic);
	SYS_LOG_DBG("message: %s", ctx->pub_msg.msg);
	ret = mqtt_tx_publish(&ctx->mqtt_ctx, &ctx->pub_msg);
	if (ret) {
		SYS_LOG_ERR("publish failed: %d", ret);
	}

	return ret;
}

static int helper_start(struct mqtt_helper_ctx *ctx)
{
	int i, ret = 0;

	/* Set everything to 0 before assigning the required fields. */
	memset(ctx, 0x00, sizeof(*ctx));

	/*
	 * Initialize the IDs etc. before doing anything else.
	 *
	 * The values used here are legacy values, which need to be
	 * cleaned up.
	 */
	snprintk(ctx->mh_id, sizeof(ctx->mh_id), "%s-%08x",
		 MQTT_HELPER_DEVICE_TYPE, product_id_get()->number);
	snprintk(ctx->client_id, sizeof(ctx->client_id), "d:%s:%s:%s",
		 "fake-bluemix-org", MQTT_HELPER_DEVICE_TYPE, ctx->mh_id);
	snprintk(ctx->mh_auth_token, sizeof(ctx->mh_auth_token),
		 "%08x", product_id_get()->number);

	k_sem_init(&ctx->wait_sem, 0, 1);

	ctx->mqtt_ctx.connect = connect_cb;
	ctx->mqtt_ctx.disconnect = disconnect_cb;
	ctx->mqtt_ctx.malformed = malformed_cb;
	ctx->mqtt_ctx.net_timeout = APP_TX_RX_TIMEOUT;
	ctx->mqtt_ctx.peer_addr_str = MQTT_HELPER_SERVER_ADDR;
	ctx->mqtt_ctx.peer_port = MQTT_HELPER_PORT;

	ret = mqtt_init(&ctx->mqtt_ctx, MQTT_APP_PUBLISHER);
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
	ctx->connect_msg.keep_alive = 0;
	ctx->connect_msg.user_name = MQTT_USERNAME;
	ctx->connect_msg.user_name_len = strlen(ctx->connect_msg.user_name);
	ctx->connect_msg.password = ctx->mh_auth_token;
	ctx->connect_msg.password_len = strlen(ctx->connect_msg.password);
	ctx->connect_msg.clean_session = 1;

	for (i = 0; i < MQTT_CONNECT_TRIES; i++) {
		ret = mqtt_connect(&ctx->mqtt_ctx);
		if (!ret) {
			break;
		}
	}

	if (ret) {
		goto out;
	}

	ret = try_to_connect(ctx);
	if (ret) {
		goto cleanup;
	}

	return 0;

cleanup:
	mqtt_close(&ctx->mqtt_ctx);
out:
	return ret;
}

static int helper_fini(struct mqtt_helper_ctx *ctx)
{
	int ret;

	ret = mqtt_tx_disconnect(&ctx->mqtt_ctx);
	if (ret) {
		SYS_LOG_ERR("mqtt_tx_disconnect: %d", ret);
		goto cleanup;
	}
	ret = wait_for_mqtt(ctx, MQTT_DISCONNECT_WAIT);
	if (ret) {
		SYS_LOG_WRN("wait_for_mqtt: %d", ret);
		/* TODO: not sure what else to do here */
	}

 cleanup:
	mqtt_close(&ctx->mqtt_ctx);
	return ret;
}

int mqtt_helper_pub_status_json(struct mqtt_helper_ctx *ctx,
				const char *fmt, ...)
{
	struct mqtt_publish_msg *pub_msg = &ctx->pub_msg;
	va_list vargs;
	int ret;

	snprintk(ctx->mh_topic, sizeof(ctx->mh_topic),
		 "iot-2/type/%s/id/%s/evt/status/fmt/json",
		 MQTT_HELPER_DEVICE_TYPE, ctx->mh_id);

	/* Fill in the initial '{"d":'. */
	ret = snprintk(ctx->mh_message, sizeof(ctx->mh_message), "{\"d\":");
	if (ret == sizeof(ctx->mh_message) - 1) {
		return -ENOMEM;
	}
	/* Add the user data. */
	va_start(vargs, fmt);
	ret += vsnprintk(ctx->mh_message + ret, sizeof(ctx->mh_message) - ret,
			 fmt, vargs);
	va_end(vargs);
	if (ret > sizeof(ctx->mh_message) - 2) {
		/* Overflow check: 2 = (1 for '\0') + (1 for "}") */
		return -ENOMEM;
	}
	/* Append the closing brace. */
	snprintk(ctx->mh_message + ret, sizeof(ctx->mh_message) - ret, "}");

	/* Fill out the MQTT publication, and ship it.
	 *
	 * IMPORTANT: don't increase the level of QoS here, even if
	 *            Zephyr claims to support it, until the Zephyr
	 *            MQTT stack can correctly parse multiple MQTT
	 *            packets within a single struct net_pkt.
	 *
	 * Working around that issue implies never receiving multiple
	 * MQTT packets in the same net_pkt.
	 *
	 * Keep this at QoS 0 to avoid receiving PUBACK or PUBREC in
	 * response to this message. Since this app is a publisher
	 * only, the remaining possible incoming messages are CONNACK
	 * and PINGRESP (depending on nonzero keep_alive). Those will
	 * never be transmitted at the same time, as we ought to wait
	 * for CONNACK before sending any PINGREQs.
	 */
	pub_msg->msg = ctx->mh_message;
	pub_msg->msg_len = strlen(pub_msg->msg);
	pub_msg->qos = MQTT_QoS0;
	pub_msg->topic = ctx->mh_topic;
	pub_msg->topic_len = strlen(pub_msg->topic);
	return publish_message(ctx);
}

static void helper_service(void *mh_cbv, void *mh_cb_data, void *p3)
{
	static struct mqtt_helper_ctx helper_context;
	mqtt_helper_cb mh_cb = mh_cbv;
	static int mqtt_helper_inited;
	int ret;

	ARG_UNUSED(p3);

	while (true) {
		k_sleep(mqtt_helper_poll_sleep);

		if (!connection_ready) {
			SYS_LOG_DBG("Network interface is not ready");
			continue;
		}

		tcp_interface_lock();

		if (!mqtt_helper_inited) {
			ret = helper_start(&helper_context);
			if (ret) {
				SYS_LOG_ERR("connection failed: %d", ret);
				ret = mh_cb(&helper_context,
					    MQTT_HELPER_EVT_CONN_FAIL,
					    mh_cb_data);
				switch (ret) {
				case MQTT_HELPER_CB_OK:
				case MQTT_HELPER_CB_RECONNECT:
					tcp_interface_unlock();
					continue;
				default:
					goto out_unlock;
				}
			} else {
				mqtt_helper_inited = 1;
			}
		}

		ret = mh_cb(&helper_context, MQTT_HELPER_EVT_POLL, mh_cb_data);
		switch (ret) {
		case MQTT_HELPER_CB_OK:
			break;
		case MQTT_HELPER_CB_RECONNECT:
			ret = helper_fini(&helper_context);
			if (ret) {
				SYS_LOG_ERR("helper_fini: %d", ret);
			}
			mqtt_helper_inited = 0;
			break;
		case MQTT_HELPER_CB_HALT:
			goto out_close;
		default:
			SYS_LOG_ERR("callback returned %d", ret);
			goto out_close;
		}

		tcp_interface_unlock();

		STACK_ANALYZE("MQTT Helper Thread", mqtt_helper_thread_stack);
	}

 out_close:
	ret = helper_fini(&helper_context);
	(void)ret;		/* Ignore the return value. */
 out_unlock:
	tcp_interface_unlock();
}

static void event_iface_up(struct net_mgmt_event_callback *cb,
			   u32_t mgmt_event, struct net_if *iface)
{
	connection_ready = true;
}

int mqtt_helper_init(mqtt_helper_cb mh_cb, void *mh_cb_data)
{
	/* TODO: default interface may not always be the one we want */
	struct net_if *iface = net_if_get_default();

	k_thread_create(&mqtt_helper_thread_data, &mqtt_helper_thread_stack[0],
			K_THREAD_STACK_SIZEOF(mqtt_helper_thread_stack),
			(k_thread_entry_t) helper_service,
			mh_cb, mh_cb_data, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

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

	return 0;
}
