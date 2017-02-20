/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "config.h"

#if (CONFIG_DM_BACKEND == BACKEND_BLUEMIX)

#define BLUEMIX_IPADDR	"fe80::d4e7:0:0:1"
#define BLUEMIX_PORT	1883

#define APP_CONNECT_TRIES	10
#define APP_SLEEP_MSECS		K_MSEC(500)
#define APP_TX_RX_TIMEOUT       K_MSEC(300)

#define CONFIG_BLUEMIX_DEVICE_TYPE	CONFIG_BOARD

int bluemix_init(void);

#endif /* (CONFIG_DM_BACKEND == BACKEND_BLUEMIX) */
