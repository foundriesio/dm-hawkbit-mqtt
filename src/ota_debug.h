/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <misc/printk.h>
#include <kernel.h>

#define DEBUG

#ifndef OTA_TSTAMP_FMT
#define OTA_TSTAMP_FMT "[%07u] "
#endif

#define OTA_INFO(fmt, ...)				\
	do {						\
		uint32_t __up_ms = k_uptime_get_32();	\
		printk(OTA_TSTAMP_FMT "ota: " fmt,	\
		       __up_ms, ##__VA_ARGS__) ;	\
	} while (0)
#define OTA_ERR(fmt, ...)					\
	do {							\
		uint32_t __up_ms = k_uptime_get_32();		\
		printk(OTA_TSTAMP_FMT "ota: %s: " fmt,		\
		       __up_ms, __func__, ##__VA_ARGS__) ;	\
	} while (0)

#ifdef DEBUG
#define OTA_DBG(fmt, ...)					\
	do {							\
		uint32_t __up_ms = k_uptime_get_32();		\
		printk(OTA_TSTAMP_FMT "ota: %s: " fmt,		\
		       __up_ms, __func__, ##__VA_ARGS__);	\
	} while (0)
#else
#define OTA_DBG(...)
#endif
