/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __FOTA_TCP_H__
#define __FOTA_TCP_H__

/*
 * TCP Buffer Logic:
 * HTTP Header (17) + 3x MTU Packets 640 = 1937
 */
#define TCP_RECV_BUF_SIZE 2048

int tcp_init(void);
void tcp_cleanup(bool put_context);
int tcp_connect(void);
int tcp_send(const unsigned char *buf, size_t size);
int tcp_recv(unsigned char *buf, size_t size, int32_t timeout);
struct net_context *tcp_get_context(void);
struct k_sem *tcp_get_recv_wait_sem(void);

#endif	/* __FOTA_TCP_H__ */
