/*
 * MLS extensions draft-10 AppDataUpdate proposal wire codec.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mls_app_data_update.h"
#include <stdlib.h>
#include <string.h>

int
mls_app_data_update_serialize(const MlsAppDataUpdate *p, MlsTlsBuf *buf)
{
    if (!p || !buf ||
        (p->operation != MLS_APP_DATA_UPDATE_OP_UPDATE &&
         p->operation != MLS_APP_DATA_UPDATE_OP_REMOVE) ||
        (p->operation == MLS_APP_DATA_UPDATE_OP_REMOVE &&
         (p->update || p->update_len != 0)) ||
        (p->operation == MLS_APP_DATA_UPDATE_OP_UPDATE &&
         p->update_len != 0 && !p->update))
        return -1;

    if (mls_tls_write_u16(buf, p->component_id) != 0 ||
        mls_tls_write_u8(buf, p->operation) != 0)
        return -1;
    if (p->operation == MLS_APP_DATA_UPDATE_OP_UPDATE)
        return mls_tls_write_opaque32(buf, p->update, p->update_len);
    return 0;
}

int
mls_app_data_update_deserialize(MlsTlsReader *reader, MlsAppDataUpdate *p)
{
    if (!reader || !p) return -1;
    memset(p, 0, sizeof(*p));
    if (mls_tls_read_u16(reader, &p->component_id) != 0 ||
        mls_tls_read_u8(reader, &p->operation) != 0)
        return -1;
    if (p->operation == MLS_APP_DATA_UPDATE_OP_REMOVE)
        return 0;
    if (p->operation != MLS_APP_DATA_UPDATE_OP_UPDATE ||
        mls_tls_read_opaque32(reader, &p->update, &p->update_len) != 0) {
        mls_app_data_update_clear(p);
        return -1;
    }
    return 0;
}

void
mls_app_data_update_clear(MlsAppDataUpdate *p)
{
    if (!p) return;
    free(p->update);
    memset(p, 0, sizeof(*p));
}

int
mls_group_extensions_supported(const uint8_t *data, size_t len)
{
    if (len && !data) return -1;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, data, len);
    while (!mls_tls_reader_done(&reader)) {
        uint16_t type;
        size_t data_len;
        /* Legacy groups historically keep opaque extension bytes even when
         * their list is not canonically encoded. Do not reinterpret or reject
         * that state here; a recognizable adopted extension still fails. */
        if (mls_tls_read_u16(&reader, &type) != 0)
            return 0;
        if (type == MLS_EXTENSION_APP_DATA_DICTIONARY)
            return -1;
        if (mls_tls_read_vli(&reader, &data_len) != 0 ||
            mls_tls_reader_remaining(&reader) < data_len)
            return 0;
        reader.pos += data_len;
    }
    return 0;
}

/* These helpers operate only on verified MLS candidate state. They do not
 * admit adopted groups into the live engine or bypass the GroupContext gate. */
static const uint8_t *
member_identity(const MlsRatchetTree *tree, uint32_t leaf)
{
    if (!tree || !tree->nodes || leaf >= tree->n_leaves ||
        (uint64_t)leaf * 2 >= tree->n_nodes)
        return NULL;
    const MlsNode *node = &tree->nodes[mls_tree_leaf_to_node(leaf)];
    if (node->type != MLS_NODE_LEAF ||
        node->leaf.credential_type != MLS_CREDENTIAL_BASIC ||
        node->leaf.credential_identity_len != 32)
        return NULL;
    return node->leaf.credential_identity;
}

static int
has_member(const MlsRatchetTree *tree, const uint8_t key[32])
{
    if (!tree) return 0;
    for (uint32_t i = 0; i < tree->n_leaves; i++) {
        const uint8_t *identity = member_identity(tree, i);
        if (identity && memcmp(identity, key, 32) == 0) return 1;
    }
    return 0;
}

/* MarmotAdminPolicyV1 is a VLI-length vector of sorted, unique 32-byte keys. */
static int
valid_admins(const uint8_t *data, size_t len, const MlsRatchetTree *tree)
{
    MlsTlsReader reader;
    size_t keys_len;
    mls_tls_reader_init(&reader, data, len);
    if (mls_tls_read_vli(&reader, &keys_len) != 0 ||
        keys_len != mls_tls_reader_remaining(&reader) ||
        keys_len == 0 || keys_len % 32 != 0)
        return 0;
    const uint8_t *previous = NULL;
    while (!mls_tls_reader_done(&reader)) {
        const uint8_t *key = reader.data + reader.pos;
        if (previous && memcmp(previous, key, 32) >= 0) return 0;
        if (!has_member(tree, key)) return 0;
        previous = key;
        reader.pos += 32;
    }
    return 1;
}

/* Require the candidate parent's admin-policy id in its required components. */
static int
admin_required(const uint8_t *data, size_t len)
{
    MlsTlsReader reader;
    size_t ids_len;
    mls_tls_reader_init(&reader, data, len);
    if (mls_tls_read_vli(&reader, &ids_len) != 0 ||
        ids_len != mls_tls_reader_remaining(&reader) || ids_len % 2 != 0)
        return 0;
    uint16_t id;
    int found = 0;
    while (!mls_tls_reader_done(&reader)) {
        if (mls_tls_read_u16(&reader, &id) != 0) return 0;
        if (id == MARMOT_COMPONENT_ADMIN_POLICY_V1) found = 1;
    }
    return found;
}

static int
read_entry(MlsTlsReader *reader, uint16_t *id,
           const uint8_t **data, size_t *len)
{
    if (mls_tls_read_u16(reader, id) != 0 ||
        mls_tls_read_vli(reader, len) != 0 ||
        *len > mls_tls_reader_remaining(reader))
        return -1;
    *data = reader->data + reader->pos;
    reader->pos += *len;
    return 0;
}

int
mls_app_data_update_admin_policy(
    const uint8_t *dictionary, size_t dictionary_len,
    const MlsRatchetTree *parent_tree, const MlsRatchetTree *result_tree,
    uint64_t parent_epoch, uint64_t proposal_epoch,
    uint32_t sender_leaf, uint32_t committer_leaf,
    const MlsAppDataUpdate *proposal,
    uint8_t **result, size_t *result_len)
{
    if (!result || !result_len) return -1;
    *result = NULL;
    *result_len = 0;
    if (!dictionary || !parent_tree || !result_tree || !proposal ||
        proposal_epoch != parent_epoch || parent_epoch == UINT64_MAX ||
        proposal->component_id != MARMOT_COMPONENT_ADMIN_POLICY_V1 ||
        proposal->operation != MLS_APP_DATA_UPDATE_OP_UPDATE ||
        !proposal->update ||
        !member_identity(parent_tree, sender_leaf) ||
        !member_identity(parent_tree, committer_leaf) ||
        !valid_admins(proposal->update, proposal->update_len, result_tree))
        return -1;

    MlsTlsReader reader;
    size_t entries_len;
    mls_tls_reader_init(&reader, dictionary, dictionary_len);
    if (mls_tls_read_vli(&reader, &entries_len) != 0 ||
        entries_len != mls_tls_reader_remaining(&reader))
        return -1;
    size_t entries_start = reader.pos;
    uint16_t previous = 0, id;
    const uint8_t *data, *old_admins = NULL, *required = NULL;
    size_t len, old_admins_len = 0, required_len = 0;
    while (!mls_tls_reader_done(&reader)) {
        if (read_entry(&reader, &id, &data, &len) != 0 || id <= previous)
            return -1;
        if (id == MLS_COMPONENT_APP_COMPONENTS) {
            required = data;
            required_len = len;
        } else if (id == MARMOT_COMPONENT_ADMIN_POLICY_V1) {
            old_admins = data;
            old_admins_len = len;
        }
        previous = id;
    }
    if (!required || !admin_required(required, required_len) ||
        !old_admins || !valid_admins(old_admins, old_admins_len, parent_tree))
        return -1;
    const uint8_t *sender = member_identity(parent_tree, sender_leaf);
    const uint8_t *committer = member_identity(parent_tree, committer_leaf);
    int sender_admin = 0, committer_admin = 0;
    MlsTlsReader admins;
    mls_tls_reader_init(&admins, old_admins, old_admins_len);
    if (mls_tls_read_vli(&admins, &len) != 0) return -1;
    while (!mls_tls_reader_done(&admins)) {
        const uint8_t *key = admins.data + admins.pos;
        if (memcmp(sender, key, 32) == 0) sender_admin = 1;
        if (memcmp(committer, key, 32) == 0) committer_admin = 1;
        admins.pos += 32;
    }
    if (!sender_admin || !committer_admin) return -1;

    MlsTlsBuf entries, encoded;
    if (mls_tls_buf_init(&entries, entries_len) != 0) return -1;
    int rc = -1;
    mls_tls_reader_init(&reader, dictionary + entries_start, entries_len);
    while (!mls_tls_reader_done(&reader)) {
        size_t start = reader.pos;
        if (read_entry(&reader, &id, &data, &len) != 0) goto done;
        if (id == MARMOT_COMPONENT_ADMIN_POLICY_V1) {
            if (mls_tls_write_u16(&entries, id) != 0 ||
                mls_tls_write_opaque32(&entries, proposal->update,
                                       proposal->update_len) != 0)
                goto done;
        } else if (mls_tls_buf_append(&entries, reader.data + start,
                                      reader.pos - start) != 0) {
            goto done;
        }
    }
    if (mls_tls_buf_init(&encoded, entries.len + 8) != 0) goto done;
    if (mls_tls_write_vli(&encoded, entries.len) == 0 &&
        mls_tls_buf_append(&encoded, entries.data, entries.len) == 0) {
        *result = encoded.data;
        *result_len = encoded.len;
        encoded.data = NULL;
        rc = 0;
    }
    mls_tls_buf_free(&encoded);
done:
    mls_tls_buf_free(&entries);
    return rc;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Applying a Commit's AppDataUpdate operations (nostrc-qp24.5.1.3)
 * ──────────────────────────────────────────────────────────────────────── */

/* An opaque<V> vector, borrowed. */
static int
read_vec_borrow(MlsTlsReader *r, const uint8_t **out, size_t *len)
{
    size_t n = 0;
    if (mls_tls_read_vli(r, &n) != 0 || n > mls_tls_reader_remaining(r)) return -1;
    *out = r->data + r->pos;
    *len = n;
    r->pos += n;
    return 0;
}

/* Ascending component id (qsort). */
static int
adu_id_cmp(const void *a, const void *b)
{
    uint16_t x = (*(const MlsAppDataUpdate *const *)a)->component_id;
    uint16_t y = (*(const MlsAppDataUpdate *const *)b)->component_id;
    return (x > y) - (x < y);
}

int
mls_app_data_update_apply(const uint8_t *exts, size_t exts_len,
                          const MlsAppDataUpdate *const *ops, size_t n_ops,
                          uint8_t **out, size_t *out_len)
{
    if (!out || !out_len || (exts_len && !exts) || (n_ops && !ops))
        return MARMOT_ERR_INVALID_ARG;
    *out = NULL;
    *out_len = 0;
    /* More operations than component ids necessarily repeats one. */
    if (n_ops > MLS_APP_DATA_UPDATE_MAX) return MARMOT_ERR_MLS_PROCESS_MESSAGE;

    /* Each a known operation; sorted by component id, one per component
     * (O(n log n): a Commit may carry tens of thousands). */
    for (size_t i = 0; i < n_ops; i++)
        if (!ops[i] || (ops[i]->operation != MLS_APP_DATA_UPDATE_OP_UPDATE &&
                        ops[i]->operation != MLS_APP_DATA_UPDATE_OP_REMOVE) ||
            (ops[i]->operation == MLS_APP_DATA_UPDATE_OP_UPDATE && ops[i]->update_len &&
             !ops[i]->update))
            return MARMOT_ERR_MLS_PROCESS_MESSAGE;
    const MlsAppDataUpdate **sorted = NULL;
    if (n_ops > 0) {
        sorted = malloc(n_ops * sizeof(*sorted));
        if (!sorted) return MARMOT_ERR_MEMORY;
        memcpy(sorted, ops, n_ops * sizeof(*sorted));
        qsort(sorted, n_ops, sizeof(*sorted), adu_id_cmp);
        for (size_t i = 1; i < n_ops; i++)
            if (sorted[i]->component_id == sorted[i - 1]->component_id) {
                free(sorted);
                return MARMOT_ERR_MLS_PROCESS_MESSAGE;
            }
    }

    /* The extension list: every extension but the dictionary is copied as
     * it is, in order; exactly one dictionary. */
    MlsTlsBuf others = {0}, entries = {0}, dict = {0}, list = {0};
    int rc = MARMOT_ERR_MEMORY;
    if (mls_tls_buf_init(&others, exts_len + 16) != 0 ||
        mls_tls_buf_init(&entries, exts_len + 64) != 0 ||
        mls_tls_buf_init(&dict, exts_len + 64) != 0 ||
        mls_tls_buf_init(&list, exts_len + 64) != 0)
        goto done;
    const uint8_t *old = NULL;
    size_t old_len = 0, n_dicts = 0;
    MlsTlsReader r;
    mls_tls_reader_init(&r, exts, exts_len);
    while (!mls_tls_reader_done(&r)) {
        size_t start = r.pos;
        uint16_t type = 0;
        const uint8_t *d = NULL;
        size_t dlen = 0;
        if (mls_tls_read_u16(&r, &type) != 0 || read_vec_borrow(&r, &d, &dlen) != 0) {
            rc = MARMOT_ERR_EXTENSION_FORMAT;
            goto done;
        }
        if (type == MLS_EXTENSION_APP_DATA_DICTIONARY) {
            old = d;
            old_len = dlen;
            n_dicts++;
        } else if (mls_tls_buf_append(&others, exts + start, r.pos - start) != 0) {
            goto done;
        }
    }
    if (n_dicts != 1) {
        rc = n_dicts == 0 ? MARMOT_ERR_MLS_PROCESS_MESSAGE : MARMOT_ERR_EXTENSION_FORMAT;
        goto done;
    }

    /* AppDataDictionary { ComponentData component_data<V>; } */
    MlsTlsReader dr;
    mls_tls_reader_init(&dr, old, old_len);
    const uint8_t *ents = NULL;
    size_t ents_len = 0;
    if (read_vec_borrow(&dr, &ents, &ents_len) != 0 || !mls_tls_reader_done(&dr)) {
        rc = MARMOT_ERR_EXTENSION_FORMAT;
        goto done;
    }

    /* Merge in ascending id order: the old entries (strictly ascending) and
     * the sorted operations.  A remove of a component with no state removes
     * nothing, as the pinned OpenMLS does when no GroupContextExtensions
     * proposal rides along (validation.rs; MDK v0.11.0 checks no presence
     * either, validate_app_component_remove_against): libmarmot follows
     * MDK there rather than draft-ietf-mls-extensions 4.7's "invalid"
     * (slice H review L3). */
    MlsTlsReader er;
    mls_tls_reader_init(&er, ents, ents_len);
    bool have_entry = false, first = true;
    uint16_t entry_id = 0, prev = 0;
    const uint8_t *entry = NULL;
    size_t entry_len = 0, k = 0;
    for (;;) {
        if (!have_entry && !mls_tls_reader_done(&er)) {
            if (mls_tls_read_u16(&er, &entry_id) != 0 ||
                read_vec_borrow(&er, &entry, &entry_len) != 0 || (!first && entry_id <= prev)) {
                rc = MARMOT_ERR_EXTENSION_FORMAT;
                goto done;
            }
            first = false;
            prev = entry_id;
            have_entry = true;
        }
        if (!have_entry && k == n_ops) break;
        uint16_t id;
        const uint8_t *data;
        size_t len;
        bool write;
        if (k < n_ops && (!have_entry || sorted[k]->component_id <= entry_id)) {
            const MlsAppDataUpdate *op = sorted[k++];
            id = op->component_id;
            if (have_entry && entry_id == id) have_entry = false;   /* replaced */
            write = op->operation == MLS_APP_DATA_UPDATE_OP_UPDATE;
            data = op->update;
            len = op->update_len;
        } else {
            id = entry_id;
            write = true;
            data = entry;
            len = entry_len;
            have_entry = false;
        }
        if (write && (mls_tls_write_u16(&entries, id) != 0 ||
                      mls_tls_write_opaque32(&entries, data, len) != 0))
            goto done;
    }

    if (mls_tls_write_opaque32(&dict, entries.data, entries.len) != 0 ||
        mls_tls_buf_append(&list, others.data, others.len) != 0 ||
        mls_tls_write_u16(&list, MLS_EXTENSION_APP_DATA_DICTIONARY) != 0 ||
        mls_tls_write_opaque32(&list, dict.data, dict.len) != 0)
        goto done;
    *out = list.data;
    *out_len = list.len;
    list.data = NULL;
    rc = 0;
done:
    free(sorted);
    mls_tls_buf_free(&others);
    mls_tls_buf_free(&entries);
    mls_tls_buf_free(&dict);
    mls_tls_buf_free(&list);
    return rc;
}
