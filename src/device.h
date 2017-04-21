/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __FOTA_DEVICE_H__
#define __FOTA_DEVICE_H__

struct product_id_t {
	const char *name;
	uint32_t number;
};

extern struct product_id_t product_id;

void set_device_id(void);

#endif	/* __FOTA_DEVICE_H__ */
