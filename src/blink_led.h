/*
 * Copyright (c) 2018 Open Source Foundries Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __FOTA_BLINK_LED_H__
#define __FOTA_BLINK_LED_H__

/**
 * @file
 * @brief Work queue-based LED heartbeat support.
 */

#include <zephyr.h>

/**
 * @brief Start blinking an LED on the application work queue.
 */
int blink_led_start(void);

#endif /* __FOTA_BLINK_LED_H__ */
