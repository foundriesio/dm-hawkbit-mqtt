/*
 * Copyright (c) 2018 Open Source Foundries Limited
 * Copyright (c) 2018 Foundries.io
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FOTA_BLINK_LED_H__
#define FOTA_BLINK_LED_H__

/**
 * @file
 * @brief Work queue-based LED heartbeat support.
 */

#include <zephyr.h>

/**
 * @brief Start blinking an LED on the application work queue.
 */
int blink_led_start(void);

#endif /* FOTA_BLINK_LED_H__ */
