/*
 * Copyright (c) 2016-2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <init.h>

static struct k_sem interface_lock;

void tcp_interface_lock(void)
{
	k_sem_take(&interface_lock, K_FOREVER);
}

void tcp_interface_unlock()
{
	k_sem_give(&interface_lock);
}


static int tcp_init(struct device *dev)
{
	k_sem_init(&interface_lock, 1, 1);
	return 0;
}

SYS_INIT(tcp_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
