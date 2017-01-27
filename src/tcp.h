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

/*
 * TCP Buffer Logic:
 * HTTP Header (17) + 3x MTU Packets 640 = 1937
 */
#define TCP_RECV_BUF_SIZE 2048

void tcp_cleanup(bool put_context);
int tcp_connect(void);
int tcp_send(const unsigned char *buf, size_t size);
int tcp_recv(unsigned char *buf, size_t size, int32_t timeout);
struct net_context *tcp_get_context(void);
struct k_sem *tcp_get_recv_wait_sem(void);
