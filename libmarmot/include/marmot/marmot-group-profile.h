/*
 * libmarmot - per-group wire profile (nostrc-qp24.5.1)
 *
 * A Marmot group speaks exactly one wire profile for its whole lifetime.
 * The profile is classified from the authenticated GroupContext when the
 * group is created or joined, persisted with the group's MLS state, and
 * never changed in place: there is no automatic fallback from one profile
 * to the other on a parse, cryptographic or authorization failure.
 *
 * Kept in its own small header so the membership-policy work (W24 slice A)
 * and adopted admission (W24 slice E) share one definition.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_GROUP_PROFILE_H
#define MARMOT_GROUP_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * MarmotGroupProfile:
 * @MARMOT_GROUP_PROFILE_LEGACY: the MDK 0.8 / MIP-01 group profile: a
 *   GroupContext carrying marmot_group_data (0xF2EE) and no
 *   app_data_dictionary. Every group libmarmot created or joined before
 *   0.12.0.
 * @MARMOT_GROUP_PROFILE_ADOPTED: the adopted Marmot specification (MDK
 *   0.11, marmot-protocol/marmot @07da8ff): a GroupContext with
 *   required_capabilities and an app_data_dictionary whose app_components
 *   require marmot.member.account-identity-proof.v2 (0x8009) and
 *   marmot.group.admin-policy.v1 (0x8003), every member leaf carrying a
 *   verified 0x8009 proof.
 *
 * Since: 0.12.0
 */
typedef enum {
    MARMOT_GROUP_PROFILE_LEGACY  = 0,
    MARMOT_GROUP_PROFILE_ADOPTED = 1,
} MarmotGroupProfile;

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_GROUP_PROFILE_H */
