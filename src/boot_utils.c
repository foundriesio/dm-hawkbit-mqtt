/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <flash.h>
#include <zephyr.h>

#include "ota_debug.h"
#include "boot_utils.h"
#include "device.h"

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
	OTA_INFO("Current boot status %x\n", img_ok);

	return img_ok;
}

void boot_status_update(void)
{
	uint32_t offset;
	/* The first byte of the Image OK area contains payload. The
	 * rest is padded with 0xff for flash write alignment. */
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
		OTA_INFO("Updated boot status to %d\n", update_buf[0]);
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

	OTA_INFO("Clearing bank 1 image_ok and copy_done\n");

	flash_write_protection_set(flash_dev, false);
	flash_write(flash_dev, copy_done_offset, copy_done, sizeof(copy_done));
	flash_write_protection_set(flash_dev, true);

	flash_write_protection_set(flash_dev, false);
	flash_write(flash_dev, image_ok_offset, image_ok, sizeof(image_ok));
	flash_write_protection_set(flash_dev, true);
}

uint32_t boot_acid_read(boot_acid_t type)
{
	struct boot_acid boot_acid;

	flash_read(flash_dev, FLASH_STATE_OFFSET, &boot_acid,
					sizeof(boot_acid));
	OTA_INFO("ACID: current %d, update %d\n", boot_acid.current,
					boot_acid.update);
	if (type == BOOT_ACID_UPDATE) {
		return boot_acid.update;
	} else {
		return boot_acid.current;
	}
}

void boot_acid_update(boot_acid_t type, uint32_t acid)
{
	struct boot_acid boot_acid;
	int ret;

	flash_read(flash_dev, FLASH_STATE_OFFSET, &boot_acid,
					sizeof(boot_acid));
	if (type == BOOT_ACID_UPDATE) {
		boot_acid.update = acid;
	} else {
		boot_acid.current = acid;
	}

	flash_write_protection_set(flash_dev, false);
	ret = flash_erase(flash_dev, FLASH_STATE_OFFSET, FLASH_STATE_SIZE);
	flash_write_protection_set(flash_dev, true);
	if (!ret) {
		flash_write_protection_set(flash_dev, false);
		ret = flash_write(flash_dev, FLASH_STATE_OFFSET, &boot_acid,
				  sizeof(boot_acid));
		flash_write_protection_set(flash_dev, true);
		if (!ret) {
			OTA_INFO("ACID updated, current %d, update %d\n",
				 boot_acid.current, boot_acid.update);
		} else {
			OTA_ERR("flash_write error %d\n", ret);
		}
	} else {
		OTA_ERR("flash_erase error %d\n", ret);
	}
}

int boot_erase_flash_bank(uint32_t bank_offset)
{
	int ret;

	flash_write_protection_set(flash_dev, false);
	ret = flash_erase(flash_dev, bank_offset, FLASH_BANK_SIZE);
	flash_write_protection_set(flash_dev, true);
	if (!ret) {
		OTA_DBG("Flash bank (offset %x) erased successfully\n",
					bank_offset);
	} else {
		OTA_ERR("flash_erase error %d\n", ret);
	}

	return ret;
}
