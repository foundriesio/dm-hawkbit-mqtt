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

#include <net/net_core.h>
#include <net/net_context.h>
#include <net/nbuf.h>
#include <net/net_if.h>

#include "ota_debug.h"
#include "tcp.h"
#include "hawkbit.h"

#define SERVER_CONNECT_TIMEOUT		K_SECONDS(5)
#define SERVER_CONNECT_MAX_WAIT_COUNT	2
#define TCP_TX_TIMEOUT			K_MSEC(500)

/* Network Config */
#if defined(CONFIG_NET_IPV6)
#define FOTA_AF_INET		AF_INET6
#define NET_SIN_FAMILY(s)	net_sin6(s)->sin6_family
#define NET_SIN_ADDR(s)		net_sin6(s)->sin6_addr
#define NET_SIN_PORT(s)		net_sin6(s)->sin6_port
#define NET_SIN_SIZE		struct sockaddr_in6
#define LOCAL_IPADDR		"::"
#ifdef CONFIG_NET_SAMPLES_PEER_IPV6_ADDR
#define PEER_IPADDR		CONFIG_NET_SAMPLES_PEER_IPV6_ADDR
#else
#define PEER_IPADDR		HAWKBIT_IPADDR
#endif
#elif defined(CONFIG_NET_IPV4)
#define FOTA_AF_INET		AF_INET
#define NET_SIN_FAMILY(s)	net_sin(s)->sin_family
#define NET_SIN_ADDR(s)		net_sin(s)->sin_addr
#define NET_SIN_PORT(s)		net_sin(s)->sin_port
#define NET_SIN_SIZE		struct sockaddr_in
#define LOCAL_IPADDR		CONFIG_NET_SAMPLES_MY_IPV4_ADDR
#define PEER_IPADDR		CONFIG_NET_SAMPLES_PEER_IPV4_ADDR
#endif

/* Global address to be set from RA */
static struct sockaddr client_addr;

#define SERVER_PORT	HAWKBIT_PORT

static struct net_context *net_ctx = { 0 };

static bool tcp_inited = false;
static struct k_sem sem_recv_wait;
static struct k_sem sem_recv_mutex;

static uint8_t tcp_read_buf[TCP_RECV_BUF_SIZE];
static uint16_t read_bytes = 0;

void tcp_cleanup(bool put_context)
{
	if (put_context && net_ctx) {
		net_context_put(net_ctx);
	}

	net_ctx = NULL;
}

static void tcp_received_cb(struct net_context *context,
			    struct net_buf *buf, int status, void *user_data)
{
	ARG_UNUSED(context);
	ARG_UNUSED(user_data);
	struct net_buf *rx_buf;
	uint8_t *ptr;
	int len;

	/* handle FIN packet */
	if (!buf) {
		/* clear out our reference to the network connection */
		tcp_cleanup(false);
		k_sem_give(&sem_recv_wait);
		return;
	} else {
		k_sem_take(&sem_recv_mutex, K_FOREVER);

		/*
		 * TODO: if overflow, return an error and save
		 * the nbuf for later processing
		 */
		if (read_bytes + net_nbuf_appdatalen(buf) >= TCP_RECV_BUF_SIZE) {
			OTA_ERR("ERROR buffer overflow! (read(%u)+bufflen(%u) >= %u)\n",
			       read_bytes, net_nbuf_appdatalen(buf), TCP_RECV_BUF_SIZE);
			net_nbuf_unref(buf);
			tcp_cleanup(true);
			k_sem_give(&sem_recv_wait);
			return;
		} else {
			rx_buf = buf->frags;
			ptr = net_nbuf_appdata(buf);
			len = rx_buf->len - (ptr - rx_buf->data);

			while (rx_buf) {
				memcpy(tcp_read_buf + read_bytes, ptr, len);
				read_bytes += len;
				rx_buf = rx_buf->frags;
				if (!rx_buf) {
					break;
				}
				ptr = rx_buf->data;
				len = rx_buf->len;
			}
		}
		tcp_read_buf[read_bytes] = 0;
		net_nbuf_unref(buf);
		k_sem_give(&sem_recv_mutex);
	}
}

static void tcp_init(void)
{
	struct net_if *iface;

	iface = net_if_get_default();
	if (!iface) {
		printk("Cannot find default network interface!\n");
		return;
	}

#if defined(CONFIG_NET_IPV6)
	net_addr_pton(FOTA_AF_INET, LOCAL_IPADDR,
		      (struct sockaddr *)&NET_SIN_ADDR(&client_addr));
	net_if_ipv6_addr_add(iface,
			     &NET_SIN_ADDR(&client_addr),
			     NET_ADDR_MANUAL, 0);
#elif defined(CONFIG_NET_IPV4)
	net_addr_pton(FOTA_AF_INET, LOCAL_IPADDR,
		      (struct sockaddr *)&NET_SIN_ADDR(&client_addr));
	net_if_ipv4_addr_add(iface,
			     &NET_SIN_ADDR(&client_addr),
			     NET_ADDR_MANUAL, 0);
#endif

	k_sem_init(&sem_recv_wait, 0, 1);
	k_sem_init(&sem_recv_mutex, 1, 1);

	tcp_inited = true;
}

int tcp_connect(void)
{
	struct sockaddr my_addr;
	struct sockaddr dst_addr;
	int rc;

	if (!tcp_inited)
		tcp_init();

	/* make sure we have a network context */
	if (!net_ctx) {
		rc = net_context_get(FOTA_AF_INET, SOCK_STREAM,
				     IPPROTO_TCP, &net_ctx);
		if (rc < 0) {
			OTA_ERR("Cannot get network context for TCP (%d)\n",
				rc);
			tcp_cleanup(true);
			return -EIO;
		}

		net_ipaddr_copy(&NET_SIN_ADDR(&my_addr),
				&NET_SIN_ADDR(&client_addr));
		NET_SIN_FAMILY(&my_addr) = FOTA_AF_INET;
		NET_SIN_PORT(&my_addr) = 0;

		rc = net_context_bind(net_ctx, &my_addr,
				      sizeof(NET_SIN_SIZE));
		if (rc < 0) {
			OTA_ERR("Cannot bind IP addr (%d)\n", rc);
			tcp_cleanup(true);
			return -EINVAL;
		}
	}

	if (!net_ctx) {
		OTA_ERR("ERROR: No TCP network context!\n");
		return -EIO;
	}

	/* if we're already connected return */
	if (net_context_get_state(net_ctx) == NET_CONTEXT_CONNECTED) {
		return 0;
	}

	net_addr_pton(FOTA_AF_INET, PEER_IPADDR,
		      (struct sockaddr *)&NET_SIN_ADDR(&dst_addr));
	NET_SIN_FAMILY(&dst_addr) = FOTA_AF_INET;
	NET_SIN_PORT(&dst_addr) = htons(SERVER_PORT);

	/* triggering the connection starts the callback sequence */
	rc = net_context_connect(net_ctx, &dst_addr,
				  sizeof(NET_SIN_SIZE), NULL,
				  SERVER_CONNECT_TIMEOUT, NULL);
	if (rc < 0) {
		OTA_ERR("Cannot connect to server (%d)\n", rc);
		tcp_cleanup(true);
		return -EIO;
	}
	return 0;
}

int tcp_send(const unsigned char *buf, size_t size)
{
	struct net_buf *send_buf;
	int rc, len;

	/* make sure we're connected */
	rc = tcp_connect();
	if (rc < 0)
		return rc;

	send_buf = net_nbuf_get_tx(net_ctx);
	if (!send_buf) {
		OTA_ERR("cannot create buf\n");
		return -EIO;
	}

	rc = net_nbuf_append(send_buf, size, (uint8_t *) buf);
	if (!rc) {
		OTA_ERR("cannot write buf\n");
		net_nbuf_unref(send_buf);
		return -EIO;
	}

	len = net_buf_frags_len(send_buf);
	k_sleep(TCP_TX_TIMEOUT);

	rc = net_context_send(send_buf, NULL, K_FOREVER, NULL, NULL);
	net_nbuf_unref(send_buf);
	if (rc < 0) {
		OTA_ERR("Cannot send data to peer (%d)\n", rc);

		if (rc == -ESHUTDOWN)
			tcp_cleanup(true);

		return -EIO;
	} else {
		return len;
	}
}

int tcp_recv(unsigned char *buf, size_t size, int32_t timeout)
{
	int rc;

	/* make sure we're connected */
	rc = tcp_connect();
	if (rc < 0)
		return rc;

	net_context_recv(net_ctx, tcp_received_cb, K_NO_WAIT, NULL);
	/* wait here for the connection to complete or timeout */
	rc = k_sem_take(&sem_recv_wait, timeout);
	if (rc < 0 && rc != -ETIMEDOUT) {
		OTA_ERR("recv_wait sem error = %d\n", rc);
		return rc;
	}

	/* take a mutex here so we don't process any more data */
	k_sem_take(&sem_recv_mutex, K_FOREVER);

	/* copy the receive buffer into the passed in buffer */
	if (read_bytes > 0) {
		if (read_bytes > size)
			read_bytes = size;
		memcpy(buf, tcp_read_buf, read_bytes);
	}
	buf[read_bytes] = 0;
	rc = read_bytes;
	read_bytes = 0;

	k_sem_give(&sem_recv_mutex);

	return rc;
}

struct net_context *tcp_get_context(void)
{
	return net_ctx;
}

struct k_sem *tcp_get_recv_wait_sem(void)
{
	return &sem_recv_wait;
}
