/* SPDX-License-Identifier: MIT */
#include "helpers.h"
int main(void)
{
    world w;
    world_init(&w);
    uint8_t id[32] = {9}, other[32] = {10}, child[32], sibling[32], grand[32], fresh[32] = {0x55},
            again[32], over[32];
    assert(porthome_child_node_key(w.key, id, NULL, child) == 0);
    assert(porthome_child_node_key(w.key, id, NULL, again) == 0 && !memcmp(child, again, 32));
    assert(porthome_child_node_key(w.key, other, NULL, sibling) == 0 && memcmp(child, sibling, 32));
    assert(porthome_child_node_key(child, other, NULL, grand) == 0);
    /* HKDF's one-wayness is assumed, not proved by unit tests; no reverse API. */
    assert(porthome_child_node_key(child, id, NULL, again) == 0 && memcmp(again, w.key, 32));
    assert(porthome_child_node_key(w.key, id, fresh, over) == 0 && !memcmp(fresh, over, 32));
    assert(porthome_child_node_key(over, other, NULL, again) == 0 && memcmp(grand, again, 32));
    assert(porthome_child_node_key(w.key, other, NULL, again) == 0 && !memcmp(again, sibling, 32));
    vault_ring r = {0};
    assert(vault_ring_init(&r, 1, child) == 0);
    uint64_t epoch;
    assert(vault_ring_append(&r, fresh, &epoch) == 0 && epoch == 2);
    assert(vault_ring_append(&r, fresh, &epoch) == VAULT_ERR_ARG && r.count == 2);
    uint8_t third[32] = {0x77};
    assert(vault_ring_append(&r, third, &epoch) == 0 && epoch == 3);
    porthome_ring_t view = vault_ring_view(&r);
    vault_ring copy = {0};
    assert(vault_ring_copy(&copy, &view) == 0);
    assert(vault_ring_drop_before(&r, 2) == 0 && r.count == 2 && r.entries[0].epoch == 2 &&
           r.entries[1].epoch == 3);
    assert(copy.count == 3 && copy.entries[0].epoch == 1 &&
           !memcmp(copy.entries[0].node_key, child, 32));
    assert(vault_ring_drop_before(&r, 4) == VAULT_ERR_EPOCH && r.count == 2);
    uint8_t *sealed = NULL, *pt = NULL;
    size_t len = 0, n = 0;
    assert(porthome_encrypt_chunk_v2(child, (const uint8_t *)"old", 3, &sealed, &len, NULL) == 0);
    view = vault_ring_view(&copy);
    assert(porthome_decrypt_chunk_v2(&view, 1, sealed, len, 3, &pt, &n) == 0 && n == 3);
    free(pt);
    assert(porthome_decrypt_chunk_v2(&view, 2, sealed, len, 3, &pt, &n) == PORTHOME_ERR_CRYPTO &&
           !pt && !n); /* Correct key exists at epoch1; no retry. */
    view = vault_ring_view(&r);
    assert(porthome_decrypt_chunk_v2(&view, 1, sealed, len, 3, &pt, &n) == PORTHOME_ERR_EPOCH &&
           !pt && !n);
    assert(porthome_decrypt_chunk_v2(&view, 2, sealed, len, 3, &pt, &n) == PORTHOME_ERR_CRYPTO &&
           !pt && !n);
    free(sealed);
    vault_ring_dispose(&r);
    vault_ring_dispose(&copy);
    porthome_ring_entry_t invalid[2] = {{.epoch = 2, .node_key = {1}},
                                        {.epoch = 1, .node_key = {2}}};
    view.entries = invalid;
    view.count = 2;
    assert(vault_ring_copy(&r, &view) == VAULT_ERR_ARG && !r.count);
    assert(vault_ring_init(&r, UINT64_MAX, child) == 0);
    assert(vault_ring_append(&r, fresh, &epoch) == PORTHOME_ERR_TOO_LARGE && r.count == 1);
    vault_ring_dispose(&r);
    world_dispose(&w);
    puts("OK vault ring derivation (section13 b)");
    return 0;
}
