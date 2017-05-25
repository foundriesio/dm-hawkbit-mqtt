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

#define MAX_FAILURES	5

#define GENERIC_MCU_TEMP_SENSOR_DEVICE	"fota-mcu-temp"
#define GENERIC_OFFCHIP_TEMP_SENSOR_DEVICE "fota-offchip-temp"

struct temp_bluemix_data {
	struct device *mcu_temp_sensor_dev;
	struct device *offchip_temp_sensor_dev;
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
	data->mcu_temp_sensor_dev =
		device_get_binding(GENERIC_MCU_TEMP_SENSOR_DEVICE);
	data->offchip_temp_sensor_dev =
		device_get_binding(GENERIC_OFFCHIP_TEMP_SENSOR_DEVICE);
	data->failures = 0;

	SYS_LOG_INF("%s MCU temperature sensor %s%s",
		    data->mcu_temp_sensor_dev ? "Found" : "Did not find",
		    GENERIC_MCU_TEMP_SENSOR_DEVICE,
		    data->mcu_temp_sensor_dev ? "" : "; using default values");
	SYS_LOG_INF("%s off-chip temperature sensor %s",
		    data->offchip_temp_sensor_dev ? "Found" : "Did not find",
		    GENERIC_OFFCHIP_TEMP_SENSOR_DEVICE);
	return 0;
}

static int get_temp_sensor_data(struct device *temp_dev,
				struct sensor_value *temp_value,
				bool use_defaults_on_null)
{
	int ret = 0;

	if (!temp_dev) {
		if (use_defaults_on_null) {
			temp_value->val1 = 23;
			temp_value->val2 = 0;
			return 0;
		} else {
			return -ENODEV;
		}
	}

	ret = sensor_sample_fetch(temp_dev);
	if (ret) {
		return ret;
	}

	return sensor_channel_get(temp_dev, SENSOR_CHAN_TEMP, temp_value);
}

static int temp_bm_conn_fail(struct bluemix_ctx *ctx, void *data)
{
	return cb_handle_result(data, -ENOTCONN);
}

static int temp_bm_poll(struct bluemix_ctx *ctx, void *datav)
{
	struct temp_bluemix_data *data = datav;
	struct sensor_value mcu_temp_value;
	struct sensor_value offchip_temp_value;
	int ret;

	/*
	 * Fetch temperature sensor values. If we don't have an MCU
	 * temperature sensor or encounter errors reading it, use
	 * these values as defaults.
	 */
	ret = get_temp_sensor_data(data->mcu_temp_sensor_dev,
				   &mcu_temp_value, true);
	if (ret) {
		SYS_LOG_ERR("MCU temperature sensor error: %d", ret);
	} else {
		SYS_LOG_DBG("Read MCU temp sensor: %d.%dC",
			    mcu_temp_value.val1, mcu_temp_value.val2);
	}

	ret = get_temp_sensor_data(data->offchip_temp_sensor_dev,
				   &offchip_temp_value, false);
	if (data->offchip_temp_sensor_dev) {
		if (ret) {
			SYS_LOG_ERR("Off-chip temperature sensor error: %d",
				    ret);
		} else {
			SYS_LOG_DBG("Read off-chip temp sensor: %d.%dC",
				    offchip_temp_value.val1,
				    offchip_temp_value.val2);
		}
	}

	/*
	 * Use the whole number portion of temperature sensor
	 * values. Don't publish off-chip values if there is no
	 * sensor, or if there were errors fetching the values.
	 */
	if (ret) {
		ret = bluemix_pub_status_json(ctx,
					      "{"
					      "\"mcutemp\":%d"
					      "}",
					      mcu_temp_value.val1);
	} else {
		ret = bluemix_pub_status_json(ctx,
					      "{"
					      "\"mcutemp\":%d,"
					      "\"temperature\":%d,"
					      "}",
					      mcu_temp_value.val1,
					      offchip_temp_value.val1);
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
