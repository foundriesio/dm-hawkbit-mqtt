/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __FOTA_BOOT_UTILS_H__
#define __FOTA_BOOT_UTILS_H__

typedef enum {
	BOOT_ACID_CURRENT = 0,
	BOOT_ACID_UPDATE,
} boot_acid_t;

struct boot_acid {
	uint32_t current;
	uint32_t update;
};

typedef enum {
	BOOT_STATUS_DONE    = 0x01,
	BOOT_STATUS_ONGOING = 0xff,
} boot_status_t;

boot_status_t boot_status_read(void);
void boot_status_update(void);
void boot_trigger_ota(void);

void boot_acid_read(struct boot_acid *boot_acid);
int boot_acid_update(boot_acid_t type, uint32_t acid);

int boot_erase_flash_bank(uint32_t bank_offset);

#endif	/* __FOTA_BOOT_UTILS_H__ */
