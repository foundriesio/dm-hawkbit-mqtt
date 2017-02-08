/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr.h>
#include <soc.h>
#include <gpio.h>
#include "device.h"

struct product_id_t product_id = {
	.name = CONFIG_BOARD,
};

#define HASH_MULTIPLIER		37
static uint32_t hash32(char *str, int len)
{
	uint32_t h = 0;
	int i;

	for (i = 0; i < len; ++i) {
		h = (h * HASH_MULTIPLIER) + str[i];
	}

	return h;
}

/* Find and set common unique device specific information */
void set_device_id(void)
{
	int i;
	char buffer[DEVICE_ID_LENGTH*8 + 1];

	for (i = 0; i < DEVICE_ID_LENGTH; i++) {
		sprintf(buffer + i*8, "%08x",
			*(((uint32_t *)DEVICE_ID_BASE) + i));
	}

	product_id.number = hash32(buffer, DEVICE_ID_LENGTH*8);
}

void set_bluetooth_led(bool state)
{
#if defined(GPIO_DRV_BT) && defined(BT_CONNECT_LED)
	struct device *gpio;

	gpio = device_get_binding(GPIO_DRV_BT);
	gpio_pin_configure(gpio, BT_CONNECT_LED, GPIO_DIR_OUT);
	gpio_pin_write(gpio, BT_CONNECT_LED, state);
#endif
}
