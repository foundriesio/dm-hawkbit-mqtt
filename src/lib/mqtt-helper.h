/*
 * Copyright (c) 2017 Linaro Limited
 * Copyright (c) 2017 Open Source Foundries Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef	__FOTA_MQTT_HELPER_H__
#define	__FOTA_MQTT_HELPER_H__

#include <net/mqtt.h>
#include <kernel.h>
#include <toolchain.h>

#define MQTT_HELPER_PORT	1883

#define MQTT_HELPER_DEVICE_TYPE	CONFIG_BOARD

struct mqtt_helper_ctx;

/**
 * Return codes for user callbacks.
 */
enum {
	/** Continue normally. */
	MQTT_HELPER_CB_OK = 0,
	/** Re-establish MQTT connection, then proceed. */
	MQTT_HELPER_CB_RECONNECT = -1,
	/** Halt background thread. */
	MQTT_HELPER_CB_HALT = -2,
};

/**
 * Events for user callbacks
 */
enum {
	/**
	 * Attempt to connect to MQTT broker failed.
	 *
	 * If callback returns MQTT_HELPER_CB_OK or MQTT_HELPER_CB_RECONNECT,
	 * another attempt to reconnect will be scheduled.
	 *
	 * Otherwise, the MQTT helper service thread will halt.
	 */
	MQTT_HELPER_EVT_CONN_FAIL = -1,

	/**
	 * MQTT connection is established; callback may perform I/O.
	 *
	 * All callback return codes are accepted.
	 */
	MQTT_HELPER_EVT_POLL      = 0,
};

/**
 * User callback from MQTT helper service thread.
 *
 * The event argument is a MQTT_HELPER_EVT_XXX.
 *
 * The return value must be one of the MQTT_HELPER_CB_XXX values.
 */
typedef int (*mqtt_helper_cb)(struct mqtt_helper_ctx *ctx, int event,
			      void *data);

/**
 * @brief mqtt_helper_ctx	Context structure for MQTT helper
 *
 * All of this state is internal. Clients should interact using the
 * API functions defined below only.
 */
struct mqtt_helper_ctx {
	struct mqtt_connect_msg connect_msg;
	struct mqtt_publish_msg pub_msg;

	struct mqtt_ctx mqtt_ctx;

	u8_t mh_id[30];		/* Buffer for device ID (TODO clean up) */
	u8_t mh_topic[255];		/* Buffer for topic names */
	u8_t mh_message[1024];		/* Buffer for message data */
	u8_t mh_auth_token[20];	/* Authentication token (TODO clean up) */
	u8_t client_id[50];		/* MQTT client ID */

	/* For waiting for a callback from the MQTT stack. */
	struct k_sem wait_sem;
};

/**
 * @brief Start a background MQTT helper thread
 *
 * The background thread attempts to connect to the MQTT broker.
 *
 * If this succeeds, it periodically invokes the user callback with
 * the event argument set to MQTT_HELPER_EVT_POLL. When this happens, it
 * is safe to publish MQTT messages from the callback; for example,
 * it's safe to call mqtt_helper_pub_status_json().
 *
 * If the attempt to connect fails, the callback is invoked with event
 * MQTT_HELPER_EVT_CONN_FAIL. The callback can then signal whether the
 * thread should attempt to reconnect, or halt.
 *
 * @param cb        User callback.
 * @param cb_data   Passed to cb as "data" argument
 * @return Zero if the thread started successfully, negative errno
 *         on error.
 */
int mqtt_helper_init(mqtt_helper_cb cb, void *cb_data);

/**
 * @brief Publish device status reading in JSON format.
 *
 * The format string and arguments should correspond to the JSON
 * value of the "d" field in a Bluemix JSON status publication. This
 * is a legacy requirement which will be removed.
 *
 * For example, to publish an "mcutemp" field with value 23, you could
 * write:
 *
 *    mqtt_helper_pub_status_json(ctx, "{\"mcutemp\":%d}", 23);
 *
 * Do *NOT* write:
 *
 *    mqtt_helper_pub_status_json(ctx, "{ \"d\": { \"mcutemp\":%d } }", 23);
 *
 * @param ctx MQTT helper context to publish status for.
 * @param fmt printf()-like format for JSON sub-string to publish as
 *            status message's data field ("d"). Remaining arguments
 *            are used to build the JSON string to publish with fmt.
 * @return 0 on success, negative errno on failure.
 */
int __printf_like(2, 3) mqtt_helper_pub_status_json(struct mqtt_helper_ctx *ctx,
						    const char *fmt, ...);

#endif	/* __FOTA_MQTT_HELPER_H__ */
