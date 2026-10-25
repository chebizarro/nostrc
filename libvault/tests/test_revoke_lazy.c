/* SPDX-License-Identifier: MIT */
#include "helpers.h"
static void items(vault_revoke_result *r, vault_reseal_item out[2])
{
    out[0] = (vault_reseal_item){r->node, &r->ring, 0, 0, 7};
    out[1] = (vault_reseal_item){r->node, &r->ring, 1, 0, 5};
}
int main(void)
{
    world w;
    world_init(&w);
    uint8_t old_hash[32];
    seal_node(&w, &w.node, old_hash);
    porthome_node_entry parent = {.kind = PORTHOME_KIND_DIR, .child_epoch = 1};
    memset(parent.name_enc, 'f', 48);
    memcpy(parent.node_id, w.node.node_id, 32);
    memcpy(parent.manifest_sha256, old_hash, 32);
    uint8_t *before = NULL, *after = NULL;
    size_t before_len = 0, after_len = 0;
    assert(porthome_dir_node_encode(&w.node, &before, &before_len) == 0);
    vault_revoke_result *r = NULL;
    assert(vault_revoke(w.pub[2], w.pub[1], &w.node, &w.ring, &parent, NULL, 2, &r) ==
               VAULT_ERR_AUTH &&
           !r);
    assert(vault_revoke(w.pub[0], w.pub[0], &w.node, &w.ring, &parent, NULL, 2, &r) ==
               VAULT_ERR_AUTH &&
           !r);
    assert(vault_revoke(w.pub[0], w.pub[1], &w.node, &w.ring, &parent, NULL, 2, &r) == 0);
    assert(r->ring.count == 2 && r->ring.entries[1].epoch == 2 && r->node->epoch == 2 &&
           r->node->vault->grants_rev == 9);
    assert(r->node->vault->writers_len == 1 && r->node->vault->readers_len == 1 &&
           !memcmp(r->node->vault->readers[0], w.pub[2], 32));
    assert(r->parent.has_key_override && r->parent.child_epoch == 2 &&
           !memcmp(r->parent.key_override, r->ring.entries[1].node_key, 32));
    uint8_t derived[32], sibling[32], again[32], sibling_id[32] = {0x77};
    assert(porthome_child_node_key(w.key, sibling_id, NULL, sibling) == 0);
    assert(porthome_child_node_key(w.key, w.node.node_id, r->parent.key_override, derived) == 0 &&
           !memcmp(derived, r->ring.entries[1].node_key, 32));
    assert(porthome_child_node_key(w.key, sibling_id, NULL, again) == 0 &&
           !memcmp(again, sibling, 32));
    assert(!parent.has_key_override && w.ring.count == 1 && w.node.epoch == 1 &&
           w.vault.grants_rev == 8);
    assert(porthome_dir_node_encode(&w.node, &after, &after_len) == 0 && before_len == after_len &&
           !memcmp(before, after, before_len));
    free(before);
    free(after);
    assert(r->node->entries[0].chunks[0].chunk_key_id == 1 &&
           r->node->entries[1].chunks[0].chunk_key_id == 1);
    porthome_ring_t old = vault_ring_view(&w.ring), current = vault_ring_view(&r->ring);
    uint8_t *pt = NULL, *fresh = NULL;
    size_t n = 0, z = 0;
    assert(porthome_decrypt_chunk_v2(&old, 1, w.store[0].bytes, w.store[0].len, 7, &pt, &n) == 0 &&
           n == 7);
    free(pt);
    /* A new-epoch-only grant must refuse old data before any key trial. */
    porthome_ring_t only_new = {&r->ring.entries[1], 1};
    assert(only_new.entries[0].epoch == 2);
    assert(porthome_decrypt_chunk_v2(&only_new, 1, w.store[0].bytes, w.store[0].len, 7, &pt, &n) ==
               PORTHOME_ERR_EPOCH &&
           !pt && !n);
    assert(porthome_encrypt_chunk_v2(r->ring.entries[1].node_key, (uint8_t *)"new", 3, &fresh, &z,
                                     NULL) == 0);
    assert(porthome_decrypt_chunk_v2(&old, 2, fresh, z, 3, &pt, &n) == PORTHOME_ERR_EPOCH && !pt &&
           !n);
    assert(porthome_decrypt_chunk_v2(&current, 2, fresh, z, 3, &pt, &n) == 0);
    free(pt);
    free(fresh);
    vault_reseal_item jobs[2];
    items(r, jobs);
    vault_reseal_task *task = NULL;
    vault_blob_io io = {&w, read_blob, put_blob};
    assert(vault_reseal_task_new(jobs, 2, 0, &task) == 0);
    assert(vault_reseal_task_run(task, 121, &io) == VAULT_ERR_BUDGET && w.reads == 0 &&
           w.puts == 0 && vault_reseal_task_reserved(task) == 0);
    assert(vault_reseal_task_run(task, 122, &io) == VAULT_ERR_BUDGET &&
           vault_reseal_task_completed(task) == 1 && w.puts == 1);
    assert(r->node->entries[0].chunks[0].chunk_key_id == 2 &&
           r->node->entries[1].chunks[0].chunk_key_id == 1);
    assert(vault_reseal_task_run(task, 122, &io) == 0 && vault_reseal_task_completed(task) == 2 &&
           vault_reseal_task_reserved(task) == 244);
    assert(r->node->entries[1].chunks[0].chunk_key_id == 2);
    /* Actual caller materializes the updated manifest after successful puts. */
    uint8_t *updated = NULL, updated_hash[32];
    size_t updated_len = 0;
    assert(porthome_dir_node_seal(r->node, r->ring.entries[1].node_key, &updated, &updated_len,
                                  updated_hash) == 0);
    porthome_dir_node *opened = NULL;
    assert(porthome_dir_node_open(&current, 2, updated, updated_len, &opened) == 0 &&
           opened->entries[1].chunks[0].chunk_key_id == 2);
    porthome_dir_node_dispose(opened);
    free(opened);
    free(updated);
    assert(vault_ring_drop_before(&r->ring, 2) == 0);
    current = vault_ring_view(&r->ring);
    assert(porthome_decrypt_chunk_v2(&current, 1, w.store[0].bytes, w.store[0].len, 7, &pt, &n) ==
               PORTHOME_ERR_EPOCH &&
           !pt && !n);
    assert(porthome_decrypt_chunk_v2(&old, 1, w.store[0].bytes, w.store[0].len, 7, &pt, &n) == 0);
    free(pt); /* Removed member retains old data. */
    vault_reseal_task_free(task);
    vault_revoke_result_free(r);
    /* Eager budget must cover ALL work before any read/write. */
    assert(vault_revoke(w.pub[0], w.pub[1], &w.node, &w.ring, &parent, NULL, 2, &r) == 0);
    items(r, jobs);
    w.reads = w.puts = 0;
    assert(vault_reseal_task_new(jobs, 2, 1, &task) == 0 &&
           vault_reseal_task_run(task, 243, &io) == VAULT_ERR_BUDGET && w.reads == 0 &&
           w.puts == 0);
    assert(vault_reseal_task_run(task, 244, &io) == 0 && w.reads == 2 && w.puts == 2);
    vault_reseal_task_free(task);
    vault_revoke_result_free(r);
    /* Do not infer a chunk's true length by stripping real trailing zeros. */
    assert(vault_revoke(w.pub[0], w.pub[1], &w.node, &w.ring, &parent, NULL, 2, &r) == 0);
    items(r, jobs);
    jobs[0].size_plain = 6;
    w.reads = w.puts = 0;
    assert(vault_reseal_task_new(jobs, 1, 0, &task) == 0 &&
           vault_reseal_task_run(task, UINT64_MAX, &io) == VAULT_ERR_ARG && w.puts == 0 &&
           r->node->entries[0].chunks[0].chunk_key_id == 1);
    vault_reseal_task_free(task);
    vault_revoke_result_free(r);
    assert(vault_revoke(w.pub[0], w.pub[1], &w.node, &w.ring, &parent, NULL, 2, &r) == 0);
    items(r, jobs);
    w.reads = w.puts = 0;
    w.fail_put = 1;
    assert(vault_reseal_task_new(jobs, 1, 0, &task) == 0 &&
           vault_reseal_task_run(task, UINT64_MAX, &io) == VAULT_ERR_IO);
    assert(vault_reseal_task_pending_address(task) && w.puts == 1 &&
           r->node->entries[0].chunks[0].chunk_key_id == 1);
    assert(vault_reseal_task_run(task, UINT64_MAX, &io) == VAULT_ERR_IO && w.puts == 1 &&
           w.reads == 1);
    w.fail_put = 0;
    vault_reseal_task_free(task);
    vault_revoke_result_free(r);
    assert(vault_revoke(w.pub[0], w.pub[1], &w.node, &w.ring, &parent, NULL, 2, &r) == 0);
    items(r, jobs);
    w.corrupt = 1;
    w.puts = 0;
    assert(vault_reseal_task_new(jobs, 1, 0, &task) == 0 &&
           vault_reseal_task_run(task, UINT64_MAX, &io) == PORTHOME_ERR_HASH_MISMATCH &&
           w.puts == 0);
    w.corrupt = 0;
    vault_reseal_task_free(task);
    vault_revoke_result_free(r);
    /* Root key is never revealed by an old-key head. Unlisted holders can be
     * revoked too: readers[] does not decide possession or access. */
    porthome_head head = {.epoch = 1, .seq = 4, .grants_rev = 8, .written_at = 1};
    memcpy(head.vault_id, w.vault.vault_id, 32);
    memcpy(head.root_manifest, old_hash, 32);
    assert(vault_revoke(w.pub[0], w.pub[3], &w.node, &w.ring, NULL, &head, 2, &r) == 0);
    assert(r->head && r->head->has_key_override && r->head->epoch == 2 && r->head->seq == 5 &&
           r->head->parents_len == 1 && r->node->vault->grants_rev == 9);
    assert(r->node->vault->readers_len == 2 && r->head_payload);
    current = vault_ring_view(&r->ring);
    porthome_head *decoded = NULL;
    assert(porthome_head_open(&current, 2, r->head_payload, &decoded) == 0 &&
           !memcmp(decoded->key_override, r->ring.entries[1].node_key, 32));
    porthome_head_dispose(decoded);
    free(decoded);
    assert(porthome_head_open(&old, 2, r->head_payload, &decoded) == PORTHOME_ERR_EPOCH &&
           !decoded);
    char *unsafe = NULL;
    assert(porthome_head_seal(r->head, w.key, &unsafe) == PORTHOME_ERR_ARG && !unsafe);
    /* The revocation proposal also traverses the real signed-head/snapshot API. */
    NostrEvent *event = NULL;
    assert(porthome_head_event_new(r->head, r->ring.entries[1].node_key, w.pub[0], &event) == 0);
    assert(nostr_event_sign(event, w.secret[0]) == 0);
    vault_manifest_object object = {r->sealed, r->sealed_len, current, 2};
    vault_record *record = NULL;
    assert(vault_record_open(event, w.vault.vault_id, &current, 2, &object, 1, &record) == 0);
    assert(vault_record_head(record)->has_key_override &&
           vault_record_root(record)->vault->grants_rev == 9);
    vault_record_free(record);
    nostr_event_free(event);
    vault_revoke_result_free(r);
    world_dispose(&w);
    puts("OK revoke/lazy/eager (section13 c)");
    return 0;
}
