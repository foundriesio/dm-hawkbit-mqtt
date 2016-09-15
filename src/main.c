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
#include <string.h>
#include <errno.h>
#include <misc/printk.h>
#include <misc/byteorder.h>
#include <misc/nano_work.h>
#include <gpio.h>
#include <flash.h>
#include <zephyr.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>

#include <net/ip_buf.h>
#include <net/net_core.h>
#include <net/net_socket.h>

#include <soc.h>

/* Local helpers and functions */
#include "bt_storage.h"
#include "bt_ipss.h"
#include "ota_debug.h"
#include "boot_utils.h"
#include "hawkbit.h"
#include "device.h"

/* Global address to be set from RA */
#define MY_IPADDR	IN6ADDR_ANY_INIT

#define STACKSIZE 2560
char fiberStack[STACKSIZE];

int poll_sleep = 15 * sys_clock_ticks_per_sec;
struct device *flash_dev;

static bool bt_connection_state = false;

/* BT LE Connect/Disconnect callbacks */
static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("BT LE Connection failed (err %u)\n", err);
	} else {
		printk("BT LE Connected\n");
		bt_connection_state = true;
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("BT LE Disconnected (reason %u)\n", reason);
	bt_connection_state = false;
}

static struct bt_conn_cb conn_callbacks = {
	.connected = connected,
	.disconnected = disconnected,
};

/* Firmware OTA fiber (Hawkbit) */
static void fota_service(void)
{
	static struct net_context *context;
	static struct net_addr peer_addr, my_addr;
	static struct in6_addr in6addr_my = MY_IPADDR;
	static struct in6_addr in6addr_peer = HAWKBIT_IPADDR;
	uint32_t acid;

	peer_addr.in6_addr = in6addr_peer;
	peer_addr.family = AF_INET6;

	my_addr.in6_addr = in6addr_my;
	my_addr.family = AF_INET6;

	OTA_INFO("Starting FOTA Service Fiber\n");

	context = net_context_get(IPPROTO_TCP,
				    &peer_addr, HAWKBIT_PORT,
				    &my_addr, 0);
	if (!context) {
		OTA_ERR("Failed to get network context\n");
		return;
	}

	flash_dev = device_get_binding(FLASH_DRIVER_NAME);
	if (!flash_dev) {
		OTA_ERR("Failed to find the flash driver\n");
		return;
	}

	/* Update boot status and acid */
	acid = boot_acid_read(BOOT_ACID_UPDATE);
	if (boot_status_read() == 0xff) {
		if (acid != -1) {
			boot_acid_update(BOOT_ACID_CURRENT, acid);
		}
		boot_status_update();
	}

	do {
		fiber_sleep(poll_sleep);
		if (!bt_connection_state) {
			OTA_DBG("No BT LE connection\n");
			continue;
		}

		hawkbit_ddi_poll(context);

		net_analyze_stack("FOTA Fiber", fiberStack, STACKSIZE);
	} while (1);
}

#define LED_BLINK	LED1_GPIO

void blink_led(void)
{
	uint32_t cnt = 0;
	struct device *gpio;

	gpio = device_get_binding(GPIO_DRV_NAME);
	gpio_pin_configure(gpio, LED_BLINK, GPIO_DIR_OUT);

	while (1) {
		gpio_pin_write(gpio, LED_BLINK, cnt % 2);
		task_sleep(SECONDS(1));
		cnt++;
	}
}

void main(void)
{
	int err;

	set_device_id();

	printk("Linaro FOTA example application\n");
	printk("Device: %s, Serial: %x\n", product_id.name, product_id.number);

	/* Storage used to provide a BT MAC based on the serial number */
	bt_storage_init();

	err = bt_enable(NULL);
	if (err) {
		printk("ERROR: Bluetooth init failed (err %d)\n", err);
		return;
	}

	net_init();

	/* Callbacks for BT LE connection state */
	ipss_init(&conn_callbacks);

	err = ipss_advertise();
	if (err) {
		printk("ERROR: Advertising failed to start (err %d)\n", err);
		return;
	}
	printk("Advertising started successfully\n");

	task_fiber_start(&fiberStack[0], STACKSIZE,
			(nano_fiber_entry_t) fota_service, 0, 0, 7, 0);

	blink_led();
}
