/*
 * Copyright (c) 2017 Linaro Limited
 * Copyright (c) 2017 Open Source Foundries Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Example MQTT client, which periodically publishes
 *        temperature data to the cloud.
 *
 * Temperature data can come from up to two sources:
 *
 * 1. One on-chip temperature sensor, named "fota-mcu-temp"
 * 2. One off-chip temperature sensor, named "fota-offchip-temp"
 *
 * The target configuration can ensure that a Zephyr temperature
 * sensor device has one of the given names, by configuring the device
 * driver name in a board-specific .conf file fragment. Examples are
 * in the board-level files in boards/.
 */

#ifndef __FOTA_MQTT_TEMPERATURE_H__
#define __FOTA_MQTT_TEMPERATURE_H__

/**
 * @brief Start the background MQTT thread
 *
 * This thread will periodically attempt to publish temperature data
 * to an MQTT broker.
 *
 * @return 0 if the thread is started successfully, and a negative
 *         errno on error.
 */
int mqtt_temperature_start(void);

#endif	/* __FOTA_MQTT_TEMPERATURE_H__ */
