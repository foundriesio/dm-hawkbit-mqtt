/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __HAWKBIT_PRIV_H__
#define __HAWKBIT_PRIV_H__

#include <zephyr/types.h>

/*
 * This file contains contains structures representing JSON results
 * received from a hawkBit server.
 *
 * For full details, see:
 *
 * https://docs.bosch-iot-rollouts.com/documentation/rest-api/rootcontroller-api-guide.html
 *
 * Each of the result types is described in its own section as
 * follows.
 */

/*
 * Common object representing an object containing a link in its
 * "href" field.
 */
struct hawkbit_href {
	const char *href;
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
 * struct hawkbit_dep_res represents results from the deployment
 * operations resource.
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

#endif /* __HAWKBIT_PRIV_H__ */
