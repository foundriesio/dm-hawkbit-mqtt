/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* these functions hidden in zephyr/subsys/bluetooth/host/hci_core.h */
extern struct net_buf *bt_hci_cmd_create(uint16_t opcode, uint8_t param_len);
extern int bt_hci_cmd_send_sync(uint16_t opcode, struct net_buf *buf,
				struct net_buf **rsp);

void ipss_init(struct bt_conn_cb *conn_callbacks);
int ipss_set_connected(void);
int ipss_advertise(void);
