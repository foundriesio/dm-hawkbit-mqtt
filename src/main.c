/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <bluetooth/conn.h>
#include <misc/stack.h>
#include <gpio.h>
#include <tc_util.h>

/* Local helpers and functions */
#include "bt_storage.h"
#include "bt_ipss.h"
#include "ota_debug.h"
#include "boot_utils.h"
#include "hawkbit.h"
#include "device.h"
#include "tcp.h"

#define STACKSIZE 3840
char threadStack[STACKSIZE];

#define MAX_POLL_FAIL	5
int poll_sleep = K_SECONDS(30);
struct device *flash_dev;

#if defined(CONFIG_BLUETOOTH)
static bool bt_connection_state = false;

/* BT LE Connect/Disconnect callbacks */
static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("BT LE Connection failed (err %u)\n", err);
	} else {
		printk("BT LE Connected\n");
		bt_connection_state = true;
		set_bluetooth_led(1);
		err = ipss_set_connected();
		if (err) {
			printk("BT LE connection name change"
			       " failed (err %u)\n", err);
		}
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("BT LE Disconnected (reason %u), rebooting!\n", reason);
	set_bluetooth_led(0);
	sys_arch_reboot(0);
}

static struct bt_conn_cb conn_callbacks = {
	.connected = connected,
	.disconnected = disconnected,
};
#endif

static int fota_service_update_acid(struct boot_acid *acid)
{
	int ret = 0;
	if (acid->update != -1) {
		ret = boot_acid_update(BOOT_ACID_CURRENT, acid->update);
		if (!ret) {
			boot_acid_read(acid);
			OTA_INFO("ACID updated, current %d, update %d\n",
				 acid->current, acid->update);
		} else {
			OTA_ERR("Failed to update ACID: %d\n", ret);
		}
	}
	return ret;
}

/* Firmware OTA thread (Hawkbit) */
static void fota_service(void)
{
	uint32_t failed_poll = 0;
	struct boot_acid acid;
	uint8_t boot_status;
	int ret;

	OTA_INFO("Starting FOTA Service Thread\n");

	flash_dev = device_get_binding(FLASH_DRIVER_NAME);
	if (!flash_dev) {
		OTA_ERR("Failed to find the flash driver\n");
		TC_END_RESULT(TC_FAIL);
		TC_END_REPORT(TC_FAIL);
		return;
	}

	/* Update boot status and acid */
	boot_acid_read(&acid);
	OTA_INFO("ACID: current %d, update %d\n",
		 acid.current, acid.update);
	boot_status = boot_status_read();
	OTA_INFO("Current boot status %x\n", boot_status);
	if (boot_status == BOOT_STATUS_ONGOING) {
		boot_status_update();
		OTA_INFO("Updated boot status to %x\n", boot_status_read());
		ret = boot_erase_flash_bank(FLASH_BANK1_OFFSET);
		if (ret) {
			OTA_ERR("flash_erase error %d\n", ret);
			TC_END_RESULT(TC_FAIL);
			TC_END_REPORT(TC_FAIL);
			return;
		} else {
			OTA_DBG("Flash bank (offset %x) erased successfully\n",
				FLASH_BANK1_OFFSET);
		}
		ret = fota_service_update_acid(&acid);
		if (ret) {
			TC_END_RESULT(TC_FAIL);
			TC_END_REPORT(TC_FAIL);
			return;
		}
	}

	TC_END_RESULT(TC_PASS);

	do {
		k_sleep(poll_sleep);
#if defined(CONFIG_BLUETOOTH)
		if (!bt_connection_state) {
			OTA_DBG("No BT LE connection\n");
			continue;
		}
#endif

		ret = hawkbit_ddi_poll();
		if (ret < 0) {
			failed_poll++;
			OTA_DBG("Failed poll attempt %d\n\n\n", failed_poll);
			if (failed_poll == MAX_POLL_FAIL) {
				printk("Too many unsuccessful poll attempts,"
						" rebooting!\n");
				sys_arch_reboot(0);
			}
		} else {
			/* restart the failed attempt counter */
			failed_poll = 0;
		}

		stack_analyze("FOTA Thread", threadStack, STACKSIZE);
	} while (1);
}

void blink_led(void)
{
	uint32_t cnt = 0;
	struct device *gpio;

	gpio = device_get_binding(LED_GPIO_PORT);
	gpio_pin_configure(gpio, LED_GPIO_PIN, GPIO_DIR_OUT);

	while (1) {
		gpio_pin_write(gpio, LED_GPIO_PIN, cnt % 2);
		k_sleep(K_SECONDS(1));
                if (cnt == 1) {
                        TC_END_RESULT(TC_PASS);
                        TC_END_REPORT(TC_PASS);
                }
		cnt++;
	}
}

void main(void)
{

	set_device_id();

	printk("Linaro FOTA example application\n");
	printk("Device: %s, Serial: %x\n", product_id.name, product_id.number);

	TC_START("Running Built in Self Test (BIST)");

#if defined(CONFIG_BLUETOOTH)
	int err;

	/* Storage used to provide a BT MAC based on the serial number */
	TC_PRINT("Setting Bluetooth MAC\n");
	bt_storage_init();

	TC_PRINT("Enabling Bluetooth\n");
	err = bt_enable(NULL);
	if (err) {
		printk("ERROR: Bluetooth init failed (err %d)\n", err);
		TC_END_RESULT(TC_FAIL);
		TC_END_REPORT(TC_FAIL);
		return;
	}
	else {
		TC_END_RESULT(TC_PASS);
	}

	/* Callbacks for BT LE connection state */
	TC_PRINT("Registering Bluetooth LE connection callbacks\n");
	ipss_init(&conn_callbacks);

	TC_PRINT("Advertising Bluetooth IP Profile\n");
	err = ipss_advertise();
	if (err) {
		printk("ERROR: Advertising failed to start (err %d)\n", err);
		return;
	}
#endif

	TC_PRINT("Starting the FOTA Service\n");
	k_thread_spawn(&threadStack[0], STACKSIZE,
			(k_thread_entry_t) fota_service,
			NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

	TC_PRINT("Blinking LED\n");
	blink_led();
}
