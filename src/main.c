/*
 * Copyright (c) 2016-2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "fota/main"
#define SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG
#include <logging/sys_log.h>

#include <misc/stack.h>
#include <gpio.h>
#include <sensor.h>
#include <tc_util.h>
#include <misc/reboot.h>
#include <board.h>

/* Local helpers and functions */
#include "tstamp_log.h"
#include "mcuboot.h"
#include "product_id.h"
#if defined(CONFIG_BLUETOOTH)
#include <bluetooth/conn.h>
#include "bt_storage.h"
#include "bt_ipss.h"
#endif
#if defined(CONFIG_FOTA_DM_BACKEND_HAWKBIT)
#include "hawkbit.h"
#endif
#if defined(CONFIG_FOTA_BLUEMIX)
#include "bluemix.h"
#endif
#if defined(CONFIG_NET_TCP)
#include "tcp.h"
#endif

/*
 * GPIOs. These can be customized by device if needed.
 */
#define LED_GPIO_PIN	LED0_GPIO_PIN
#define LED_GPIO_PORT	LED0_GPIO_PORT
#if defined(CONFIG_BOARD_96B_NITROGEN) || defined(CONFIG_BOARD_96B_CARBON)
#define BT_CONNECT_LED	BT_GPIO_PIN
#define GPIO_DRV_BT	BT_GPIO_PORT
#endif

#define FOTA_STACK_SIZE 3840
char fota_thread_stack[FOTA_STACK_SIZE];

#define MAX_SERVER_FAIL	5
int poll_sleep = K_SECONDS(30);
struct device *flash_dev;

#if defined(CONFIG_BLUETOOTH)
static bool bt_connection_state = false;

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
		bt_connection_state = true;
		set_bluetooth_led(1);
		err = ipss_set_connected();
		if (err) {
			SYS_LOG_ERR("BT LE connection name change failed: %u",
				    err);
		}
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
#endif

static int fota_init(void)
{
	int ret;

	TC_PRINT("Initializing FOTA backend\n");
#if defined(CONFIG_FOTA_DM_BACKEND_HAWKBIT)
	ret = hawkbit_init();
#else
	SYS_LOG_ERR("Unsupported device management backend");
	ret = -EINVAL;
#endif
	if (ret) {
		TC_END_RESULT(TC_FAIL);
	} else {
		TC_END_RESULT(TC_PASS);
	}

	return ret;
}

/* Firmware OTA thread (Hawkbit) */
static void fota_service(void)
{
#if defined(CONFIG_FOTA_DM_BACKEND_HAWKBIT)
	u32_t hawkbit_failures = 0;
	int ret;
#endif

	SYS_LOG_INF("Starting FOTA Service Thread");

	do {
		k_sleep(poll_sleep);
#if defined(CONFIG_BLUETOOTH)
		if (!bt_connection_state) {
			SYS_LOG_DBG("No BT LE connection");
			continue;
		}
#endif

		tcp_interface_lock();

#if defined(CONFIG_FOTA_DM_BACKEND_HAWKBIT)
		ret = hawkbit_ddi_poll();
		if (ret < 0) {
			hawkbit_failures++;
			if (hawkbit_failures == MAX_SERVER_FAIL) {
				SYS_LOG_ERR("Too many unsuccessful poll"
					    " attempts, rebooting!");
				sys_reboot(0);
			}
		} else {
			/* restart the failed attempt counter */
			hawkbit_failures = 0;
		}
#else
		SYS_LOG_ERR("Unsupported device management backend");
#endif /* CONFIG_FOTA_DM_BACKEND_HAWKBIT */

		tcp_interface_unlock();

		stack_analyze("FOTA Thread", fota_thread_stack, FOTA_STACK_SIZE);
	} while (1);
}

void blink_led(void)
{
	u32_t cnt = 0;
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
	int err;

	tstamp_hook_install();

	SYS_LOG_INF("Linaro FOTA example application");
	SYS_LOG_INF("Device: %s, Serial: %x",
		    product_id_get()->name, product_id_get()->number);

	TC_START("Running Built in Self Test (BIST)");

#if defined(CONFIG_BLUETOOTH)
	/* Storage used to provide a BT MAC based on the serial number */
	TC_PRINT("Setting Bluetooth MAC\n");
	bt_storage_init();

	TC_PRINT("Enabling Bluetooth\n");
	err = bt_enable(NULL);
	if (err) {
		SYS_LOG_ERR("Bluetooth init failed: %d", err);
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
		SYS_LOG_ERR("Advertising failed to start: %d", err);
		return;
	}
#endif

#if defined(CONFIG_NET_TCP)
	TC_PRINT("Initializing TCP\n");
	if (tcp_init()) {
		_TC_END_RESULT(TC_FAIL, "tcp_init");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	_TC_END_RESULT(TC_PASS, "tcp_init");
#endif

	err = fota_init();
	if (err) {
		TC_END_REPORT(TC_FAIL);
 		return;
	}

	TC_PRINT("Starting the FOTA Service\n");
	k_thread_spawn(&fota_thread_stack[0], FOTA_STACK_SIZE,
			(k_thread_entry_t) fota_service,
			NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

#if defined(CONFIG_FOTA_BLUEMIX)
	TC_PRINT("Initializing Bluemix Client service\n");
	if (bluemix_init()) {
		_TC_END_RESULT(TC_FAIL, "bluemix_init");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	_TC_END_RESULT(TC_PASS, "bluemix_init");
#endif

	TC_PRINT("Blinking LED\n");
	blink_led();
}
