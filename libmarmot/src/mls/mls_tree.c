/*
 * libmarmot - MLS Ratchet Tree implementation (RFC 9420 §4, §7, Appendix C)
 *
 * Array-based left-balanced binary tree for TreeKEM.
 *
 * Node layout: leaf i is at index 2*i, parents at odd indices.
 *   Tree with n leaves has 2*(n-1)+1 total nodes.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls_tree.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Array-based tree math (Appendix C)
 * ══════════════════════════════════════════════════════════════════════════ */

static uint32_t
log2_floor(uint32_t x)
{
    if (x == 0) return 0;
    uint32_t k = 0;
    while ((x >> k) > 0) k++;
    return k - 1;
}

uint32_t
mls_tree_level(uint32_t x)
{
    if ((x & 1) == 0) return 0;
    uint32_t k = 0;
    while (((x >> k) & 1) == 1) k++;
    return k;
}

uint32_t
mls_tree_node_width(uint32_t n)
{
    if (n == 0) return 0;
    /* Check for overflow: 2*(n-1)+1 must fit in uint32_t */
    if (n > (UINT32_MAX / 2)) return 0; /* Overflow would occur */
    return 2 * (n - 1) + 1;
}

uint32_t
mls_tree_root(uint32_t n)
{
    uint32_t w = mls_tree_node_width(n);
    return (1u << log2_floor(w)) - 1;
}

uint32_t
mls_tree_left(uint32_t x)
{
    uint32_t k = mls_tree_level(x);
    if (k == 0) return x; /* leaf has no children */
    return x ^ (1u << (k - 1));
}

uint32_t
mls_tree_right(uint32_t x)
{
    uint32_t k = mls_tree_level(x);
    if (k == 0) return x;
    return x ^ (3u << (k - 1));
}

uint32_t
mls_tree_parent(uint32_t x, uint32_t n)
{
    uint32_t r = mls_tree_root(n);
    if (x == r) return r; /* root has no parent */

    uint32_t k = mls_tree_level(x);
    uint32_t b = (x >> (k + 1)) & 1;
    return (x | (1u << k)) ^ (b << (k + 1));
}

uint32_t
mls_tree_sibling(uint32_t x, uint32_t n)
{
    uint32_t p = mls_tree_parent(x, n);
    if (x < p)
        return mls_tree_right(p);
    else
        return mls_tree_left(p);
}

int
mls_tree_direct_path(uint32_t x, uint32_t n,
                      uint32_t *path, uint32_t max_len,
                      uint32_t *path_len)
{
    if (!path_len || (max_len > 0 && !path) || n == 0) return -1;
    *path_len = 0;
    uint32_t r = mls_tree_root(n);
    if (x == r) return 0;

    uint32_t count = 0;
    uint32_t cur = x;
    while (cur != r) {
        if (count >= max_len) return -1;
        cur = mls_tree_parent(cur, n);
        path[count++] = cur;
    }
    *path_len = count;
    return 0;
}

int
mls_tree_copath(uint32_t x, uint32_t n,
                 uint32_t *copath, uint32_t max_len,
                 uint32_t *copath_len)
{
    if (!copath_len || (max_len > 0 && !copath) || n == 0) return -1;
    *copath_len = 0;
    uint32_t r = mls_tree_root(n);
    if (x == r) return 0;

    uint32_t count = 0;
    uint32_t cur = x;
    while (cur != r) {
        if (count >= max_len) return -1;
        copath[count++] = mls_tree_sibling(cur, n);
        cur = mls_tree_parent(cur, n);
    }
    *copath_len = count;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Node lifecycle
 * ══════════════════════════════════════════════════════════════════════════ */

void
mls_leaf_node_clear(MlsLeafNode *node)
{
    if (!node) return;
    free(node->credential_identity);
    free(node->versions);
    free(node->ciphersuites);
    free(node->cap_extensions);
    free(node->proposals);
    free(node->cap_credentials);
    free(node->extensions_data);
    free(node->parent_hash);
    memset(node, 0, sizeof(*node));
}

void
mls_parent_node_clear(MlsParentNode *node)
{
    if (!node) return;
    free(node->parent_hash);
    free(node->unmerged_leaves);
    memset(node, 0, sizeof(*node));
}

void
mls_tree_blank_node(MlsNode *node)
{
    if (!node) return;
    switch (node->type) {
        case MLS_NODE_LEAF:
            mls_leaf_node_clear(&node->leaf);
            break;
        case MLS_NODE_PARENT:
            mls_parent_node_clear(&node->parent);
            break;
        default:
            break;
    }
    node->type = MLS_NODE_BLANK;
}

int
mls_leaf_node_clone(MlsLeafNode *dst, const MlsLeafNode *src)
{
    if (!dst || !src) return -1;

    /* Zero dst first to avoid double-free on error */
    memset(dst, 0, sizeof(*dst));

    /* Copy non-pointer fields */
    memcpy(dst->encryption_key, src->encryption_key, MLS_KEM_PK_LEN);
    memcpy(dst->signature_key, src->signature_key, MLS_SIG_PK_LEN);
    dst->credential_type = src->credential_type;
    dst->credential_identity_len = src->credential_identity_len;
    dst->version_count = src->version_count;
    dst->ciphersuite_count = src->ciphersuite_count;
    dst->cap_extension_count = src->cap_extension_count;
    dst->proposal_count = src->proposal_count;
    dst->cap_credential_count = src->cap_credential_count;
    dst->extensions_len = src->extensions_len;
    memcpy(dst->signature, src->signature, MLS_SIG_LEN);
    dst->signature_len = src->signature_len;
    dst->parent_hash_len = src->parent_hash_len;
    dst->leaf_node_source = src->leaf_node_source;
    dst->lifetime_not_before = src->lifetime_not_before;
    dst->lifetime_not_after = src->lifetime_not_after;

    /* Deep-copy heap allocations */
    dst->credential_identity = NULL;
    dst->versions = NULL;
    dst->ciphersuites = NULL;
    dst->cap_extensions = NULL;
    dst->proposals = NULL;
    dst->cap_credentials = NULL;
    dst->extensions_data = NULL;
    dst->parent_hash = NULL;

#define CLONE_U16_VEC(field, count_field) \
    if (src->field && src->count_field > 0) { \
        size_t sz = src->count_field * sizeof(uint16_t); \
        dst->field = malloc(sz); \
        if (!dst->field) goto fail; \
        memcpy(dst->field, src->field, sz); \
    }

    if (src->credential_identity && src->credential_identity_len > 0) {
        dst->credential_identity = malloc(src->credential_identity_len);
        if (!dst->credential_identity) goto fail;
        memcpy(dst->credential_identity, src->credential_identity, src->credential_identity_len);
    }
    CLONE_U16_VEC(versions, version_count)
    CLONE_U16_VEC(ciphersuites, ciphersuite_count)
    CLONE_U16_VEC(cap_extensions, cap_extension_count)
    CLONE_U16_VEC(proposals, proposal_count)
    CLONE_U16_VEC(cap_credentials, cap_credential_count)
    if (src->extensions_data && src->extensions_len > 0) {
        dst->extensions_data = malloc(src->extensions_len);
        if (!dst->extensions_data) goto fail;
        memcpy(dst->extensions_data, src->extensions_data, src->extensions_len);
    }
    if (src->parent_hash && src->parent_hash_len > 0) {
        dst->parent_hash = malloc(src->parent_hash_len);
        if (!dst->parent_hash) goto fail;
        memcpy(dst->parent_hash, src->parent_hash, src->parent_hash_len);
    }

#undef CLONE_U16_VEC
    return 0;

fail:
    mls_leaf_node_clear(dst);
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Ratchet tree lifecycle
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_tree_new(MlsRatchetTree *tree, uint32_t n_leaves)
{
    if (!tree) return -1;
    memset(tree, 0, sizeof(*tree));

    if (n_leaves == 0) return 0;

    tree->n_leaves = n_leaves;
    tree->n_nodes = mls_tree_node_width(n_leaves);
    tree->nodes = calloc(tree->n_nodes, sizeof(MlsNode));
    if (!tree->nodes) return -1;

    /* All nodes start as blank */
    for (uint32_t i = 0; i < tree->n_nodes; i++) {
        tree->nodes[i].type = MLS_NODE_BLANK;
    }
    return 0;
}

void
mls_tree_free(MlsRatchetTree *tree)
{
    if (!tree || !tree->nodes) return;
    for (uint32_t i = 0; i < tree->n_nodes; i++) {
        mls_tree_blank_node(&tree->nodes[i]);
    }
    free(tree->nodes);
    memset(tree, 0, sizeof(*tree));
}

int
mls_tree_add_leaf(MlsRatchetTree *tree, uint32_t *out_leaf_node_idx)
{
    if (!tree) return -1;

    /* First leaf: special case */
    if (tree->n_leaves == 0) {
        tree->n_leaves = 1;
        tree->n_nodes = 1;
        tree->nodes = calloc(1, sizeof(MlsNode));
        if (!tree->nodes) return -1;
        tree->nodes[0].type = MLS_NODE_BLANK;
        if (out_leaf_node_idx) *out_leaf_node_idx = 0;
        return 0;
    }

    for (uint32_t leaf = 0; leaf < tree->n_leaves; leaf++) {
        uint32_t node_idx = mls_tree_leaf_to_node(leaf);
        if (node_idx < tree->n_nodes && tree->nodes[node_idx].type == MLS_NODE_BLANK) {
            if (out_leaf_node_idx) *out_leaf_node_idx = node_idx;
            return 0;
        }
    }

    uint32_t old_n_leaves = tree->n_leaves;
    uint32_t new_n_leaves = old_n_leaves * 2;
    uint32_t new_n_nodes = mls_tree_node_width(new_n_leaves);

    /* Grow the array: append (new_n_nodes - old_n_nodes) blank nodes */
    MlsNode *new_nodes = realloc(tree->nodes, new_n_nodes * sizeof(MlsNode));
    if (!new_nodes) return -1;

    /* Zero-initialize the new entries */
    for (uint32_t i = tree->n_nodes; i < new_n_nodes; i++) {
        memset(&new_nodes[i], 0, sizeof(MlsNode));
        new_nodes[i].type = MLS_NODE_BLANK;
    }

    tree->nodes = new_nodes;
    tree->n_nodes = new_n_nodes;
    tree->n_leaves = new_n_leaves;

    uint32_t new_leaf_node_idx = mls_tree_leaf_to_node(old_n_leaves);
    if (out_leaf_node_idx) *out_leaf_node_idx = new_leaf_node_idx;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Resolution (RFC 9420 §4.1.1)
 * ══════════════════════════════════════════════════════════════════════════ */

static int
resolution_recursive(const MlsRatchetTree *tree, uint32_t node_idx,
                     uint32_t *out, uint32_t *out_len, uint32_t max_len)
{
    if (node_idx >= tree->n_nodes) return 0;
    const MlsNode *node = &tree->nodes[node_idx];

    if (node->type != MLS_NODE_BLANK) {
        /* Non-blank node: include it plus unmerged leaves */
        if (*out_len >= max_len) return -1;
        out[(*out_len)++] = node_idx;

        /* Add unmerged leaves for parent nodes */
        if (node->type == MLS_NODE_PARENT) {
            /* Validate unmerged_leaves consistency */
            if (node->parent.unmerged_leaf_count > 0 && !node->parent.unmerged_leaves) return -1;
            for (size_t i = 0; i < node->parent.unmerged_leaf_count; i++) {
                if (*out_len >= max_len) return -1;
                out[(*out_len)++] = mls_tree_leaf_to_node(node->parent.unmerged_leaves[i]);
            }
        }
        return 0;
    }

    /* Blank node */
    if (mls_tree_is_leaf(node_idx)) {
        /* Blank leaf: empty resolution */
        return 0;
    }

    /* Blank parent: concatenate resolution(left) + resolution(right) */
    int rc = resolution_recursive(tree, mls_tree_left(node_idx), out, out_len, max_len);
    if (rc != 0) return rc;
    return resolution_recursive(tree, mls_tree_right(node_idx), out, out_len, max_len);
}

int
mls_tree_resolution(const MlsRatchetTree *tree, uint32_t node_idx,
                    uint32_t *out, uint32_t max_len,
                    uint32_t *out_len)
{
    if (!tree || !out || !out_len) return -1;
    *out_len = 0;
    return resolution_recursive(tree, node_idx, out, out_len, max_len);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Filtered direct path (RFC 9420 §4.1.2)
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_tree_filtered_direct_path(const MlsRatchetTree *tree, uint32_t leaf_idx,
                               uint32_t *out, uint32_t max_len,
                               uint32_t *out_len)
{
    if (!tree || !out || !out_len || tree->n_leaves == 0) return -1;
    *out_len = 0;

    uint32_t node_idx = mls_tree_leaf_to_node(leaf_idx);
    if (node_idx >= tree->n_nodes) return -1;
    uint32_t r = mls_tree_root(tree->n_leaves);
    uint32_t *res = calloc(tree->n_nodes ? tree->n_nodes : 1, sizeof(uint32_t));
    if (!res) return -1;

    /* Walk from leaf to root */
    uint32_t cur = node_idx;
    while (cur != r) {
        uint32_t p = mls_tree_parent(cur, tree->n_leaves);
        /* Find the copath child (sibling of cur under p) */
        uint32_t copath_child = mls_tree_sibling(cur, tree->n_leaves);

        /* Check if copath child has non-empty resolution */
        uint32_t res_len = 0;
        int rc = mls_tree_resolution(tree, copath_child, res, tree->n_nodes, &res_len);
        if (rc != 0) { free(res); return rc; }

        if (res_len > 0) {
            /* Copath child has non-empty resolution: include p in filtered path */
            if (*out_len >= max_len) { free(res); return -1; }
            out[(*out_len)++] = p;
        }
        /* Else: skip p (copath child is empty) */

        cur = p;
    }
    free(res);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * TLS serialization for nodes
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_leaf_node_serialize(const MlsLeafNode *node, MlsTlsBuf *buf)
{
    if (!node || !buf) return -1;

    /* encryption_key: opaque HPKEPublicKey<V>; (use opaque16 per MLS) */
    if (mls_tls_write_opaque16(buf, node->encryption_key, MLS_KEM_PK_LEN) != 0)
        return -1;
    /* signature_key: opaque SignaturePublicKey<V>; */
    if (mls_tls_write_opaque16(buf, node->signature_key, MLS_SIG_PK_LEN) != 0)
        return -1;
    /* credential_type: uint16 */
    if (mls_tls_write_u16(buf, node->credential_type) != 0) return -1;
    /* credential identity (basic): opaque identity<V>; */
    if (mls_tls_write_opaque16(buf, node->credential_identity,
                                node->credential_identity_len) != 0)
        return -1;
    /* capabilities (RFC 9420 §7.2): versions, ciphersuites, extensions, proposals, credentials */
#define WRITE_U16_VEC(arr, count) do { \
        size_t total = (count) * 2; \
        if (mls_tls_write_vli(buf, total) != 0) return -1; \
        for (size_t _i = 0; _i < (count); _i++) { \
            if (mls_tls_write_u16(buf, (arr)[_i]) != 0) return -1; \
        } \
    } while (0)

    WRITE_U16_VEC(node->versions, node->version_count);
    WRITE_U16_VEC(node->ciphersuites, node->ciphersuite_count);
    WRITE_U16_VEC(node->cap_extensions, node->cap_extension_count);
    WRITE_U16_VEC(node->proposals, node->proposal_count);
    WRITE_U16_VEC(node->cap_credentials, node->cap_credential_count);

#undef WRITE_U16_VEC
    /* leaf_node_source: uint8 */
    if (mls_tls_write_u8(buf, node->leaf_node_source) != 0) return -1;
    /* source-dependent fields (RFC 9420 §7.2) */
    if (node->leaf_node_source == 1) { /* key_package: Lifetime */
        if (mls_tls_write_u64(buf, node->lifetime_not_before) != 0) return -1;
        if (mls_tls_write_u64(buf, node->lifetime_not_after) != 0) return -1;
    } else if (node->leaf_node_source == 3) { /* commit: parent_hash */
        if (mls_tls_write_opaque8(buf, node->parent_hash, node->parent_hash_len) != 0)
            return -1;
    }
    /* extensions: opaque<V> */
    if (mls_tls_write_opaque32(buf, node->extensions_data, node->extensions_len) != 0)
        return -1;
    /* signature: opaque<V> */
    if (mls_tls_write_opaque16(buf, node->signature, node->signature_len) != 0)
        return -1;
    return 0;
}

int
mls_parent_node_serialize(const MlsParentNode *node, MlsTlsBuf *buf)
{
    if (!node || !buf) return -1;

    /* Validate parent_hash consistency */
    if (node->parent_hash_len > 0 && !node->parent_hash) return -1;

    /* encryption_key */
    if (mls_tls_write_opaque16(buf, node->encryption_key, MLS_KEM_PK_LEN) != 0)
        return -1;
    /* parent_hash */
    if (mls_tls_write_opaque8(buf, node->parent_hash, node->parent_hash_len) != 0)
        return -1;
    /* unmerged_leaves: uint32 list (VLI length prefix) */
    {
        size_t total = node->unmerged_leaf_count * 4;
        if (mls_tls_write_vli(buf, total) != 0) return -1;
        for (size_t i = 0; i < node->unmerged_leaf_count; i++) {
            if (mls_tls_write_u32(buf, node->unmerged_leaves[i]) != 0) return -1;
        }
    }
    return 0;
}

int
mls_leaf_node_deserialize(MlsTlsReader *reader, MlsLeafNode *node)
{
    if (!reader || !node) return -1;
    memset(node, 0, sizeof(*node));

    /* encryption_key */
    uint8_t *enc_key = NULL; size_t enc_key_len = 0;
    if (mls_tls_read_opaque16(reader, &enc_key, &enc_key_len) != 0) return -1;
    if (enc_key_len != MLS_KEM_PK_LEN) { free(enc_key); return -1; }
    memcpy(node->encryption_key, enc_key, MLS_KEM_PK_LEN);
    free(enc_key);

    /* signature_key */
    uint8_t *sig_key = NULL; size_t sig_key_len = 0;
    if (mls_tls_read_opaque16(reader, &sig_key, &sig_key_len) != 0) return -1;
    if (sig_key_len != MLS_SIG_PK_LEN) { free(sig_key); return -1; }
    memcpy(node->signature_key, sig_key, MLS_SIG_PK_LEN);
    free(sig_key);

    /* credential_type */
    if (mls_tls_read_u16(reader, &node->credential_type) != 0) return -1;

    /* credential identity */
    if (mls_tls_read_opaque16(reader, &node->credential_identity,
                               &node->credential_identity_len) != 0)
        return -1;

    /* capabilities (RFC 9420 §7.2): versions, ciphersuites, extensions, proposals, credentials */
#define READ_U16_VEC(field, count_field) do { \
        size_t _bytes; \
        if (mls_tls_read_vli(reader, &_bytes) != 0) return -1; \
        if (_bytes % 2 != 0) return -1; \
        node->count_field = _bytes / 2; \
        if (node->count_field > 0) { \
            node->field = malloc(node->count_field * sizeof(uint16_t)); \
            if (!node->field) return -1; \
            for (size_t _i = 0; _i < node->count_field; _i++) { \
                if (mls_tls_read_u16(reader, &node->field[_i]) != 0) return -1; \
            } \
        } \
    } while (0)

    READ_U16_VEC(versions, version_count);
    READ_U16_VEC(ciphersuites, ciphersuite_count);
    READ_U16_VEC(cap_extensions, cap_extension_count);
    READ_U16_VEC(proposals, proposal_count);
    READ_U16_VEC(cap_credentials, cap_credential_count);

#undef READ_U16_VEC

    /* leaf_node_source */
    if (mls_tls_read_u8(reader, &node->leaf_node_source) != 0) return -1;

    /* source-dependent fields (RFC 9420 §7.2) */
    if (node->leaf_node_source == 1) { /* key_package: Lifetime */
        if (mls_tls_read_u64(reader, &node->lifetime_not_before) != 0) return -1;
        if (mls_tls_read_u64(reader, &node->lifetime_not_after) != 0) return -1;
    } else if (node->leaf_node_source == 3) { /* commit: parent_hash */
        if (mls_tls_read_opaque8(reader, &node->parent_hash,
                                  &node->parent_hash_len) != 0)
            return -1;
    }

    /* extensions */
    if (mls_tls_read_opaque32(reader, &node->extensions_data,
                               &node->extensions_len) != 0)
        return -1;

    /* signature */
    uint8_t *sig = NULL; size_t sig_len = 0;
    if (mls_tls_read_opaque16(reader, &sig, &sig_len) != 0) return -1;
    if (sig_len != MLS_SIG_LEN) { free(sig); return -1; }
    memcpy(node->signature, sig, sig_len);
    node->signature_len = sig_len;
    free(sig);

    return 0;
}

int
mls_parent_node_deserialize(MlsTlsReader *reader, MlsParentNode *node)
{
    if (!reader || !node) return -1;
    memset(node, 0, sizeof(*node));

    /* encryption_key */
    uint8_t *enc_key = NULL; size_t enc_key_len = 0;
    if (mls_tls_read_opaque16(reader, &enc_key, &enc_key_len) != 0) return -1;
    if (enc_key_len != MLS_KEM_PK_LEN) { free(enc_key); return -1; }
    memcpy(node->encryption_key, enc_key, MLS_KEM_PK_LEN);
    free(enc_key);

    /* parent_hash */
    if (mls_tls_read_opaque8(reader, &node->parent_hash,
                              &node->parent_hash_len) != 0)
        return -1;

    /* unmerged_leaves (VLI length prefix) */
    size_t ul_bytes;
    if (mls_tls_read_vli(reader, &ul_bytes) != 0) return -1;
    /* Validate that ul_bytes is divisible by 4 (4 bytes per uint32) */
    if (ul_bytes % 4 != 0) return -1;
    node->unmerged_leaf_count = ul_bytes / 4;
    if (node->unmerged_leaf_count > 0) {
        node->unmerged_leaves = malloc(node->unmerged_leaf_count * sizeof(uint32_t));
        if (!node->unmerged_leaves) return -1;
        for (size_t i = 0; i < node->unmerged_leaf_count; i++) {
            if (mls_tls_read_u32(reader, &node->unmerged_leaves[i]) != 0) return -1;
        }
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tree hash (RFC 9420 §7.8)
 *
 * TreeHashInput:
 *   NodeType node_type;
 *   select (node_type) {
 *     case leaf:   LeafNodeHashInput   { uint32 leaf_index; optional<LeafNode> }
 *     case parent: ParentNodeHashInput { optional<ParentNode>; left_hash; right_hash }
 *   }
 * ══════════════════════════════════════════════════════════════════════════ */

static uint32_t
mls_tree_right_bounded(uint32_t x, uint32_t n_nodes)
{
    uint32_t r = mls_tree_right(x);
    while (r >= n_nodes && mls_tree_level(r) > 0)
        r = mls_tree_left(r);
    return r;
}

int
mls_tree_hash(const MlsRatchetTree *tree, uint32_t node_idx,
              uint8_t out[MLS_HASH_LEN])
{
    if (!tree || !out) return -1;
    if (node_idx >= tree->n_nodes) return -1;

    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 256) != 0) return -1;

    const MlsNode *node = &tree->nodes[node_idx];

    if (mls_tree_is_leaf(node_idx)) {
        /* LeafNodeHashInput */
        if (mls_tls_write_u8(&buf, 1) != 0) goto fail; /* NodeType::leaf */
        /* leaf_index */
        uint32_t leaf_idx = mls_tree_node_to_leaf(node_idx);
        if (mls_tls_write_u32(&buf, leaf_idx) != 0) goto fail;
        /* optional<LeafNode>: present bit + data */
        if (node->type == MLS_NODE_LEAF) {
            if (mls_tls_write_u8(&buf, 1) != 0) goto fail; /* present */
            if (mls_leaf_node_serialize(&node->leaf, &buf) != 0) goto fail;
        } else {
            if (mls_tls_write_u8(&buf, 0) != 0) goto fail; /* absent */
        }
    } else {
        /* ParentNodeHashInput */
        if (mls_tls_write_u8(&buf, 2) != 0) goto fail; /* NodeType::parent */
        /* optional<ParentNode> */
        if (node->type == MLS_NODE_PARENT) {
            if (mls_tls_write_u8(&buf, 1) != 0) goto fail;
            if (mls_parent_node_serialize(&node->parent, &buf) != 0) goto fail;
        } else {
            if (mls_tls_write_u8(&buf, 0) != 0) goto fail;
        }
        /* left_hash */
        uint8_t left_hash[MLS_HASH_LEN];
        if (mls_tree_hash(tree, mls_tree_left(node_idx), left_hash) != 0) goto fail;
        if (mls_tls_write_opaque8(&buf, left_hash, MLS_HASH_LEN) != 0) goto fail;
        /* right_hash */
        uint8_t right_hash[MLS_HASH_LEN];
        if (mls_tree_hash(tree, mls_tree_right_bounded(node_idx, tree->n_nodes), right_hash) != 0) goto fail;
        if (mls_tls_write_opaque8(&buf, right_hash, MLS_HASH_LEN) != 0) goto fail;
    }

    int rc = mls_crypto_hash(out, buf.data, buf.len);
    mls_tls_buf_free(&buf);
    return rc;

fail:
    mls_tls_buf_free(&buf);
    return -1;
}

int
mls_tree_root_hash(const MlsRatchetTree *tree, uint8_t out[MLS_HASH_LEN])
{
    if (!tree || tree->n_leaves == 0) return -1;
    return mls_tree_hash(tree, mls_tree_root(tree->n_leaves), out);
}

int
mls_ratchet_tree_serialize(const MlsRatchetTree *tree,
                           uint8_t **out_data, size_t *out_len)
{
    if (!tree || !out_data || !out_len) return -1;
    if (tree->n_leaves == 0 || tree->n_nodes == 0 || !tree->nodes) return -1;

    uint32_t node_count = tree->n_nodes;
    while (node_count > 0 && tree->nodes[node_count - 1].type == MLS_NODE_BLANK)
        node_count--;
    if (node_count == 0) return -1;

    MlsTlsBuf nodes;
    if (mls_tls_buf_init(&nodes, node_count * 64 + 8) != 0) return -1;

    for (uint32_t i = 0; i < node_count; i++) {
        const MlsNode *node = &tree->nodes[i];
        if (node->type == MLS_NODE_BLANK) {
            if (mls_tls_write_u8(&nodes, 0) != 0) goto fail_nodes; /* optional<Node>: absent */
        } else if (node->type == MLS_NODE_LEAF && mls_tree_is_leaf(i)) {
            if (mls_tls_write_u8(&nodes, 1) != 0 || /* present */
                mls_tls_write_u8(&nodes, 1) != 0 || /* NodeType::leaf */
                mls_leaf_node_serialize(&node->leaf, &nodes) != 0) goto fail_nodes;
        } else if (node->type == MLS_NODE_PARENT && !mls_tree_is_leaf(i)) {
            if (mls_tls_write_u8(&nodes, 1) != 0 || /* present */
                mls_tls_write_u8(&nodes, 2) != 0 || /* NodeType::parent */
                mls_parent_node_serialize(&node->parent, &nodes) != 0) goto fail_nodes;
        } else {
            goto fail_nodes;
        }
    }

    MlsTlsBuf out;
    if (mls_tls_buf_init(&out, nodes.len + 4) != 0) goto fail_nodes;
    if (mls_tls_write_opaque32(&out, nodes.data, nodes.len) != 0) {
        mls_tls_buf_free(&out);
        goto fail_nodes;
    }
    mls_tls_buf_free(&nodes);
    *out_data = out.data;
    *out_len = out.len;
    return 0;

fail_nodes:
    mls_tls_buf_free(&nodes);
    return -1;
}

int
mls_ratchet_tree_deserialize(const uint8_t *data, size_t len,
                             MlsRatchetTree *tree)
{
    if (!data || !tree) return -1;
    memset(tree, 0, sizeof(*tree));

    MlsTlsReader reader;
    mls_tls_reader_init(&reader, data, len);

    uint8_t *nodes_data = NULL;
    size_t nodes_len = 0;
    if (mls_tls_read_opaque32(&reader, &nodes_data, &nodes_len) != 0) return -1;
    if (!mls_tls_reader_done(&reader) || nodes_len == 0) {
        free(nodes_data);
        return -1;
    }

    MlsTlsReader nodes_reader;
    mls_tls_reader_init(&nodes_reader, nodes_data, nodes_len);

    MlsRatchetTree tmp;
    memset(&tmp, 0, sizeof(tmp));
    uint32_t capacity = 0;

    while (!mls_tls_reader_done(&nodes_reader)) {
        if (tmp.n_nodes == capacity) {
            uint32_t new_capacity = capacity ? capacity * 2 : 16;
            if (new_capacity <= capacity) goto fail;
            MlsNode *new_nodes = realloc(tmp.nodes, (size_t)new_capacity * sizeof(MlsNode));
            if (!new_nodes) goto fail;
            tmp.nodes = new_nodes;
            capacity = new_capacity;
        }

        MlsNode *node = &tmp.nodes[tmp.n_nodes];
        memset(node, 0, sizeof(*node));
        node->type = MLS_NODE_BLANK;

        uint8_t present = 0;
        if (mls_tls_read_u8(&nodes_reader, &present) != 0) goto fail;
        if (present == 1) {
            uint8_t node_type = 0;
            if (mls_tls_read_u8(&nodes_reader, &node_type) != 0) goto fail;
            if (node_type == 1 && mls_tree_is_leaf(tmp.n_nodes)) {
                node->type = MLS_NODE_LEAF;
                if (mls_leaf_node_deserialize(&nodes_reader, &node->leaf) != 0) goto fail;
            } else if (node_type == 2 && !mls_tree_is_leaf(tmp.n_nodes)) {
                node->type = MLS_NODE_PARENT;
                if (mls_parent_node_deserialize(&nodes_reader, &node->parent) != 0) goto fail;
            } else {
                goto fail;
            }
        } else if (present != 0) {
            goto fail;
        }
        tmp.n_nodes++;
    }

    if (tmp.n_nodes == 0 || tmp.nodes[tmp.n_nodes - 1].type == MLS_NODE_BLANK) goto fail;

    uint32_t full_nodes = 1;
    while (full_nodes < tmp.n_nodes) {
        if (full_nodes > (UINT32_MAX - 1) / 2) goto fail;
        full_nodes = full_nodes * 2 + 1;
    }
    if (full_nodes > tmp.n_nodes) {
        MlsNode *new_nodes = realloc(tmp.nodes, (size_t)full_nodes * sizeof(MlsNode));
        if (!new_nodes) goto fail;
        tmp.nodes = new_nodes;
        for (uint32_t i = tmp.n_nodes; i < full_nodes; i++) {
            memset(&tmp.nodes[i], 0, sizeof(MlsNode));
            tmp.nodes[i].type = MLS_NODE_BLANK;
        }
        tmp.n_nodes = full_nodes;
    }
    tmp.n_leaves = (tmp.n_nodes + 1) / 2;
    if (mls_tree_node_width(tmp.n_leaves) != tmp.n_nodes) goto fail;

    free(nodes_data);
    *tree = tmp;
    return 0;

fail:
    free(nodes_data);
    mls_tree_free(&tmp);
    return -1;
}

static int
subtree_contains_node(const MlsRatchetTree *tree, uint32_t subtree_root, uint32_t node_idx)
{
    if (!tree || subtree_root >= tree->n_nodes || node_idx >= tree->n_nodes) return 0;
    if (subtree_root == node_idx) return 1;
    if (mls_tree_is_leaf(subtree_root)) return 0;
    return subtree_contains_node(tree, mls_tree_left(subtree_root), node_idx) ||
           subtree_contains_node(tree, mls_tree_right(subtree_root), node_idx);
}

static int
parent_node_has_unmerged_leaf(const MlsParentNode *parent, uint32_t leaf_idx)
{
    if (!parent) return 0;
    for (size_t i = 0; i < parent->unmerged_leaf_count; i++) {
        if (parent->unmerged_leaves[i] == leaf_idx) return 1;
    }
    return 0;
}

static int
mls_parent_node_serialize_without_unmerged(const MlsParentNode *node,
                                           const MlsParentNode *blanked,
                                           MlsTlsBuf *buf)
{
    if (!node || !buf) return -1;
    if (node->parent_hash_len > 0 && !node->parent_hash) return -1;
    if (mls_tls_write_opaque16(buf, node->encryption_key, MLS_KEM_PK_LEN) != 0)
        return -1;
    if (mls_tls_write_opaque8(buf, node->parent_hash, node->parent_hash_len) != 0)
        return -1;

    size_t kept = 0;
    for (size_t i = 0; i < node->unmerged_leaf_count; i++) {
        if (!parent_node_has_unmerged_leaf(blanked, node->unmerged_leaves[i]))
            kept++;
    }
    if (mls_tls_write_vli(buf, kept * 4) != 0) return -1;
    for (size_t i = 0; i < node->unmerged_leaf_count; i++) {
        if (parent_node_has_unmerged_leaf(blanked, node->unmerged_leaves[i]))
            continue;
        if (mls_tls_write_u32(buf, node->unmerged_leaves[i]) != 0) return -1;
    }
    return 0;
}

static int
mls_tree_hash_with_unmerged_blanked(const MlsRatchetTree *tree, uint32_t node_idx,
                                    const MlsParentNode *blanked,
                                    uint8_t out[MLS_HASH_LEN])
{
    if (!tree || !out || node_idx >= tree->n_nodes) return -1;

    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 256) != 0) return -1;
    const MlsNode *node = &tree->nodes[node_idx];

    if (mls_tree_is_leaf(node_idx)) {
        if (mls_tls_write_u8(&buf, 1) != 0) goto fail;
        if (mls_tls_write_u32(&buf, mls_tree_node_to_leaf(node_idx)) != 0) goto fail;
        if (node->type == MLS_NODE_LEAF &&
            !parent_node_has_unmerged_leaf(blanked, mls_tree_node_to_leaf(node_idx))) {
            if (mls_tls_write_u8(&buf, 1) != 0) goto fail;
            if (mls_leaf_node_serialize(&node->leaf, &buf) != 0) goto fail;
        } else {
            if (mls_tls_write_u8(&buf, 0) != 0) goto fail;
        }
    } else {
        if (mls_tls_write_u8(&buf, 2) != 0) goto fail;
        if (node->type == MLS_NODE_PARENT) {
            if (mls_tls_write_u8(&buf, 1) != 0) goto fail;
            if (mls_parent_node_serialize_without_unmerged(&node->parent, blanked, &buf) != 0)
                goto fail;
        } else {
            if (mls_tls_write_u8(&buf, 0) != 0) goto fail;
        }
        uint8_t left_hash[MLS_HASH_LEN];
        if (mls_tree_hash_with_unmerged_blanked(tree, mls_tree_left(node_idx),
                                                blanked, left_hash) != 0) goto fail;
        if (mls_tls_write_opaque8(&buf, left_hash, MLS_HASH_LEN) != 0) goto fail;
        uint8_t right_hash[MLS_HASH_LEN];
        if (mls_tree_hash_with_unmerged_blanked(tree, mls_tree_right(node_idx),
                                                blanked, right_hash) != 0) goto fail;
        if (mls_tls_write_opaque8(&buf, right_hash, MLS_HASH_LEN) != 0) goto fail;
    }

    int rc = mls_crypto_hash(out, buf.data, buf.len);
    mls_tls_buf_free(&buf);
    return rc;

fail:
    mls_tls_buf_free(&buf);
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Parent hash (RFC 9420 §7.9)
 *
 * ParentHashInput:
 *   opaque encryption_key<V>;
 *   opaque parent_hash<V>;       -- of the parent of this node (or empty at root)
 *   opaque original_sibling_tree_hash<V>;
 *
 * parent_hash(N) = H(ParentHashInput)
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_tree_parent_hash(const MlsRatchetTree *tree, uint32_t parent_node_idx,
                      uint32_t original_child, uint8_t out[MLS_HASH_LEN])
{
    if (!tree || !out) return -1;
    if (parent_node_idx >= tree->n_nodes) return -1;
    if (mls_tree_is_leaf(parent_node_idx)) return -1; /* must be a parent */

    const MlsNode *pnode = &tree->nodes[parent_node_idx];
    if (pnode->type != MLS_NODE_PARENT) return -1;

    /* Determine the sibling subtree of original_child under parent_node_idx. */
    uint32_t sibling_idx;
    uint32_t left = mls_tree_left(parent_node_idx);
    uint32_t right = mls_tree_right(parent_node_idx);
    if (subtree_contains_node(tree, left, original_child)) {
        sibling_idx = right;
    } else if (subtree_contains_node(tree, right, original_child)) {
        sibling_idx = left;
    } else {
        return -1;
    }

    /* Compute original_sibling_tree_hash after blanking P.unmerged_leaves. */
    uint8_t sibling_hash[MLS_HASH_LEN];
    if (mls_tree_hash_with_unmerged_blanked(tree, sibling_idx,
                                            &pnode->parent, sibling_hash) != 0)
        return -1;

    /* Get this node's own parent_hash field (or empty if at root). */
    uint8_t parent_parent_hash[MLS_HASH_LEN];
    size_t  parent_parent_hash_len = 0;
    uint32_t r = mls_tree_root(tree->n_leaves);
    if (parent_node_idx != r) {
        if (pnode->parent.parent_hash_len > 0) {
            if (!pnode->parent.parent_hash || pnode->parent.parent_hash_len != MLS_HASH_LEN)
                return -1;
            memcpy(parent_parent_hash, pnode->parent.parent_hash, MLS_HASH_LEN);
            parent_parent_hash_len = MLS_HASH_LEN;
        }
    } else if (pnode->parent.parent_hash_len != 0) {
        return -1;
    }

    /* Serialize ParentHashInput */
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 128) != 0) return -1;

    /* encryption_key<V> */
    if (mls_tls_write_opaque16(&buf, pnode->parent.encryption_key, MLS_KEM_PK_LEN) != 0)
        goto fail;
    /* parent_hash<V> */
    if (mls_tls_write_opaque8(&buf, parent_parent_hash, parent_parent_hash_len) != 0)
        goto fail;
    /* original_sibling_tree_hash<V> */
    if (mls_tls_write_opaque8(&buf, sibling_hash, MLS_HASH_LEN) != 0)
        goto fail;

    int rc = mls_crypto_hash(out, buf.data, buf.len);
    mls_tls_buf_free(&buf);
    return rc;

fail:
    mls_tls_buf_free(&buf);
    return -1;
}

static int
node_parent_hash_field(const MlsRatchetTree *tree, uint32_t node_idx,
                       const uint8_t **out, size_t *out_len)
{
    if (!tree || node_idx >= tree->n_nodes || !out || !out_len) return -1;
    const MlsNode *node = &tree->nodes[node_idx];
    *out = NULL;
    *out_len = 0;
    if (node->type == MLS_NODE_PARENT) {
        *out = node->parent.parent_hash;
        *out_len = node->parent.parent_hash_len;
        return 1;
    }
    if (node->type == MLS_NODE_LEAF && node->leaf.leaf_node_source == 3) {
        *out = node->leaf.parent_hash;
        *out_len = node->leaf.parent_hash_len;
        return 1;
    }
    return 0;
}

static int
parent_unmerged_contains(const MlsParentNode *parent, uint32_t leaf_idx)
{
    if (!parent) return 0;
    for (size_t i = 0; i < parent->unmerged_leaf_count; i++) {
        if (parent->unmerged_leaves[i] == leaf_idx) return 1;
    }
    return 0;
}

static int
parent_hash_step_valid(const MlsRatchetTree *tree, uint32_t child_node,
                       uint32_t parent_node)
{
    if (!tree || parent_node >= tree->n_nodes || child_node >= tree->n_nodes) return 0;
    const MlsNode *pnode = &tree->nodes[parent_node];
    if (pnode->type != MLS_NODE_PARENT) return 0;
    if (!subtree_contains_node(tree, parent_node, child_node)) return 0;

    const uint8_t *received = NULL;
    size_t received_len = 0;
    int has_hash = node_parent_hash_field(tree, child_node, &received, &received_len);
    if (has_hash != 1 || !received || received_len != MLS_HASH_LEN) return 0;

    uint8_t expected[MLS_HASH_LEN];
    if (mls_tree_parent_hash(tree, parent_node, child_node, expected) != 0) return 0;
    if (memcmp(received, expected, MLS_HASH_LEN) != 0) return 0;

    uint32_t left = mls_tree_left(parent_node);
    uint32_t right = mls_tree_right(parent_node);
    uint32_t path_child;
    if (subtree_contains_node(tree, left, child_node))
        path_child = left;
    else if (subtree_contains_node(tree, right, child_node))
        path_child = right;
    else
        return 0;

    uint32_t *resolution = calloc(tree->n_nodes ? tree->n_nodes : 1, sizeof(uint32_t));
    if (!resolution) return 0;
    uint32_t resolution_len = 0;
    if (mls_tree_resolution(tree, path_child, resolution, tree->n_nodes, &resolution_len) != 0) {
        free(resolution);
        return 0;
    }

    int child_in_resolution = 0;
    size_t resolution_without_child = 0;
    for (uint32_t i = 0; i < resolution_len; i++) {
        if (resolution[i] == child_node) {
            child_in_resolution = 1;
        } else {
            resolution_without_child++;
            if (!mls_tree_is_leaf(resolution[i])) {
                free(resolution);
                return 0;
            }
            uint32_t leaf_idx = mls_tree_node_to_leaf(resolution[i]);
            if (!parent_unmerged_contains(&pnode->parent, leaf_idx)) {
                free(resolution);
                return 0;
            }
        }
    }
    if (!child_in_resolution) {
        free(resolution);
        return 0;
    }

    size_t unmerged_in_subtree = 0;
    for (size_t i = 0; i < pnode->parent.unmerged_leaf_count; i++) {
        uint32_t leaf_node = mls_tree_leaf_to_node(pnode->parent.unmerged_leaves[i]);
        if (subtree_contains_node(tree, path_child, leaf_node))
            unmerged_in_subtree++;
    }
    free(resolution);
    return unmerged_in_subtree == resolution_without_child;
}

int
mls_tree_verify_parent_hashes(const MlsRatchetTree *tree)
{
    if (!tree || tree->n_leaves == 0 || !tree->nodes) return -1;
    uint32_t root = mls_tree_root(tree->n_leaves);

    int *valid_parent = calloc(tree->n_nodes ? tree->n_nodes : 1, sizeof(int));
    if (!valid_parent) return -1;

    int changed = 1;
    while (changed) {
        changed = 0;
        for (uint32_t p = 0; p < tree->n_nodes; p++) {
            if (valid_parent[p] || tree->nodes[p].type != MLS_NODE_PARENT)
                continue;

            for (uint32_t d = 0; d < tree->n_nodes; d++) {
                if (d == p) continue;
                int candidate = 0;
                if (tree->nodes[d].type == MLS_NODE_LEAF &&
                    tree->nodes[d].leaf.leaf_node_source == 3) {
                    candidate = 1;
                } else if (tree->nodes[d].type == MLS_NODE_PARENT && valid_parent[d]) {
                    candidate = 1;
                }
                if (!candidate) continue;
                if (parent_hash_step_valid(tree, d, p)) {
                    valid_parent[p] = 1;
                    changed = 1;
                    break;
                }
            }
        }
    }

    for (uint32_t i = 0; i < tree->n_nodes; i++) {
        if (tree->nodes[i].type == MLS_NODE_PARENT && !valid_parent[i]) {
            free(valid_parent);
            return -1;
        }
        if (i == root && tree->nodes[i].type == MLS_NODE_PARENT &&
            tree->nodes[i].parent.parent_hash_len != 0) {
            free(valid_parent);
            return -1;
        }
    }

    free(valid_parent);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Path secret derivation & UpdatePath encryption (RFC 9420 §7.4–7.5)
 *
 * path_secret[0] = random
 * path_secret[n] = DeriveSecret(path_secret[n-1], "path")
 * node_secret[n] = DeriveSecret(path_secret[n], "node")
 * node_priv[n], node_pub[n] = DeriveKeyPair(node_secret[n])
 *
 * For each node in the filtered direct path, encrypt the path_secret
 * to each node in the copath resolution using HPKE.
 *
 * commit_secret = DeriveSecret(path_secret[n], "path")
 *   where n is the last filtered path index
 * ══════════════════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════════════════
 * Path secret derivation helpers (RFC 9420 §7.4)
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_tree_derive_node_keypair(const uint8_t path_secret[MLS_HASH_LEN],
                              uint8_t node_sk_out[MLS_KEM_SK_LEN],
                              uint8_t node_pk_out[MLS_KEM_PK_LEN])
{
    if (!path_secret || !node_sk_out || !node_pk_out) return -1;

    /* node_secret = DeriveSecret(path_secret, "node") */
    uint8_t node_secret[MLS_HASH_LEN];
    if (mls_crypto_derive_secret(node_secret, path_secret, "node") != 0) return -1;

    /* RFC 9420 TreeKEM uses HPKE DHKEM DeriveKeyPair(node_secret). */
    int rc = mls_crypto_kem_derive_keypair(node_secret, MLS_HASH_LEN,
                                           node_sk_out, node_pk_out);
    memset(node_secret, 0, MLS_HASH_LEN);
    return rc;
}

int
mls_tree_derive_next_path_secret(const uint8_t current[MLS_HASH_LEN],
                                  uint8_t next_out[MLS_HASH_LEN])
{
    if (!current || !next_out) return -1;
    return mls_crypto_derive_secret(next_out, current, "path");
}

const uint8_t *
mls_tree_node_encryption_key(const MlsRatchetTree *tree, uint32_t node_idx)
{
    if (!tree || node_idx >= tree->n_nodes) return NULL;
    const MlsNode *node = &tree->nodes[node_idx];
    switch (node->type) {
        case MLS_NODE_LEAF:   return node->leaf.encryption_key;
        case MLS_NODE_PARENT: return node->parent.encryption_key;
        default:              return NULL;
    }
}
