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

uint32_t
mls_tree_direct_path(uint32_t x, uint32_t n,
                      uint32_t *path, uint32_t max_len)
{
    uint32_t r = mls_tree_root(n);
    if (x == r) return 0;

    uint32_t count = 0;
    uint32_t cur = x;
    while (cur != r && count < max_len) {
        cur = mls_tree_parent(cur, n);
        path[count++] = cur;
    }
    return count;
}

uint32_t
mls_tree_copath(uint32_t x, uint32_t n,
                 uint32_t *copath, uint32_t max_len)
{
    uint32_t r = mls_tree_root(n);
    if (x == r) return 0;

    /* Build the path from x to root (inclusive of x, exclusive of root) */
    uint32_t dp[64]; /* max tree depth */
    uint32_t dp_len = 0;

    if (dp_len >= 64) return 0; /* Safety check */
    dp[dp_len++] = x;
    uint32_t cur = x;
    while (cur != r) {
        cur = mls_tree_parent(cur, n);
        if (cur != r) {
            if (dp_len >= 64) return 0; /* Prevent overflow */
            dp[dp_len++] = cur;
        }
    }

    /* Copath = siblings of each node in dp */
    uint32_t count = 0;
    for (uint32_t i = 0; i < dp_len && count < max_len; i++) {
        copath[count++] = mls_tree_sibling(dp[i], n);
    }
    return count;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Node lifecycle
 * ══════════════════════════════════════════════════════════════════════════ */

void
mls_leaf_node_clear(MlsLeafNode *node)
{
    if (!node) return;
    free(node->credential_identity);
    free(node->ciphersuites);
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
    dst->ciphersuite_count = src->ciphersuite_count;
    dst->extensions_len = src->extensions_len;
    memcpy(dst->signature, src->signature, MLS_SIG_LEN);
    dst->signature_len = src->signature_len;
    dst->parent_hash_len = src->parent_hash_len;
    dst->leaf_node_source = src->leaf_node_source;

    /* Deep-copy heap allocations */
    dst->credential_identity = NULL;
    dst->ciphersuites = NULL;
    dst->extensions_data = NULL;
    dst->parent_hash = NULL;

    if (src->credential_identity && src->credential_identity_len > 0) {
        dst->credential_identity = malloc(src->credential_identity_len);
        if (!dst->credential_identity) goto fail;
        memcpy(dst->credential_identity, src->credential_identity, src->credential_identity_len);
    }
    if (src->ciphersuites && src->ciphersuite_count > 0) {
        size_t sz = src->ciphersuite_count * sizeof(uint16_t);
        dst->ciphersuites = malloc(sz);
        if (!dst->ciphersuites) goto fail;
        memcpy(dst->ciphersuites, src->ciphersuites, sz);
    }
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

    uint32_t new_n_leaves = tree->n_leaves + 1;
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

    uint32_t new_leaf_node_idx = mls_tree_leaf_to_node(new_n_leaves - 1);
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
                    uint32_t *out, uint32_t *out_len)
{
    if (!tree || !out || !out_len) return -1;
    *out_len = 0;
    return resolution_recursive(tree, node_idx, out, out_len, tree->n_nodes);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Filtered direct path (RFC 9420 §4.1.2)
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_tree_filtered_direct_path(const MlsRatchetTree *tree, uint32_t leaf_idx,
                               uint32_t *out, uint32_t *out_len)
{
    if (!tree || !out || !out_len) return -1;
    *out_len = 0;

    uint32_t node_idx = mls_tree_leaf_to_node(leaf_idx);
    uint32_t r = mls_tree_root(tree->n_leaves);

    /* Walk from leaf to root */
    uint32_t cur = node_idx;
    while (cur != r) {
        uint32_t p = mls_tree_parent(cur, tree->n_leaves);
        /* Find the copath child (sibling of cur under p) */
        uint32_t copath_child = mls_tree_sibling(cur, tree->n_leaves);

        /* Check if copath child has non-empty resolution */
        uint32_t res[256];
        uint32_t res_len = 0;
        int rc = mls_tree_resolution(tree, copath_child, res, &res_len);
        if (rc != 0) return rc;

        if (res_len > 0) {
            /* Copath child has non-empty resolution: include p in filtered path */
            out[(*out_len)++] = p;
        }
        /* Else: skip p (copath child is empty) */

        cur = p;
    }
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
    /* capabilities: ciphersuites<V> */
    {
        /* We encode as opaque: 2 bytes per ciphersuite, wrapped in opaque16 */
        /* Check for overflow before casting to uint16_t */
        if (node->ciphersuite_count > (UINT16_MAX / 2)) return -1;
        uint16_t total = (uint16_t)(node->ciphersuite_count * 2);
        if (mls_tls_write_u16(buf, total) != 0) return -1;
        for (size_t i = 0; i < node->ciphersuite_count; i++) {
            if (mls_tls_write_u16(buf, node->ciphersuites[i]) != 0) return -1;
        }
    }
    /* leaf_node_source: uint8 */
    if (mls_tls_write_u8(buf, node->leaf_node_source) != 0) return -1;
    /* extensions: opaque<V> */
    if (mls_tls_write_opaque32(buf, node->extensions_data, node->extensions_len) != 0)
        return -1;
    /* signature: opaque<V> */
    if (mls_tls_write_opaque16(buf, node->signature, node->signature_len) != 0)
        return -1;
    /* parent_hash (optional, present when leaf_node_source == commit) */
    if (node->leaf_node_source == 3) { /* commit */
        if (mls_tls_write_opaque8(buf, node->parent_hash, node->parent_hash_len) != 0)
            return -1;
    }
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
    /* unmerged_leaves: uint32 list, wrapped in opaque16 */
    {
        /* Check for overflow before casting to uint16_t */
        if (node->unmerged_leaf_count > (UINT16_MAX / 4)) return -1;
        uint16_t total = (uint16_t)(node->unmerged_leaf_count * 4);
        if (mls_tls_write_u16(buf, total) != 0) return -1;
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

    /* ciphersuites */
    uint16_t cs_bytes;
    if (mls_tls_read_u16(reader, &cs_bytes) != 0) return -1;
    /* Validate that cs_bytes is even (2 bytes per ciphersuite) */
    if (cs_bytes % 2 != 0) return -1;
    node->ciphersuite_count = cs_bytes / 2;
    if (node->ciphersuite_count > 0) {
        node->ciphersuites = malloc(node->ciphersuite_count * sizeof(uint16_t));
        if (!node->ciphersuites) return -1;
        for (size_t i = 0; i < node->ciphersuite_count; i++) {
            if (mls_tls_read_u16(reader, &node->ciphersuites[i]) != 0) return -1;
        }
    }

    /* leaf_node_source */
    if (mls_tls_read_u8(reader, &node->leaf_node_source) != 0) return -1;

    /* extensions */
    if (mls_tls_read_opaque32(reader, &node->extensions_data,
                               &node->extensions_len) != 0)
        return -1;

    /* signature */
    uint8_t *sig = NULL; size_t sig_len = 0;
    if (mls_tls_read_opaque16(reader, &sig, &sig_len) != 0) return -1;
    if (sig_len > MLS_SIG_LEN) { free(sig); return -1; }
    if (sig_len > 0) memcpy(node->signature, sig, sig_len);
    node->signature_len = sig_len;
    free(sig);

    /* parent_hash (if commit) */
    if (node->leaf_node_source == 3) {
        if (mls_tls_read_opaque8(reader, &node->parent_hash,
                                  &node->parent_hash_len) != 0)
            return -1;
    }
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

    /* unmerged_leaves */
    uint16_t ul_bytes;
    if (mls_tls_read_u16(reader, &ul_bytes) != 0) return -1;
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
        if (mls_tree_hash(tree, mls_tree_right(node_idx), right_hash) != 0) goto fail;
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
