/* SPDX-License-Identifier: MIT */
#include "nostrc-test-vault.h"
#include <openssl/crypto.h>
#include <vault/tree.h>
static int retained(const vault_fold_result *r, const uint8_t hash[32])
{
    for (size_t i = 0; i < r->retained_roots_len; ++i)
        if (!memcmp(r->retained_roots[i], hash, 32))
            return 1;
    return 0;
}
int main(void)
{
    world w;
    world_init(&w);
    vault_record *base = record_make(&w, 0, 1, 8, "base", NULL, 0, 0);
    porthome_head_parent p = parent_of(base, w.pub[0]);
    vault_record *owner = record_make(&w, 0, 2, 8, "owner edit", &p, 1, 0);
    vault_record *writer = record_make(&w, 1, 2, 8, "writer edit", &p, 1, 0);
    vault_record *stale = record_make(&w, 1, 1, 7, "stale", NULL, 0, 0);
    vault_record *reader = record_make(&w, 2, 2, 8, "reader", NULL, 0, 0);
    vault_record *forged = record_make(&w, 1, 3, 999, "forged policy", NULL, 0, 2);
    const vault_record *records[] = {base, owner, writer, stale, reader, forged};
    vault_fold_result *r = NULL;
    size_t stored = w.stored;
    assert(vault_fold(&w.vault, records, 6, &r) == 0 && r->view == 1 && r->frontier_len == 2 &&
           r->conflicts_len == 1);
    assert(r->dispositions[3] == VAULT_HEAD_STALE && r->dispositions[4] == VAULT_HEAD_NONWRITER &&
           r->dispositions[5] == VAULT_HEAD_POLICY);
    assert(r->conflicts[0].in_owner && r->conflicts[0].in_other &&
           r->conflicts[0].other_record == 2);
    assert(!strcmp(r->conflicts[0].path_enc, w.entries[0].name_enc));
    for (size_t i = 0; i < 6; ++i)
        assert(retained(r, vault_record_head(records[i])->root_manifest));
    assert(retained(r, p.root_manifest) && w.stored == stored && w.puts == 0 && w.reads == 0);
    vault_fold_result_free(r);
    /* Input order cannot change the owner view or per-path conflict identity. */
    for (size_t offset = 0; offset < 6; ++offset) {
        const vault_record *permuted[6];
        for (size_t i = 0; i < 6; ++i)
            permuted[i] = records[(i + offset) % 6];
        assert(vault_fold(&w.vault, permuted, 6, &r) == 0 && permuted[r->view] == owner &&
               r->conflicts_len == 1);
        assert(!memcmp(r->conflicts[0].writer, w.pub[1], 32) &&
               !strcmp(r->conflicts[0].path_enc, w.entries[0].name_enc));
        vault_fold_result_free(r);
    }
    const vault_record *single[] = {base, writer};
    assert(vault_fold(&w.vault, single, 2, &r) == 0 && r->frontier_len == 1 && r->view == 1);
    vault_fold_result_free(r);
    const vault_record *missing[] = {owner, writer};
    assert(vault_fold(&w.vault, missing, 2, &r) == VAULT_ERR_INCOMPLETE && r->view == SIZE_MAX &&
           retained(r, p.root_manifest));
    vault_fold_result_free(r);
    const vault_record *duplicate[] = {base, owner, writer, writer};
    assert(vault_fold(&w.vault, duplicate, 4, &r) == 0 && r->frontier_len == 2 &&
           r->dispositions[3] == VAULT_HEAD_DUPLICATE);
    vault_fold_result_free(r);
    vault_record *reader_a = record_make(&w, 2, 7, 8, "same ignored root", NULL, 0, 0),
                 *reader_b = record_make(&w, 2, 7, 8, "same ignored root", &p, 1, 0);
    const vault_record *ignored[] = {base, owner, writer, reader_a, reader_b};
    assert(vault_fold(&w.vault, ignored, 5, &r) == 0 && r->view == 1);
    vault_fold_result_free(r);
    /* Owner may be an ancestor, not a frontier: still the sequencer view. */
    vault_record *writer_fork = record_make(&w, 1, 2, 8, "other writer fork", &p, 1, 0);
    const vault_record *ancestor[] = {base, writer, writer_fork};
    assert(vault_fold(&w.vault, ancestor, 3, &r) == 0 && r->view == 0 && r->frontier_len == 2 &&
           r->conflicts_len == 2);
    vault_fold_result_free(r);
    vault_record *deleted = record_make(&w, 0, 2, 8, NULL, &p, 1, 0);
    const vault_record *deletion[] = {base, deleted, writer};
    assert(vault_fold(&w.vault, deletion, 3, &r) == 0 && r->view == 1 && r->conflicts_len == 1 &&
           !r->conflicts[0].in_owner && r->conflicts[0].in_other);
    vault_fold_result_free(r);
    /* Only owner-signed metadata can advance the writer policy. The removed
     * writer remains listed as a READER; that must not admit its head. */
    porthome_head_parent op = parent_of(owner, w.pub[0]);
    vault_record *new_owner = record_make(&w, 0, 3, 9, "post removal", &op, 1, 1);
    const vault_record *updated[] = {base, owner, writer, new_owner, forged, reader};
    assert(vault_fold(&w.vault, updated, 6, &r) == 0 && r->view == 3 && r->frontier_len == 1 &&
           r->dispositions[2] == VAULT_HEAD_NONWRITER);
    for (size_t i = 0; i < 6; ++i)
        assert(retained(r, vault_record_head(updated[i])->root_manifest));
    vault_fold_result_free(r);
    vault_record *bad_policy = record_make(&w, 0, 99, 8, "policy without rev", NULL, 0, 1);
    const vault_record *ambiguous[] = {base, bad_policy};
    assert(vault_fold(&w.vault, ambiguous, 2, &r) == VAULT_ERR_CONFLICT && r->view == SIZE_MAX);
    vault_fold_result_free(r);
    /* Cyclic, signed DAG data cannot manufacture a frontier or an owner view. */
    vault_record *ta = record_make(&w, 0, 10, 8, "cycle a", NULL, 0, 0),
                 *tb = record_make(&w, 1, 10, 8, "cycle b", NULL, 0, 0);
    porthome_head_parent pa = parent_of(ta, w.pub[0]), pb = parent_of(tb, w.pub[1]);
    vault_record *ca = record_make(&w, 0, 10, 8, "cycle a", &pb, 1, 0),
                 *cb = record_make(&w, 1, 10, 8, "cycle b", &pa, 1, 0);
    const vault_record *cycle[] = {ca, cb};
    assert(vault_fold(&w.vault, cycle, 2, &r) == VAULT_ERR_CONFLICT && r->view == SIZE_MAX &&
           r->retained_roots_len == 2);
    vault_fold_result_free(r);
    /* Verification is a real entrypoint, not caller-supplied fold fixtures. */
    uint8_t hash[32];
    seal_node(&w, &w.node, hash);
    porthome_head h = {.epoch = 1, .seq = 1, .grants_rev = 8};
    memcpy(h.vault_id, w.vault.vault_id, 32);
    memcpy(h.root_manifest, hash, 32);
    NostrEvent *event = NULL;
    assert(porthome_head_event_new(&h, w.key, w.pub[0], &event) == 0 &&
           nostr_event_sign(event, w.secret[0]) == 0);
    porthome_ring_t view = vault_ring_view(&w.ring);
    vault_record *bad = NULL;
    assert(vault_record_open(event, w.vault.vault_id, &view, 1, NULL, 0, &bad) ==
               VAULT_ERR_INCOMPLETE &&
           !bad);
    blob *last = &w.store[w.stored - 1];
    uint8_t *corrupt = malloc(last->len);
    assert(corrupt);
    memcpy(corrupt, last->bytes, last->len);
    corrupt[0] ^= 1;
    vault_manifest_object object = {corrupt, last->len, view, 1};
    assert(vault_record_open(event, w.vault.vault_id, &view, 1, &object, 1, &bad) ==
               VAULT_ERR_INCOMPLETE &&
           !bad);
    free(corrupt);
    event->sig[0] = event->sig[0] == '0' ? '1' : '0';
    object.sealed = last->bytes;
    assert(vault_record_open(event, w.vault.vault_id, &view, 1, &object, 1, &bad) ==
               PORTHOME_ERR_CRYPTO &&
           !bad);
    nostr_event_free(event);
    /* Fresh member: only an authenticated grant and current signed records,
     * never the owner's private history or the missing base record. */
    vault_grant grant = {.ring = w.ring, .cap = VAULT_CAP_WRITE, .issued_at = 1};
    memcpy(grant.vault_id, w.vault.vault_id, 32);
    memcpy(grant.node_id, w.node.node_id, 32);
    memcpy(grant.owner, w.pub[0], 32);
    memcpy(grant.root_manifest, vault_record_head(owner)->root_manifest, 32);
    char *grant_body = NULL;
    vault_grant *received = NULL;
    assert(vault_grant_encode(&grant, &grant_body) == 0);
    assert(vault_grant_decode(grant_body, &received) == 0);
    OPENSSL_cleanse(grant_body, strlen(grant_body));
    free(grant_body);
    const vault_record *current[] = {owner, writer, stale, reader, forged};
    vault_record *cold[5] = {0};
    const vault_record *cold_current[5];
    const unsigned authors[] = {0, 1, 1, 2, 1};
    porthome_ring_t granted = vault_ring_view(&received->ring);
    vault_blob_io remote = {&w, read_blob, NULL};
    unsigned cold_reads = w.reads;
    for (size_t i = 0; i < 5; ++i) {
        NostrEvent *latest = NULL;
        const porthome_head *body = vault_record_head(current[i]);
        assert(porthome_head_event_new(body, granted.entries[0].node_key, w.pub[authors[i]],
                                       &latest) == 0);
        assert(nostr_event_sign(latest, w.secret[authors[i]]) == 0);
        vault_tree *tree = NULL;
        assert(vault_record_fetch(latest, received->vault_id, received->node_id, &granted,
                                  body->epoch, NULL, NULL, &remote, &cold[i], &tree) == 0);
        assert(vault_tree_count(tree) == 1);
        vault_tree_free(tree);
        nostr_event_free(latest);
        cold_current[i] = cold[i];
    }
    assert(w.reads == cold_reads + 5); /* No missing ancestor was requested. */
    assert(vault_fold_checkpoint(received, cold_current, 5, 0, &r) == 0 && r->view == 0 &&
           r->frontier_len == 2 && r->conflicts_len == 1 &&
           r->dispositions[2] == VAULT_HEAD_STALE && r->dispositions[3] == VAULT_HEAD_NONWRITER);
    vault_fold_result_free(r);
    for (size_t i = 0; i < 5; ++i)
        vault_record_free(cold[i]);
    assert(vault_fold_checkpoint(received, current, 5, 0, &r) == 0 && r->view == 0 &&
           r->frontier_len == 2 && r->conflicts_len == 1);
    assert(r->dispositions[2] == VAULT_HEAD_STALE && r->dispositions[3] == VAULT_HEAD_NONWRITER &&
           r->dispositions[4] == VAULT_HEAD_POLICY);
    assert(r->conflicts[0].other_record == 1 &&
           !strcmp(r->conflicts[0].path_enc, w.entries[0].name_enc));
    assert(retained(r, p.root_manifest));
    vault_fold_result_free(r);
    for (size_t offset = 0; offset < 5; ++offset) {
        const vault_record *permuted[5];
        size_t oi = 0;
        for (size_t i = 0; i < 5; ++i) {
            permuted[i] = current[(i + offset) % 5];
            if (permuted[i] == owner)
                oi = i;
        }
        assert(vault_fold_checkpoint(received, permuted, 5, oi, &r) == 0 &&
               permuted[r->view] == owner && r->conflicts_len == 1);
        vault_fold_result_free(r);
    }
    assert(vault_fold_checkpoint(received, current, 5, 1, &r) == VAULT_ERR_AUTH && !r);
    const vault_record *writer_forks[] = {owner, writer, writer_fork};
    assert(vault_fold_checkpoint(received, writer_forks, 3, 0, &r) == 0 && r->view == 0 &&
           r->frontier_len == 3 && r->conflicts_len == 2);
    vault_fold_result_free(r);
    /* A parent at a missing higher sequence dominates a current older head of
     * that writer: no exact-root match or archived intermediate is required. */
    porthome_head_parent coverage = parent_of(writer, w.pub[1]);
    coverage.seq = 5;
    memset(coverage.root_manifest, 0x77, 32);
    vault_record *covered = record_make(&w, 0, 3, 8, "covered", &coverage, 1, 0);
    const vault_record *dominated[] = {covered, writer};
    assert(vault_fold_checkpoint(received, dominated, 2, 0, &r) == 0 && r->view == 0 &&
           r->frontier_len == 1);
    vault_fold_result_free(r);
    /* Neither stale nor unauthorized heads may suppress the owner's frontier. */
    porthome_head_parent false_cover = parent_of(owner, w.pub[0]);
    false_cover.seq = 999;
    vault_record *stale_cover = record_make(&w, 1, 6, 7, "stale cover", &false_cover, 1, 0);
    vault_record *reader_cover = record_make(&w, 2, 6, 8, "reader cover", &false_cover, 1, 0);
    const vault_record *untrusted[] = {owner, stale_cover, reader_cover};
    assert(vault_fold_checkpoint(received, untrusted, 3, 0, &r) == 0 && r->view == 0 &&
           r->frontier_len == 1);
    vault_fold_result_free(r);
    const vault_record *new_policy[] = {new_owner, writer, forged, reader};
    assert(vault_fold_checkpoint(received, new_policy, 4, 0, &r) == 0 && r->view == 0 &&
           r->dispositions[1] == VAULT_HEAD_NONWRITER);
    vault_fold_result_free(r);
    assert(vault_fold_checkpoint(received, cycle, 2, 0, &r) == VAULT_ERR_CONFLICT);
    vault_fold_result_free(r);
    assert(vault_fold_checkpoint(received, ambiguous, 2, 0, &r) == VAULT_ERR_CONFLICT && !r);
    /* Publishing carries every incorporated writer and their prior coverage;
     * compression is by highest seq per writer, not just direct predecessor. */
    porthome_head successor = {.seq = 4};
    memcpy(successor.vault_id, w.vault.vault_id, 32);
    const vault_record *incorporated[] = {owner, writer};
    assert(vault_head_cover(&successor, incorporated, 2) == 0 && successor.parents_len == 2);
    for (size_t i = 0; i < successor.parents_len; ++i) {
        assert(successor.parents[i].seq == 2);
        assert(!memcmp(successor.parents[i].writer, w.pub[0], 32) ||
               !memcmp(successor.parents[i].writer, w.pub[1], 32));
    }
    const vault_record *carry[] = {covered};
    assert(vault_head_cover(&successor, carry, 1) == 0 && successor.parents_len == 2);
    for (size_t i = 0; i < successor.parents_len; ++i)
        assert(successor.parents[i].seq == (!memcmp(successor.parents[i].writer, w.pub[0], 32) ? 3 : 5));
    porthome_head_parent saved[2];
    memcpy(saved, successor.parents, sizeof(saved));
    const vault_record *forks[] = {writer, writer_fork};
    porthome_head fork_head = {0};
    memcpy(fork_head.vault_id, w.vault.vault_id, 32);
    assert(vault_head_cover(&fork_head, forks, 2) == VAULT_ERR_CONFLICT && !fork_head.parents);
    assert(!memcmp(saved, successor.parents, sizeof(saved)));
    const vault_record *cover_forks[] = {covered, writer, writer_fork};
    for (size_t offset = 0; offset < 3; ++offset) {
        const vault_record *permuted[3];
        for (size_t i = 0; i < 3; ++i)
            permuted[i] = cover_forks[(i + offset) % 3];
        assert(vault_head_cover(&successor, permuted, 3) == VAULT_ERR_CONFLICT &&
               successor.parents_len == 2 && !memcmp(saved, successor.parents, sizeof(saved)));
    }
    porthome_head capped = {0};
    memcpy(capped.vault_id, w.vault.vault_id, 32);
    capped.parents = calloc(PORTHOME_HEAD_MAX_PARENTS, sizeof(*capped.parents));
    assert(capped.parents);
    capped.parents_len = PORTHOME_HEAD_MAX_PARENTS;
    for (size_t i = 0; i < capped.parents_len; ++i) {
        capped.parents[i].writer[0] = (uint8_t)(100 + i);
        capped.parents[i].seq = 1;
    }
    porthome_head_parent *unchanged = capped.parents;
    assert(vault_head_cover(&capped, incorporated, 2) == PORTHOME_ERR_TOO_LARGE &&
           capped.parents == unchanged && capped.parents_len == PORTHOME_HEAD_MAX_PARENTS);
    porthome_head_dispose(&capped);
    porthome_head_dispose(&successor);
    /* Checkpoint frontier is exactly current-head sequence dominance, not the
     * strict history DAG validator. A covered writer component cannot veto an
     * uncovered owner checkpoint. */
    porthome_vault_info saved_policy = w.vault;
    uint8_t three_writers[3][32];
    memcpy(three_writers[0], w.pub[0], 32);
    memcpy(three_writers[1], w.pub[1], 32);
    memcpy(three_writers[2], w.pub[3], 32);
    w.vault.writers = three_writers;
    w.vault.writers_len = 3;
    vault_record *component_a = record_make(&w, 1, 20, 8, "component a", NULL, 0, 0);
    vault_record *component_b = record_make(&w, 3, 20, 8, "component b", NULL, 0, 0);
    porthome_head_parent comp_a = parent_of(component_a, w.pub[1]);
    porthome_head_parent comp_b = parent_of(component_b, w.pub[3]);
    vault_record *covered_a = record_make(&w, 1, 20, 8, "component a", &comp_b, 1, 0);
    vault_record *covered_b = record_make(&w, 3, 20, 8, "component b", &comp_a, 1, 0);
    vault_record *checkpoint_owner = record_make(&w, 0, 20, 8, "owner checkpoint", NULL, 0, 0);
    const vault_record *component[] = {checkpoint_owner, covered_a, covered_b};
    assert(vault_fold(&w.vault, component, 3, &r) == VAULT_ERR_CONFLICT && r->view == SIZE_MAX);
    vault_fold_result_free(r);
    assert(vault_fold_checkpoint(received, component, 3, 0, &r) == 0 && r->view == 0 &&
           r->frontier_len == 1 && r->conflicts_len == 0);
    vault_fold_result_free(r);
    w.vault = saved_policy;
    vault_record_free(component_a); vault_record_free(component_b);
    vault_record_free(covered_a); vault_record_free(covered_b); vault_record_free(checkpoint_owner);
    vault_grant_free(received);
    vault_record_free(covered);
    vault_record_free(stale_cover);
    vault_record_free(reader_cover);
    vault_record *all[] = {base,     owner,    writer,      stale,   reader,    forged,
                           reader_a, reader_b, writer_fork, deleted, new_owner, bad_policy,
                           ta,       tb,       ca,          cb};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i)
        vault_record_free(all[i]);
    world_dispose(&w);
    puts("OK owner frontier/conflicts/retention (section13 d)");
    return 0;
}
