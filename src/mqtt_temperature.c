/*
 * Copyright (c) 2017 Linaro Limited
 * Copyright (c) 2017 Open Source Foundries Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mqtt_temperature.h"

#define SYS_LOG_DOMAIN "mqtt_temp"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_FOTA_LEVEL
#include <logging/sys_log.h>

#include <errno.h>
#include <stdio.h>

#include <zephyr.h>
#include <device.h>
#include <misc/reboot.h>
#include <sensor.h>
#include <tc_util.h>

/* TODO consolidate this into one file. */
#include "mqtt-helper.h"

#include "app_work_queue.h"

#define MAX_FAILURES		5
#define NUM_TEST_RESULTS	5
#define MCU_TEMP_DEV		"fota-mcu-temp"
#define OFFCHIP_TEMP_DEV	"fota-offchip-temp"

struct temp_mqtt_data {
	struct device *mcu_dev;
	struct device *offchip_dev;
	int failures;

	/* For test reporting */
	struct k_work tc_work;
	u8_t tc_results[NUM_TEST_RESULTS];
	u8_t tc_count;
};

static struct temp_mqtt_data temp_data;

static int cb_handle_result(struct temp_mqtt_data *data, int result)
{
	if (result) {
		if (++data->failures >= MAX_FAILURES) {
			SYS_LOG_ERR("Too many MQTT errors, rebooting!");
			sys_reboot(0);
		}
	} else {
		data->failures = 0;
	}
	/* No reboot was necessary, so keep going. */
	return MQTT_HELPER_CB_OK;
}

/*
 * This work handler prints the results for publishing temperature
 * readings to the MQTT broker. It doesn't actually take temperature readings
 * or publish them via the network -- it's only responsible for
 * printing the test results themselves.
 */
static void temp_mqtt_publish_result(struct k_work *work)
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

static int init_temp_data(struct temp_mqtt_data *data)
{
	data->mcu_dev = device_get_binding(MCU_TEMP_DEV);
	data->offchip_dev = device_get_binding(OFFCHIP_TEMP_DEV);
	data->failures = 0;
	k_work_init(&data->tc_work, temp_mqtt_publish_result);
	data->tc_count = 0;

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

static void handle_test_result(struct temp_mqtt_data *data, u8_t result)
{
	if (data->tc_count >= NUM_TEST_RESULTS) {
		return;
	}

	data->tc_results[data->tc_count++] = result;

	if (data->tc_count == NUM_TEST_RESULTS) {
		app_wq_submit(&data->tc_work);
	}
}

static int temp_mqtt_conn_fail(struct mqtt_helper_ctx *ctx, void *data)
{
	return cb_handle_result(data, -ENOTCONN);
}

static int temp_mqtt_poll(struct mqtt_helper_ctx *ctx, void *datav)
{
	struct temp_mqtt_data *data = datav;
	struct sensor_value mcu_val;
	struct sensor_value offchip_val;
	int ret = 0;

	/*
	 * Try to read temperature sensor values, and publish the
	 * whole number portion of temperatures that are read.
	 */
	if (data->mcu_dev) {
		ret = read_temperature(data->mcu_dev, &mcu_val);
	}
	if (ret) {
		goto out;
	}
	if (data->offchip_dev) {
		ret = read_temperature(data->offchip_dev, &offchip_val);
	}
	if (ret) {
		goto out;
	}

	if (data->mcu_dev && data->offchip_dev) {
		ret = mqtt_helper_pub_status_json(ctx,
						  "{"
						  "\"mcutemp\":%d,"
						  "\"temperature\":%d"
						  "}",
						  mcu_val.val1,
						  offchip_val.val1);
	} else if (data->mcu_dev) {
		ret = mqtt_helper_pub_status_json(ctx,
						  "{"
						  "\"mcutemp\":%d"
						  "}",
						  mcu_val.val1);
	} else {
		/* We know we have at least one device. */
		ret = mqtt_helper_pub_status_json(ctx,
						  "{"
						  "\"temperature\":%d"
						  "}",
						  offchip_val.val1);
	}

 out:
	handle_test_result(data, ret ? TC_FAIL : TC_PASS);
	return cb_handle_result(data, ret);
}

static int temp_mqtt_cb(struct mqtt_helper_ctx *ctx, int event, void *data)
{
	switch (event) {
	case MQTT_HELPER_EVT_CONN_FAIL:
		return temp_mqtt_conn_fail(ctx, data);
	case MQTT_HELPER_EVT_POLL:
		return temp_mqtt_poll(ctx, data);
	default:
		SYS_LOG_ERR("unexpected callback event %d", event);
		return MQTT_HELPER_CB_HALT;
	}
}

int mqtt_temperature_start(void)
{
	int ret;

	ret = init_temp_data(&temp_data);
	if (ret) {
		SYS_LOG_ERR("can't initialize temperature sensors: %d", ret);
		return ret;
	}

	return mqtt_helper_init(temp_mqtt_cb, &temp_data);
}
