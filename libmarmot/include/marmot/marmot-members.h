/*
 * libmarmot - member identity status (whether a member leaf carries its
 * account's proof) and KeyPackage evidence for members without one
 *
 * Since 0.12.0 (nostrc-6ukh). Included by <marmot/marmot.h>. The group's
 * wire profile (MarmotGroupProfile) is <marmot/marmot-group-profile.h>.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_MEMBERS_H
#define MARMOT_MEMBERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "marmot-error.h"
#include "marmot-types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * MarmotMemberIdentityStatus:
 * @MARMOT_MEMBER_IDENTITY_PROVEN: the leaf carries a valid account proof:
 *   its account signed this device's leaf key.
 * @MARMOT_MEMBER_IDENTITY_UNPROVEN: no proof (legacy groups only). The
 *   credential names an account, but only the admin who added the leaf saw
 *   the KeyPackage event binding the two. An application may still confirm
 *   it with marmot_key_package_event_matches_member().
 * @MARMOT_MEMBER_IDENTITY_INVALID: a proof that does not verify. libmarmot
 *   never admits such a leaf, so this means damaged state.
 */
typedef enum {
    MARMOT_MEMBER_IDENTITY_PROVEN = 0,
    MARMOT_MEMBER_IDENTITY_UNPROVEN = 1,
    MARMOT_MEMBER_IDENTITY_INVALID = 2,
} MarmotMemberIdentityStatus;

/**
 * MarmotMemberIdentity:
 * @account_pubkey: the account the leaf's credential names (x-only)
 * @signature_key: the leaf's Ed25519 signature key (the device)
 * @leaf_index: the MLS leaf
 * @status: see MarmotMemberIdentityStatus
 *
 * One occupied leaf. One account may have several (devices).
 */
typedef struct {
    uint8_t account_pubkey[32];
    uint8_t signature_key[32];
    uint32_t leaf_index;
    MarmotMemberIdentityStatus status;
} MarmotMemberIdentity;

/**
 * marmot_get_group_member_identities:
 * @out_members: (out) (transfer full): one entry per occupied leaf, in leaf
 *   order, our own included; free() it
 * @out_count: (out): entries
 *
 * Returns: MARMOT_OK, or MARMOT_ERR_GROUP_NOT_FOUND
 */
MarmotError marmot_get_group_member_identities(Marmot *m,
                                                const MarmotGroupId *mls_group_id,
                                                MarmotMemberIdentity **out_members,
                                                size_t *out_count);

/**
 * marmot_set_allow_unproven_members:
 *
 * Changes MarmotConfig.allow_unproven_members from the next operation on:
 * the Welcomes accepted, Adds made and Commits processed after it. Members
 * already in a stored group stay.
 *
 * Returns: MARMOT_OK, or MARMOT_ERR_INVALID_ARG
 */
MarmotError marmot_set_allow_unproven_members(Marmot *m, bool allow);

/**
 * marmot_key_package_event_matches_member:
 * @event_json: a signed KeyPackage event: kind 30443 (the MDK 0.8 shape or
 *   the adopted one) or the older kind 443 (hex or base64 content)
 * @member: the leaf to confirm (marmot_get_group_member_identities())
 * @out_matches: (out): whether the event is that leaf's KeyPackage
 *
 * Evidence that an UNPROVEN member's account published this device: the
 * event's id and signature verify, its author is @member's account, the
 * KeyPackage inside verifies, its credential names the author, it carries
 * no invalid account proof, and its leaf has @member's signature key. A
 * KeyPackage that expired still counts: it shows the account published the
 * key then. This is not admission: such an event is never accepted for an
 * Add unless it also parses as a current kind 30443.
 *
 * Threat model: the evidence has no age bound. A device key the account
 * published long ago, then lost, and that later leaks, still "matches";
 * nothing in a legacy group can revoke it short of removing the leaf. A
 * caller should present a match as "this account published this device's
 * key", never as proof that the account controls it now.
 *
 * Returns: MARMOT_OK with *out_matches set (false: a valid KeyPackage of
 * the account, another device); an error for an event that is no valid
 * KeyPackage of its author (*out_matches false).
 */
MarmotError marmot_key_package_event_matches_member(const char *event_json,
                                                     const MarmotMemberIdentity *member,
                                                     bool *out_matches);

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_MEMBERS_H */
