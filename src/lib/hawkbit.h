/*
 * Copyright (c) 2016-2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __FOTA_HAWKBIT_H__
#define __FOTA_HAWKBIT_H__

#define HAWKBIT_HOST	"mgmt.foundries.io:8080"
#define HAWKBIT_PORT	8080
#define HAWKBIT_JSON_URL "/DEFAULT/controller/v1"

int hawkbit_init(void);

#endif	/* __FOTA_HAWKBIT_H__ */
