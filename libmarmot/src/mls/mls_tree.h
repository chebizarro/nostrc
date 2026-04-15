/*
 * libmarmot - MLS Ratchet Tree (RFC 9420 §4, §7, Appendix C)
 *
 * Array-based left-balanced binary tree for TreeKEM.
 * Uses even indices for leaves, odd for parents.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MLS_TREE_H
#define MLS_TREE_H

#include "mls-internal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * Array-based tree math (Appendix C)
 *
 * Leaf index L maps to node index 2*L.
 * Parent nodes live at odd indices.
 * All functions take 'n' = number of leaves.
 * ──────────────────────────────────────────────────────────────────────── */

/** Level of node x. Leaves are level 0. */
uint32_t mls_tree_level(uint32_t x);

/** Total nodes needed for n leaves: 2*(n-1)+1 for n>0, 0 for n=0. */
uint32_t mls_tree_node_width(uint32_t n);

/** Root node index for tree with n leaves. */
uint32_t mls_tree_root(uint32_t n);

/** Left child of intermediate node x. */
uint32_t mls_tree_left(uint32_t x);

/** Right child of intermediate node x. */
uint32_t mls_tree_right(uint32_t x);

/** Parent of node x in tree with n leaves. */
uint32_t mls_tree_parent(uint32_t x, uint32_t n);

/** Sibling of node x in tree with n leaves. */
uint32_t mls_tree_sibling(uint32_t x, uint32_t n);

/**
 * Direct path from node x to root (exclusive of x, inclusive of root).
 * Returns count of entries written. path must have room for at least
 * log2(n)+1 entries.
 */
uint32_t mls_tree_direct_path(uint32_t x, uint32_t n,
                               uint32_t *path, uint32_t max_len);

/**
 * Copath of node x: siblings of nodes on the direct path.
 * Returns count. copath must have room for at least log2(n)+1 entries.
 */
uint32_t mls_tree_copath(uint32_t x, uint32_t n,
                          uint32_t *copath, uint32_t max_len);

/** Convert leaf index to node index. */
static inline uint32_t mls_tree_leaf_to_node(uint32_t leaf_idx) {
    return leaf_idx * 2;
}

/** Convert node index to leaf index (only valid for even node indices). */
static inline uint32_t mls_tree_node_to_leaf(uint32_t node_idx) {
    return node_idx / 2;
}

/** Check if node index is a leaf. */
static inline bool mls_tree_is_leaf(uint32_t x) {
    return (x & 1) == 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Ratchet tree node types
 * ──────────────────────────────────────────────────────────────────────── */

/** Credential type (we only support basic for Marmot) */
#define MLS_CREDENTIAL_BASIC    0x0001

/**
 * MlsLeafNode:
 *
 * A leaf in the ratchet tree. Contains the member's HPKE public key,
 * signing identity, and metadata.
 */
typedef struct {
    uint8_t  encryption_key[MLS_KEM_PK_LEN];  /**< HPKE public key */
    uint8_t  signature_key[MLS_SIG_PK_LEN];   /**< Ed25519 public key */
    uint16_t credential_type;                  /**< MLS_CREDENTIAL_BASIC */
    uint8_t *credential_identity;              /**< Basic credential identity */
    size_t   credential_identity_len;

    /** Capabilities (RFC 9420 §7.2) */
    uint16_t *versions;                        /**< Supported protocol versions */
    size_t    version_count;
    uint16_t *ciphersuites;                    /**< Supported ciphersuites */
    size_t    ciphersuite_count;
    uint16_t *cap_extensions;                  /**< Supported extension types */
    size_t    cap_extension_count;
    uint16_t *proposals;                       /**< Supported proposal types */
    size_t    proposal_count;
    uint16_t *cap_credentials;                 /**< Supported credential types */
    size_t    cap_credential_count;

    /** Leaf node source: key_package(1), update(2), commit(3) */
    uint8_t   leaf_node_source;

    /** Lifetime (present when leaf_node_source == key_package) */
    uint64_t  lifetime_not_before;
    uint64_t  lifetime_not_after;

    /** Parent hash (present when leaf_node_source == commit) */
    uint8_t  *parent_hash;
    size_t    parent_hash_len;

    /** Extensions (serialized TLS) */
    uint8_t  *extensions_data;
    size_t    extensions_len;

    /** Signature over LeafNodeTBS */
    uint8_t   signature[MLS_SIG_LEN];
    size_t    signature_len;                   /**< 0 if not yet signed */
} MlsLeafNode;

/**
 * MlsParentNode:
 *
 * An interior node in the ratchet tree.
 */
typedef struct {
    uint8_t   encryption_key[MLS_KEM_PK_LEN]; /**< HPKE public key */
    uint8_t  *parent_hash;                     /**< Hash linking to ancestor */
    size_t    parent_hash_len;
    uint32_t *unmerged_leaves;                 /**< Leaf indices not yet merged */
    size_t    unmerged_leaf_count;
} MlsParentNode;

/**
 * MlsNode:
 *
 * A node in the array-based ratchet tree. May be blank, a leaf, or a parent.
 */
typedef struct {
    enum { MLS_NODE_BLANK, MLS_NODE_LEAF, MLS_NODE_PARENT } type;
    union {
        MlsLeafNode   leaf;
        MlsParentNode parent;
    };
} MlsNode;

/**
 * MlsRatchetTree:
 *
 * The complete ratchet tree. Array-based: node at index 2*i is leaf i,
 * odd indices are parents.
 */
typedef struct {
    MlsNode *nodes;          /**< Array of nodes, length = node_width(n_leaves) */
    uint32_t n_leaves;       /**< Number of leaves */
    uint32_t n_nodes;        /**< = node_width(n_leaves) */
} MlsRatchetTree;

/* ──────────────────────────────────────────────────────────────────────────
 * Ratchet tree lifecycle
 * ──────────────────────────────────────────────────────────────────────── */

/** Create a new ratchet tree with the given number of leaves (all blank). */
int mls_tree_new(MlsRatchetTree *tree, uint32_t n_leaves);

/** Free a ratchet tree's internal storage. */
void mls_tree_free(MlsRatchetTree *tree);

/** Extend the tree to accommodate a new leaf at the right edge. Returns the
 *  new leaf's node index. */
int mls_tree_add_leaf(MlsRatchetTree *tree, uint32_t *out_leaf_node_idx);

/** Blank a node (free its contents, set to BLANK). */
void mls_tree_blank_node(MlsNode *node);

/** Deep-copy a leaf node into dst. */
int mls_leaf_node_clone(MlsLeafNode *dst, const MlsLeafNode *src);

/** Free leaf node internals (but not the struct itself). */
void mls_leaf_node_clear(MlsLeafNode *node);

/** Free parent node internals. */
void mls_parent_node_clear(MlsParentNode *node);

/* ──────────────────────────────────────────────────────────────────────────
 * Resolution (RFC 9420 §4.1.1)
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Compute the resolution of a node. Resolution is the set of non-blank
 * nodes that collectively cover all non-blank descendants.
 *
 * @param tree      The ratchet tree
 * @param node_idx  Node to resolve
 * @param out       Array of node indices (caller-allocated, max n_nodes)
 * @param out_len   Number of entries written
 * @return 0 on success
 */
int mls_tree_resolution(const MlsRatchetTree *tree, uint32_t node_idx,
                        uint32_t *out, uint32_t *out_len);

/* ──────────────────────────────────────────────────────────────────────────
 * Filtered direct path (RFC 9420 §4.1.2)
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Compute the filtered direct path of a leaf.
 *
 * The filtered direct path is the direct path with nodes removed whose
 * copath child has an empty resolution.
 *
 * @param tree      The ratchet tree
 * @param leaf_idx  Leaf index (not node index)
 * @param out       Array of node indices (caller-allocated)
 * @param out_len   Number of entries written
 * @return 0 on success
 */
int mls_tree_filtered_direct_path(const MlsRatchetTree *tree, uint32_t leaf_idx,
                                   uint32_t *out, uint32_t *out_len);

/* ──────────────────────────────────────────────────────────────────────────
 * Tree hash (RFC 9420 §7.8)
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Compute the tree hash of a node.
 *
 * @param tree      The ratchet tree
 * @param node_idx  Node to hash
 * @param out       Output hash (MLS_HASH_LEN bytes)
 * @return 0 on success
 */
int mls_tree_hash(const MlsRatchetTree *tree, uint32_t node_idx,
                  uint8_t out[MLS_HASH_LEN]);

/**
 * Compute the tree hash of the root (the overall tree hash).
 */
int mls_tree_root_hash(const MlsRatchetTree *tree, uint8_t out[MLS_HASH_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * TLS serialization for tree nodes
 * ──────────────────────────────────────────────────────────────────────── */

/* ──────────────────────────────────────────────────────────────────────────
 * Parent hash (RFC 9420 §7.9)
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Compute the parent hash for a parent node (RFC 9420 §7.9).
 *
 * parent_hash = H(ParentHashInput)
 * ParentHashInput = { encryption_key, parent_hash_of_parent, original_sibling_tree_hash }
 *
 * @param tree              The ratchet tree
 * @param parent_node_idx   Index of the parent node
 * @param original_child    Index of the child being updated (to determine sibling)
 * @param out               Output hash (MLS_HASH_LEN bytes)
 * @return 0 on success
 */
int mls_tree_parent_hash(const MlsRatchetTree *tree, uint32_t parent_node_idx,
                          uint32_t original_child, uint8_t out[MLS_HASH_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * Path secret derivation helpers (RFC 9420 §7.4)
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Derive node keys from a path secret.
 *
 * node_secret = DeriveSecret(path_secret, "node")
 * Then derives HPKE keypair from node_secret.
 *
 * @param path_secret   Input path secret (MLS_HASH_LEN bytes)
 * @param node_sk_out   Output HPKE private key (MLS_KEM_SK_LEN)
 * @param node_pk_out   Output HPKE public key (MLS_KEM_PK_LEN)
 * @return 0 on success
 */
int mls_tree_derive_node_keypair(const uint8_t path_secret[MLS_HASH_LEN],
                                  uint8_t node_sk_out[MLS_KEM_SK_LEN],
                                  uint8_t node_pk_out[MLS_KEM_PK_LEN]);

/**
 * Derive the next path secret from the current one.
 *
 * path_secret[n+1] = DeriveSecret(path_secret[n], "path")
 *
 * @param current   Current path secret (MLS_HASH_LEN)
 * @param next_out  Output next path secret (MLS_HASH_LEN)
 * @return 0 on success
 */
int mls_tree_derive_next_path_secret(const uint8_t current[MLS_HASH_LEN],
                                      uint8_t next_out[MLS_HASH_LEN]);

/**
 * Get the HPKE public key for a node in the tree.
 *
 * @param tree      The ratchet tree
 * @param node_idx  Node index
 * @return pointer to MLS_KEM_PK_LEN bytes, or NULL if node is blank
 */
const uint8_t *mls_tree_node_encryption_key(const MlsRatchetTree *tree,
                                             uint32_t node_idx);

/* ──────────────────────────────────────────────────────────────────────────
 * TLS serialization for tree nodes
 * ──────────────────────────────────────────────────────────────────────── */

/** Serialize a LeafNode to TLS format. */
int mls_leaf_node_serialize(const MlsLeafNode *node, MlsTlsBuf *buf);

/** Serialize a ParentNode to TLS format. */
int mls_parent_node_serialize(const MlsParentNode *node, MlsTlsBuf *buf);

/** Deserialize a LeafNode from TLS format. */
int mls_leaf_node_deserialize(MlsTlsReader *reader, MlsLeafNode *node);

/** Deserialize a ParentNode from TLS format. */
int mls_parent_node_deserialize(MlsTlsReader *reader, MlsParentNode *node);

#ifdef __cplusplus
}
#endif

#endif /* MLS_TREE_H */
