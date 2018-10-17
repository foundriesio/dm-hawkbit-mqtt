/*
 * Copyright (c) 2017 Linaro Limited
 * Copyright (c) 2018 Foundries.io
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HAWKBIT_PRIV_H__
#define HAWKBIT_PRIV_H__

#include <zephyr/types.h>

/*
 * This file contains contains structures representing JSON messages
 * exchanged with a hawkBit server.
 *
 * For full details, see:
 *
 * https://docs.bosch-iot-rollouts.com/documentation/rest-api/rootcontroller-api-guide.html
 *
 * Each of the result types is described in its own section as
 * follows.
 */

/*
 * Common objects.
 */

struct hawkbit_href {
	const char *href;
};

enum hawkbit_status_fini {
	HAWKBIT_STATUS_FINISHED_SUCCESS = 0,
	HAWKBIT_STATUS_FINISHED_FAILURE,
	HAWKBIT_STATUS_FINISHED_NONE,
};

struct hawkbit_status_result {
	/*
	 * hawkbit_status_finished() converts from enum hawkbit_status_fini.
	 */
	const char			*finished;
	/*
	 * The "progress" field is unsupported. Its purpose is not
	 * clear in the documentation, and the "of" and "cnt" fields
	 * it contains seem to be unused in the result handling code.
	 */
};

enum hawkbit_status_exec {
	HAWKBIT_STATUS_EXEC_CLOSED = 0,
	HAWKBIT_STATUS_EXEC_PROCEEDING,
	HAWKBIT_STATUS_EXEC_CANCELED,
	HAWKBIT_STATUS_EXEC_SCHEDULED,
	HAWKBIT_STATUS_EXEC_REJECTED,
	HAWKBIT_STATUS_EXEC_RESUMED,
};

struct hawkbit_status {
	/*
	 * hawkbit_status_execution() converts from enum hawkbit_status_exec.
	 */
	const char			*execution;
	struct hawkbit_status_result	 result;
	/* The "details" field is currently unsupported. */
};

/*
 * struct hawkbit_ctl_res represents results from a hawkBit
 * controller's base poll resource.
 *
 * In general, it sometimes looks like this:
 *
 * {
 *      "config": {
 *          "polling": {
 *            "sleep": "00:00:30"
 *          }
 *      },
 *      "_links": {
 *          "deploymentBase": {
 *            "href": "https://some_url"
 *          },
 *          "configData": {
 *            "href": "https://some_url"
 *          }
 *      }
 * }
 *
 * And at other times looks like this:
 *
 * {
 *     "config": {
 *         "polling": {
 *             "sleep": "00:00:30"
 *         }
 *     },
 *     "_links": {
 *         "cancelAction": {
 *             "href": "https://some_url"
 *          },
 *          "configData": {
 *              "href": "https://some_url"
 *          }
 *     }
 * }
 */

/* Sleep format: HH:MM:SS */
#define HAWKBIT_SLEEP_LENGTH 8

struct hawkbit_ctl_res_sleep {
	const char *sleep;
};

struct hawkbit_ctl_res_polling {
	struct hawkbit_ctl_res_sleep polling;
};

struct hawkbit_ctl_res_links {
	struct hawkbit_href deploymentBase;
	struct hawkbit_href configData;
	struct hawkbit_href cancelAction;
};

struct hawkbit_ctl_res {
	struct hawkbit_ctl_res_polling config;
	struct hawkbit_ctl_res_links _links;
};

/*
 * struct hawkbit_cfg represents metadata sent to a target's
 * configData resource.
 */

/*
 * FIXME: "data" is actually a generic key/value list; support this.
 */
struct hawkbit_cfg_data {
	const char *board;
	const char *serial;
};

struct hawkbit_cfg {
	const char		*id;
	/* The "time" field is currently unsupported. */
	struct hawkbit_status	 status;
	struct hawkbit_cfg_data  data;
};

/*
 * struct hawkbit_dep_res represents results from the deployment
 * operations resource.
 *
 * struct hawkbit_dep_fbk represents a response sent to its feedback
 * channel.
 */

/* Maximum number of chunks we support */
#define HAWKBIT_DEP_MAX_CHUNKS		1
/* Maximum number of artifacts per chunk. */
#define HAWKBIT_DEP_MAX_CHUNK_ARTS	1

struct hawkbit_dep_res_hashes {
	const char *sha1;
	const char *md5;
};

struct hawkbit_dep_res_links {
	struct hawkbit_href download;
	struct hawkbit_href md5sum;
	struct hawkbit_href download_http;
	struct hawkbit_href md5sum_http;
};

struct hawkbit_dep_res_arts {
	const char			*filename;
	s32_t				 size;
	struct hawkbit_dep_res_hashes	 hashes;
	struct hawkbit_dep_res_links	 _links;
};

struct hawkbit_dep_res_chunk {
	const char			*part;
	const char			*name;
	const char			*version;
	struct hawkbit_dep_res_arts	 artifacts[HAWKBIT_DEP_MAX_CHUNK_ARTS];
	size_t				 num_artifacts;
};

struct hawkbit_dep_res_deploy {
	const char			*download;
	const char			*update;
	struct hawkbit_dep_res_chunk	 chunks[HAWKBIT_DEP_MAX_CHUNKS];
	size_t                           num_chunks;
};

struct hawkbit_dep_res {
	const char			*id;
	struct hawkbit_dep_res_deploy	 deployment;
};

struct hawkbit_dep_fbk {
	const char		*id;
	/* The "time" field is currently unsupported. */
	struct hawkbit_status	 status;
};

#endif /* HAWKBIT_PRIV_H__ */
