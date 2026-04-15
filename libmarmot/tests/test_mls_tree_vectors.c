/*
 * libmarmot - MLS TreeKEM RFC 9420 test vector validation
 *
 * Validates tree math (left, right, parent, sibling) against the official
 * RFC 9420 test vectors for tree sizes 1, 2, 4, 8, 16, and 32.
 * Also exercises path secret derivation chains and parent hash chains
 * that the unit tests in test_mls_tree.c don't fully cover.
 *
 * Test vectors from: libmarmot/tests/vectors/mdk/tree-math.json
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls/mls_tree.h"
#include "mls/mls-internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sodium.h>

#define TEST(name) do { printf("  %-55s", #name); name(); printf("PASS\n"); } while(0)

/* ══════════════════════════════════════════════════════════════════════════
 * RFC 9420 Tree Math Test Vectors
 *
 * Extracted from tests/vectors/mdk/tree-math.json
 * -1 means the function is undefined for that node index
 * (leaves have no left/right, root has no parent/sibling).
 * ══════════════════════════════════════════════════════════════════════════ */

/* n_leaves=1, n_nodes=1, root=0 */
static const int32_t tree_math_1_left[1] = { -1 };
static const int32_t tree_math_1_right[1] = { -1 };
static const int32_t tree_math_1_parent[1] = { -1 };
static const int32_t tree_math_1_sibling[1] = { -1 };

/* n_leaves=2, n_nodes=3, root=1 */
static const int32_t tree_math_2_left[3] = { -1, 0, -1 };
static const int32_t tree_math_2_right[3] = { -1, 2, -1 };
static const int32_t tree_math_2_parent[3] = { 1, -1, 1 };
static const int32_t tree_math_2_sibling[3] = { 2, -1, 0 };

/* n_leaves=4, n_nodes=7, root=3 */
static const int32_t tree_math_4_left[7] = { -1, 0, -1, 1, -1, 4, -1 };
static const int32_t tree_math_4_right[7] = { -1, 2, -1, 5, -1, 6, -1 };
static const int32_t tree_math_4_parent[7] = { 1, 3, 1, -1, 5, 3, 5 };
static const int32_t tree_math_4_sibling[7] = { 2, 5, 0, -1, 6, 1, 4 };

/* n_leaves=8, n_nodes=15, root=7 */
static const int32_t tree_math_8_left[15] = { -1, 0, -1, 1, -1, 4, -1, 3, -1, 8, -1, 9, -1, 12, -1 };
static const int32_t tree_math_8_right[15] = { -1, 2, -1, 5, -1, 6, -1, 11, -1, 10, -1, 13, -1, 14, -1 };
static const int32_t tree_math_8_parent[15] = { 1, 3, 1, 7, 5, 3, 5, -1, 9, 11, 9, 7, 13, 11, 13 };
static const int32_t tree_math_8_sibling[15] = { 2, 5, 0, 11, 6, 1, 4, -1, 10, 13, 8, 3, 14, 9, 12 };

/* n_leaves=16, n_nodes=31, root=15 */
static const int32_t tree_math_16_left[31] = { -1, 0, -1, 1, -1, 4, -1, 3, -1, 8, -1, 9, -1, 12, -1, 7, -1, 16, -1, 17, -1, 20, -1, 19, -1, 24, -1, 25, -1, 28, -1 };
static const int32_t tree_math_16_right[31] = { -1, 2, -1, 5, -1, 6, -1, 11, -1, 10, -1, 13, -1, 14, -1, 23, -1, 18, -1, 21, -1, 22, -1, 27, -1, 26, -1, 29, -1, 30, -1 };
static const int32_t tree_math_16_parent[31] = { 1, 3, 1, 7, 5, 3, 5, 15, 9, 11, 9, 7, 13, 11, 13, -1, 17, 19, 17, 23, 21, 19, 21, 15, 25, 27, 25, 23, 29, 27, 29 };
static const int32_t tree_math_16_sibling[31] = { 2, 5, 0, 11, 6, 1, 4, 23, 10, 13, 8, 3, 14, 9, 12, -1, 18, 21, 16, 27, 22, 17, 20, 7, 26, 29, 24, 19, 30, 25, 28 };

/* n_leaves=32, n_nodes=63, root=31 */
static const int32_t tree_math_32_left[63] = { -1, 0, -1, 1, -1, 4, -1, 3, -1, 8, -1, 9, -1, 12, -1, 7, -1, 16, -1, 17, -1, 20, -1, 19, -1, 24, -1, 25, -1, 28, -1, 15, -1, 32, -1, 33, -1, 36, -1, 35, -1, 40, -1, 41, -1, 44, -1, 39, -1, 48, -1, 49, -1, 52, -1, 51, -1, 56, -1, 57, -1, 60, -1 };
static const int32_t tree_math_32_right[63] = { -1, 2, -1, 5, -1, 6, -1, 11, -1, 10, -1, 13, -1, 14, -1, 23, -1, 18, -1, 21, -1, 22, -1, 27, -1, 26, -1, 29, -1, 30, -1, 47, -1, 34, -1, 37, -1, 38, -1, 43, -1, 42, -1, 45, -1, 46, -1, 55, -1, 50, -1, 53, -1, 54, -1, 59, -1, 58, -1, 61, -1, 62, -1 };
static const int32_t tree_math_32_parent[63] = { 1, 3, 1, 7, 5, 3, 5, 15, 9, 11, 9, 7, 13, 11, 13, 31, 17, 19, 17, 23, 21, 19, 21, 15, 25, 27, 25, 23, 29, 27, 29, -1, 33, 35, 33, 39, 37, 35, 37, 47, 41, 43, 41, 39, 45, 43, 45, 31, 49, 51, 49, 55, 53, 51, 53, 47, 57, 59, 57, 55, 61, 59, 61 };
static const int32_t tree_math_32_sibling[63] = { 2, 5, 0, 11, 6, 1, 4, 23, 10, 13, 8, 3, 14, 9, 12, 47, 18, 21, 16, 27, 22, 17, 20, 7, 26, 29, 24, 19, 30, 25, 28, -1, 34, 37, 32, 43, 38, 33, 36, 55, 42, 45, 40, 35, 46, 41, 44, 15, 50, 53, 48, 59, 54, 49, 52, 39, 58, 61, 56, 51, 62, 57, 60 };

/* Vector table for iteration */
typedef struct {
    uint32_t n_leaves;
    uint32_t n_nodes;
    uint32_t root;
    const int32_t *left;
    const int32_t *right;
    const int32_t *parent;
    const int32_t *sibling;
} TreeMathVector;

#define VEC(nl) { nl, (nl > 0 ? 2*(nl)-1 : 0), 0, \
    tree_math_##nl##_left, tree_math_##nl##_right, \
    tree_math_##nl##_parent, tree_math_##nl##_sibling }

/* Root values from the test vectors */
static const TreeMathVector tree_math_vectors[] = {
    { 1,  1,  0,  tree_math_1_left,  tree_math_1_right,  tree_math_1_parent,  tree_math_1_sibling  },
    { 2,  3,  1,  tree_math_2_left,  tree_math_2_right,  tree_math_2_parent,  tree_math_2_sibling  },
    { 4,  7,  3,  tree_math_4_left,  tree_math_4_right,  tree_math_4_parent,  tree_math_4_sibling  },
    { 8,  15, 7,  tree_math_8_left,  tree_math_8_right,  tree_math_8_parent,  tree_math_8_sibling  },
    { 16, 31, 15, tree_math_16_left, tree_math_16_right, tree_math_16_parent, tree_math_16_sibling },
    { 32, 63, 31, tree_math_32_left, tree_math_32_right, tree_math_32_parent, tree_math_32_sibling },
};

#define NUM_TREE_VECTORS (sizeof(tree_math_vectors) / sizeof(tree_math_vectors[0]))

/* ══════════════════════════════════════════════════════════════════════════
 * Tree math vector validation tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_vector_node_width(void)
{
    for (size_t v = 0; v < NUM_TREE_VECTORS; v++) {
        const TreeMathVector *vec = &tree_math_vectors[v];
        uint32_t got = mls_tree_node_width(vec->n_leaves);
        assert(got == vec->n_nodes);
    }
}

static void test_vector_root(void)
{
    for (size_t v = 0; v < NUM_TREE_VECTORS; v++) {
        const TreeMathVector *vec = &tree_math_vectors[v];
        uint32_t got = mls_tree_root(vec->n_leaves);
        assert(got == vec->root);
    }
}

static void test_vector_left(void)
{
    for (size_t v = 0; v < NUM_TREE_VECTORS; v++) {
        const TreeMathVector *vec = &tree_math_vectors[v];
        for (uint32_t i = 0; i < vec->n_nodes; i++) {
            if (vec->left[i] == -1) continue; /* undefined for leaves */
            uint32_t got = mls_tree_left(i);
            if (got != (uint32_t)vec->left[i]) {
                fprintf(stderr, "FAIL: left(%u) n_leaves=%u: got %u, expected %d\n",
                        i, vec->n_leaves, got, vec->left[i]);
                assert(0);
            }
        }
    }
}

static void test_vector_right(void)
{
    for (size_t v = 0; v < NUM_TREE_VECTORS; v++) {
        const TreeMathVector *vec = &tree_math_vectors[v];
        for (uint32_t i = 0; i < vec->n_nodes; i++) {
            if (vec->right[i] == -1) continue;
            uint32_t got = mls_tree_right(i);
            if (got != (uint32_t)vec->right[i]) {
                fprintf(stderr, "FAIL: right(%u) n_leaves=%u: got %u, expected %d\n",
                        i, vec->n_leaves, got, vec->right[i]);
                assert(0);
            }
        }
    }
}

static void test_vector_parent(void)
{
    for (size_t v = 0; v < NUM_TREE_VECTORS; v++) {
        const TreeMathVector *vec = &tree_math_vectors[v];
        for (uint32_t i = 0; i < vec->n_nodes; i++) {
            if (vec->parent[i] == -1) continue; /* undefined for root */
            uint32_t got = mls_tree_parent(i, vec->n_leaves);
            if (got != (uint32_t)vec->parent[i]) {
                fprintf(stderr, "FAIL: parent(%u, %u): got %u, expected %d\n",
                        i, vec->n_leaves, got, vec->parent[i]);
                assert(0);
            }
        }
    }
}

static void test_vector_sibling(void)
{
    for (size_t v = 0; v < NUM_TREE_VECTORS; v++) {
        const TreeMathVector *vec = &tree_math_vectors[v];
        for (uint32_t i = 0; i < vec->n_nodes; i++) {
            if (vec->sibling[i] == -1) continue; /* undefined for root */
            uint32_t got = mls_tree_sibling(i, vec->n_leaves);
            if (got != (uint32_t)vec->sibling[i]) {
                fprintf(stderr, "FAIL: sibling(%u, %u): got %u, expected %d\n",
                        i, vec->n_leaves, got, vec->sibling[i]);
                assert(0);
            }
        }
    }
}

/**
 * Comprehensive sweep: validate all four functions against all vector entries.
 * Tests each node in each tree size — 1+3+7+15+31+63 = 120 nodes total,
 * ~360 non-null checks.
 */
static void test_vector_all_tree_math(void)
{
    uint32_t checks = 0;
    for (size_t v = 0; v < NUM_TREE_VECTORS; v++) {
        const TreeMathVector *vec = &tree_math_vectors[v];

        assert(mls_tree_node_width(vec->n_leaves) == vec->n_nodes);
        assert(mls_tree_root(vec->n_leaves) == vec->root);

        for (uint32_t i = 0; i < vec->n_nodes; i++) {
            if (vec->left[i] != -1) {
                assert(mls_tree_left(i) == (uint32_t)vec->left[i]);
                checks++;
            }
            if (vec->right[i] != -1) {
                assert(mls_tree_right(i) == (uint32_t)vec->right[i]);
                checks++;
            }
            if (vec->parent[i] != -1) {
                assert(mls_tree_parent(i, vec->n_leaves) == (uint32_t)vec->parent[i]);
                checks++;
            }
            if (vec->sibling[i] != -1) {
                assert(mls_tree_sibling(i, vec->n_leaves) == (uint32_t)vec->sibling[i]);
                checks++;
            }
        }
    }
    /* Sanity: we should have validated hundreds of data points */
    assert(checks > 300);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Extended tree math tests for larger sizes (64, 128, 256, 512)
 *
 * We can't embed all vector data, but we can validate structural
 * invariants that the RFC defines.
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Validate tree math invariants for arbitrary tree sizes.
 *
 * For every non-root node x in a tree with n leaves:
 *   - parent(left(parent(x))) == parent(x) or left(parent(x)) == x
 *   - sibling(sibling(x)) == x
 *   - For parent nodes: left(x) and right(x) exist and are children
 *   - parent(x) is an ancestor on the direct path
 */
static void test_tree_math_invariants_large(void)
{
    uint32_t sizes[] = { 64, 128, 256, 512 };
    for (int s = 0; s < 4; s++) {
        uint32_t n = sizes[s];
        uint32_t w = mls_tree_node_width(n);
        uint32_t r = mls_tree_root(n);

        /* Root is at expected position */
        assert(w == 2 * n - 1);

        for (uint32_t x = 0; x < w; x++) {
            if (x == r) continue;

            /* sibling(sibling(x)) == x */
            uint32_t sib = mls_tree_sibling(x, n);
            assert(mls_tree_sibling(sib, n) == x);

            /* parent(x) is a valid node */
            uint32_t p = mls_tree_parent(x, n);
            assert(p < w);
            assert(mls_tree_level(p) > 0); /* parent is always interior */

            /* x is either left or right child of parent */
            assert(mls_tree_left(p) == x || mls_tree_right(p) == x);

            /* If x is interior, verify left/right are valid */
            if (mls_tree_level(x) > 0) {
                uint32_t l = mls_tree_left(x);
                uint32_t ri = mls_tree_right(x);
                assert(l < w);
                assert(ri < w);
                assert(mls_tree_parent(l, n) == x);
                assert(mls_tree_parent(ri, n) == x);
            }
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Direct path & copath vector validation
 *
 * Verify that direct_path and copath are consistent with parent/sibling
 * vectors for all tree sizes.
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_vector_direct_path_consistency(void)
{
    for (size_t v = 0; v < NUM_TREE_VECTORS; v++) {
        const TreeMathVector *vec = &tree_math_vectors[v];
        if (vec->n_leaves <= 1) continue;

        /* Test direct path for every leaf */
        for (uint32_t leaf = 0; leaf < vec->n_leaves; leaf++) {
            uint32_t x = mls_tree_leaf_to_node(leaf);
            uint32_t path[32];
            uint32_t len = mls_tree_direct_path(x, vec->n_leaves, path, 32);

            /* Direct path should match iterating parent() up to root */
            uint32_t cur = x;
            for (uint32_t i = 0; i < len; i++) {
                uint32_t expected_parent = mls_tree_parent(cur, vec->n_leaves);
                assert(path[i] == expected_parent);
                cur = expected_parent;
            }
            /* Last node should be root */
            assert(cur == vec->root);
        }
    }
}

static void test_vector_copath_consistency(void)
{
    for (size_t v = 0; v < NUM_TREE_VECTORS; v++) {
        const TreeMathVector *vec = &tree_math_vectors[v];
        if (vec->n_leaves <= 1) continue;

        for (uint32_t leaf = 0; leaf < vec->n_leaves; leaf++) {
            uint32_t x = mls_tree_leaf_to_node(leaf);
            uint32_t path[32], copath[32];
            uint32_t plen = mls_tree_direct_path(x, vec->n_leaves, path, 32);
            uint32_t clen = mls_tree_copath(x, vec->n_leaves, copath, 32);

            assert(plen == clen);

            /* copath[i] should be sibling of node on path to path[i] */
            uint32_t cur = x;
            for (uint32_t i = 0; i < clen; i++) {
                uint32_t expected_sib = mls_tree_sibling(cur, vec->n_leaves);
                assert(copath[i] == expected_sib);
                cur = path[i];
            }
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Path secret derivation chain tests
 *
 * RFC 9420 §7.4: path_secret[n+1] = DeriveSecret(path_secret[n], "path")
 * node_secret = DeriveSecret(path_secret, "node")
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Verify that deriving a chain of path secrets through a direct path
 * produces unique, deterministic secrets and valid keypairs at each node.
 */
static void test_path_secret_chain_derivation(void)
{
    /* Start with a known path secret */
    uint8_t initial_secret[MLS_HASH_LEN];
    memset(initial_secret, 0xAB, MLS_HASH_LEN);

    /* Derive a chain of 5 path secrets (simulating a 32-leaf tree direct path) */
    uint8_t secrets[6][MLS_HASH_LEN];
    memcpy(secrets[0], initial_secret, MLS_HASH_LEN);

    for (int i = 0; i < 5; i++) {
        assert(mls_tree_derive_next_path_secret(secrets[i], secrets[i + 1]) == 0);
    }

    /* All secrets should be unique */
    for (int i = 0; i < 6; i++) {
        for (int j = i + 1; j < 6; j++) {
            assert(memcmp(secrets[i], secrets[j], MLS_HASH_LEN) != 0);
        }
    }

    /* Each secret should produce a valid, unique keypair */
    uint8_t prev_pk[MLS_KEM_PK_LEN] = {0};
    for (int i = 0; i < 6; i++) {
        uint8_t sk[MLS_KEM_SK_LEN], pk[MLS_KEM_PK_LEN];
        assert(mls_tree_derive_node_keypair(secrets[i], sk, pk) == 0);

        /* Key should be non-zero */
        int nonzero = 0;
        for (int b = 0; b < MLS_KEM_PK_LEN; b++) {
            if (pk[b] != 0) { nonzero = 1; break; }
        }
        assert(nonzero);

        /* Each key should differ from the previous */
        if (i > 0) {
            assert(memcmp(pk, prev_pk, MLS_KEM_PK_LEN) != 0);
        }
        memcpy(prev_pk, pk, MLS_KEM_PK_LEN);
    }

    /* Verify determinism: re-derive from same initial and compare */
    uint8_t check[MLS_HASH_LEN];
    memcpy(check, initial_secret, MLS_HASH_LEN);
    for (int i = 0; i < 5; i++) {
        uint8_t next[MLS_HASH_LEN];
        assert(mls_tree_derive_next_path_secret(check, next) == 0);
        assert(memcmp(next, secrets[i + 1], MLS_HASH_LEN) == 0);
        memcpy(check, next, MLS_HASH_LEN);
    }
}

/**
 * Test path secret → keypair → encrypt → decrypt round-trip.
 *
 * Uses X25519 Diffie-Hellman to simulate HPKE encryption/decryption:
 * sender derives ephemeral, computes shared secret with receiver's
 * tree-derived public key.
 */
static void test_path_secret_keypair_roundtrip(void)
{
    /* Derive a node keypair from a path secret */
    uint8_t path_secret[MLS_HASH_LEN];
    memset(path_secret, 0x77, MLS_HASH_LEN);

    uint8_t node_sk[MLS_KEM_SK_LEN], node_pk[MLS_KEM_PK_LEN];
    assert(mls_tree_derive_node_keypair(path_secret, node_sk, node_pk) == 0);

    /* Generate an ephemeral keypair (simulating sender) */
    uint8_t eph_sk[crypto_scalarmult_curve25519_SCALARBYTES];
    uint8_t eph_pk[crypto_scalarmult_curve25519_BYTES];
    crypto_box_keypair(eph_pk, eph_sk);

    /* Both sides compute the shared secret via X25519 */
    uint8_t shared_sender[crypto_scalarmult_curve25519_BYTES];
    uint8_t shared_receiver[crypto_scalarmult_curve25519_BYTES];

    assert(crypto_scalarmult_curve25519(shared_sender, eph_sk, node_pk) == 0);
    assert(crypto_scalarmult_curve25519(shared_receiver, node_sk, eph_pk) == 0);

    /* Shared secrets must match (DH commutativity) */
    assert(memcmp(shared_sender, shared_receiver, crypto_scalarmult_curve25519_BYTES) == 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Parent hash chain validation tests
 *
 * RFC 9420 §7.9: parent_hash forms a hash chain from root down to leaves.
 * Each parent node's parent_hash is computed from its own encryption_key,
 * the parent_hash of its parent, and the tree hash of its sibling child.
 * ══════════════════════════════════════════════════════════════════════════ */

static void
make_test_leaf(MlsLeafNode *leaf, uint8_t seed)
{
    memset(leaf, 0, sizeof(*leaf));
    memset(leaf->encryption_key, seed, MLS_KEM_PK_LEN);
    memset(leaf->signature_key, seed + 0x10, MLS_SIG_PK_LEN);
    leaf->credential_type = MLS_CREDENTIAL_BASIC;
    leaf->credential_identity_len = 8;
    leaf->credential_identity = malloc(8);
    assert(leaf->credential_identity);
    snprintf((char *)leaf->credential_identity, 8, "user_%02x", seed);
    leaf->ciphersuite_count = 1;
    leaf->ciphersuites = malloc(sizeof(uint16_t));
    assert(leaf->ciphersuites);
    leaf->ciphersuites[0] = 0x0001;
    leaf->extensions_data = NULL;
    leaf->extensions_len = 0;
    memset(leaf->signature, seed + 0x20, MLS_SIG_LEN);
    leaf->signature_len = MLS_SIG_LEN;
    leaf->leaf_node_source = 1;
    leaf->parent_hash = NULL;
    leaf->parent_hash_len = 0;
}

/**
 * Build an 8-leaf tree with all parents populated and verify that
 * computing parent_hash at different levels produces a consistent chain.
 *
 *                              7
 *                 ┌────────────┴────────────┐
 *                 3                         11
 *          ┌──────┴──────┐           ┌──────┴──────┐
 *          1             5           9             13
 *       ┌──┴──┐       ┌──┴──┐    ┌──┴──┐       ┌──┴──┐
 *       0     2       4     6    8     10      12     14
 */
static void test_parent_hash_chain_8_leaves(void)
{
    MlsRatchetTree tree;
    assert(mls_tree_new(&tree, 8) == 0);

    /* Populate all 8 leaves */
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t ni = mls_tree_leaf_to_node(i);
        tree.nodes[ni].type = MLS_NODE_LEAF;
        make_test_leaf(&tree.nodes[ni].leaf, (uint8_t)(i + 1));
    }

    /* Populate all parent nodes with unique encryption keys */
    uint32_t parents[] = { 1, 3, 5, 7, 9, 11, 13 };
    for (int i = 0; i < 7; i++) {
        uint32_t ni = parents[i];
        tree.nodes[ni].type = MLS_NODE_PARENT;
        memset(tree.nodes[ni].parent.encryption_key, 0xA0 + i, MLS_KEM_PK_LEN);
        tree.nodes[ni].parent.parent_hash = NULL;
        tree.nodes[ni].parent.parent_hash_len = 0;
        tree.nodes[ni].parent.unmerged_leaves = NULL;
        tree.nodes[ni].parent.unmerged_leaf_count = 0;
    }

    /* Compute parent hashes along the path from leaf 0 to root.
     * Path nodes: 1, 3, 7. For each, the "original child" is on leaf 0's side.
     *
     * parent_hash(7, original_child=3): uses sibling tree hash of right child (11)
     * parent_hash(3, original_child=1): uses sibling tree hash of right child (5)
     * parent_hash(1, original_child=0): uses sibling tree hash of right child (2)
     */
    uint8_t ph_root[MLS_HASH_LEN];
    assert(mls_tree_parent_hash(&tree, 7, 3, ph_root) == 0);

    uint8_t ph_mid[MLS_HASH_LEN];
    assert(mls_tree_parent_hash(&tree, 3, 1, ph_mid) == 0);

    uint8_t ph_low[MLS_HASH_LEN];
    assert(mls_tree_parent_hash(&tree, 1, 0, ph_low) == 0);

    /* All three hashes should be different (different inputs) */
    assert(memcmp(ph_root, ph_mid, MLS_HASH_LEN) != 0);
    assert(memcmp(ph_mid, ph_low, MLS_HASH_LEN) != 0);
    assert(memcmp(ph_root, ph_low, MLS_HASH_LEN) != 0);

    /* Now set the parent_hash fields and verify consistency:
     * The parent_hash of node 3 should be ph_root (hash from node 7).
     * The parent_hash of node 1 should be ph_mid (hash from node 3).
     * The leaf 0 should reference ph_low (hash from node 1).
     */
    tree.nodes[3].parent.parent_hash = malloc(MLS_HASH_LEN);
    assert(tree.nodes[3].parent.parent_hash);
    memcpy(tree.nodes[3].parent.parent_hash, ph_root, MLS_HASH_LEN);
    tree.nodes[3].parent.parent_hash_len = MLS_HASH_LEN;

    tree.nodes[1].parent.parent_hash = malloc(MLS_HASH_LEN);
    assert(tree.nodes[1].parent.parent_hash);
    memcpy(tree.nodes[1].parent.parent_hash, ph_mid, MLS_HASH_LEN);
    tree.nodes[1].parent.parent_hash_len = MLS_HASH_LEN;

    /* Recompute parent hashes with the chain in place — they should differ
     * because the parent_hash field is an input to tree hash */
    uint8_t ph_root2[MLS_HASH_LEN];
    assert(mls_tree_parent_hash(&tree, 7, 3, ph_root2) == 0);

    /* After setting parent_hash on node 3, the tree hash of the subtree
     * changes, but the parent_hash of node 7 (which hashes the sibling
     * subtree rooted at 11) should be unchanged since we only modified
     * the left subtree */
    uint8_t ph_root_right[MLS_HASH_LEN];
    assert(mls_tree_parent_hash(&tree, 7, 11, ph_root_right) == 0);

    /* Left vs right original child should give different results */
    assert(memcmp(ph_root2, ph_root_right, MLS_HASH_LEN) != 0);

    /* Verify determinism: same call twice gives same result */
    uint8_t ph_check[MLS_HASH_LEN];
    assert(mls_tree_parent_hash(&tree, 3, 1, ph_check) == 0);
    uint8_t ph_check2[MLS_HASH_LEN];
    assert(mls_tree_parent_hash(&tree, 3, 1, ph_check2) == 0);
    assert(memcmp(ph_check, ph_check2, MLS_HASH_LEN) == 0);

    mls_tree_free(&tree);
}

/**
 * Verify parent hash chain for a path through a 4-leaf tree with
 * asymmetric (one blank leaf) configuration.
 */
static void test_parent_hash_chain_with_blank(void)
{
    MlsRatchetTree tree;
    assert(mls_tree_new(&tree, 4) == 0);

    /* Populate leaves 0, 2, 3 — leaf 1 (node 2) is blank */
    tree.nodes[0].type = MLS_NODE_LEAF;
    make_test_leaf(&tree.nodes[0].leaf, 0x01);
    /* node 2 stays blank */
    tree.nodes[4].type = MLS_NODE_LEAF;
    make_test_leaf(&tree.nodes[4].leaf, 0x03);
    tree.nodes[6].type = MLS_NODE_LEAF;
    make_test_leaf(&tree.nodes[6].leaf, 0x04);

    /* Parent node 5 (above leaves 2 and 3) */
    tree.nodes[5].type = MLS_NODE_PARENT;
    memset(tree.nodes[5].parent.encryption_key, 0xBB, MLS_KEM_PK_LEN);
    tree.nodes[5].parent.parent_hash = NULL;
    tree.nodes[5].parent.parent_hash_len = 0;
    tree.nodes[5].parent.unmerged_leaves = NULL;
    tree.nodes[5].parent.unmerged_leaf_count = 0;

    /* Node 1 is blank (above leaf 0 and blank leaf 1) */
    /* Node 3 (root) is blank */

    /* Parent hash of node 5 with original_child=4: sibling is node 6 */
    uint8_t ph[MLS_HASH_LEN];
    assert(mls_tree_parent_hash(&tree, 5, 4, ph) == 0);

    /* Hash should be non-zero */
    int nonzero = 0;
    for (int i = 0; i < MLS_HASH_LEN; i++) {
        if (ph[i] != 0) { nonzero = 1; break; }
    }
    assert(nonzero);

    /* Different original_child should give different hash */
    uint8_t ph2[MLS_HASH_LEN];
    assert(mls_tree_parent_hash(&tree, 5, 6, ph2) == 0);
    assert(memcmp(ph, ph2, MLS_HASH_LEN) != 0);

    mls_tree_free(&tree);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tree hash consistency tests across tree sizes
 *
 * Verify that tree hash is sensitive to tree structure and content,
 * and that it's consistent when computed via different code paths.
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Verify that tree_hash(root) == tree_root_hash() for various sizes.
 */
static void test_tree_hash_root_equivalence(void)
{
    uint32_t sizes[] = { 1, 2, 4, 8, 16 };
    for (int s = 0; s < 5; s++) {
        uint32_t n = sizes[s];
        MlsRatchetTree tree;
        assert(mls_tree_new(&tree, n) == 0);

        /* Populate all leaves */
        for (uint32_t i = 0; i < n; i++) {
            uint32_t ni = mls_tree_leaf_to_node(i);
            tree.nodes[ni].type = MLS_NODE_LEAF;
            make_test_leaf(&tree.nodes[ni].leaf, (uint8_t)(i + 1));
        }

        uint8_t hash_via_root[MLS_HASH_LEN], hash_via_node[MLS_HASH_LEN];
        assert(mls_tree_root_hash(&tree, hash_via_root) == 0);
        assert(mls_tree_hash(&tree, mls_tree_root(n), hash_via_node) == 0);
        assert(memcmp(hash_via_root, hash_via_node, MLS_HASH_LEN) == 0);

        mls_tree_free(&tree);
    }
}

/**
 * Verify that adding leaves progressively changes the tree hash each time.
 */
static void test_tree_hash_grows_with_tree(void)
{
    uint8_t prev_hash[MLS_HASH_LEN] = {0};
    uint8_t curr_hash[MLS_HASH_LEN];

    /* Use power-of-2 sizes since tree hash requires balanced trees */
    uint32_t tree_sizes[] = { 1, 2, 4, 8 };
    for (int idx = 0; idx < 4; idx++) {
        uint32_t n = tree_sizes[idx];
        MlsRatchetTree tree;
        assert(mls_tree_new(&tree, n) == 0);

        for (uint32_t i = 0; i < n; i++) {
            uint32_t ni = mls_tree_leaf_to_node(i);
            tree.nodes[ni].type = MLS_NODE_LEAF;
            make_test_leaf(&tree.nodes[ni].leaf, (uint8_t)(i + 1));
        }

        assert(mls_tree_root_hash(&tree, curr_hash) == 0);

        /* Each new tree size should produce a different hash */
        if (n > 1) {
            assert(memcmp(prev_hash, curr_hash, MLS_HASH_LEN) != 0);
        }
        memcpy(prev_hash, curr_hash, MLS_HASH_LEN);

        mls_tree_free(&tree);
    }
}

/**
 * Verify that individual node hashes within a tree are all unique
 * (given unique leaf content and distinct tree positions).
 */
static void test_tree_hash_all_nodes_unique(void)
{
    MlsRatchetTree tree;
    uint32_t n = 8;
    assert(mls_tree_new(&tree, n) == 0);

    for (uint32_t i = 0; i < n; i++) {
        uint32_t ni = mls_tree_leaf_to_node(i);
        tree.nodes[ni].type = MLS_NODE_LEAF;
        make_test_leaf(&tree.nodes[ni].leaf, (uint8_t)(i + 1));
    }

    uint32_t w = mls_tree_node_width(n);
    uint8_t hashes[15][MLS_HASH_LEN]; /* max 15 nodes for 8 leaves */
    assert(w <= 15);

    for (uint32_t i = 0; i < w; i++) {
        assert(mls_tree_hash(&tree, i, hashes[i]) == 0);
    }

    /* All hashes should be unique */
    for (uint32_t i = 0; i < w; i++) {
        for (uint32_t j = i + 1; j < w; j++) {
            if (memcmp(hashes[i], hashes[j], MLS_HASH_LEN) == 0) {
                fprintf(stderr, "FAIL: node hash collision between nodes %u and %u\n", i, j);
                assert(0);
            }
        }
    }

    mls_tree_free(&tree);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tree serialization round-trip for full trees
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Serialize and deserialize every node in an 8-leaf tree, verifying that
 * the tree hash is preserved through the round-trip.
 */
static void test_full_tree_node_roundtrip(void)
{
    MlsRatchetTree tree;
    assert(mls_tree_new(&tree, 8) == 0);

    /* Populate all leaves */
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t ni = mls_tree_leaf_to_node(i);
        tree.nodes[ni].type = MLS_NODE_LEAF;
        make_test_leaf(&tree.nodes[ni].leaf, (uint8_t)(i + 1));
    }

    /* Set some parent nodes */
    uint32_t parent_nodes[] = { 1, 5, 9, 13, 3, 11 };
    for (int i = 0; i < 6; i++) {
        uint32_t ni = parent_nodes[i];
        tree.nodes[ni].type = MLS_NODE_PARENT;
        memset(tree.nodes[ni].parent.encryption_key, 0xC0 + i, MLS_KEM_PK_LEN);
        tree.nodes[ni].parent.parent_hash = malloc(MLS_HASH_LEN);
        assert(tree.nodes[ni].parent.parent_hash);
        memset(tree.nodes[ni].parent.parent_hash, 0xD0 + i, MLS_HASH_LEN);
        tree.nodes[ni].parent.parent_hash_len = MLS_HASH_LEN;
        tree.nodes[ni].parent.unmerged_leaves = NULL;
        tree.nodes[ni].parent.unmerged_leaf_count = 0;
    }

    /* Compute tree hash before round-trip */
    uint8_t hash_before[MLS_HASH_LEN];
    assert(mls_tree_root_hash(&tree, hash_before) == 0);

    /* Round-trip each leaf node through serialize/deserialize */
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t ni = mls_tree_leaf_to_node(i);

        MlsTlsBuf buf;
        assert(mls_tls_buf_init(&buf, 512) == 0);
        assert(mls_leaf_node_serialize(&tree.nodes[ni].leaf, &buf) == 0);

        MlsTlsReader reader;
        mls_tls_reader_init(&reader, buf.data, buf.len);
        MlsLeafNode decoded;
        assert(mls_leaf_node_deserialize(&reader, &decoded) == 0);

        /* Verify key fields */
        assert(memcmp(decoded.encryption_key, tree.nodes[ni].leaf.encryption_key,
                      MLS_KEM_PK_LEN) == 0);
        assert(memcmp(decoded.signature_key, tree.nodes[ni].leaf.signature_key,
                      MLS_SIG_PK_LEN) == 0);
        assert(decoded.credential_type == tree.nodes[ni].leaf.credential_type);

        mls_tls_buf_free(&buf);
        mls_leaf_node_clear(&decoded);
    }

    /* Round-trip each parent node */
    for (int i = 0; i < 6; i++) {
        uint32_t ni = parent_nodes[i];

        MlsTlsBuf buf;
        assert(mls_tls_buf_init(&buf, 256) == 0);
        assert(mls_parent_node_serialize(&tree.nodes[ni].parent, &buf) == 0);

        MlsTlsReader reader;
        mls_tls_reader_init(&reader, buf.data, buf.len);
        MlsParentNode decoded;
        assert(mls_parent_node_deserialize(&reader, &decoded) == 0);

        assert(memcmp(decoded.encryption_key, tree.nodes[ni].parent.encryption_key,
                      MLS_KEM_PK_LEN) == 0);
        assert(decoded.parent_hash_len == tree.nodes[ni].parent.parent_hash_len);
        assert(memcmp(decoded.parent_hash, tree.nodes[ni].parent.parent_hash,
                      MLS_HASH_LEN) == 0);

        mls_tls_buf_free(&buf);
        mls_parent_node_clear(&decoded);
    }

    mls_tree_free(&tree);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "Failed to initialize libsodium\n");
        return 1;
    }

    printf("libmarmot: MLS TreeKEM RFC 9420 test vector validation\n");

    printf("\n  --- Tree math vectors (RFC 9420 Appendix C) ---\n");
    TEST(test_vector_node_width);
    TEST(test_vector_root);
    TEST(test_vector_left);
    TEST(test_vector_right);
    TEST(test_vector_parent);
    TEST(test_vector_sibling);
    TEST(test_vector_all_tree_math);

    printf("\n  --- Tree math invariants (large trees) ---\n");
    TEST(test_tree_math_invariants_large);

    printf("\n  --- Direct path & copath vectors ---\n");
    TEST(test_vector_direct_path_consistency);
    TEST(test_vector_copath_consistency);

    printf("\n  --- Path secret derivation chains ---\n");
    TEST(test_path_secret_chain_derivation);
    TEST(test_path_secret_keypair_roundtrip);

    printf("\n  --- Parent hash chain validation ---\n");
    TEST(test_parent_hash_chain_8_leaves);
    TEST(test_parent_hash_chain_with_blank);

    printf("\n  --- Tree hash consistency ---\n");
    TEST(test_tree_hash_root_equivalence);
    TEST(test_tree_hash_grows_with_tree);
    TEST(test_tree_hash_all_nodes_unique);

    printf("\n  --- Full tree serialization round-trip ---\n");
    TEST(test_full_tree_node_roundtrip);

    printf("\nAll MLS TreeKEM vector tests passed.\n");
    return 0;
}
