/*
 * libmarmot - member identity helpers (internal)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_MEMBERS_INTERNAL_H
#define MARMOT_MEMBERS_INTERNAL_H

#include <marmot/marmot-members.h>
#include <stdbool.h>
#include "mls/mls_group.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Whether @group is of the legacy (MIP-01, MDK 0.8) wire profile, as slice
 * E defines it: MlsGroup.profile, pinned when the group was admitted,
 * created or loaded, is LEGACY and its GroupContext still classifies as
 * legacy (mls_group_context_profile_of(): no app_data_dictionary). Anything
 * else -- adopted, mixed, unreadable, NULL -- is not, and fails closed: no
 * leaf without the account proof, no MIP-01 judgement (nostrc-6ukh, W24
 * review L3). A group without GroupData is legacy (with no admin). */
bool marmot_mls_group_is_legacy(const MlsGroup *group);

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_MEMBERS_INTERNAL_H */
