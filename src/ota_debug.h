/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <misc/printk.h>

#define DEBUG

#define OTA_INFO(fmt, ...) printk("ota: " fmt,  ##__VA_ARGS__)
#define OTA_ERR(fmt, ...) printk("ota: %s: " fmt, __func__, ##__VA_ARGS__)

#ifdef DEBUG
#define OTA_DBG(fmt, ...) printk("ota: %s: " fmt, __func__, ##__VA_ARGS__)
#else
#define OTA_DBG(...)
#endif
