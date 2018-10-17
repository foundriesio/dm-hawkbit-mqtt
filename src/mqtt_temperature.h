/*
 * Copyright (c) 2017 Linaro Limited
 * Copyright (c) 2017 Open Source Foundries Limited
 * Copyright (c) 2018 Foundries.io
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
 * 1. One die temperature sensor, named "fota-die-temp".  This doesn't
 *    have to be the SoC die. It could be an off-chip temperature
 *    sensor that nonetheless reports on-die temperature.
 *
 * 2. One ambient temperature sensor, named "fota-ambient-temp".
 *
 * The target configuration can ensure that a Zephyr temperature
 * sensor device has one of the given names, by configuring the device
 * driver name in a board-specific .conf file fragment. Examples are
 * in the board-level files in boards/.
 */

#ifndef FOTA_MQTT_TEMPERATURE_H__
#define FOTA_MQTT_TEMPERATURE_H__

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

#endif	/* FOTA_MQTT_TEMPERATURE_H__ */
