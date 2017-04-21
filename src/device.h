/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __FOTA_DEVICE_H__
#define __FOTA_DEVICE_H__

#include <board.h>

/* Bluetooth */
#define DEVICE_NAME "Linaro IPSP node"
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)
#define DEVICE_CONNECTED_NAME "Connected IPSP node"
#define DEVICE_CONNECTED_NAME_LEN (sizeof(DEVICE_CONNECTED_NAME) - 1)
#define UNKNOWN_APPEARANCE 0x0000

/*
 * General hardware specific configs
 *
 * DEVICE_ID_BASE  : beginning of HW UID registers
 * DEVICE_ID_LENGTH: length of HW UID registers in 32-bit words
 */
#if defined(CONFIG_SOC_SERIES_NRF52X)
#define DEVICE_ID_BASE		(&NRF_FICR->DEVICEID[0])
#define DEVICE_ID_LENGTH	2
#elif defined(CONFIG_SOC_SERIES_STM32F4X)
#define DEVICE_ID_BASE		UID_BASE
#define DEVICE_ID_LENGTH	3
#elif defined(CONFIG_SOC_SERIES_KINETIS_K6X)
#define DEVICE_ID_BASE		(&SIM->UIDH)
#define DEVICE_ID_LENGTH	4
#endif

struct product_id_t {
	const char *name;
	uint32_t number;
};

extern struct product_id_t product_id;

void set_device_id(void);

#endif	/* __FOTA_DEVICE_H__ */
