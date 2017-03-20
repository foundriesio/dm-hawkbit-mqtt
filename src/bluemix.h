/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef	__FOTA_BLUEMIX_H__
#define	__FOTA_BLUEMIX_H__

#include <net/mqtt.h>
#include <kernel.h>

#ifdef CONFIG_FOTA_DM_BACKEND_BLUEMIX
#include "bluemix_dm.h"
#endif

#define BLUEMIX_PORT	1883

#define APP_CONNECT_TRIES	10
#define APP_SLEEP_MSECS		K_MSEC(500)
#define APP_TX_RX_TIMEOUT       K_MSEC(300)

#define CONFIG_FOTA_BLUEMIX_DEVICE_TYPE	CONFIG_BOARD

#define BM_UUID_LEN 36

/**
 * @brief bluemix_ctx	Context structure for Bluemix
 *
 * All of this state is internal. Clients should interact using the
 * API functions defined below only.
 */
struct bluemix_ctx {
	struct mqtt_connect_msg connect_msg;
	struct mqtt_publish_msg pub_msg;

	struct mqtt_ctx mqtt_ctx;

	/*
	 * This variable will be passed to the connect callback, declared inside
	 * the mqtt context struct. If not used, it could be set to NULL.
	 */
	void *connect_data;

	/*
	 * This variable will be passed to the disconnect callback, declared
	 * inside the mqtt context struct. If not used, it could be set to NULL.
	 */
	void *disconnect_data;

	/*
	 * This variable will be passed to the publish_tx callback, declared
	 * inside the mqtt context struct. If not used, it could be set to NULL.
	 */
	void *publish_data;

	uint8_t bm_id[30];	   /* Bluemix device ID */
	uint8_t bm_topic[255];	   /* Buffer for topic names */
	uint8_t bm_message[1024];  /* Buffer for message data */
	uint8_t bm_auth_token[20]; /* Bluemix authentication token */
	uint8_t client_id[50];	   /* MQTT client ID */

	uint8_t bm_req_id[BM_UUID_LEN+1]; /* Request UUID scratch space */
	int     bm_next_req_id;           /* Per-session counter, for bm_req_id */

	int     bm_fatal_err;	/* Set when fatal errors occur */

	/*
	 * HACK: Semaphore for waiting for a response from Bluemix.
	 *
	 * The Zephyr MQTT stack currently assumes that at most one
	 * MQTT command packet is stored in a net_buf. When two or
	 * more are received and packed into the same net_buf, this is
	 * resulting in all of them being interpreted as a single
	 * malformed packet.
	 *
	 * To work around that, try to make the MQTT stack's
	 * assumption valid by waiting whenever we expect a response
	 * via MQTT, to avoid multiple inbound response packets.
	 */
	struct k_sem reply_sem;
};

int bluemix_init(struct bluemix_ctx *ctx);
int bluemix_fini(struct bluemix_ctx *ctx);

/**
 * @brief Publish sensor data reading from the device.
 * @param ctx Bluemix context to publish temperature on.
 * @param mcu_temp MCU temperature reading in degrees centigrade.
 * @return 0 on success, negative errno on failure.
 */
int bluemix_pub_sensor_c(struct bluemix_ctx *ctx, int mcu_temp);

#endif	/* __FOTA_BLUEMIX_H__ */
