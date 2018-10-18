/*
 * Copyright (c) 2017 Linaro Limited
 * Copyright (c) 2017 Open Source Foundries Limited
 * Copyright (c) 2018 Foundries.io
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Much of the initial code here was pulled from
 * samples/net/mqtt_publisher
 */

#define LOG_MODULE_NAME fota_mqtt_temp
#define LOG_LEVEL CONFIG_FOTA_LOG_LEVEL

#include <logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include <device.h>
#include <json.h>
#include <logging/log_ctrl.h>
#include <misc/reboot.h>
#include <net/net_app.h>
#include <net/net_event.h>
#include <net/net_if.h>
#include <net/net_mgmt.h>
#include <net/mqtt.h>
#include <sensor.h>
#include <tc_util.h>
#include <toolchain.h>
#include <zephyr.h>

#include "product_id.h"
#include "app_work_queue.h"
#include "mqtt_temperature.h"
#ifdef CONFIG_NET_L2_BT
#include "bluetooth.h"
#endif

#define MAX_FAILURES		5
#define NUM_TEST_RESULTS	5
#define AMB_TEMP_DEV		"fota-ambient-temp"
#define DIE_TEMP_DEV		"fota-die-temp"
#define MQTT_PORT		1883
#define MQTT_USERNAME		CONFIG_FOTA_MQTT_USERNAME
#define MQTT_PASSWORD		CONFIG_FOTA_MQTT_PASSWORD
#define MQTT_CONNECT_TRIES	10
#define APP_CONNECT_TRIES	10
#define CONNECT_WAIT_TIMEOUT	K_MSEC(500)
#define PUBLISH_DELAY_TIME	K_SECONDS(3)
#define MQTT_NET_TIMEOUT	K_MSEC(300)

/* Network configuration checks */
#if defined(CONFIG_NET_IPV6)
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_CONFIG_PEER_IPV6_ADDR) > 1,
		"CONFIG_NET_CONFIG_PEER_IPV6_ADDR must be defined in boards/$(BOARD)-local.conf");
#define MQTT_HELPER_SERVER_ADDR    CONFIG_NET_CONFIG_PEER_IPV6_ADDR
#elif defined(CONFIG_NET_IPV4)
#if !defined(CONFIG_NET_DHCPV4)
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_CONFIG_MY_IPV4_ADDR) > 1,
		"DHCPv4 must be enabled, or CONFIG_NET_CONFIG_MY_IPV4_ADDR must be defined, in boards/$(BOARD)-local.conf");
#endif
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_CONFIG_PEER_IPV4_ADDR) > 1,
		"CONFIG_NET_CONFIG_PEER_IPV4_ADDR must be defined in boards/$(BOARD)-local.conf");
#define MQTT_HELPER_SERVER_ADDR    CONFIG_NET_CONFIG_PEER_IPV4_ADDR
#endif

/*
 * All possible sources of data.
 *
 * This (or a subset of it) is what gets serialized into JSON.
 */
struct mqtt_sensor_data {
	int amb_temp;
	int die_temp;
};

#define MAX_SENSOR_DATA 2

/*
 * Main context object.
 */
struct temp_mqtt_data {
	/* MQTT plumbing. */
	u8_t mqtt_client_id[30];	/* Device-unique identifier */
	u8_t mqtt_password[20];	/* MQTT password */
	u8_t mqtt_topic[255];		/* Buffer for topic names */
	u8_t mqtt_message[1024];	/* Buffer for message data */
	struct mqtt_ctx mqtt;
	struct mqtt_connect_msg connect_msg;
	struct mqtt_publish_msg pub_msg;
	struct k_sem mqtt_wait_sem;
	struct k_delayed_work mqtt_work;
	int failures;

	/* Sensor data sources. */
	struct device *amb_dev;
	struct device *die_dev;
	struct mqtt_sensor_data sensor_data;
	/*
	 * This gets built up at runtime depending on the devices that
	 * were discovered.
	 */
	struct json_obj_descr sensor_json_descr[MAX_SENSOR_DATA];
	int sensor_num_sources;

	/* Test reporting. */
	struct k_work tc_work;
	u8_t tc_results[NUM_TEST_RESULTS];
	u8_t tc_count;
};

static const struct json_obj_descr json_amb_temp_descr =
	JSON_OBJ_DESCR_PRIM(struct mqtt_sensor_data, amb_temp, JSON_TOK_NUMBER);

static const struct json_obj_descr json_die_temp_descr =
	JSON_OBJ_DESCR_PRIM(struct mqtt_sensor_data, die_temp,
			    JSON_TOK_NUMBER);

static struct temp_mqtt_data temp_data;

#if defined(CONFIG_NET_MGMT_EVENT)
static struct net_mgmt_event_callback net_mgmt_cb;
#endif

#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
NET_PKT_TX_SLAB_DEFINE(mqtt_client_tx, 5);
NET_PKT_DATA_POOL_DEFINE(mqtt_client_data, 15);

static struct k_mem_slab *tx_slab(void)
{
	return &mqtt_client_tx;
}

static struct net_buf_pool *data_pool(void)
{
	return &mqtt_client_data;
}
#else
#if defined(CONFIG_NET_L2_BT)
#error "TCP connections over Bluetooth need CONFIG_NET_CONTEXT_NET_PKT_POOL "\
	"defined."
#endif /* CONFIG_NET_L2_BT */

#define tx_slab NULL
#define data_pool NULL
#endif /* CONFIG_NET_CONTEXT_NET_PKT_POOL */

static void temp_mqtt_reboot_check(struct temp_mqtt_data *data, int result)
{
	if (result) {
		if (++data->failures >= MAX_FAILURES) {
			LOG_ERR("Too many MQTT errors, rebooting!");
#ifdef CONFIG_NET_L2_BT
			bt_network_disable();
#endif
			LOG_PANIC();
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
			    enum sensor_channel temp_channel,
			    struct sensor_value *temp_val)
{
	__unused const char *name = temp_dev->config->name;
	int ret;

	ret = sensor_sample_fetch(temp_dev);
	if (ret) {
		LOG_ERR("%s: I/O error: %d", name, ret);
		return ret;
	}

	ret = sensor_channel_get(temp_dev, temp_channel, temp_val);
	if (ret) {
		LOG_ERR("%s: can't get data: %d", name, ret);
		return ret;
	}

	LOG_DBG("%s: read %d.%d C", name, temp_val->val1, temp_val->val2);
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
	LOG_DBG("connected");
	k_sem_give(&mqtt_to_data(mqtt)->mqtt_wait_sem);
}

static void temp_mqtt_disconnect_cb(struct mqtt_ctx *mqtt)
{
	LOG_DBG("disconnected");
	k_sem_give(&mqtt_to_data(mqtt)->mqtt_wait_sem);
}

static void temp_mqtt_malformed_cb(struct mqtt_ctx *mqtt, u16_t pkt_type)
{
	LOG_DBG("malformed data, type 0x%x", pkt_type);
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
			LOG_ERR("mqtt_tx_connect: %d", ret);
			continue;
		}
		ret = temp_mqtt_wait(data, CONNECT_WAIT_TIMEOUT);

		if (mqtt->connected) {
			return 0;
		}
	}

	mqtt_close(&data->mqtt);
	LOG_ERR("timed out");
	return -ETIMEDOUT;
}

static int temp_mqtt_publish(struct temp_mqtt_data *data)
{
	struct mqtt_publish_msg *pub_msg = &data->pub_msg;
	int ret;

	/*
	 * Set up the topic and message contents.
	 */
	snprintk(data->mqtt_topic, sizeof(data->mqtt_topic),
		 "id/%s/sensor-data/json", data->mqtt_client_id);
	ret =  json_obj_encode_buf(data->sensor_json_descr,
				   data->sensor_num_sources,
				   &data->sensor_data,
				   data->mqtt_message,
				   sizeof(data->mqtt_message) - 1);
	if (ret) {
		LOG_ERR("json_obj_encode_buf: %d", ret);
		return ret;
	}

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
	pub_msg->msg = data->mqtt_message;
	pub_msg->msg_len = strlen(pub_msg->msg);
	pub_msg->qos = MQTT_QoS0;
	pub_msg->topic = data->mqtt_topic;
	pub_msg->topic_len = strlen(pub_msg->topic);

	LOG_DBG("topic: %s", data->pub_msg.topic);
	LOG_DBG("message: %s", data->pub_msg.msg);
	ret = mqtt_tx_publish(&data->mqtt, &data->pub_msg);
	if (ret) {
		LOG_ERR("publish failed: %d", ret);
	}

	return ret;
}

static void temp_mqtt_try_to_publish(struct k_work *work)
{
	struct temp_mqtt_data *data =
		CONTAINER_OF(work, struct temp_mqtt_data, mqtt_work);
	struct sensor_value mcu_val;
	struct sensor_value die_val;
	int ret = 0;

	if (!data->mqtt.connected) {
		ret = temp_mqtt_connect(data);
		if (ret) {
			LOG_ERR("connection failed: %d", ret);
			goto out;
		}
	}

	/*
	 * Try to read temperature sensor values, and publish the
	 * whole number portion of temperatures that are read.
	 */
	if (data->amb_dev) {
		ret = read_temperature(data->amb_dev,
				       SENSOR_CHAN_AMBIENT_TEMP,
				       &mcu_val);
	}
	if (ret) {
		goto out_handle_result;
	}
	if (data->die_dev) {
		ret = read_temperature(data->die_dev,
				       SENSOR_CHAN_DIE_TEMP,
				       &die_val);
	}
	if (ret) {
		goto out_handle_result;
	}

	if (data->amb_dev) {
		data->sensor_data.amb_temp = mcu_val.val1;
	}
	if (data->die_dev) {
		data->sensor_data.die_temp = die_val.val1;
	}

	ret = temp_mqtt_publish(data);

 out_handle_result:
	temp_mqtt_handle_test_result(data, ret ? TC_FAIL : TC_PASS);
 out:
	temp_mqtt_reboot_check(data, ret);
	app_wq_submit_delayed(&data->mqtt_work, PUBLISH_DELAY_TIME);
}

/*
 * Initialization
 */

static int init_mqtt_plumbing(struct temp_mqtt_data *data)
{
	int ret;

	snprintk(data->mqtt_client_id, sizeof(data->mqtt_client_id), "%s-%08x",
		 CONFIG_BOARD, product_id_get()->number);
	if (!strcmp(MQTT_PASSWORD, "")) {
		snprintk(data->mqtt_password, sizeof(data->mqtt_password),
			 "%08x", product_id_get()->number);
	} else {
		snprintk(data->mqtt_password, sizeof(data->mqtt_password),
			 "%s", MQTT_PASSWORD);
	}

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

#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
	net_app_set_net_pkt_pool(&data->mqtt.net_app_ctx, tx_slab, data_pool);
#endif

	data->connect_msg.client_id = data->mqtt_client_id;
	data->connect_msg.client_id_len = strlen(data->connect_msg.client_id);
	data->connect_msg.keep_alive = 0;
	data->connect_msg.user_name = MQTT_USERNAME;
	data->connect_msg.user_name_len = strlen(data->connect_msg.user_name);
	data->connect_msg.password = data->mqtt_client_id;
	data->connect_msg.password_len = strlen(data->connect_msg.password);
	data->connect_msg.clean_session = 1;

	k_sem_init(&data->mqtt_wait_sem, 0, 1);
	k_delayed_work_init(&data->mqtt_work, temp_mqtt_try_to_publish);

	data->failures = 0;

	return 0;
}

static int init_sensor_sources(struct temp_mqtt_data *data)
{
	int num_sources = 0;
	data->amb_dev = device_get_binding(AMB_TEMP_DEV);
	data->die_dev = device_get_binding(DIE_TEMP_DEV);

	LOG_INF("%s ambient temperature sensor %s",
		data->amb_dev ? "Found" : "Did not find", AMB_TEMP_DEV);
	if (data->amb_dev) {
		memcpy(&data->sensor_json_descr[num_sources],
		       &json_amb_temp_descr,
		       sizeof(struct json_obj_descr));
		num_sources++;
	}

	LOG_INF("%s die temperature sensor %s",
		data->die_dev ? "Found" : "Did not find", DIE_TEMP_DEV);
	if (data->die_dev) {
		memcpy(&data->sensor_json_descr[num_sources],
		       &json_die_temp_descr,
		       sizeof(struct json_obj_descr));
		num_sources++;
	}

	if (num_sources == 0) {
		LOG_ERR("No temperature devices found.");
		return -ENODEV;
	}

	data->sensor_num_sources = num_sources;
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
		LOG_ERR("can't initialize: %d", ret);
		return ret;
	}

	/*
	 * Try to start publishing sensor data if the network
	 * interface is up. If it's not up, wait until it is to start
	 * publishing.
	 */
#if defined(CONFIG_NET_MGMT_EVENT)
	/* Subscribe to NET_EVENT_IF_UP if interface is not ready */
	if (!net_if_is_up(iface)) {
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
