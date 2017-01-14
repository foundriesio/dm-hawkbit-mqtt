/*
 * Copyright (c) 2016 Linaro Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr.h>

#include <net/ip_buf.h>
#include <net/net_core.h>
#include <net/net_socket.h>

#include "ota_debug.h"
#include "tcp.h"

int tcp_send(struct net_context *context, const char *buf, unsigned len)
{
	struct net_buf *send_buf;
	uint8_t *ptr;
	int ret;

	if (!buf) {
		return -EINVAL;
	}

	/* TODO: Clean possible remaining packages in the rx queue */

	OTA_DBG("Requesting TX buffer\n");
	send_buf = ip_buf_get_tx(context);
	if (!send_buf) {
		OTA_ERR("Unable to get TX buffer, not enough memory.\n");
		return -EIO;
	}

	ptr = net_buf_add(send_buf, len);
	memcpy(ptr, buf, len);
	ip_buf_appdatalen(send_buf) = len;

	OTA_DBG("Sending %p buflen %d datalen %d\n", send_buf,
			send_buf->len, ip_buf_appdatalen(send_buf));
	while (true) {
		ret = net_send(send_buf);
		if (ret >= 0) {
			OTA_DBG("NET SEND >= 0, break %p\n", send_buf);
			break;
		} else if (ret == -EINPROGRESS || ret == -EALREADY) {
			/* EINPROGRESS means waiting SYNACK, while
			 * EALREADY is waiting for our own ACK */
			OTA_DBG("EINPROGRESS || EALREADY, break %p\n",
							send_buf);
			ret = 0;
			break;
		} else if (ret == -ECONNRESET) {
			OTA_DBG("Connection reset, trying again\n");
		} else {
			OTA_DBG("Connection error, buf %p, ret = %d\n",
						send_buf, ret);
			/* TODO: find what to do with SHUTDOWN */
			ip_buf_unref(send_buf);
			break;
		}
		k_sleep(TCP_TX_RETRY_TIMEOUT);
	}

	return ret;
}

int tcp_recv(struct net_context *context, char *buf, unsigned size)
{
	struct net_buf *recv_buf;
	int remaining, len;
	uint8_t *ptr;

	if (!buf) {
		return -EINVAL;
	}

	ptr = buf;
	memset(ptr, 0, size);
	remaining = size - 1;

	while (true) {
		recv_buf = net_receive(context, TCP_RX_TIMEOUT);
		if (!recv_buf) {
			OTA_DBG("net_receive timeout\n");
			break;
		}
		len = ip_buf_appdatalen(recv_buf);
		if (remaining) {
			if (len > remaining) {
				len = remaining;
			}
			OTA_DBG("Storing pkt %p len %d\n", recv_buf, len);
			memcpy(ptr, ip_buf_appdata(recv_buf), len);
			remaining -= len;
			ptr += len;
		}
		ip_buf_unref(recv_buf);
	}
	OTA_DBG("Received total of %d bytes\n", size - remaining);

	return size - remaining;
}
