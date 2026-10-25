/* SPDX-License-Identifier: MIT */
#include "test_helpers.h"

int main(void) {
    uint8_t root[32], child[32], sibling[32], grandchild[32], changed[32];
    uint8_t id[32] = {7}, override[32] = {0x83}, derived[32], name[32], head[32];
    fixture(root, child);
    expect_hex(root, 32, "053dd775a4aef3b20cb9865949129cbe33e681cfbc1174cc0fbf67bd089423de");
    expect_hex(child, 32, "6c94a54de3167fb03de876d5764aea1be2e29202f0a327798c29deca20948d14");
    assert(porthome_name_key_v2(child, name) == 0);
    expect_hex(name, 32, "4f79fbe63adf27dd5e9715c32e0dc03e05ffa6114e23eb42f4a54c18f35f449b");
    assert(porthome_head_key_v2(child, head) == 0);
    expect_hex(head, 32, "5c86db2288c6482f356153c067a16e0679bdbc0173bb6b84f2e715321aa38b53");
    uint8_t order[32];
    assert(porthome_from_hex64("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141", order) == 0);
    assert(memcmp(head, order, 32) < 0);
    assert(memcmp(head, (uint8_t[32]){0}, 32));
    char label[PORTHOME_NAME_HEX_LEN], other[PORTHOME_NAME_HEX_LEN];
    assert(porthome_encrypt_name_v2(child, "report.pdf", label) == 0);
    assert(!strcmp(label, "616af000f072b6d7ad438145dceb8c56b24a33a8c8838703"));
    assert(porthome_encrypt_name(child, "report.pdf", other) == 0);
    assert(strcmp(label, other));
    assert(porthome_encrypt_name_v2(child, "../x", other) == PORTHOME_ERR_PATH);
    assert(porthome_encrypt_name_v2(child, "..", other) == PORTHOME_ERR_PATH);

    assert(porthome_child_node_key(root, id, NULL, sibling) == 0);
    assert(memcmp(child, sibling, 32));
    assert(porthome_child_node_key(child, id, NULL, grandchild) == 0);
    /* A reverse invocation is not a parent-recovery API. HKDF's one-wayness
     * is a cryptographic assumption, not something a unit test can prove. */
    assert(porthome_child_node_key(child, id, NULL, derived) == 0);
    assert(memcmp(root, derived, 32));
    assert(porthome_child_node_key(root, id, override, changed) == 0);
    assert(!memcmp(changed, override, 32));
    assert(porthome_child_node_key(changed, id, NULL, derived) == 0);
    assert(memcmp(grandchild, derived, 32));
    /* Unrelated sibling and parent do not change when a child is overridden. */
    assert(porthome_child_node_key(root, id, NULL, changed) == 0);
    assert(!memcmp(changed, sibling, 32));
    assert(porthome_child_node_key(NULL, NULL, override, changed) == 0);
    assert(!memcmp(changed, override, 32));
    assert(porthome_child_node_key(changed, id, changed, changed) == 0);
    assert(!memcmp(changed, override, 32));
    memcpy(changed, root, 32);
    assert(porthome_child_node_key(changed, id, NULL, changed) == 0);
    assert(!memcmp(changed, sibling, 32));

    porthome_ring_entry_t entries[2] = {{.epoch = 1}, {.epoch = 3}};
    memcpy(entries[0].node_key, child, 32);
    memcpy(entries[1].node_key, override, 32);
    porthome_ring_t ring = {entries, 2}, old = {entries, 1}, new = {entries+1, 1};
    const uint8_t pt[] = "epoch material";
    uint8_t *blob = NULL, *out = NULL, *fresh = NULL;
    size_t len = 0, got = 0, fresh_len = 0;
    assert(porthome_encrypt_chunk_v2(child, pt, sizeof(pt), &blob, &len, NULL) == 0);
    assert(porthome_decrypt_chunk_v2(&old, 1, blob, len, sizeof(pt), &out, &got) == 0);
    free(out);
    assert(porthome_decrypt_chunk_v2(&ring, 1, blob, len, sizeof(pt), &out, &got) == 0);
    free(out);
    assert(porthome_decrypt_chunk_v2(&new, 1, blob, len, sizeof(pt), &out, &got) == PORTHOME_ERR_EPOCH);
    assert(!out && !got);
    assert(porthome_decrypt_chunk_v2(&ring, 2, blob, len, sizeof(pt), &out, &got) == PORTHOME_ERR_EPOCH);
    /* Matching epoch is wrong, another entry would succeed: MUST NOT retry. */
    assert(porthome_decrypt_chunk_v2(&ring, 3, blob, len, sizeof(pt), &out, &got) == PORTHOME_ERR_CRYPTO);
    assert(!out && !got);
    assert(porthome_encrypt_chunk_v2(override, pt, sizeof(pt), &fresh, &fresh_len, NULL) == 0);
    assert(len == fresh_len && memcmp(blob, fresh, len));
    assert(porthome_decrypt_chunk_v2(&new, 3, fresh, fresh_len, sizeof(pt), &out, &got) == 0);
    free(out);
    free(blob);
    free(fresh);

    assert(porthome_encrypt_manifest_v2(child, pt, sizeof(pt), &blob, &len, NULL) == 0);
    assert(porthome_decrypt_manifest_v2(&new, 1, blob, len, &out, &got) == PORTHOME_ERR_EPOCH);
    assert(porthome_decrypt_manifest_v2(&ring, 3, blob, len, &out, &got) == PORTHOME_ERR_CRYPTO);
    assert(!out && !got);
    entries[1].epoch = 1;
    assert(porthome_decrypt_manifest_v2(&ring, 1, blob, len, &out, &got) == PORTHOME_ERR_ARG);
    entries[0].epoch = 0;
    assert(porthome_decrypt_manifest_v2(&ring, 1, blob, len, &out, &got) == PORTHOME_ERR_ARG);
    porthome_ring_t empty = {NULL, 0};
    assert(porthome_decrypt_manifest_v2(&empty, 1, blob, len, &out, &got) == PORTHOME_ERR_EPOCH);
    free(blob);
    return 0;
}
