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
#include "bluemix.h"
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

#define BLUEMIX_STACK_SIZE 1024
char bluemix_thread_stack[BLUEMIX_STACK_SIZE];

#define MAX_SERVER_FAIL	5
int poll_sleep = K_SECONDS(30);
struct device *flash_dev;

#define GENERIC_MCU_TEMP_SENSOR_DEVICE	"fota-mcu-temp"
#define GENERIC_OFFCHIP_TEMP_SENSOR_DEVICE "fota-offchip-temp"
struct device *mcu_temp_sensor_dev;
struct device *offchip_temp_sensor_dev;
int bluemix_sleep = K_SECONDS(3);

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

static int temp_init(void)
{
	mcu_temp_sensor_dev =
		device_get_binding(GENERIC_MCU_TEMP_SENSOR_DEVICE);
	offchip_temp_sensor_dev =
		device_get_binding(GENERIC_OFFCHIP_TEMP_SENSOR_DEVICE);

	SYS_LOG_INF("%s MCU temperature sensor %s%s",
		 mcu_temp_sensor_dev ? "Found" : "Did not find",
		 GENERIC_MCU_TEMP_SENSOR_DEVICE,
		 mcu_temp_sensor_dev ? "" : "\n(Using default values)");
	SYS_LOG_INF("%s off-chip temperature sensor %s",
		 offchip_temp_sensor_dev ? "Found" : "Did not find",
		 GENERIC_OFFCHIP_TEMP_SENSOR_DEVICE);
	return 0;
}

static int get_temp_sensor_data(struct device *temp_dev,
				struct sensor_value *temp_value,
				bool use_defaults_on_null)
{
	int ret = 0;

	if (!temp_dev) {
		if (use_defaults_on_null) {
			temp_value->val1 = 23;
			temp_value->val2 = 0;
			return 0;
		} else {
			return -ENODEV;
		}
	}

	ret = sensor_sample_fetch(temp_dev);
	if (ret) {
		return ret;
	}

	return sensor_channel_get(temp_dev, SENSOR_CHAN_TEMP, temp_value);
}

static void bluemix_service(void)
{
	static struct bluemix_ctx bluemix_context;
	static int bluemix_inited = 0;
	u32_t bluemix_failures = 0;
	struct sensor_value mcu_temp_value;
	struct sensor_value offchip_temp_value;
	int ret;

	while (bluemix_failures < MAX_SERVER_FAIL) {
		k_sleep(bluemix_sleep);
#if defined(CONFIG_BLUETOOTH)
		if (!bt_connection_state) {
			SYS_LOG_DBG("No BT LE connection");
			continue;
		}
#endif

		tcp_interface_lock();

		if (!bluemix_inited) {
			ret = bluemix_init(&bluemix_context);
			if (!ret) {
				/* restart the failed attempt counter */
				bluemix_failures = 0;
				bluemix_inited = 1;
			} else {
				bluemix_failures++;
				SYS_LOG_DBG("Failed Bluemix init -"
					    " attempt %d\n\n",
					    bluemix_failures);
				tcp_interface_unlock();
				continue;
			}
		}

		/*
		 * Fetch temperature sensor values. If we don't have
		 * an MCU temperature sensor or encounter errors
		 * reading it, use these values as defaults.
		 */
		ret = get_temp_sensor_data(mcu_temp_sensor_dev,
					   &mcu_temp_value, true);
		if (ret) {
			SYS_LOG_ERR("MCU temperature sensor error: %d", ret);
		} else {
			SYS_LOG_DBG("Read MCU temp sensor: %d.%dC",
				    mcu_temp_value.val1, mcu_temp_value.val2);
		}

		ret = get_temp_sensor_data(offchip_temp_sensor_dev,
					   &offchip_temp_value, false);
		if (offchip_temp_sensor_dev) {
			if (ret) {
				SYS_LOG_ERR("Off-chip temperature sensor error:"
					    " %d", ret);
			} else {
				SYS_LOG_DBG("Read off-chip temp sensor: %d.%dC",
					    offchip_temp_value.val1,
					    offchip_temp_value.val2);
			}
		}

		/*
		 * Use the whole number portion of temperature sensor
		 * values. Don't publish off-chip values if there is
		 * no sensor, or if there were errors fetching the
		 * values.
		 */
		if (ret) {
			ret = bluemix_pub_status_json(&bluemix_context,
						      "{"
						              "\"mcutemp\":%d"
						      "}",
						      mcu_temp_value.val1);
		} else {
			ret = bluemix_pub_status_json(&bluemix_context,
						      "{"
						              "\"mcutemp\":%d,"
						              "\"temperature\":%d,"
						      "}",
						      mcu_temp_value.val1,
						      offchip_temp_value.val1);
		}

		if (ret) {
			SYS_LOG_ERR("bluemix_pub_status_json: %d", ret);
			bluemix_failures++;
		} else {
			bluemix_failures = 0;
		}

		/* Either way, shut it down. */
		if (ret) {
			ret = bluemix_fini(&bluemix_context);
			SYS_LOG_ERR("bluemix_fini: %d", ret);
		}

		tcp_interface_unlock();

		stack_analyze("Bluemix Thread", bluemix_thread_stack,
			      BLUEMIX_STACK_SIZE);
	}

	SYS_LOG_ERR("Too many bluemix errors, rebooting!");
	sys_reboot(0);
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

	err = temp_init();
	if (err) {
		TC_END_REPORT(TC_FAIL);
		return;
	}

	TC_PRINT("Starting the FOTA Service\n");
	k_thread_spawn(&fota_thread_stack[0], FOTA_STACK_SIZE,
			(k_thread_entry_t) fota_service,
			NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

	TC_PRINT("Starting the Bluemix Service\n");
	k_thread_spawn(&bluemix_thread_stack[0], BLUEMIX_STACK_SIZE,
			(k_thread_entry_t) bluemix_service,
			NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

	TC_PRINT("Blinking LED\n");
	blink_led();
}
