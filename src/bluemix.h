/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef	__FOTA_BLUEMIX_H__
#define	__FOTA_BLUEMIX_H__

#include "config.h"

#if (CONFIG_DM_BACKEND == BACKEND_BLUEMIX)

#define BLUEMIX_IPADDR	"fe80::d4e7:0:0:1"
#define BLUEMIX_PORT	1883

#define APP_CONNECT_TRIES	10
#define APP_SLEEP_MSECS		K_MSEC(500)
#define APP_TX_RX_TIMEOUT       K_MSEC(300)

#define CONFIG_BLUEMIX_DEVICE_TYPE	CONFIG_BOARD

/* Response codes for device management requests. */
#define BLUEMIX_RC_SUCCEEDED 200
#define BLUEMIX_RC_ACCEPTED 202
#define BLUEMIX_RC_CHANGED 204
#define BLUEMIX_RC_BAD_REQUEST 400
#define BLUEMIX_RC_NOT_FOUND 404
#define BLUEMIX_RC_CONFLICT 409
#define BLUEMIX_RC_ERROR 500
#define BLUEMIX_RC_UNIMPLEMENTED 501

/* Status of firmware actions, i.e. mgmt.firmware.state.
 * Stored on device. */
#define BLUEMIX_FW_STATE_IDLE 0
#define BLUEMIX_FW_STATE_DOWNLOADING 1
#define BLUEMIX_FW_STATE_DOWNLOADED 2

/* Status of firmware update, i.e. mgmt.firmware.updateStatus.  Stored
 * on device. */
#define BLUEMIX_FW_UPDATE_NONE (-1) /* No update; Bluemix shouldn't query */
#define BLUEMIX_FW_UPDATE_SUCCESS 0
#define BLUEMIX_FW_UPDATE_IN_PROGRESS 1
#define BLUEMIX_FW_UPDATE_OOM 2
#define BLUEMIX_FW_UPDATE_CONN_LOST 3
#define BLUEMIX_FW_UPDATE_VERIFY_FAIL 4
#define BLUEMIX_FW_UPDATE_UNSUPP_IMAGE 5
#define BLUEMIX_FW_UPDATE_INVALID_URI 6

int bluemix_init(void);

#endif /* (CONFIG_DM_BACKEND == BACKEND_BLUEMIX) */

#endif	/* __FOTA_BLUEMIX_H__ */
