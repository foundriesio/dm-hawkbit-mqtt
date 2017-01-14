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

#define TCP_TX_RETRY_TIMEOUT K_SECONDS(1)
#define TCP_RX_TIMEOUT K_SECONDS(3)

int tcp_send(struct net_context *context, const char *buf, unsigned len);
int tcp_recv(struct net_context *context, char *buf, unsigned size);
