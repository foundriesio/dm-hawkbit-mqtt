/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bluemix_temperature.h"

#define SYS_LOG_DOMAIN "bluemix_temp"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_FOTA_LEVEL
#include <logging/sys_log.h>

#include <errno.h>

#include <zephyr.h>
#include <device.h>
#include <misc/reboot.h>
#include <sensor.h>

#include "bluemix.h"

#define MAX_FAILURES		5
#define MCU_TEMP_DEV		"fota-mcu-temp"
#define OFFCHIP_TEMP_DEV	"fota-offchip-temp"

struct temp_bluemix_data {
	struct device *mcu_dev;
	struct device *offchip_dev;
	int failures;
};

static struct temp_bluemix_data temp_bm_data;

static int cb_handle_result(struct temp_bluemix_data *data, int result)
{
	if (result) {
		if (++data->failures >= MAX_FAILURES) {
			SYS_LOG_ERR("Too many Bluemix errors, rebooting!");
			sys_reboot(0);
		}
	} else {
		data->failures = 0;
	}
	/* No reboot was necessary, so keep going. */
	return BLUEMIX_CB_OK;
}

static int init_temp_data(struct temp_bluemix_data *data)
{
	data->mcu_dev = device_get_binding(MCU_TEMP_DEV);
	data->offchip_dev = device_get_binding(OFFCHIP_TEMP_DEV);
	data->failures = 0;

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

static int temp_bm_conn_fail(struct bluemix_ctx *ctx, void *data)
{
	return cb_handle_result(data, -ENOTCONN);
}

static int temp_bm_poll(struct bluemix_ctx *ctx, void *datav)
{
	struct temp_bluemix_data *data = datav;
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
		return cb_handle_result(data, ret);
	}
	if (data->offchip_dev) {
		ret = read_temperature(data->offchip_dev, &offchip_val);
	}
	if (ret) {
		return cb_handle_result(data, ret);
	}

	if (data->mcu_dev && data->offchip_dev) {
		ret = bluemix_pub_status_json(ctx,
					      "{"
					      "\"mcutemp\":%d,"
					      "\"temperature\":%d"
					      "}",
					      mcu_val.val1,
					      offchip_val.val1);
	} else if (data->mcu_dev) {
		ret = bluemix_pub_status_json(ctx,
					      "{"
					      "\"mcutemp\":%d"
					      "}",
					      mcu_val.val1);
	} else {
		/* We know we have at least one device. */
		ret = bluemix_pub_status_json(ctx,
					      "{"
					      "\"temperature\":%d"
					      "}",
					      offchip_val.val1);
	}

	return cb_handle_result(data, ret);
}

static int temp_bm_cb(struct bluemix_ctx *ctx, int event, void *data)
{
	switch (event) {
	case BLUEMIX_EVT_CONN_FAIL:
		return temp_bm_conn_fail(ctx, data);
	case BLUEMIX_EVT_POLL:
		return temp_bm_poll(ctx, data);
	default:
		SYS_LOG_ERR("unexpected callback event %d");
		return BLUEMIX_CB_HALT;
	}
}

int bluemix_temperature_start(void)
{
	int ret;

	ret = init_temp_data(&temp_bm_data);
	if (ret) {
		SYS_LOG_ERR("can't initialize temperature sensors: %d", ret);
		return ret;
	}

	return bluemix_init(temp_bm_cb, &temp_bm_data);
}
