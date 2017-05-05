/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __FOTA_MCUBOOT_H__
#define __FOTA_MCUBOOT_H__

/* Flash specific configs */
#if defined(CONFIG_SOC_SERIES_NRF52X)
#define FLASH_DRIVER_NAME	CONFIG_SOC_FLASH_NRF5_DEV_NAME
#if defined(CONFIG_BOARD_96B_NITROGEN)
#define FLASH_BANK0_OFFSET	0x00008000
#define FLASH_BANK1_OFFSET	0x0003C000
#define FLASH_BANK_SIZE		(208 * 1024)
#define FLASH_STATE_OFFSET	0x0007d000
#elif defined(CONFIG_BOARD_NRF52840_PCA10056)
#define FLASH_BANK0_OFFSET	0x00008000
#define FLASH_BANK1_OFFSET	0x00074000
#define FLASH_BANK_SIZE		(432 * 1024)
#define FLASH_STATE_OFFSET	0x000FD000
#else
#error Unknown NRF52X board
#endif	/* CONFIG_BOARD_96B_NITROGEN */
#define FLASH_STATE_SIZE	(12 * 1024)
#define FLASH_MIN_WRITE_SIZE	4
#elif defined(CONFIG_SOC_SERIES_STM32F4X)
/* TEMP: maintain compatibility with old STM flash driver */
#ifndef CONFIG_SOC_FLASH_STM32_DEV_NAME
#define FLASH_DRIVER_NAME	CONFIG_SOC_FLASH_STM32F4_DEV_NAME
#else
#define FLASH_DRIVER_NAME	CONFIG_SOC_FLASH_STM32_DEV_NAME
#endif
#define FLASH_BANK0_OFFSET	0x00020000
#define FLASH_BANK1_OFFSET	0x00040000
#define FLASH_BANK_SIZE		(128 * 1024)
#define FLASH_STATE_OFFSET	0x0000c000
#define FLASH_STATE_SIZE	(16 * 1024)
#define FLASH_MIN_WRITE_SIZE	1
#elif defined(CONFIG_SOC_SERIES_KINETIS_K6X)
#define FLASH_DRIVER_NAME	CONFIG_SOC_FLASH_MCUX_DEV_NAME
#define FLASH_BANK0_OFFSET	0x00020000
#define FLASH_BANK1_OFFSET	0x00080000
#define FLASH_BANK_SIZE	(384 * 1024)
#define FLASH_STATE_OFFSET	0x00010000
#define FLASH_STATE_SIZE	(32 * 1024)
#define FLASH_MIN_WRITE_SIZE	8
#endif

typedef enum {
	BOOT_STATUS_DONE    = 0x01,
	BOOT_STATUS_ONGOING = 0xff,
} boot_status_t;

extern struct device *flash_dev;

boot_status_t boot_status_read(void);
void boot_status_update(void);
void boot_trigger_ota(void);

int boot_erase_flash_bank(u32_t bank_offset);

#endif	/* __FOTA_MCUBOOT_H__ */
