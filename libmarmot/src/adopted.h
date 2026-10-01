/*
 * libmarmot - adopted-profile groups, Marmot layer (nostrc-qp24.5.1)
 *
 * The cryptographic and Marmot-semantic half of admitting an adopted group
 * (the structural half is mls/mls_app_components.h): every member's
 * account proof, the inviter's admin standing, the GroupContext a new group
 * starts with, and the MarmotGroup fields its components carry.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_ADOPTED_H
#define MARMOT_ADOPTED_H

#include "marmot-internal.h"
#include "mls/mls_group.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Every member leaf of @g carries a verified marmot.member.account-identity-
 * proof.v2 (BIP-340 by its credential's account over its MLS signature key,
 * under the group's ciphersuite).  No exemption, no legacy mode: an adopted
 * group without a proof on every leaf is invalid.  MARMOT_OK or
 * MARMOT_ERR_KEY_PACKAGE_IDENTITY.
 */
MarmotError marmot_adopted_members_proven(const MlsGroup *g);

/* Whether the member leaf @leaf of adopted group @g belongs to an account
 * listed in its marmot.group.admin-policy.v1 state. */
bool marmot_adopted_leaf_is_admin(const MlsGroup *g, uint32_t leaf);

/*
 * The GroupContext extension list of a new adopted group (group-setup.md
 * "Creation flow", MDK 0.11 do_create_group): required_capabilities
 * {extensions [0x0006], proposals [0x0008]}, then one app_data_dictionary
 * with app_components requiring MLS_ADOPTED_SUPPORTED_COMPONENTS,
 * marmot.group.profile.v1 (@name, @description; NULL is empty),
 * marmot.group.admin-policy.v1 (@admins, sorted and unique),
 * marmot.transport.nostr.routing.v1 (@nostr_group_id, @relays sorted and
 * unique) and marmot.group.lifecycle.v1 = active.  Inputs are validated
 * (MARMOT_ERR_INVALID_ARG); *out is malloc()ed.
 */
MarmotError marmot_adopted_group_context_build(const char *name, const char *description,
                                               const uint8_t (*admins)[32], size_t admin_count,
                                               const uint8_t nostr_group_id[32],
                                               const char *const *relays, size_t relay_count,
                                               uint8_t **out, size_t *out_len);

/*
 * Sort and deduplicate @relays (NUL-terminated) into a malloc()ed array of
 * borrowed pointers, after checking each against the Nostr relay URL
 * profile; 1..16 must remain.  MARMOT_ERR_INVALID_ARG otherwise.
 */
MarmotError marmot_adopted_canonical_relays(const char *const *relays, size_t count,
                                            const char ***out, size_t *out_count);

/*
 * A new MarmotGroup for adopted group @g: its MLS group id and epoch, and
 * from the GroupContext components its nostr_group_id (0x8004), name and
 * description (0x8001; empty when absent) and admins (0x8003).  The relay
 * URLs of 0x8004 are returned in *relays_out (NULL-free array of malloc()ed
 * strings; free each and the array).
 */
MarmotError marmot_adopted_group_from_mls(const MlsGroup *g, MarmotGroup **group_out,
                                          char ***relays_out, size_t *relay_count_out);

/* Free a relay array from marmot_adopted_group_from_mls(). */
void marmot_adopted_relays_free(char **relays, size_t count);

/*
 * marmot_get_group_components() of an adopted GroupContext extension list
 * (MlsGroup.extensions_data) at @epoch.  The list must pass
 * mls_adopted_group_context_parse() (its error otherwise).
 */
MarmotError marmot_adopted_components_from_extensions(const uint8_t *exts, size_t exts_len,
                                                      uint64_t epoch,
                                                      MarmotGroupComponents *out);

/*
 * The profile of the stored group @gid (its MLS state).  For the public
 * marmot_get_group_profile().
 */
MarmotError marmot_adopted_stored_profile(Marmot *m, const MarmotGroupId *gid,
                                          MarmotGroupProfile *out);

#endif /* MARMOT_ADOPTED_H */
