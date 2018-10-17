/*
 * Copyright (c) 2017 Linaro Limited
 * Copyright (c) 2018 Foundries.io
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Zephyr syslog backend hook that also prints timestamps.
 */

#ifndef FOTA_TSTAMP_LOG_H__
#define FOTA_TSTAMP_LOG_H__

#ifndef CONFIG_SYS_LOG_EXT_HOOK
static inline void tstamp_hook_install(void)
{
}
#else
/**
 * @brief Install timestamp backend hook.
 *
 * Calling this routine will modify Zephyr's syslog behavior to print
 * a timestamp before the log output. The timestamp is currently a
 * 32-bit monotonic uptime counter, in milliseconds.
 */
void tstamp_hook_install(void);
#endif	/* !defined(CONFIG_SYS_LOG_EXT_HOOK) */

#endif	/* FOTA_TSTAMP_LOG_H__ */
