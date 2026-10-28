/* Shared NIP-42 response used by syncd's pull and pointer-publish paths.
 * SPDX-License-Identifier: MIT */
#ifndef NH_SYNCD_AUTH_H
#define NH_SYNCD_AUTH_H

#include <stdbool.h>

typedef struct NostrRelay NostrRelay;

/* Signs kind 22242 with the account secret, sends AUTH, and returns its id.
 * The caller must wait for a correlated OK before using authenticated access. */
bool nh_syncd_send_auth(NostrRelay *relay, const char *secret,
                        const char *challenge, char auth_id[65]);

#endif
