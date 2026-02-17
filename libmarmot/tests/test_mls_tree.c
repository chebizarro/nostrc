/*
 * libmarmot - MLS TreeKEM ratchet tree tests
 *
 * Tests array-based tree math (RFC 9420 Appendix C), node lifecycle,
 * resolution, filtered direct path, tree hash, and TLS serialization.
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
 * Tree math tests (RFC 9420 Appendix C)
 *
 * Reference tree with 8 leaves (n=8, 15 nodes):
 *
 *                              7
 *                 ┌────────────┴────────────┐
 *                 3                         11
 *          ┌──────┴──────┐           ┌──────┴──────┐
 *          1             5           9             13
 *       ┌──┴──┐       ┌──┴──┐    ┌──┴──┐       ┌──┴──┐
 *       0     2       4     6    8     10      12     14
 *      L0    L1      L2    L3   L4     L5      L6     L7
 *
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_tree_level(void)
{
    /* Leaves (even) have level 0 */
    assert(mls_tree_level(0) == 0);
    assert(mls_tree_level(2) == 0);
    assert(mls_tree_level(4) == 0);
    assert(mls_tree_level(14) == 0);

    /* Level 1 parents: 1, 5, 9, 13 */
    assert(mls_tree_level(1) == 1);
    assert(mls_tree_level(5) == 1);
    assert(mls_tree_level(9) == 1);
    assert(mls_tree_level(13) == 1);

    /* Level 2 parents: 3, 11 */
    assert(mls_tree_level(3) == 2);
    assert(mls_tree_level(11) == 2);

    /* Level 3: root 7 */
    assert(mls_tree_level(7) == 3);
}

static void test_node_width(void)
{
    assert(mls_tree_node_width(0) == 0);
    assert(mls_tree_node_width(1) == 1);
    assert(mls_tree_node_width(2) == 3);
    assert(mls_tree_node_width(3) == 5);
    assert(mls_tree_node_width(4) == 7);
    assert(mls_tree_node_width(8) == 15);
    assert(mls_tree_node_width(16) == 31);
}

static void test_root(void)
{
    assert(mls_tree_root(1) == 0);
    assert(mls_tree_root(2) == 1);
    assert(mls_tree_root(3) == 3);
    assert(mls_tree_root(4) == 3);
    assert(mls_tree_root(5) == 7);
    assert(mls_tree_root(8) == 7);
}

static void test_left_right(void)
{
    /* Level-3 node 7: left=3, right=11 */
    assert(mls_tree_left(7) == 3);
    assert(mls_tree_right(7) == 11);

    /* Level-2 node 3: left=1, right=5 */
    assert(mls_tree_left(3) == 1);
    assert(mls_tree_right(3) == 5);

    /* Level-2 node 11: left=9, right=13 */
    assert(mls_tree_left(11) == 9);
    assert(mls_tree_right(11) == 13);

    /* Level-1 node 1: left=0, right=2 */
    assert(mls_tree_left(1) == 0);
    assert(mls_tree_right(1) == 2);

    /* Level-1 node 5: left=4, right=6 */
    assert(mls_tree_left(5) == 4);
    assert(mls_tree_right(5) == 6);

    /* Leaf nodes return themselves */
    assert(mls_tree_left(0) == 0);
    assert(mls_tree_right(0) == 0);
}

static void test_parent(void)
{
    uint32_t n = 8;
    /* Leaf 0 (node 0): parent = 1 */
    assert(mls_tree_parent(0, n) == 1);
    /* Leaf 1 (node 2): parent = 1 */
    assert(mls_tree_parent(2, n) == 1);
    /* Node 1: parent = 3 */
    assert(mls_tree_parent(1, n) == 3);
    /* Node 5: parent = 3 */
    assert(mls_tree_parent(5, n) == 3);
    /* Node 3: parent = 7 */
    assert(mls_tree_parent(3, n) == 7);
    /* Node 11: parent = 7 */
    assert(mls_tree_parent(11, n) == 7);
    /* Root 7: parent = 7 (self) */
    assert(mls_tree_parent(7, n) == 7);
}

static void test_sibling(void)
{
    uint32_t n = 8;
    /* Node 0 and 2 are siblings under 1 */
    assert(mls_tree_sibling(0, n) == 2);
    assert(mls_tree_sibling(2, n) == 0);
    /* Node 1 and 5 are siblings under 3 */
    assert(mls_tree_sibling(1, n) == 5);
    assert(mls_tree_sibling(5, n) == 1);
    /* Node 3 and 11 are siblings under 7 */
    assert(mls_tree_sibling(3, n) == 11);
    assert(mls_tree_sibling(11, n) == 3);
}

static void test_direct_path(void)
{
    uint32_t n = 8;
    uint32_t path[16];
    uint32_t len;

    /* Leaf 0 (node 0) → 1, 3, 7 */
    len = mls_tree_direct_path(0, n, path, 16);
    assert(len == 3);
    assert(path[0] == 1);
    assert(path[1] == 3);
    assert(path[2] == 7);

    /* Leaf 3 (node 6) → 5, 3, 7 */
    len = mls_tree_direct_path(6, n, path, 16);
    assert(len == 3);
    assert(path[0] == 5);
    assert(path[1] == 3);
    assert(path[2] == 7);

    /* Leaf 7 (node 14) → 13, 11, 7 */
    len = mls_tree_direct_path(14, n, path, 16);
    assert(len == 3);
    assert(path[0] == 13);
    assert(path[1] == 11);
    assert(path[2] == 7);

    /* Root has empty direct path */
    len = mls_tree_direct_path(7, n, path, 16);
    assert(len == 0);
}

static void test_copath(void)
{
    uint32_t n = 8;
    uint32_t copath[16];
    uint32_t len;

    /* Leaf 0 (node 0): direct path is 1, 3, 7.
     * Copath is siblings: sibling(0)=2, sibling(1)=5, sibling(3)=11 */
    len = mls_tree_copath(0, n, copath, 16);
    assert(len == 3);
    assert(copath[0] == 2);
    assert(copath[1] == 5);
    assert(copath[2] == 11);
}

static void test_leaf_node_conversion(void)
{
    assert(mls_tree_leaf_to_node(0) == 0);
    assert(mls_tree_leaf_to_node(1) == 2);
    assert(mls_tree_leaf_to_node(3) == 6);
    assert(mls_tree_leaf_to_node(7) == 14);

    assert(mls_tree_node_to_leaf(0) == 0);
    assert(mls_tree_node_to_leaf(2) == 1);
    assert(mls_tree_node_to_leaf(6) == 3);
    assert(mls_tree_node_to_leaf(14) == 7);

    assert(mls_tree_is_leaf(0));
    assert(mls_tree_is_leaf(2));
    assert(!mls_tree_is_leaf(1));
    assert(!mls_tree_is_leaf(3));
    assert(!mls_tree_is_leaf(7));
}

/* ══════════════════════════════════════════════════════════════════════════
 * Helper: create a leaf node with deterministic data
 * ══════════════════════════════════════════════════════════════════════════ */

static void
make_test_leaf(MlsLeafNode *leaf, uint8_t seed)
{
    memset(leaf, 0, sizeof(*leaf));
    memset(leaf->encryption_key, seed, MLS_KEM_PK_LEN);
    memset(leaf->signature_key, seed + 0x10, MLS_SIG_PK_LEN);
    leaf->credential_type = MLS_CREDENTIAL_BASIC;

    /* Identity: 8-byte string "user_XX\0" */
    leaf->credential_identity_len = 8;
    leaf->credential_identity = malloc(8);
    assert(leaf->credential_identity);
    snprintf((char *)leaf->credential_identity, 8, "user_%02x", seed);

    /* Capabilities: support ciphersuite 0x0001 */
    leaf->ciphersuite_count = 1;
    leaf->ciphersuites = malloc(sizeof(uint16_t));
    assert(leaf->ciphersuites);
    leaf->ciphersuites[0] = 0x0001;

    /* No extensions */
    leaf->extensions_data = NULL;
    leaf->extensions_len = 0;

    /* Signature placeholder */
    memset(leaf->signature, seed + 0x20, MLS_SIG_LEN);
    leaf->signature_len = MLS_SIG_LEN;

    /* leaf_node_source = key_package (1) */
    leaf->leaf_node_source = 1;

    /* No parent hash for key_package source */
    leaf->parent_hash = NULL;
    leaf->parent_hash_len = 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tree lifecycle tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_tree_new_empty(void)
{
    MlsRatchetTree tree;
    assert(mls_tree_new(&tree, 0) == 0);
    assert(tree.n_leaves == 0);
    assert(tree.n_nodes == 0);
    assert(tree.nodes == NULL);
    mls_tree_free(&tree);
}

static void test_tree_new_with_leaves(void)
{
    MlsRatchetTree tree;
    assert(mls_tree_new(&tree, 4) == 0);
    assert(tree.n_leaves == 4);
    assert(tree.n_nodes == 7);

    /* All nodes should be blank */
    for (uint32_t i = 0; i < tree.n_nodes; i++) {
        assert(tree.nodes[i].type == MLS_NODE_BLANK);
    }
    mls_tree_free(&tree);
}

static void test_tree_add_leaf(void)
{
    MlsRatchetTree tree;
    assert(mls_tree_new(&tree, 0) == 0);

    /* Add first leaf */
    uint32_t idx;
    assert(mls_tree_add_leaf(&tree, &idx) == 0);
    assert(idx == 0);
    assert(tree.n_leaves == 1);
    assert(tree.n_nodes == 1);

    /* Add second leaf */
    assert(mls_tree_add_leaf(&tree, &idx) == 0);
    assert(idx == 2);
    assert(tree.n_leaves == 2);
    assert(tree.n_nodes == 3);

    /* Add third leaf */
    assert(mls_tree_add_leaf(&tree, &idx) == 0);
    assert(idx == 4);
    assert(tree.n_leaves == 3);
    assert(tree.n_nodes == 5);

    /* Add fourth leaf */
    assert(mls_tree_add_leaf(&tree, &idx) == 0);
    assert(idx == 6);
    assert(tree.n_leaves == 4);
    assert(tree.n_nodes == 7);

    mls_tree_free(&tree);
}

static void test_node_blank(void)
{
    MlsNode node;
    memset(&node, 0, sizeof(node));
    node.type = MLS_NODE_LEAF;
    make_test_leaf(&node.leaf, 0x42);

    /* Should have heap allocations */
    assert(node.leaf.credential_identity != NULL);
    assert(node.leaf.ciphersuites != NULL);

    mls_tree_blank_node(&node);
    assert(node.type == MLS_NODE_BLANK);
}

static void test_leaf_node_clone(void)
{
    MlsLeafNode src, dst;
    make_test_leaf(&src, 0xAA);

    assert(mls_leaf_node_clone(&dst, &src) == 0);

    /* Verify deep copy */
    assert(memcmp(dst.encryption_key, src.encryption_key, MLS_KEM_PK_LEN) == 0);
    assert(memcmp(dst.signature_key, src.signature_key, MLS_SIG_PK_LEN) == 0);
    assert(dst.credential_identity != src.credential_identity); /* different pointers */
    assert(dst.credential_identity_len == src.credential_identity_len);
    assert(memcmp(dst.credential_identity, src.credential_identity,
                  src.credential_identity_len) == 0);
    assert(dst.ciphersuites != src.ciphersuites);
    assert(dst.ciphersuite_count == src.ciphersuite_count);
    assert(dst.ciphersuites[0] == src.ciphersuites[0]);

    mls_leaf_node_clear(&src);
    mls_leaf_node_clear(&dst);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Resolution tests (RFC 9420 §4.1.1)
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_resolution_all_populated(void)
{
    /* 4-leaf tree with all leaves set (no blanks):
     *        3
     *      /   \
     *     1     5
     *    / \   / \
     *   0   2 4   6
     *
     * Resolution of any non-blank node = just itself.
     * Resolution of root (node 3) should be {3} since it's blank→children.
     * But we keep it blank — so it recursively resolves children.
     */
    MlsRatchetTree tree;
    assert(mls_tree_new(&tree, 4) == 0);

    /* Set leaves 0,1,2,3 to leaf nodes */
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t ni = mls_tree_leaf_to_node(i);
        tree.nodes[ni].type = MLS_NODE_LEAF;
        make_test_leaf(&tree.nodes[ni].leaf, (uint8_t)(i + 1));
    }

    /* Resolution of a populated leaf = just itself */
    uint32_t res[16];
    uint32_t res_len = 0;
    assert(mls_tree_resolution(&tree, 0, res, &res_len) == 0);
    assert(res_len == 1);
    assert(res[0] == 0);

    /* Resolution of blank parent node 1 = resolution(left) + resolution(right) = {0, 2} */
    res_len = 0;
    assert(mls_tree_resolution(&tree, 1, res, &res_len) == 0);
    assert(res_len == 2);
    assert(res[0] == 0); /* leaf 0 */
    assert(res[1] == 2); /* leaf 1 */

    /* Resolution of root (blank) = all leaves */
    res_len = 0;
    assert(mls_tree_resolution(&tree, 3, res, &res_len) == 0);
    assert(res_len == 4);

    mls_tree_free(&tree);
}

static void test_resolution_with_blanks(void)
{
    /* 4-leaf tree where leaf 1 (node 2) is blank:
     *        3 (blank)
     *      /   \
     *     1 (blank) 5 (blank)
     *    / \       / \
     *   0   2     4   6
     *  (L) (B)   (L) (L)
     *
     * Resolution of node 1 (blank parent): res(0) + res(2)
     *   res(0) = {0} (populated leaf)
     *   res(2) = {} (blank leaf)
     *   → {0}
     */
    MlsRatchetTree tree;
    assert(mls_tree_new(&tree, 4) == 0);

    /* Set leaves 0, 2, 3 — leave leaf 1 blank */
    tree.nodes[0].type = MLS_NODE_LEAF;
    make_test_leaf(&tree.nodes[0].leaf, 0x01);
    /* node 2 (leaf 1) stays blank */
    tree.nodes[4].type = MLS_NODE_LEAF;
    make_test_leaf(&tree.nodes[4].leaf, 0x03);
    tree.nodes[6].type = MLS_NODE_LEAF;
    make_test_leaf(&tree.nodes[6].leaf, 0x04);

    uint32_t res[16];
    uint32_t res_len = 0;

    /* Resolution of node 1: only leaf 0 (blank leaf 1 contributes nothing) */
    assert(mls_tree_resolution(&tree, 1, res, &res_len) == 0);
    assert(res_len == 1);
    assert(res[0] == 0);

    /* Resolution of node 5: both children populated */
    res_len = 0;
    assert(mls_tree_resolution(&tree, 5, res, &res_len) == 0);
    assert(res_len == 2);
    assert(res[0] == 4);
    assert(res[1] == 6);

    /* Resolution of blank leaf 1 (node 2): empty */
    res_len = 0;
    assert(mls_tree_resolution(&tree, 2, res, &res_len) == 0);
    assert(res_len == 0);

    mls_tree_free(&tree);
}

static void test_resolution_with_unmerged(void)
{
    /* Test that unmerged leaves of a parent node are included in its resolution */
    MlsRatchetTree tree;
    assert(mls_tree_new(&tree, 4) == 0);

    /* Make node 1 a parent with an unmerged leaf (leaf index 2) */
    tree.nodes[1].type = MLS_NODE_PARENT;
    memset(tree.nodes[1].parent.encryption_key, 0xBB, MLS_KEM_PK_LEN);
    tree.nodes[1].parent.unmerged_leaf_count = 1;
    tree.nodes[1].parent.unmerged_leaves = malloc(sizeof(uint32_t));
    assert(tree.nodes[1].parent.unmerged_leaves);
    tree.nodes[1].parent.unmerged_leaves[0] = 2; /* leaf index 2 = node index 4 */
    tree.nodes[1].parent.parent_hash = NULL;
    tree.nodes[1].parent.parent_hash_len = 0;

    uint32_t res[16];
    uint32_t res_len = 0;
    assert(mls_tree_resolution(&tree, 1, res, &res_len) == 0);
    /* Should be: {1 (the parent itself), 4 (node of unmerged leaf 2)} */
    assert(res_len == 2);
    assert(res[0] == 1);
    assert(res[1] == 4); /* leaf_to_node(2) = 4 */

    mls_tree_free(&tree);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Filtered direct path tests (RFC 9420 §4.1.2)
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_filtered_direct_path_all_populated(void)
{
    /* 4-leaf tree, all leaves populated, all parents blank.
     * Filtered direct path of leaf 0:
     *   Direct path: 1, 3
     *   For node 1: copath child = sibling(0) = 2. Resolution of 2 = {2} → non-empty, include 1
     *   For node 3: copath child = sibling(1) = 5. Resolution of 5 = {4,6} → non-empty, include 3
     *   → filtered = {1, 3}
     */
    MlsRatchetTree tree;
    assert(mls_tree_new(&tree, 4) == 0);

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t ni = mls_tree_leaf_to_node(i);
        tree.nodes[ni].type = MLS_NODE_LEAF;
        make_test_leaf(&tree.nodes[ni].leaf, (uint8_t)(i + 1));
    }

    uint32_t out[16];
    uint32_t out_len = 0;
    assert(mls_tree_filtered_direct_path(&tree, 0, out, &out_len) == 0);
    assert(out_len == 2);
    assert(out[0] == 1);
    assert(out[1] == 3);

    mls_tree_free(&tree);
}

static void test_filtered_direct_path_with_blank_copath(void)
{
    /* 4-leaf tree, leaf 1 (node 2) is blank:
     * Filtered direct path of leaf 0:
     *   Direct path: 1, 3
     *   For node 1: copath child = 2. Resolution of 2 = {} → EMPTY, skip 1
     *   For node 3: copath child = 5. Resolution of 5 = {4,6} → non-empty, include 3
     *   → filtered = {3}
     */
    MlsRatchetTree tree;
    assert(mls_tree_new(&tree, 4) == 0);

    tree.nodes[0].type = MLS_NODE_LEAF;
    make_test_leaf(&tree.nodes[0].leaf, 0x01);
    /* node 2 blank */
    tree.nodes[4].type = MLS_NODE_LEAF;
    make_test_leaf(&tree.nodes[4].leaf, 0x03);
    tree.nodes[6].type = MLS_NODE_LEAF;
    make_test_leaf(&tree.nodes[6].leaf, 0x04);

    uint32_t out[16];
    uint32_t out_len = 0;
    assert(mls_tree_filtered_direct_path(&tree, 0, out, &out_len) == 0);
    assert(out_len == 1);
    assert(out[0] == 3);

    mls_tree_free(&tree);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tree hash tests (RFC 9420 §7.8)
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_tree_hash_deterministic(void)
{
    /* Same tree should produce same hash */
    MlsRatchetTree tree;
    assert(mls_tree_new(&tree, 4) == 0);

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t ni = mls_tree_leaf_to_node(i);
        tree.nodes[ni].type = MLS_NODE_LEAF;
        make_test_leaf(&tree.nodes[ni].leaf, (uint8_t)(i + 1));
    }

    uint8_t hash1[MLS_HASH_LEN], hash2[MLS_HASH_LEN];
    assert(mls_tree_root_hash(&tree, hash1) == 0);
    assert(mls_tree_root_hash(&tree, hash2) == 0);
    assert(memcmp(hash1, hash2, MLS_HASH_LEN) == 0);

    mls_tree_free(&tree);
}

static void test_tree_hash_changes_with_content(void)
{
    /* Changing a leaf should change the tree hash */
    MlsRatchetTree tree1, tree2;
    assert(mls_tree_new(&tree1, 4) == 0);
    assert(mls_tree_new(&tree2, 4) == 0);

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t ni = mls_tree_leaf_to_node(i);
        tree1.nodes[ni].type = MLS_NODE_LEAF;
        make_test_leaf(&tree1.nodes[ni].leaf, (uint8_t)(i + 1));
        tree2.nodes[ni].type = MLS_NODE_LEAF;
        make_test_leaf(&tree2.nodes[ni].leaf, (uint8_t)(i + 1));
    }

    /* Trees are identical → same hash */
    uint8_t hash1[MLS_HASH_LEN], hash2[MLS_HASH_LEN];
    assert(mls_tree_root_hash(&tree1, hash1) == 0);
    assert(mls_tree_root_hash(&tree2, hash2) == 0);
    assert(memcmp(hash1, hash2, MLS_HASH_LEN) == 0);

    /* Modify leaf 0 in tree2 */
    memset(tree2.nodes[0].leaf.encryption_key, 0xFF, MLS_KEM_PK_LEN);

    assert(mls_tree_root_hash(&tree2, hash2) == 0);
    assert(memcmp(hash1, hash2, MLS_HASH_LEN) != 0);

    mls_tree_free(&tree1);
    mls_tree_free(&tree2);
}

static void test_tree_hash_leaf_vs_blank(void)
{
    /* A tree with blank leaves should hash differently from one with populated leaves */
    MlsRatchetTree tree_blank, tree_populated;
    assert(mls_tree_new(&tree_blank, 2) == 0);
    assert(mls_tree_new(&tree_populated, 2) == 0);

    /* Populate tree_populated */
    tree_populated.nodes[0].type = MLS_NODE_LEAF;
    make_test_leaf(&tree_populated.nodes[0].leaf, 0x01);
    tree_populated.nodes[2].type = MLS_NODE_LEAF;
    make_test_leaf(&tree_populated.nodes[2].leaf, 0x02);

    uint8_t hash_blank[MLS_HASH_LEN], hash_pop[MLS_HASH_LEN];
    assert(mls_tree_root_hash(&tree_blank, hash_blank) == 0);
    assert(mls_tree_root_hash(&tree_populated, hash_pop) == 0);
    assert(memcmp(hash_blank, hash_pop, MLS_HASH_LEN) != 0);

    mls_tree_free(&tree_blank);
    mls_tree_free(&tree_populated);
}

/* ══════════════════════════════════════════════════════════════════════════
 * TLS serialization round-trip tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_leaf_node_roundtrip(void)
{
    MlsLeafNode src;
    make_test_leaf(&src, 0x42);

    /* Serialize */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 256) == 0);
    assert(mls_leaf_node_serialize(&src, &buf) == 0);
    assert(buf.len > 0);

    /* Deserialize */
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    MlsLeafNode dst;
    assert(mls_leaf_node_deserialize(&reader, &dst) == 0);

    /* Verify fields match */
    assert(memcmp(dst.encryption_key, src.encryption_key, MLS_KEM_PK_LEN) == 0);
    assert(memcmp(dst.signature_key, src.signature_key, MLS_SIG_PK_LEN) == 0);
    assert(dst.credential_type == src.credential_type);
    assert(dst.credential_identity_len == src.credential_identity_len);
    assert(memcmp(dst.credential_identity, src.credential_identity,
                  src.credential_identity_len) == 0);
    assert(dst.ciphersuite_count == src.ciphersuite_count);
    assert(dst.ciphersuites[0] == src.ciphersuites[0]);
    assert(dst.leaf_node_source == src.leaf_node_source);
    assert(dst.signature_len == src.signature_len);
    assert(memcmp(dst.signature, src.signature, src.signature_len) == 0);

    mls_tls_buf_free(&buf);
    mls_leaf_node_clear(&src);
    mls_leaf_node_clear(&dst);
}

static void test_leaf_node_commit_with_parent_hash(void)
{
    /* Test serialization of a leaf with leaf_node_source = commit (3) and parent_hash */
    MlsLeafNode src;
    make_test_leaf(&src, 0x55);
    src.leaf_node_source = 3; /* commit */
    src.parent_hash_len = MLS_HASH_LEN;
    src.parent_hash = malloc(MLS_HASH_LEN);
    assert(src.parent_hash);
    memset(src.parent_hash, 0xCC, MLS_HASH_LEN);

    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 256) == 0);
    assert(mls_leaf_node_serialize(&src, &buf) == 0);

    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    MlsLeafNode dst;
    assert(mls_leaf_node_deserialize(&reader, &dst) == 0);

    assert(dst.leaf_node_source == 3);
    assert(dst.parent_hash_len == MLS_HASH_LEN);
    assert(memcmp(dst.parent_hash, src.parent_hash, MLS_HASH_LEN) == 0);

    mls_tls_buf_free(&buf);
    mls_leaf_node_clear(&src);
    mls_leaf_node_clear(&dst);
}

static void test_parent_node_roundtrip(void)
{
    MlsParentNode src;
    memset(&src, 0, sizeof(src));
    memset(src.encryption_key, 0xDD, MLS_KEM_PK_LEN);
    src.parent_hash_len = MLS_HASH_LEN;
    src.parent_hash = malloc(MLS_HASH_LEN);
    assert(src.parent_hash);
    memset(src.parent_hash, 0xEE, MLS_HASH_LEN);
    src.unmerged_leaf_count = 2;
    src.unmerged_leaves = malloc(2 * sizeof(uint32_t));
    assert(src.unmerged_leaves);
    src.unmerged_leaves[0] = 3;
    src.unmerged_leaves[1] = 5;

    /* Serialize */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 256) == 0);
    assert(mls_parent_node_serialize(&src, &buf) == 0);

    /* Deserialize */
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    MlsParentNode dst;
    assert(mls_parent_node_deserialize(&reader, &dst) == 0);

    assert(memcmp(dst.encryption_key, src.encryption_key, MLS_KEM_PK_LEN) == 0);
    assert(dst.parent_hash_len == MLS_HASH_LEN);
    assert(memcmp(dst.parent_hash, src.parent_hash, MLS_HASH_LEN) == 0);
    assert(dst.unmerged_leaf_count == 2);
    assert(dst.unmerged_leaves[0] == 3);
    assert(dst.unmerged_leaves[1] == 5);

    mls_tls_buf_free(&buf);
    mls_parent_node_clear(&src);
    mls_parent_node_clear(&dst);
}

static void test_parent_node_no_unmerged(void)
{
    MlsParentNode src;
    memset(&src, 0, sizeof(src));
    memset(src.encryption_key, 0xAA, MLS_KEM_PK_LEN);
    src.parent_hash = NULL;
    src.parent_hash_len = 0;
    src.unmerged_leaves = NULL;
    src.unmerged_leaf_count = 0;

    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 256) == 0);
    assert(mls_parent_node_serialize(&src, &buf) == 0);

    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    MlsParentNode dst;
    assert(mls_parent_node_deserialize(&reader, &dst) == 0);

    assert(memcmp(dst.encryption_key, src.encryption_key, MLS_KEM_PK_LEN) == 0);
    assert(dst.parent_hash_len == 0);
    assert(dst.unmerged_leaf_count == 0);

    mls_tls_buf_free(&buf);
    mls_parent_node_clear(&dst);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tree math edge cases
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_tree_single_leaf(void)
{
    /* Tree with 1 leaf: root=0, no direct path */
    assert(mls_tree_root(1) == 0);
    assert(mls_tree_node_width(1) == 1);

    uint32_t path[4];
    uint32_t len = mls_tree_direct_path(0, 1, path, 4);
    assert(len == 0);

    MlsRatchetTree tree;
    assert(mls_tree_new(&tree, 1) == 0);
    assert(tree.n_nodes == 1);

    tree.nodes[0].type = MLS_NODE_LEAF;
    make_test_leaf(&tree.nodes[0].leaf, 0x01);

    uint8_t hash[MLS_HASH_LEN];
    assert(mls_tree_root_hash(&tree, hash) == 0);

    /* Hash should be non-zero */
    int all_zero = 1;
    for (int i = 0; i < MLS_HASH_LEN; i++) {
        if (hash[i] != 0) { all_zero = 0; break; }
    }
    assert(!all_zero);

    mls_tree_free(&tree);
}

static void test_tree_two_leaves(void)
{
    /* Tree with 2 leaves: root=1 (a parent) */
    assert(mls_tree_root(2) == 1);
    assert(mls_tree_node_width(2) == 3);
    assert(mls_tree_parent(0, 2) == 1);
    assert(mls_tree_parent(2, 2) == 1);
    assert(mls_tree_sibling(0, 2) == 2);
    assert(mls_tree_sibling(2, 2) == 0);
}

static void test_tree_large(void)
{
    /* Tree with 32 leaves: node_width = 63 */
    uint32_t n = 32;
    assert(mls_tree_node_width(n) == 63);

    MlsRatchetTree tree;
    assert(mls_tree_new(&tree, n) == 0);
    assert(tree.n_nodes == 63);

    /* Root should be at 31 */
    assert(mls_tree_root(n) == 31);

    /* Direct path of leaf 0 should have 5 entries (log2(32) = 5) */
    uint32_t path[16];
    uint32_t len = mls_tree_direct_path(0, n, path, 16);
    assert(len == 5);

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

    printf("libmarmot: MLS TreeKEM ratchet tree tests\n");

    printf("\n  --- Tree math ---\n");
    TEST(test_tree_level);
    TEST(test_node_width);
    TEST(test_root);
    TEST(test_left_right);
    TEST(test_parent);
    TEST(test_sibling);
    TEST(test_direct_path);
    TEST(test_copath);
    TEST(test_leaf_node_conversion);

    printf("\n  --- Tree lifecycle ---\n");
    TEST(test_tree_new_empty);
    TEST(test_tree_new_with_leaves);
    TEST(test_tree_add_leaf);
    TEST(test_node_blank);
    TEST(test_leaf_node_clone);

    printf("\n  --- Resolution ---\n");
    TEST(test_resolution_all_populated);
    TEST(test_resolution_with_blanks);
    TEST(test_resolution_with_unmerged);

    printf("\n  --- Filtered direct path ---\n");
    TEST(test_filtered_direct_path_all_populated);
    TEST(test_filtered_direct_path_with_blank_copath);

    printf("\n  --- Tree hash ---\n");
    TEST(test_tree_hash_deterministic);
    TEST(test_tree_hash_changes_with_content);
    TEST(test_tree_hash_leaf_vs_blank);

    printf("\n  --- TLS serialization ---\n");
    TEST(test_leaf_node_roundtrip);
    TEST(test_leaf_node_commit_with_parent_hash);
    TEST(test_parent_node_roundtrip);
    TEST(test_parent_node_no_unmerged);

    printf("\n  --- Edge cases ---\n");
    TEST(test_tree_single_leaf);
    TEST(test_tree_two_leaves);
    TEST(test_tree_large);

    printf("\nAll MLS TreeKEM tests passed.\n");
    return 0;
}
