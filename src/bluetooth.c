/*
 * Copyright (c) 2016-2017 Linaro Limited
 * Copyright (c) 2018 Open Source Foundries Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "fota/bluetooth"
#define SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG
#include <logging/sys_log.h>

#include <zephyr/types.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/storage.h>
#include <bluetooth/conn.h>
#include "bluetooth.h"

#include <init.h>
#include <misc/reboot.h>
#include <tc_util.h>
#include <board.h>
#include <gpio.h>
#include <soc.h>

#include "product_id.h"

#if defined(CONFIG_BOARD_96B_NITROGEN) || defined(CONFIG_BOARD_96B_CARBON)
#define BT_CONNECT_LED	BT_GPIO_PIN
#define GPIO_DRV_BT	BT_GPIO_PORT
#endif

/* Any by default, can change depending on the hardware implementation */
static bt_addr_le_t bt_addr;

static ssize_t storage_read(const bt_addr_le_t *addr, u16_t key, void *data,
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

static ssize_t storage_write(const bt_addr_le_t *addr, u16_t key,
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
	int i;
	u8_t tmp;

	/*
	 * Generate a static BT addr using the unique product number.
	 */
	for (i = 0; i < 4; i++) {
		tmp = (product_id_get()->number >> i * 8) & 0xff;
		bt_addr.a.val[i] = tmp;
	}

	bt_addr.a.val[4] = 0xe7;
/*
 * For CONFIG_NET_L2_BT_ZEP1656, the U/L bit of the BT MAC is toggled by Zephyr.
 * Later that MAC is used to create the 6LOWPAN IPv6 address.  To account for
 * that, we set 0xd6 here which toggles to 0xd4 later (matching the Linux
 * gateway configuration for bt0).
 */
#if defined(CONFIG_NET_L2_BT_ZEP1656)
	bt_addr.a.val[5] = 0xd6;
#else
	bt_addr.a.val[5] = 0xd4;
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

	SYS_LOG_DBG("Bluetooth storage driver registered");

	return 0;
}

/* BT LE Connect/Disconnect callbacks */
static void set_bluetooth_led(bool state)
{
#if defined(GPIO_DRV_BT) && defined(BT_CONNECT_LED)
	struct device *gpio;

	gpio = device_get_binding(GPIO_DRV_BT);
	gpio_pin_configure(gpio, BT_CONNECT_LED, GPIO_DIR_OUT);
	gpio_pin_write(gpio, BT_CONNECT_LED, state);
#endif
}

static void connected(struct bt_conn *conn, u8_t err)
{
	if (err) {
		SYS_LOG_ERR("BT LE Connection failed: %u", err);
	} else {
		SYS_LOG_INF("BT LE Connected");
		set_bluetooth_led(1);
	}
}

static void disconnected(struct bt_conn *conn, u8_t reason)
{
	SYS_LOG_ERR("BT LE Disconnected (reason %u), rebooting!", reason);
	set_bluetooth_led(0);
	sys_reboot(0);
}

static struct bt_conn_cb conn_callbacks = {
	.connected = connected,
	.disconnected = disconnected,
};

static int network_init(struct device *dev)
{
	/* Storage used to provide a BT MAC based on the serial number */
	TC_PRINT("Setting Bluetooth MAC\n");
	bt_storage_init();
	bt_conn_cb_register(&conn_callbacks);
	return 0;
}

/* last priority in the POST_KERNEL init levels */
SYS_INIT(network_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
