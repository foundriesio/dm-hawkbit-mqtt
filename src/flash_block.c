/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <flash.h>

#include "ota_debug.h"
#include "tcp.h"

/*
 * TODO:
 * create flash_block lifecycle struct which contains:
 * flash buffer
 * verify buffer
 * tracking indexes
 * status
 *
 * add flash_block_init()
 * add flash_block_destroy()
 */

#define BLOCK_BUFFER_SIZE	512

uint8_t flash_buf[BLOCK_BUFFER_SIZE];
static uint16_t flash_bytes = 0;

extern uint8_t tcp_buf[TCP_RECV_BUF_SIZE];

/* TODO: remove use of tcp_buf */
static bool flash_block_verify(struct device *dev, off_t offset,
			 uint8_t *data, int len)
{
	int i, rc;

	memset(tcp_buf, 0x00, TCP_RECV_BUF_SIZE);
	rc = flash_read(dev, offset, tcp_buf, len);
	if (rc) {
		OTA_ERR("flash_read error %d location=0x%08x, len=%d\n",
			rc, offset, len);
		return false;
	}

	for (i = 0; i < len; i++) {
		if (data[i] != tcp_buf[i]) {
			OTA_ERR("offset=0x%x, index=%d/%d VERIFY FAIL\n",
				offset, i, len);
			break;
		}
	}

	if (i < len) {
		return false;
	} else {
		return true;
	}
}

/* buffer data into block writes */
int flash_block_write(struct device *dev,
		      off_t offset, int *bytes_written,
		      uint8_t *data, int len,
		      bool finished)
{
	int rc = 0;
	int processed = 0;

	while ((len - processed) > (BLOCK_BUFFER_SIZE - flash_bytes)) {
		memcpy(flash_buf + flash_bytes, data + processed,
		       (BLOCK_BUFFER_SIZE - flash_bytes));

		flash_write_protection_set(dev, false);
		rc = flash_write(dev, offset + *bytes_written,
				 flash_buf, BLOCK_BUFFER_SIZE);
		flash_write_protection_set(dev, true);
		if (rc) {
			OTA_ERR("flash_write error %d offset=0x%08x\n",
				rc, offset + *bytes_written);
			return rc;
		}

		if (!rc &&
		    !flash_block_verify(dev, offset + *bytes_written,
					flash_buf, BLOCK_BUFFER_SIZE)) {
			return -EIO;
		}

		*bytes_written += BLOCK_BUFFER_SIZE;
		processed += (BLOCK_BUFFER_SIZE - flash_bytes);
		flash_bytes = 0;
	}

	/* place rest of the data into flash_buf */
	if (processed < len) {
		memcpy(flash_buf + flash_bytes,
		       data + processed, len - processed);
		flash_bytes += len - processed;
	}

	if (finished && flash_bytes > 0) {
		/* pad the rest of flash_buf and write it out */
		memset(flash_buf + flash_bytes, 0,
		       BLOCK_BUFFER_SIZE - flash_bytes);

		flash_write_protection_set(dev, false);
		rc = flash_write(dev, offset + *bytes_written,
				 flash_buf, BLOCK_BUFFER_SIZE);
		flash_write_protection_set(dev, true);
		if (rc) {
			OTA_ERR("flash_write error %d offset=0x%08x\n",
				rc, offset + *bytes_written);
			return rc;
		}

		if (!rc &&
		    !flash_block_verify(dev, offset + *bytes_written,
					flash_buf, BLOCK_BUFFER_SIZE)) {
			return -EIO;
		}

		*bytes_written = *bytes_written + flash_bytes;
		flash_bytes = 0;
	}

	return rc;
}
