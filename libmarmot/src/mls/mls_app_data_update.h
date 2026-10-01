/*
 * MLS extensions draft-10 AppDataUpdate proposal: wire codec, and (since
 * libmarmot 0.12.0, nostrc-qp24.5.1.3) its application to an adopted
 * group's GroupContext app_data_dictionary.
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
 * adopted group changes components only through AppDataUpdate
 * (mls_app_data_update_apply(); libmarmot applies no GroupContextExtensions
 * there at all).  Creation, Welcome and load admit an adopted GroupContext
 * through mls_app_components.h instead (nostrc-qp24.5.1).  Legacy opaque
 * extension bytes, including malformed lists, retain their read behavior. */
int mls_group_extensions_supported(const uint8_t *data, size_t len);

/* At most this many AppDataUpdate operations in one Commit: one per
 * component id (app-components/README.md; MDK v0.11.0
 * validate_app_data_update_batch), so one per u16 value -- MDK's limit,
 * which bounds nothing else.  More necessarily repeats an id.  Callers size
 * their operation arrays by the Commit's proposal count, never by this. */
#define MLS_APP_DATA_UPDATE_MAX 65536u

/*
 * The GroupContext extension list @exts with @ops applied to its
 * app_data_dictionary, as the pinned OpenMLS (erskingardner/openmls@59e7d3b,
 * MDK v0.11.0) applies a Commit's AppDataUpdate proposals
 * (apply_app_data_update_proposals): an `update` sets the component's
 * entry to its bytes (full replacement), a `remove` deletes it; entries stay
 * strictly ascending by component id; the dictionary extension is then
 * removed from the list and appended at its end (Extensions::add_or_replace),
 * every other extension keeping its bytes and order.
 *
 * Refused, as invalid (MARMOT_ERR_MLS_PROCESS_MESSAGE): more than one
 * operation for one component (draft-ietf-mls-extensions 4.7; Marmot
 * app-components/README.md "GroupContext Update Processing"), an unknown
 * operation, no dictionary to update.  A remove of a component with no
 * state removes nothing, as in the pinned OpenMLS and MDK v0.11.0 (the
 * draft calls it invalid; slice H review L3).  MARMOT_ERR_MEMORY.
 * MARMOT_ERR_EXTENSION_FORMAT: @exts or its dictionary does not parse
 * canonically.  Component bytes are not interpreted here: the resulting
 * state is validated by mls_group_profile_check_entered() and the Marmot
 * layer.  *out is malloc()ed.
 */
int mls_app_data_update_apply(const uint8_t *exts, size_t exts_len,
                              const MlsAppDataUpdate *const *ops, size_t n_ops,
                              uint8_t **out, size_t *out_len);

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
