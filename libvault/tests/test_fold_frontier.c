/* SPDX-License-Identifier: MIT */
#include "helpers.h"
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
    vault_record *all[] = {base,     owner,    writer,      stale,   reader,    forged,
                           reader_a, reader_b, writer_fork, deleted, new_owner, bad_policy,
                           ta,       tb,       ca,          cb};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i)
        vault_record_free(all[i]);
    world_dispose(&w);
    puts("OK owner frontier/conflicts/retention (section13 d)");
    return 0;
}
