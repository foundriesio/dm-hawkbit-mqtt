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

#include "mqtt_temperature.h"

#define SYS_LOG_DOMAIN "mqtt_temp"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_FOTA_LEVEL
#include <logging/sys_log.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include <device.h>
#include <misc/reboot.h>
#include <net/net_app.h>
#include <net/net_event.h>
#include <net/net_mgmt.h>
#include <net/mqtt.h>
#include <sensor.h>
#include <tc_util.h>
#include <toolchain.h>
#include <zephyr.h>

#include "product_id.h"
#include "tcp.h"
#include "app_work_queue.h"

#define MAX_FAILURES		5
#define NUM_TEST_RESULTS	5
#define MCU_TEMP_DEV		"fota-mcu-temp"
#define OFFCHIP_TEMP_DEV	"fota-offchip-temp"
#define MQTT_PORT		1883
#define MQTT_DEVICE_TYPE	CONFIG_BOARD
#define MQTT_USERNAME		"make-this-configurable"
#define MQTT_CONNECT_TRIES	10
#define APP_CONNECT_TRIES	10
#define CONNECT_WAIT_TIMEOUT	K_MSEC(500)
#define PUBLISH_DELAY_TIME	K_SECONDS(3)
#define MQTT_NET_TIMEOUT	K_MSEC(300)

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

struct temp_mqtt_data {
	/* MQTT plumbing. */
	u8_t mh_id[30];		/* Buffer for device ID (TODO clean up) */
	u8_t client_id[50];		/* MQTT client ID */
	u8_t mh_auth_token[20];	/* Authentication token (TODO clean up) */
	u8_t mh_topic[255];		/* Buffer for topic names */
	u8_t mh_message[1024];		/* Buffer for message data */
	struct mqtt_ctx mqtt;
	struct mqtt_connect_msg connect_msg;
	struct mqtt_publish_msg pub_msg;
	struct k_sem mqtt_wait_sem;
	struct k_delayed_work mqtt_work;
	int failures;

	/* Sensor data sources. */
	struct device *mcu_dev;
	struct device *offchip_dev;

	/* Test reporting. */
	struct k_work tc_work;
	u8_t tc_results[NUM_TEST_RESULTS];
	u8_t tc_count;
};

static struct temp_mqtt_data temp_data;

#if defined(CONFIG_NET_MGMT_EVENT)
static struct net_mgmt_event_callback net_mgmt_cb;
#endif

static void temp_mqtt_reboot_check(struct temp_mqtt_data *data, int result)
{
	if (result) {
		if (++data->failures >= MAX_FAILURES) {
			SYS_LOG_ERR("Too many MQTT errors, rebooting!");
			sys_reboot(0);
		}
	} else {
		data->failures = 0;
	}
}

/*
 * Sensor data handling
 */

static int read_temperature(struct device *temp_dev,
			    struct sensor_value *temp_val)
{
	__unused const char *name = temp_dev->config->name;
	int ret;

	ret = sensor_sample_fetch(temp_dev);
	if (ret) {
		SYS_LOG_ERR("%s: I/O error: %d", name, ret);
		return ret;
	}

	ret = sensor_channel_get(temp_dev, SENSOR_CHAN_TEMP, temp_val);
	if (ret) {
		SYS_LOG_ERR("%s: can't get data: %d", name, ret);
		return ret;
	}

	SYS_LOG_DBG("%s: read %d.%d C",
		    name, temp_val->val1, temp_val->val2);
	return 0;
}

/*
 * Test reporting.
 */

/*
 * This work handler prints the results for publishing temperature
 * readings to the MQTT broker. It doesn't actually take temperature readings
 * or publish them via the network -- it's only responsible for
 * printing the test results themselves.
 */
static void temp_mqtt_print_result(struct k_work *work)
{
	struct temp_mqtt_data *data =
		CONTAINER_OF(work, struct temp_mqtt_data, tc_work);
	/*
	 * `result_name' is long enough for the function name, '_',
	 * two digits of test result, and '\0'. If NUM_TEST_RESULTS is
	 * 100 or more, space for more digits is needed.
	 */
	size_t result_len = strlen(__func__) + 1 + 2 + 1;
	char result_name[result_len];
	u8_t result, final_result = TC_PASS;
	size_t i;

	/* Ensure we have enough space to print the result name. */
	BUILD_ASSERT_MSG(NUM_TEST_RESULTS <= 99,
			 "result_len is too small to print test number");

	TC_START("Publish temperature to MQTT broker");
	for (i = 0; i < data->tc_count; i++) {
		result = data->tc_results[i];
		snprintk(result_name, sizeof(result_name), "%s_%zu",
			 __func__, i);
		if (result == TC_FAIL) {
			final_result = TC_FAIL;
		}
		_TC_END_RESULT(result, result_name);
	}
	TC_END_REPORT(final_result);
}

static void temp_mqtt_handle_test_result(struct temp_mqtt_data *data,
					 u8_t result)
{
	if (data->tc_count >= NUM_TEST_RESULTS) {
		return;
	}

	data->tc_results[data->tc_count++] = result;

	if (data->tc_count == NUM_TEST_RESULTS) {
		app_wq_submit(&data->tc_work);
	}
}

/*
 * MQTT callbacks and other plumbing.
 */

static inline struct temp_mqtt_data *mqtt_to_data(struct mqtt_ctx *mqtt)
{
	return CONTAINER_OF(mqtt, struct temp_mqtt_data, mqtt);
}

static inline int temp_mqtt_wait(struct temp_mqtt_data *data, s32_t timeout)
{
	return k_sem_take(&data->mqtt_wait_sem, timeout);
}

static void temp_mqtt_connect_cb(struct mqtt_ctx *mqtt)
{
	SYS_LOG_DBG("connected");
	k_sem_give(&mqtt_to_data(mqtt)->mqtt_wait_sem);
}

static void temp_mqtt_disconnect_cb(struct mqtt_ctx *mqtt)
{
	SYS_LOG_DBG("disconnected");
	k_sem_give(&mqtt_to_data(mqtt)->mqtt_wait_sem);
}

static void temp_mqtt_malformed_cb(struct mqtt_ctx *mqtt, u16_t pkt_type)
{
	SYS_LOG_DBG("malformed data, type 0x%x", pkt_type);
}

/*
 * Try to connect to the MQTT broker. The helper context must have
 * properly initialized mqtt and connect_msg fields.
 */
static int temp_mqtt_connect(struct temp_mqtt_data *data)
{
	struct mqtt_ctx *mqtt = &data->mqtt;
	struct mqtt_connect_msg *msg = &data->connect_msg;
	int i = 0;
	int ret = 0;

	for (i = 0; i < MQTT_CONNECT_TRIES; i++) {
		ret = mqtt_connect(mqtt);
		if (!ret) {
			break;
		}
	}

	if (ret) {
		return ret;
	}

	for (i = 0; i < APP_CONNECT_TRIES; i++) {
		ret = mqtt_tx_connect(mqtt, msg);
		if (ret) {
			SYS_LOG_ERR("mqtt_tx_connect: %d", ret);
			continue;
		}
		ret = temp_mqtt_wait(data, CONNECT_WAIT_TIMEOUT);

		if (mqtt->connected) {
			return 0;
		}
	}

	mqtt_close(&data->mqtt);
	SYS_LOG_ERR("timed out");
	return -ETIMEDOUT;
}

/*
 * Publish data to the topic and in the format expected by a Bluemix
 * broker.
 *
 * FIXME: remove the Bluemix formatting.
 */
static int temp_mqtt_pub_bluemix_json(struct temp_mqtt_data *data,
				      const char *fmt, ...)
{
	struct mqtt_publish_msg *pub_msg = &data->pub_msg;
	va_list vargs;
	int ret;

	snprintk(data->mh_topic, sizeof(data->mh_topic),
		 "iot-2/type/%s/id/%s/evt/status/fmt/json",
		 MQTT_DEVICE_TYPE, data->mh_id);

	/* Fill in the initial '{"d":'. */
	ret = snprintk(data->mh_message, sizeof(data->mh_message), "{\"d\":");
	if (ret == sizeof(data->mh_message) - 1) {
		return -ENOMEM;
	}
	/* Add the user data. */
	va_start(vargs, fmt);
	ret += vsnprintk(data->mh_message + ret, sizeof(data->mh_message) - ret,
			 fmt, vargs);
	va_end(vargs);
	if (ret > sizeof(data->mh_message) - 2) {
		/* Overflow check: 2 = (1 for '\0') + (1 for "}") */
		return -ENOMEM;
	}
	/* Append the closing brace. */
	snprintk(data->mh_message + ret, sizeof(data->mh_message) - ret, "}");

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
	pub_msg->msg = data->mh_message;
	pub_msg->msg_len = strlen(pub_msg->msg);
	pub_msg->qos = MQTT_QoS0;
	pub_msg->topic = data->mh_topic;
	pub_msg->topic_len = strlen(pub_msg->topic);

	SYS_LOG_DBG("topic: %s", data->pub_msg.topic);
	SYS_LOG_DBG("message: %s", data->pub_msg.msg);
	ret = mqtt_tx_publish(&data->mqtt, &data->pub_msg);
	if (ret) {
		SYS_LOG_ERR("publish failed: %d", ret);
	}

	return ret;
}

static void temp_mqtt_try_to_publish(struct k_work *work)
{
	struct temp_mqtt_data *data =
		CONTAINER_OF(work, struct temp_mqtt_data, mqtt_work);
	struct sensor_value mcu_val;
	struct sensor_value offchip_val;
	int ret = 0;

	tcp_interface_lock();

	if (!data->mqtt.connected) {
		ret = temp_mqtt_connect(data);
		if (ret) {
			SYS_LOG_ERR("connection failed: %d", ret);
			goto out;
		}
	}

	/*
	 * Try to read temperature sensor values, and publish the
	 * whole number portion of temperatures that are read.
	 */
	if (data->mcu_dev) {
		ret = read_temperature(data->mcu_dev, &mcu_val);
	}
	if (ret) {
		goto out_handle_result;
	}
	if (data->offchip_dev) {
		ret = read_temperature(data->offchip_dev, &offchip_val);
	}
	if (ret) {
		goto out_handle_result;
	}

	if (data->mcu_dev && data->offchip_dev) {
		ret = temp_mqtt_pub_bluemix_json(data,
						 "{"
						 "\"mcutemp\":%d,"
						 "\"temperature\":%d"
						 "}",
						 mcu_val.val1,
						 offchip_val.val1);
	} else if (data->mcu_dev) {
		ret = temp_mqtt_pub_bluemix_json(data,
						 "{"
						 "\"mcutemp\":%d"
						 "}",
						 mcu_val.val1);
	} else {
		/* We know we have at least one device. */
		ret = temp_mqtt_pub_bluemix_json(data,
						 "{"
						 "\"temperature\":%d"
						 "}",
						 offchip_val.val1);
	}

 out_handle_result:
	temp_mqtt_handle_test_result(data, ret ? TC_FAIL : TC_PASS);
 out:
	tcp_interface_unlock();
	temp_mqtt_reboot_check(data, ret);
	app_wq_submit_delayed(&data->mqtt_work, PUBLISH_DELAY_TIME);
}

/*
 * Initialization
 */

static int init_mqtt_plumbing(struct temp_mqtt_data *data)
{
	int ret;

	/*
	 * Initialize the IDs etc. before doing anything else.
	 *
	 * The values used here are legacy values, which need to be
	 * cleaned up.
	 */
	snprintk(data->mh_id, sizeof(data->mh_id), "%s-%08x",
		 MQTT_DEVICE_TYPE, product_id_get()->number);
	snprintk(data->client_id, sizeof(data->client_id), "d:%s:%s:%s",
		 "fake-bluemix-org", MQTT_DEVICE_TYPE, data->mh_id);
	snprintk(data->mh_auth_token, sizeof(data->mh_auth_token),
		 "%08x", product_id_get()->number);

	data->mqtt.connect = temp_mqtt_connect_cb;
	data->mqtt.disconnect = temp_mqtt_disconnect_cb;
	data->mqtt.malformed = temp_mqtt_malformed_cb;
	data->mqtt.net_timeout = MQTT_NET_TIMEOUT;
	data->mqtt.peer_addr_str = MQTT_HELPER_SERVER_ADDR;
	data->mqtt.peer_port = MQTT_PORT;
	ret = mqtt_init(&data->mqtt, MQTT_APP_PUBLISHER);
	if (ret) {
		return ret;
	}

	data->connect_msg.client_id = data->client_id;
	data->connect_msg.client_id_len = strlen(data->connect_msg.client_id);
	data->connect_msg.keep_alive = 0;
	data->connect_msg.user_name = MQTT_USERNAME;
	data->connect_msg.user_name_len = strlen(data->connect_msg.user_name);
	data->connect_msg.password = data->mh_auth_token;
	data->connect_msg.password_len = strlen(data->connect_msg.password);
	data->connect_msg.clean_session = 1;

	k_sem_init(&data->mqtt_wait_sem, 0, 1);
	k_delayed_work_init(&data->mqtt_work, temp_mqtt_try_to_publish);

	data->failures = 0;

	return 0;
}

static int init_sensor_sources(struct temp_mqtt_data *data)
{
	data->mcu_dev = device_get_binding(MCU_TEMP_DEV);
	data->offchip_dev = device_get_binding(OFFCHIP_TEMP_DEV);

	SYS_LOG_INF("%s MCU temperature sensor %s",
		    data->mcu_dev ? "Found" : "Did not find",
		    MCU_TEMP_DEV);
	SYS_LOG_INF("%s off-chip temperature sensor %s",
		    data->offchip_dev ? "Found" : "Did not find",
		    OFFCHIP_TEMP_DEV);

	if (!data->mcu_dev && !data->offchip_dev) {
		SYS_LOG_ERR("No temperature devices found.");
		return -ENODEV;
	}

	return 0;
}

static int init_test_reporting(struct temp_mqtt_data *data)
{
	k_work_init(&data->tc_work, temp_mqtt_print_result);
	data->tc_count = 0;
	return 0;
}

static int temp_mqtt_init_data(struct temp_mqtt_data *data)
{
	int ret;

	ret = init_mqtt_plumbing(data);
	if (ret) {
		return ret;
	}

	ret = init_sensor_sources(data);
	if (ret) {
		return ret;
	}

	return init_test_reporting(data);
}

static void temp_mqtt_start(struct net_mgmt_event_callback *cb,
			    u32_t mgmt_event, struct net_if *iface)
{
	struct temp_mqtt_data *data = &temp_data;

	app_wq_submit_delayed(&data->mqtt_work, PUBLISH_DELAY_TIME);
}

int mqtt_temperature_start(void)
{
	/* TODO: default interface may not be the one used by MQTT. */
	struct net_if *iface = net_if_get_default();
	int ret;

	ret = temp_mqtt_init_data(&temp_data);
	if (ret) {
		SYS_LOG_ERR("can't initialize: %d", ret);
		return ret;
	}

	/*
	 * Try to start publishing sensor data if the network
	 * interface is up. If it's not up, wait until it is to start
	 * publishing.
	 */
#if defined(CONFIG_NET_MGMT_EVENT)
	/* Subscribe to NET_IF_UP if interface is not ready */
	if (!atomic_test_bit(iface->flags, NET_IF_UP)) {
		net_mgmt_init_event_callback(&net_mgmt_cb,
					     temp_mqtt_start,
					     NET_EVENT_IF_UP);
		net_mgmt_add_event_callback(&net_mgmt_cb);
		return 0;
	}
#endif

	temp_mqtt_start(NULL, NET_EVENT_IF_UP, iface);
	return 0;
}
