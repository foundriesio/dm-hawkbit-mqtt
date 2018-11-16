/*
 * Copyright (c) 2018 Open Source Foundries Limited
 * Copyright (c) 2018 Foundries.io
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "blink_led.h"

#include <device.h>
#include <gpio.h>

#include "app_work_queue.h"

/*
 * GPIOs. These can be customized by device if needed.
 */
#define LED_GPIO_PIN	LED0_GPIO_PIN
#if defined(LED0_GPIO_PORT)
#define LED_GPIO_PORT	LED0_GPIO_PORT
#else
#define LED_GPIO_PORT	LED0_GPIO_CONTROLLER
#endif

#define BLINK_DELAY     K_SECONDS(1)

static struct k_delayed_work blink_work;
static struct device *blink_gpio;
static bool blink_enable = true;

static void blink_handler(struct k_work *work)
{
	gpio_pin_write(blink_gpio, LED_GPIO_PIN, blink_enable);
	blink_enable = !blink_enable;
	app_wq_submit_delayed(&blink_work, BLINK_DELAY);
}

int blink_led_start(void)
{
	k_delayed_work_init(&blink_work, blink_handler);

	blink_gpio = device_get_binding(LED_GPIO_PORT);
	if (blink_gpio == NULL) {
		return -ENODEV;
	}

	gpio_pin_configure(blink_gpio, LED_GPIO_PIN, GPIO_DIR_OUT);
	gpio_pin_write(blink_gpio, LED_GPIO_PIN, blink_enable);
	app_wq_submit_delayed(&blink_work, BLINK_DELAY);

	return 0;
}
