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
uint32_t boot_acid_read(boot_acid_t type);
void boot_acid_update(boot_acid_t type, uint32_t acid);
int boot_erase_flash_bank(uint32_t bank_offset);
