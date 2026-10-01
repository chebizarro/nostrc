/*
 * MLS extensions draft-10 AppDataUpdate proposal wire codec.
 * The Marmot group engine does not apply these proposals yet.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MLS_APP_DATA_UPDATE_H
#define MLS_APP_DATA_UPDATE_H

#include "mls-internal.h"
#include "mls_tree.h"
#include <stddef.h>
#include <stdint.h>

#define MLS_EXTENSION_APP_DATA_DICTIONARY 0x0006
#define MLS_APP_DATA_UPDATE_OP_UPDATE 1
#define MLS_APP_DATA_UPDATE_OP_REMOVE 2
#define MLS_COMPONENT_APP_COMPONENTS 0x0001
#define MARMOT_COMPONENT_ADMIN_POLICY_V1 0x8003

typedef struct {
    uint16_t component_id;
    uint8_t  operation;
    uint8_t *update;
    size_t   update_len;
} MlsAppDataUpdate;

/* These encode/decode only the proposal body (after the u16 proposal type).
 * The component-specific payload and authorization are not validated here. */
int  mls_app_data_update_serialize(const MlsAppDataUpdate *p, MlsTlsBuf *buf);
int  mls_app_data_update_deserialize(MlsTlsReader *reader, MlsAppDataUpdate *p);
void mls_app_data_update_clear(MlsAppDataUpdate *p);

/* The gate on GroupContextExtensions proposals (produced or received):
 * reject a recognizable app_data_dictionary (0x0006).  In a legacy group it
 * would change the group's profile in place, which no profile allows; an
 * adopted group changes components only through AppDataUpdate, which is not
 * applied yet.  Creation, Welcome and load admit an adopted GroupContext
 * through mls_app_components.h instead (nostrc-qp24.5.1).  Legacy opaque
 * extension bytes, including malformed lists, retain their read behavior. */
int mls_group_extensions_supported(const uint8_t *data, size_t len);

/* Bounded, non-publishing state transition for a same-epoch admin-policy
 * replacement. Both trees must be MLS-authenticated parent/resulting trees;
 * sender and committer are parent-epoch leaf indices. Returns a newly
 * allocated AppDataDictionary body on success, leaving input untouched.
 * Other components, removals, and older-epoch proposals remain unsupported. */
int mls_app_data_update_admin_policy(
    const uint8_t *dictionary, size_t dictionary_len,
    const MlsRatchetTree *parent_tree, const MlsRatchetTree *result_tree,
    uint64_t parent_epoch, uint64_t proposal_epoch,
    uint32_t sender_leaf, uint32_t committer_leaf,
    const MlsAppDataUpdate *proposal,
    uint8_t **result, size_t *result_len);

#endif /* MLS_APP_DATA_UPDATE_H */
