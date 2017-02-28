/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "config.h"

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
#include "bluemix.h"

#define SERVER_CONNECT_TIMEOUT		K_SECONDS(5)
#define SERVER_CONNECT_MAX_WAIT_COUNT	2
#define TCP_TX_TIMEOUT			K_MSEC(500)

/* Network Config */
#if defined(CONFIG_NET_IPV6)
#define FOTA_AF_INET		AF_INET6
#define NET_SIN_FAMILY(s)	net_sin6(s)->sin6_family
#define NET_SIN_ADDR(s)		net_sin6(s)->sin6_addr
#define NET_SIN_PORT(s)		net_sin6(s)->sin6_port
#define NET_SIN_SIZE		sizeof(struct sockaddr_in6)
#define LOCAL_IPADDR		"::"
#ifdef CONFIG_NET_SAMPLES_PEER_IPV6_ADDR
#define PEER_IPADDR		CONFIG_NET_SAMPLES_PEER_IPV6_ADDR
#else
#define PEER_IPADDR		"fe80::d4e7:0:0:1" /* tinyproxy gateway */
#endif
#elif defined(CONFIG_NET_IPV4)
#define FOTA_AF_INET		AF_INET
#define NET_SIN_FAMILY(s)	net_sin(s)->sin_family
#define NET_SIN_ADDR(s)		net_sin(s)->sin_addr
#define NET_SIN_PORT(s)		net_sin(s)->sin_port
#define NET_SIN_SIZE		sizeof(struct sockaddr_in)
#define LOCAL_IPADDR		CONFIG_NET_SAMPLES_MY_IPV4_ADDR
#define PEER_IPADDR		CONFIG_NET_SAMPLES_PEER_IPV4_ADDR
#endif

/* Global address to be set from RA */
static struct sockaddr client_addr;

struct tcp_context {
	struct net_context *net_ctx;
	struct k_sem sem_recv_wait;
	struct k_sem sem_recv_mutex;
	uint8_t read_buf[TCP_RECV_BUF_SIZE];
	uint16_t read_bytes;
	uint16_t peer_port;
};

static struct tcp_context contexts[TCP_CTX_MAX];

static inline int invalid_id(enum tcp_context_id id)
{
	return id < TCP_CTX_HAWKBIT || id >= TCP_CTX_MAX;
}

static void tcp_cleanup_context(struct tcp_context *ctx, bool put_net_context)
{
	if (put_net_context && ctx->net_ctx) {
		net_context_put(ctx->net_ctx);
	}

	ctx->net_ctx = NULL;
}

void tcp_cleanup(enum tcp_context_id id, bool put_net_context)
{
	if (invalid_id(id)) {
		return;
	}
	tcp_cleanup_context(&contexts[id], put_net_context);
}

static void tcp_received_cb(struct net_context *context,
			    struct net_buf *buf, int status, void *user_data)
{
	ARG_UNUSED(context);
	struct tcp_context *ctx = user_data;
	struct net_buf *rx_buf;
	uint8_t *ptr;
	int len;

	/* handle FIN packet */
	if (!buf) {
		OTA_DBG("FIN received, closing network context\n");
		/* clear out our reference to the network connection */
		tcp_cleanup_context(ctx, false);
		k_sem_give(&ctx->sem_recv_wait);
		return;
	} else {
		k_sem_take(&ctx->sem_recv_mutex, K_FOREVER);

		/*
		 * TODO: if overflow, return an error and save
		 * the nbuf for later processing
		 */
		if (ctx->read_bytes + net_nbuf_appdatalen(buf) >= TCP_RECV_BUF_SIZE) {
			OTA_ERR("ERROR buffer overflow! (read(%u)+bufflen(%u) >= %u)\n",
				ctx->read_bytes, net_nbuf_appdatalen(buf),
				TCP_RECV_BUF_SIZE);
			net_nbuf_unref(buf);
			tcp_cleanup_context(ctx, true);
			k_sem_give(&ctx->sem_recv_wait);
			return;
		} else {
			rx_buf = buf->frags;
			ptr = net_nbuf_appdata(buf);
			len = rx_buf->len - (ptr - rx_buf->data);

			while (rx_buf) {
				memcpy(ctx->read_buf + ctx->read_bytes, ptr, len);
				ctx->read_bytes += len;
				rx_buf = rx_buf->frags;
				if (!rx_buf) {
					break;
				}
				ptr = rx_buf->data;
				len = rx_buf->len;
			}
		}
		ctx->read_buf[ctx->read_bytes] = 0;
		net_nbuf_unref(buf);
		k_sem_give(&ctx->sem_recv_mutex);
	}
}

int tcp_init(void)
{
	struct net_if *iface;
	int i;

	iface = net_if_get_default();
	if (!iface) {
		printk("Cannot find default network interface!\n");
		return -ENETDOWN;
	}

#if defined(CONFIG_NET_IPV6)
	net_addr_pton(FOTA_AF_INET, LOCAL_IPADDR,
		      (struct sockaddr *)&NET_SIN_ADDR(&client_addr));
	net_if_ipv6_addr_add(iface,
			     &NET_SIN_ADDR(&client_addr),
			     NET_ADDR_MANUAL, 0);
/*
 * For IPv6 via ethernet, Zephyr does not support an autoconfiguration
 * method such as DHCPv6.  Use IPv4 until it's implemented if this is
 * required.
 */
#elif defined(CONFIG_NET_IPV4)
#if defined(CONFIG_NET_DHCPV4)
	net_dhcpv4_start(iface);

	/* Add delays so DHCP can assign IP */
	/* TODO: add a timeout/retry */
	OTA_INFO("Waiting for DHCP ");
	do {
		OTA_INFO(".");
		k_sleep(K_SECONDS(1));
	} while (net_is_ipv4_addr_unspecified(&iface->dhcpv4.requested_ip));
	OTA_INFO(" Done!\n");

	/* TODO: add a timeout */
	OTA_INFO("Waiting for IP assignment ");
	do {
		OTA_INFO(".");
		k_sleep(K_SECONDS(1));
	} while (!net_is_my_ipv4_addr(&iface->dhcpv4.requested_ip));
	OTA_INFO(" Done!\n");

	net_ipaddr_copy(&NET_SIN_ADDR(&client_addr),
			&iface->dhcpv4.requested_ip);
#else
	net_addr_pton(FOTA_AF_INET, LOCAL_IPADDR,
		      (struct sockaddr *)&NET_SIN_ADDR(&client_addr));
	net_if_ipv4_addr_add(iface,
			     &NET_SIN_ADDR(&client_addr),
			     NET_ADDR_MANUAL, 0);
#endif
#endif

	memset(contexts, 0x00, sizeof(contexts));
	for (i = 0; i < TCP_CTX_MAX; i++) {
		k_sem_init(&contexts[i].sem_recv_wait, 0, 1);
		k_sem_init(&contexts[i].sem_recv_mutex, 1, 1);
	}
	contexts[TCP_CTX_HAWKBIT].peer_port = HAWKBIT_PORT;
	contexts[TCP_CTX_BLUEMIX].peer_port = BLUEMIX_PORT;

	return 0;
}

static int tcp_connect_context(struct tcp_context *ctx)
{
	struct sockaddr my_addr;
	struct sockaddr dst_addr;
	int rc;

	/* make sure we have a network context */
	if (!ctx->net_ctx) {
		rc = net_context_get(FOTA_AF_INET, SOCK_STREAM,
				     IPPROTO_TCP, &ctx->net_ctx);
		if (rc < 0) {
			OTA_ERR("Cannot get network context for TCP (%d)\n",
				rc);
			tcp_cleanup_context(ctx, true);
			return -EIO;
		}

		net_ipaddr_copy(&NET_SIN_ADDR(&my_addr),
				&NET_SIN_ADDR(&client_addr));
		NET_SIN_FAMILY(&my_addr) = FOTA_AF_INET;
		NET_SIN_PORT(&my_addr) = 0;

		rc = net_context_bind(ctx->net_ctx, &my_addr, NET_SIN_SIZE);
		if (rc < 0) {
			OTA_ERR("Cannot bind IP addr (%d)\n", rc);
			tcp_cleanup_context(ctx, true);
			return -EINVAL;
		}
	}

	if (!ctx->net_ctx) {
		OTA_ERR("ERROR: No TCP network context!\n");
		return -EIO;
	}

	/* if we're already connected return */
	if (net_context_get_state(ctx->net_ctx) == NET_CONTEXT_CONNECTED) {
		return 0;
	}

	net_addr_pton(FOTA_AF_INET, PEER_IPADDR,
		      (struct sockaddr *)&NET_SIN_ADDR(&dst_addr));
	NET_SIN_FAMILY(&dst_addr) = FOTA_AF_INET;
	NET_SIN_PORT(&dst_addr) = htons(ctx->peer_port);

	/* triggering the connection starts the callback sequence */
	rc = net_context_connect(ctx->net_ctx, &dst_addr, NET_SIN_SIZE,
				 NULL, SERVER_CONNECT_TIMEOUT, NULL);
	if (rc < 0) {
		OTA_ERR("Cannot connect to server (%d)\n", rc);
		tcp_cleanup_context(ctx, true);
		return -EIO;
	}
	return 0;
}

int tcp_connect(enum tcp_context_id id)
{
	if (invalid_id(id)) {
		return -EINVAL;
	}
	return tcp_connect_context(&contexts[id]);
}

static int tcp_send_context(struct tcp_context *ctx, const unsigned char *buf,
			    size_t size)
{
	struct net_buf *send_buf;
	int rc, len;

	/* make sure we're connected */
	rc = tcp_connect_context(ctx);
	if (rc < 0)
		return rc;

	send_buf = net_nbuf_get_tx(ctx->net_ctx, K_FOREVER);
	if (!send_buf) {
		OTA_ERR("cannot create buf\n");
		return -EIO;
	}

	rc = net_nbuf_append(send_buf, size, (uint8_t *) buf, K_FOREVER);
	if (!rc) {
		OTA_ERR("cannot write buf\n");
		net_nbuf_unref(send_buf);
		return -EIO;
	}

	len = net_buf_frags_len(send_buf);

	rc = net_context_send(send_buf, NULL, TCP_TX_TIMEOUT, NULL, NULL);
	if (rc < 0) {
		OTA_ERR("Cannot send data to peer (%d)\n", rc);
		net_nbuf_unref(send_buf);

		if (rc == -ESHUTDOWN)
			tcp_cleanup_context(ctx, true);

		return -EIO;
	} else {
		return len;
	}
}

int tcp_send(enum tcp_context_id id, const unsigned char *buf, size_t size)
{
	if (invalid_id(id)) {
		return -EINVAL;
	}
	return tcp_send_context(&contexts[id], buf, size);
}

static int tcp_recv_context(struct tcp_context *ctx, unsigned char *buf,
			    size_t size, int32_t timeout)
{
	int rc;

	/* make sure we're connected */
	rc = tcp_connect_context(ctx);
	if (rc < 0)
		return rc;

	net_context_recv(ctx->net_ctx, tcp_received_cb, K_NO_WAIT, ctx);
	/* wait here for the connection to complete or timeout */
	rc = k_sem_take(&ctx->sem_recv_wait, timeout);
	if (rc < 0 && rc != -ETIMEDOUT) {
		OTA_ERR("recv_wait sem error = %d\n", rc);
		return rc;
	}

	/* take a mutex here so we don't process any more data */
	k_sem_take(&ctx->sem_recv_mutex, K_FOREVER);

	/* copy the receive buffer into the passed in buffer */
	if (ctx->read_bytes > 0) {
		if (ctx->read_bytes > size)
			ctx->read_bytes = size;
		memcpy(buf, ctx->read_buf, ctx->read_bytes);
	}
	buf[ctx->read_bytes] = 0;
	rc = ctx->read_bytes;
	ctx->read_bytes = 0;

	k_sem_give(&ctx->sem_recv_mutex);

	return rc;
}

int tcp_recv(enum tcp_context_id id, unsigned char *buf, size_t size, int32_t timeout)
{
	if (invalid_id(id)) {
		return -EINVAL;
	}
	return tcp_recv_context(&contexts[id], buf, size, timeout);
}

struct net_context *tcp_get_net_context(enum tcp_context_id id)
{
	if (invalid_id(id)) {
		return NULL;
	}
	return contexts[id].net_ctx;
}

struct k_sem *tcp_get_recv_wait_sem(enum tcp_context_id id)
{
	if (invalid_id(id)) {
		return NULL;
	}
	return &contexts[id].sem_recv_wait;
}
