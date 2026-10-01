/*
 * libmarmot - group wire profiles and member identity status (nostrc-6ukh)
 *
 * A group's profile decides whether a member leaf may lack the account
 * proof: only legacy (MIP-era) groups admit such members, and only while
 * MarmotConfig.allow_unproven_members. The policy itself is applied where
 * leaves are admitted (commits.c: marmot_commit_authorize() and
 * marmot_tree_members_bound()); this file classifies and reports.
 *
 * SPDX-License-Identifier: MIT
 */

#include "members.h"

#include "marmot-internal.h"
#include "kp_profile.h"
#include "mls/mls-internal.h"
#include "mls/mls_app_components.h"

#include <sodium.h>
#include <stdlib.h>
#include <string.h>

bool
marmot_mls_group_is_legacy(const MlsGroup *group)
{
    /* Slice E's profile: pinned when the group was admitted, created or
     * loaded, and the GroupContext must still say so (a Commit never
     * changes it; mls_group_process_commit() and marmot_commit_authorize()
     * refuse one that would). */
    return group && group->profile == MARMOT_GROUP_PROFILE_LEGACY &&
           mls_group_context_profile_of(group->extensions_data, group->extensions_len) ==
               MARMOT_GROUP_PROFILE_LEGACY;
}

static MarmotError
load_group(Marmot *m, const MarmotGroupId *gid, MlsGroup *out)
{
    if (!m->storage || !m->storage->mls_load) return MARMOT_ERR_STORAGE;
    uint8_t *state = NULL;
    size_t state_len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, "mls_group", gid->data, gid->len,
                                            &state, &state_len);
    if (err != MARMOT_OK || !state) return MARMOT_ERR_GROUP_NOT_FOUND;
    int rc = mls_group_deserialize(state, state_len, out);
    sodium_memzero(state, state_len);   /* epoch secrets */
    free(state);
    return rc == 0 ? MARMOT_OK : MARMOT_ERR_GROUP_NOT_FOUND;
}

#define WELCOME_SIGNER_LABEL "welcome_signer"

MarmotError
marmot_welcome_signer_record(Marmot *m, const MlsGroup *group, uint32_t signer_leaf,
                             const uint8_t *sender)
{
    if (!m || !group || !m->storage) return MARMOT_ERR_INVALID_ARG;
    MarmotStorage *s = m->storage;
    uint32_t node_idx = mls_tree_leaf_to_node(signer_leaf);
    const MlsNode *node = signer_leaf < group->tree.n_leaves && node_idx < group->tree.n_nodes
                              ? &group->tree.nodes[node_idx] : NULL;
    bool vouched = sender && node && node->type == MLS_NODE_LEAF &&
                   node->leaf.credential_identity_len == 32 && node->leaf.credential_identity &&
                   sodium_memcmp(node->leaf.credential_identity, sender, 32) == 0;
    if (!vouched) {
        if (!s->mls_delete) return MARMOT_OK;
        MarmotError err = s->mls_delete(s->ctx, WELCOME_SIGNER_LABEL, group->group_id,
                                        group->group_id_len);
        return err == MARMOT_ERR_STORAGE_NOT_FOUND ? MARMOT_OK : err;
    }
    if (!s->mls_store) return MARMOT_ERR_STORAGE;
    uint8_t value[4 + 32];
    value[0] = (uint8_t)(signer_leaf >> 24);
    value[1] = (uint8_t)(signer_leaf >> 16);
    value[2] = (uint8_t)(signer_leaf >> 8);
    value[3] = (uint8_t)signer_leaf;
    memcpy(value + 4, node->leaf.signature_key, 32);
    return s->mls_store(s->ctx, WELCOME_SIGNER_LABEL, group->group_id, group->group_id_len,
                        value, sizeof(value));
}

/* The device the Welcome's sender vouched for, if recorded. */
static bool
welcome_signer_load(Marmot *m, const MarmotGroupId *gid, uint32_t *leaf, uint8_t key[32])
{
    if (!m->storage->mls_load) return false;
    uint8_t *value = NULL;
    size_t len = 0;
    if (m->storage->mls_load(m->storage->ctx, WELCOME_SIGNER_LABEL, gid->data, gid->len,
                             &value, &len) != MARMOT_OK || !value)
        return false;
    bool ok = len == 4 + 32;
    if (ok) {
        *leaf = (uint32_t)value[0] << 24 | (uint32_t)value[1] << 16 |
                (uint32_t)value[2] << 8 | value[3];
        memcpy(key, value + 4, 32);
    }
    free(value);
    return ok;
}

MarmotError
marmot_get_group_member_identities(Marmot *m, const MarmotGroupId *mls_group_id,
                                   MarmotMemberIdentity **out_members, size_t *out_count)
{
    if (!m || !mls_group_id || !mls_group_id->data || !out_members || !out_count)
        return MARMOT_ERR_INVALID_ARG;
    *out_members = NULL;
    *out_count = 0;
    MlsGroup mls;
    memset(&mls, 0, sizeof(mls));
    MarmotError err = load_group(m, mls_group_id, &mls);
    if (err != MARMOT_OK) return err;

    MarmotMemberIdentity *members = NULL;
    if (mls.tree.n_leaves > 0) {
        members = calloc(mls.tree.n_leaves, sizeof(*members));
        if (!members) {
            mls_group_free(&mls);
            return MARMOT_ERR_MEMORY;
        }
    }
    uint32_t signer_leaf = UINT32_MAX;
    uint8_t signer_key[32];
    bool have_signer = welcome_signer_load(m, mls_group_id, &signer_leaf, signer_key);
    size_t n = 0;
    for (uint32_t i = 0; i < mls.tree.n_leaves; i++) {
        uint32_t node_idx = mls_tree_leaf_to_node(i);
        if (node_idx >= mls.tree.n_nodes) continue;
        const MlsNode *node = &mls.tree.nodes[node_idx];
        if (node->type != MLS_NODE_LEAF || node->leaf.credential_identity_len != 32 ||
            !node->leaf.credential_identity)
            continue;
        MarmotMemberIdentity *out = &members[n++];
        memcpy(out->account_pubkey, node->leaf.credential_identity, 32);
        memcpy(out->signature_key, node->leaf.signature_key, 32);
        out->leaf_index = i;
        out->welcome_signer = have_signer && signer_leaf == i &&
                              sodium_memcmp(signer_key, node->leaf.signature_key, 32) == 0;
        switch (marmot_leaf_proof_status(&node->leaf, MARMOT_CIPHERSUITE)) {
        case MARMOT_LEAF_PROOF_VALID:
            out->status = MARMOT_MEMBER_IDENTITY_PROVEN;
            break;
        case MARMOT_LEAF_PROOF_ABSENT:
            out->status = MARMOT_MEMBER_IDENTITY_UNPROVEN;
            break;
        case MARMOT_LEAF_PROOF_INVALID:
        default:
            out->status = MARMOT_MEMBER_IDENTITY_INVALID;
            break;
        }
    }
    mls_group_free(&mls);
    if (n == 0) {
        free(members);
        members = NULL;
    }
    *out_members = members;
    *out_count = n;
    return MARMOT_OK;
}

MarmotError
marmot_set_allow_unproven_members(Marmot *m, bool allow)
{
    if (!m) return MARMOT_ERR_INVALID_ARG;
    m->config.allow_unproven_members = allow;
    return MARMOT_OK;
}
