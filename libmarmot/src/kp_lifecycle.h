/*
 * libmarmot - KeyPackage transport lifecycle internals (nostrc-0bdg).
 * See kp_lifecycle.c and marmot_key_package_confirm_published() in marmot.h.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MARMOT_KP_LIFECYCLE_H
#define MARMOT_KP_LIFECYCLE_H

#include <marmot/marmot.h>
#include "marmot-internal.h"
#include "mls/mls_key_package.h"
#include <stdbool.h>
#include <stdint.h>

/* The KeyPackage a Welcome was opened with (welcome.c). */
typedef struct {
    bool    have;
    uint8_t ref[32];           /* KeyPackageRef */
    uint8_t owner[32];         /* its credential identity: the account */
    bool    last_resort;       /* carries the last-resort marker */
} MarmotKpUse;

/* Whether @kp is a last-resort KeyPackage: the empty-data
 * last_resort_key_package (0x0004) entry of a KeyPackage-level
 * app_data_dictionary (adopted profile), or the legacy last_resort
 * extension type 0x000a (MDK 0.8 profile). */
bool marmot_kp_is_last_resort(const MlsKeyPackage *kp);

/* The created_at of @owner's next kind:30443: @now, or one second after the
 * slot's newest event when that is not older (transports/nostr.md: within a
 * (pubkey, 30443, d) slot the newest created_at wins, and an equal one falls
 * back to the lower id -- a replacement must be strictly newer). */
MarmotError marmot_kp_lifecycle_created_at(Marmot *m, const uint8_t owner[32], int64_t now,
                                           int64_t *out);

/* A KeyPackage of @owner (its event dated @created_at) was just made and its
 * private material stored (inside the creating transaction): it becomes the
 * newest, unconfirmed entry of the account's publication slot. */
MarmotError marmot_kp_lifecycle_register(Marmot *m, const uint8_t owner[32],
                                         const uint8_t ref[32], uint64_t not_after,
                                         bool last_resort, int64_t created_at);

/* A Welcome opened with @use was joined (inside the accepting
 * transaction): a non-last-resort KeyPackage's private material is deleted
 * now; a last-resort one is kept until its replacement is confirmed or its
 * Lifetime ends. */
MarmotError marmot_kp_lifecycle_consumed(Marmot *m, const MarmotKpUse *use);

#endif /* MARMOT_KP_LIFECYCLE_H */
