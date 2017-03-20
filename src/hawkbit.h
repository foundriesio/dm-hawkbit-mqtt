/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __FOTA_HAWKBIT_H__
#define __FOTA_HAWKBIT_H__

#include "config.h"

#define HAWKBIT_HOST	"gitci.com:8080"
#define HAWKBIT_PORT	8080
#define HAWKBIT_JSON_URL "/DEFAULT/controller/v1"

extern int poll_sleep;

int hawkbit_ddi_poll(void);

#endif	/* __FOTA_HAWKBIT_H__ */
