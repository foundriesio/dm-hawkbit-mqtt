/*
 * Copyright (c) 2016-2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Based on the ipsp sample available in Zephyr, done by Intel
 *
 */

#include <stddef.h>
#include <errno.h>
#include <misc/byteorder.h>
#include <zephyr.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>

#include "bt_ipss.h"

#define DEVICE_NAME "Linaro IPSP node"
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)
#define DEVICE_CONNECTED_NAME "Connected IPSP node"
#define DEVICE_CONNECTED_NAME_LEN (sizeof(DEVICE_CONNECTED_NAME) - 1)
#define UNKNOWN_APPEARANCE 0x0000

#if !defined(CONFIG_BLUETOOTH_GATT_DYNAMIC_DB)
static ssize_t read_name(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, u16_t len, u16_t offset)
{
	const char *name = attr->user_data;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, name,
				 strlen(name));
}

static ssize_t read_appearance(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr, void *buf,
			       u16_t len, u16_t offset)
{
	u16_t appearance = sys_cpu_to_le16(UNKNOWN_APPEARANCE);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &appearance,
				 sizeof(appearance));
}

static ssize_t read_model(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, u16_t len, u16_t offset)
{
	const char *value = attr->user_data;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
				 strlen(value));
}

static ssize_t read_manuf(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, u16_t len, u16_t offset)
{
	const char *value = attr->user_data;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
				 strlen(value));
}
#endif /* CONFIG_BLUETOOTH_GATT_DYNAMIC_DB */

static struct bt_gatt_attr attrs[] = {
#if !defined(CONFIG_BLUETOOTH_GATT_DYNAMIC_DB)
	BT_GATT_PRIMARY_SERVICE(BT_UUID_GAP),
	BT_GATT_CHARACTERISTIC(BT_UUID_GAP_DEVICE_NAME, BT_GATT_CHRC_READ),
	BT_GATT_DESCRIPTOR(BT_UUID_GAP_DEVICE_NAME, BT_GATT_PERM_READ,
			   read_name, NULL, DEVICE_NAME),
	BT_GATT_CHARACTERISTIC(BT_UUID_GAP_APPEARANCE, BT_GATT_CHRC_READ),
	BT_GATT_DESCRIPTOR(BT_UUID_GAP_APPEARANCE, BT_GATT_PERM_READ,
			   read_appearance, NULL, NULL),
	/* Device Information Service Declaration */
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DIS),
	BT_GATT_CHARACTERISTIC(BT_UUID_DIS_MODEL_NUMBER, BT_GATT_CHRC_READ),
	BT_GATT_DESCRIPTOR(BT_UUID_DIS_MODEL_NUMBER, BT_GATT_PERM_READ,
			   read_model, NULL, CONFIG_SOC),
	BT_GATT_CHARACTERISTIC(BT_UUID_DIS_MANUFACTURER_NAME,
			       BT_GATT_CHRC_READ),
	BT_GATT_DESCRIPTOR(BT_UUID_DIS_MANUFACTURER_NAME, BT_GATT_PERM_READ,
			   read_manuf, NULL, "Manufacturer"),
#endif /* CONFIG_BLUETOOTH_GATT_DYNAMIC_DB */
	/* IP Support Service Declaration */
	BT_GATT_PRIMARY_SERVICE(BT_UUID_IPSS),
};

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x20, 0x18),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd_connected[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_CONNECTED_NAME,
		DEVICE_CONNECTED_NAME_LEN),
};

int ipss_init(struct bt_conn_cb *conn_callbacks)
{
	int ret;

	ret = bt_gatt_register(attrs, ARRAY_SIZE(attrs));
	if (!ret) {
		bt_conn_cb_register(conn_callbacks);
	}

	return ret;
}

/* local copy of set_ad() in subsys/bluetooth/host/hci_core */
static int ipss_set_ad(u16_t hci_op, const struct bt_data *ad, size_t ad_len)
{
	struct bt_hci_cp_le_set_adv_data *set_data;
	struct net_buf *buf;
	int i;

	buf = bt_hci_cmd_create(hci_op, sizeof(*set_data));
	if (!buf) {
		return -ENOBUFS;
	}

	set_data = net_buf_add(buf, sizeof(*set_data));

	memset(set_data, 0, sizeof(*set_data));

	for (i = 0; i < ad_len; i++) {
		/* Check if ad fit in the remaining buffer */
		if (set_data->len + ad[i].data_len + 2 > 31) {
			net_buf_unref(buf);
			return -EINVAL;
		}

		set_data->data[set_data->len++] = ad[i].data_len + 1;
		set_data->data[set_data->len++] = ad[i].type;

		memcpy(&set_data->data[set_data->len], ad[i].data,
		       ad[i].data_len);
		set_data->len += ad[i].data_len;
	}

	return bt_hci_cmd_send_sync(hci_op, buf, NULL);
}

int ipss_set_connected(void)
{
	int err = 0;

	/*
	 * Clearing BT_HCI_OP_LE_SET_SCAN_RSP_DATA is done by calling
	 * set_ad() with NULL data and zero len.
	 */
	err = ipss_set_ad(BT_HCI_OP_LE_SET_SCAN_RSP_DATA, NULL, 0);
	if (!err) {
		err = ipss_set_ad(BT_HCI_OP_LE_SET_SCAN_RSP_DATA,
				  sd_connected, ARRAY_SIZE(sd_connected));
	}

	return err;
}

int ipss_advertise(void)
{
	int err;

	err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));

	return err;
}
