/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "fota/main"
#define SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG
#include <logging/sys_log.h>

#include <bluetooth/conn.h>
#include <misc/stack.h>
#include <gpio.h>
#include <sensor.h>
#include <tc_util.h>
#include <misc/reboot.h>

/* Local helpers and functions */
#include "tstamp_log.h"
#include "bt_storage.h"
#include "bt_ipss.h"
#include "boot_utils.h"
#if defined(CONFIG_FOTA_DM_BACKEND_HAWKBIT)
#include "hawkbit.h"
#endif
#include "bluemix.h"
#include "device.h"
#include "tcp.h"

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
static void connected(struct bt_conn *conn, uint8_t err)
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

static void disconnected(struct bt_conn *conn, uint8_t reason)
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

static int start_tcp(void)
{
	int ret;

	TC_PRINT("Initializing TCP\n");
	ret = tcp_init();
	if (ret) {
		TC_END_RESULT(TC_FAIL);
	} else {
		TC_END_RESULT(TC_PASS);
	}
	return ret;
}

static int fota_update_acid(struct boot_acid *acid)
{
	int ret = 0;
	if (acid->update != -1) {
		ret = boot_acid_update(BOOT_ACID_CURRENT, acid->update);
		if (!ret) {
			boot_acid_read(acid);
			SYS_LOG_INF("ACID updated, current %d, update %d",
				    acid->current, acid->update);
		} else {
			SYS_LOG_ERR("Failed to update ACID: %d", ret);
		}
	}
	return ret;
}

static int fota_init(void)
{
	struct boot_acid acid;
	uint8_t boot_status;
	int ret;

	TC_PRINT("Initializing FOTA backend\n");

	flash_dev = device_get_binding(FLASH_DRIVER_NAME);
	if (!flash_dev) {
		SYS_LOG_ERR("Failed to find the flash driver");
		TC_END_RESULT(TC_FAIL);
		return -ENODEV;
	}

	/* Update boot status and acid */
	boot_acid_read(&acid);
	SYS_LOG_INF("ACID: current %d, update %d",
		 acid.current, acid.update);
	boot_status = boot_status_read();
	SYS_LOG_INF("Current boot status %x", boot_status);
	if (boot_status == BOOT_STATUS_ONGOING) {
		boot_status_update();
		SYS_LOG_INF("Updated boot status to %x", boot_status_read());
		ret = boot_erase_flash_bank(FLASH_BANK1_OFFSET);
		if (ret) {
			SYS_LOG_ERR("flash_erase error %d", ret);
			TC_END_RESULT(TC_FAIL);
			return ret;
		} else {
			SYS_LOG_DBG("Flash bank (offset %x) erased"
				    " successfully",
				    FLASH_BANK1_OFFSET);
		}
		ret = fota_update_acid(&acid);
		if (ret) {
			TC_END_RESULT(TC_FAIL);
			return ret;
		}
	}

	TC_END_RESULT(TC_PASS);
	return 0;
}

/* Firmware OTA thread (Hawkbit) */
static void fota_service(void)
{
#if defined(CONFIG_FOTA_DM_BACKEND_HAWKBIT)
	uint32_t hawkbit_failures = 0;
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
	uint32_t bluemix_failures = 0;
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
	int err;

	set_device_id();
	tstamp_hook_install();

	SYS_LOG_INF("Linaro FOTA example application");
	SYS_LOG_INF("Device: %s, Serial: %x",
		    product_id.name, product_id.number);

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

	err = start_tcp();
	if (err) {
		TC_END_REPORT(TC_FAIL);
		return;
	}

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
