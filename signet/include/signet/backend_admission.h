/* SPDX-License-Identifier: MIT */
#ifndef SIGNET_BACKEND_ADMISSION_H
#define SIGNET_BACKEND_ADMISSION_H
#include "signet/store.h"
#include <stdbool.h>
/* Dedicated independent authority, not a credential or NIP-46 writer lease.
 * Borrows store; detach management ingress before freeing, before store close. */
typedef struct SignetBackendAdmission SignetBackendAdmission;
SignetBackendAdmission *signet_backend_admission_new(SignetStore *store,
    const char *authority_id, const char *operator_pubkey);
/* Store-open validation: absent namespace is valid legacy state; partial or
 * inconsistent history is an error, never recreated. */
int signet_backend_admission_validate_store(SignetStore *store);
void signet_backend_admission_free(SignetBackendAdmission *authority);
/* True for the reserved backend/ prefix, including unknown methods. */
bool signet_backend_admission_request(const char *json);
/* Internal ingress: caller MUST have opened and authenticated a NIP-59 seal.
 * Returns an allocated, redacted JSON-RPC response (g_free). No network I/O.
 * denied is supplied by the daemon's shared live deny list. */
char *signet_backend_admission_dispatch(SignetBackendAdmission *authority,
    const char *sender, const char *json, bool denied);
#endif
