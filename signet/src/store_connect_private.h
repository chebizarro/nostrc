/* SPDX-License-Identifier: MIT */
#ifndef SIGNET_STORE_CONNECT_PRIVATE_H
#define SIGNET_STORE_CONNECT_PRIVATE_H
#include "signet/store.h"

/* Called after non-consuming resolution and before any mutation, under the
 * backend's pairing lock/transaction. Must not re-enter the key store or mutate
 * the store. False leaves the secret, handoff and existing binding unchanged. */
typedef bool (*SignetConnectAdmissionFn)(const char *agent_id,
                                        const char *agent_pubkey, void *data);

/* 0 = admitted and paired, 1 = no pending secret, 2 = admission refused,
 * -1 = storage error. out_agent_id is set (g_free) only on success. */
int signet_store_admit_connect_client(SignetStore *store, const char *secret,
                                      const char *client, int64_t now,
                                      SignetConnectAdmissionFn admit, void *data,
                                      char **out_agent_id);
#endif
