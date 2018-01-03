/*
 * Copyright (c) 2016-2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "fota/main"
#define SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG
#include <logging/sys_log.h>

#include <misc/stack.h>
#include <gpio.h>
#include <sensor.h>
#include <tc_util.h>
#include <init.h>

/* Local helpers and functions */
#include "tstamp_log.h"
#include "app_work_queue.h"
#include "blink_led.h"
#include "mcuboot.h"
#include "product_id.h"
#include "hawkbit.h"
#include "mqtt_temperature.h"

struct device *flash_dev;

void main(void)
{
	tstamp_hook_install();
	app_wq_init();

	SYS_LOG_INF("Linaro FOTA example application");
	SYS_LOG_INF("Device: %s, Serial: %x",
		    product_id_get()->name, product_id_get()->number);

	TC_START("Running Built in Self Test (BIST)");

	TC_PRINT("Initializing Hawkbit backend\n");
	if (hawkbit_init()) {
		_TC_END_RESULT(TC_FAIL, "hawkbit_init");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	_TC_END_RESULT(TC_PASS, "hawkbit_init");

	TC_PRINT("Initializing MQTT temperature service\n");
	if (mqtt_temperature_start()) {
		_TC_END_RESULT(TC_FAIL, "mqtt_init");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	_TC_END_RESULT(TC_PASS, "mqtt_init");

	TC_PRINT("Blinking LED\n");
	if (blink_led_start()) {
		_TC_END_RESULT(TC_FAIL, "blink_led");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	_TC_END_RESULT(TC_PASS, "blink_led");

	TC_END_REPORT(TC_PASS);

	/*
	 * From this point on, just handle work.
	 */
	app_wq_run();
}
