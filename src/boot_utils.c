/*
 * Copyright (c) 2016 Linaro Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stddef.h>
#include <errno.h>
#include <flash.h>
#include <zephyr.h>

#include "ota_debug.h"
#include "boot_utils.h"
#include "device.h"

uint8_t boot_status_read(void)
{
	uint32_t offset;
	uint8_t img_ok = 0;

	offset = FLASH_BANK0_OFFSET + FLASH_BANK_SIZE -
				sizeof(struct boot_img_trailer);
	offset += (sizeof(uint32_t) + sizeof(uint8_t));
	flash_read(flash_dev, offset, &img_ok, sizeof(uint8_t));
	OTA_INFO("Current boot status %x\n", img_ok);

	return img_ok;
}

void boot_status_update(void)
{
	uint32_t offset;
	uint8_t img_ok = 0;

	offset = FLASH_BANK0_OFFSET + FLASH_BANK_SIZE -
				sizeof(struct boot_img_trailer);
	offset += (sizeof(uint32_t) + sizeof(uint8_t));
	flash_read(flash_dev, offset, &img_ok, sizeof(uint8_t));
	if (img_ok == 0xff) {
		img_ok = 0;
		flash_write_protection_set(flash_dev, false);
		flash_write(flash_dev, offset, &img_ok, sizeof(uint8_t));
		flash_write_protection_set(flash_dev, true);
		OTA_INFO("Updated boot status to %d\n", img_ok);
	}
}

void boot_trigger_ota(void)
{
	uint32_t offset;
	struct boot_img_trailer img_trailer;

	img_trailer.bit_copy_start = BOOT_IMG_MAGIC;
	img_trailer.bit_copy_done = 0xff;
	img_trailer.bit_img_ok = 0xff;

	offset = FLASH_BANK1_OFFSET + FLASH_BANK_SIZE -
				sizeof(struct boot_img_trailer);

	OTA_INFO("Setting trailer magic number and copy_done\n");
	flash_write_protection_set(flash_dev, false);
	flash_write(flash_dev, offset, &img_trailer,
				sizeof(struct boot_img_trailer));
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

	flash_read(flash_dev, FLASH_STATE_OFFSET, &boot_acid,
					sizeof(boot_acid));
	if (type == BOOT_ACID_UPDATE) {
		boot_acid.update = acid;
	} else {
		boot_acid.current = acid;
	}
	flash_erase(flash_dev, FLASH_STATE_OFFSET, FLASH_STATE_SIZE);
	flash_write_protection_set(flash_dev, false);
	flash_write(flash_dev, FLASH_STATE_OFFSET, &boot_acid,
					sizeof(boot_acid));
	flash_write_protection_set(flash_dev, true);
	OTA_INFO("ACID updated, current %d, update %d\n",
			boot_acid.current, boot_acid.update);
}
