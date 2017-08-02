/*
 * Copyright (c) 2016-2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __FOTA_TCP_H__
#define __FOTA_TCP_H__

/* Currently, tcp contexts must lock the network interface before using it. */
void tcp_interface_lock(void);
void tcp_interface_unlock(void);

#endif	/* __FOTA_TCP_H__ */
