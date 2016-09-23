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

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <misc/printk.h>
#include <zephyr.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/storage.h>

#include <soc.h>
#include <tc_util.h>

/* Any by default, can change depending on the hardware implementation */
static bt_addr_le_t bt_addr;

static ssize_t storage_read(const bt_addr_le_t *addr, uint16_t key, void *data,
			       size_t length)
{
	if (addr) {
		return -ENOENT;
	}

	if (key == BT_STORAGE_ID_ADDR && length == sizeof(bt_addr)) {
		bt_addr_le_copy(data, &bt_addr);
		return sizeof(bt_addr);
	}

	return -EIO;
}

static ssize_t storage_write(const bt_addr_le_t *addr, uint16_t key,
				const void *data, size_t length)
{
	return -ENOSYS;
}

static ssize_t storage_clear(const bt_addr_le_t *addr)
{
	return -ENOSYS;
}

static void set_own_bt_addr(void)
{
#ifdef CONFIG_SOC_FAMILY_NRF5
	int i;
	uint8_t tmp;

	/* There is no public address by default on nRF5, so generate a
	 * random address based on the device address (unique).
	 */
	for (i = 0; i < 4; i++) {
		tmp = (NRF_FICR->DEVICEADDR[0] >> i * 8) & 0xff;
		bt_addr.a.val[i] = tmp;
	}
	bt_addr.a.val[4] = 0xe7;
	bt_addr.a.val[5] = 0xd6;
#elif CONFIG_SOC_SERIES_STM32F4X
	int i;

	/* There is no public address by default on nRF51, so generate a
	 * random address based on the STM32 device id (unique).
	 */
	for (i = 0; i < 4; i++) {
		bt_addr.a.val[i] = *((uint8_t *) 0x1fff7a10 + (i << 1));
	}
	bt_addr.a.val[4] = 0xe7;
	bt_addr.a.val[5] = 0xd6;
#else
	/* Set a random address as default */
	bt_addr_copy(&bt_addr.a, BT_ADDR_ANY);
#endif
}

int bt_storage_init(void)
{
	static const struct bt_storage storage = {
		.read = storage_read,
		.write = storage_write,
		.clear = storage_clear,
	};

	bt_addr.type = BT_ADDR_LE_RANDOM;

	set_own_bt_addr();

	bt_storage_register(&storage);

	printk("Bluetooth storage driver registered\n");

	TC_END_RESULT(TC_PASS);

	return 0;
}
