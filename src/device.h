/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <board.h>

/* Flash specific configs */
#if defined(CONFIG_SOC_SERIES_NRF52X)
#define FLASH_DRIVER_NAME	"NRF5_FLASH"
#define FLASH_BANK0_OFFSET	0x00008000
#define FLASH_BANK1_OFFSET	0x00042000
#define FLASH_BANK_SIZE		(232 * 1024)
#define FLASH_STATE_OFFSET	0x0007d000
#define FLASH_STATE_SIZE	(12 * 1024)
#define FLASH_MIN_WRITE_SIZE	4
#elif defined(CONFIG_SOC_SERIES_STM32F4X)
#define FLASH_DRIVER_NAME	"STM32F4_FLASH"
#define FLASH_BANK0_OFFSET	0x00020000
#define FLASH_BANK1_OFFSET	0x00040000
#define FLASH_BANK_SIZE		(128 * 1024)
#define FLASH_STATE_OFFSET	0x0000c000
#define FLASH_STATE_SIZE	(16 * 1024)
#define FLASH_MIN_WRITE_SIZE	1
#elif defined(CONFIG_SOC_SERIES_KINETIS_K6X)
#define FLASH_DRIVER_NAME	"MCUX_FLASH"
#define FLASH_BANK0_OFFSET	0x00020000
#define FLASH_BANK1_OFFSET	0x00040000
#define FLASH_BANK_SIZE		(128 * 1024)
#define FLASH_STATE_OFFSET	0x00010000
#define FLASH_STATE_SIZE	(32 * 1024)
#define FLASH_MIN_WRITE_SIZE	8
#endif

/* GPIO */
/* This can be customized by device if needed */
#define LED_GPIO_PIN	LED0_GPIO_PIN
#define LED_GPIO_PORT	LED0_GPIO_PORT
#if defined(CONFIG_BOARD_96B_NITROGEN) || defined(CONFIG_BOARD_96B_CARBON)
#define BT_CONNECT_LED	BT_GPIO_PIN
#define GPIO_DRV_BT	BT_GPIO_PORT
#endif

/* Bluetooth */
#define DEVICE_NAME "Linaro IPSP node"
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)
#define UNKNOWN_APPEARANCE 0x0000

/* General hardware specific configs */
#if defined(CONFIG_SOC_SERIES_STM32F4X)
#define DEVICE_ID_BASE	0x1fff7a10
#endif

struct product_id_t {
	const char *name;
	uint32_t number;
};

extern struct product_id_t product_id;
extern struct device *flash_dev;

void set_device_id(void);
void set_bluetooth_led(bool state);
