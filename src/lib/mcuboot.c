/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "fota/mcuboot"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_FOTA_LEVEL
#include <logging/sys_log.h>

#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <flash.h>
#include <zephyr.h>
#include <init.h>

#include "mcuboot.h"
#include "product_id.h"

/*
 * Helpers for image trailer, as defined by mcuboot.
 */

#define TRAILER_IMAGE_MAGIC_SIZE	(4 * sizeof(uint32_t))
#define TRAILER_SWAP_STATUS_SIZE	(128 * FLASH_MIN_WRITE_SIZE * 3)
#define TRAILER_COPY_DONE_SIZE		FLASH_MIN_WRITE_SIZE
#define TRAILER_IMAGE_OK_SIZE		FLASH_MIN_WRITE_SIZE

#define TRAILER_SIZE			(TRAILER_IMAGE_MAGIC_SIZE +	\
					 TRAILER_SWAP_STATUS_SIZE +	\
					 TRAILER_COPY_DONE_SIZE +	\
					 TRAILER_IMAGE_OK_SIZE)

#define TRAILER_COPY_DONE		0x01
#define TRAILER_PADDING			0xff

static uint32_t boot_trailer(uint32_t bank_offset)
{
	return bank_offset + FLASH_BANK_SIZE - TRAILER_SIZE;
}

static uint32_t boot_trailer_magic(uint32_t bank_offset)
{
	return boot_trailer(bank_offset);
}

static uint32_t boot_trailer_swap_status(uint32_t bank_offset)
{
	return boot_trailer_magic(bank_offset) +
		TRAILER_IMAGE_MAGIC_SIZE;
}

static uint32_t boot_trailer_copy_done(uint32_t bank_offset)
{
	return boot_trailer_swap_status(bank_offset) +
		TRAILER_SWAP_STATUS_SIZE;
}

static uint32_t boot_trailer_image_ok(uint32_t bank_offset)
{
	return boot_trailer_copy_done(bank_offset) +
		TRAILER_COPY_DONE_SIZE;
}

uint8_t boot_status_read(void)
{
	uint32_t offset;
	uint8_t img_ok = 0;

	offset = boot_trailer_image_ok(FLASH_BANK0_OFFSET);
	flash_read(flash_dev, offset, &img_ok, sizeof(uint8_t));

	return img_ok;
}

void boot_status_update(void)
{
	uint32_t offset;
	/*
	 * The first byte of the Image OK area contains payload. The
	 * rest is padded with 0xff for flash write alignment.
	 */
	uint8_t img_ok;
	uint8_t update_buf[TRAILER_IMAGE_OK_SIZE];

	offset = boot_trailer_image_ok(FLASH_BANK0_OFFSET);
	flash_read(flash_dev, offset, &img_ok, sizeof(uint8_t));
	if (img_ok == BOOT_STATUS_ONGOING) {
		memset(update_buf, TRAILER_PADDING, sizeof(update_buf));
		update_buf[0] = BOOT_STATUS_DONE;

		flash_write_protection_set(flash_dev, false);
		flash_write(flash_dev, offset, update_buf, sizeof(update_buf));
		flash_write_protection_set(flash_dev, true);
	}
}

void boot_trigger_ota(void)
{
	uint32_t copy_done_offset, image_ok_offset;
	uint8_t copy_done[TRAILER_COPY_DONE_SIZE];
	uint8_t image_ok[TRAILER_IMAGE_OK_SIZE];

	copy_done_offset = boot_trailer_copy_done(FLASH_BANK1_OFFSET);
	image_ok_offset = boot_trailer_image_ok(FLASH_BANK1_OFFSET);
	memset(copy_done, TRAILER_PADDING, sizeof(copy_done));
	memset(image_ok, TRAILER_PADDING, sizeof(image_ok));

	flash_write_protection_set(flash_dev, false);
	flash_write(flash_dev, copy_done_offset, copy_done, sizeof(copy_done));
	flash_write_protection_set(flash_dev, true);

	flash_write_protection_set(flash_dev, false);
	flash_write(flash_dev, image_ok_offset, image_ok, sizeof(image_ok));
	flash_write_protection_set(flash_dev, true);
}

int boot_erase_flash_bank(uint32_t bank_offset)
{
	int ret;

	flash_write_protection_set(flash_dev, false);
	ret = flash_erase(flash_dev, bank_offset, FLASH_BANK_SIZE);
	flash_write_protection_set(flash_dev, true);

	return ret;
}

static int boot_init(struct device *dev)
{
	ARG_UNUSED(dev);
	flash_dev = device_get_binding(FLASH_DRIVER_NAME);
	if (!flash_dev) {
		SYS_LOG_ERR("Failed to find the flash driver");
		return -ENODEV;
	}
	return 0;
}

SYS_INIT(boot_init, APPLICATION, 99);
