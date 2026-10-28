/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "nh_syncd_gc.h"
#include "../../../../libvault/tests/helpers.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int nh_syncd_state_upsert_file_(nh_syncd_state *, const char *, uint32_t, uint32_t, uint32_t,
                                uint64_t, uint64_t, const char *, const char *const *, size_t);
int nh_syncd_state_upsert_dir_(nh_syncd_state *, const char *, uint32_t, uint32_t, uint32_t, uint64_t);
int nh_syncd_state_set_lazy_dir_(nh_syncd_state *, const char *, bool);
int nh_syncd_state_delete_(nh_syncd_state *, const char *);
void nh_syncd_state_bump_generation_(nh_syncd_state *);
static void mkdir_at(const char *path) { assert(!mkdir(path, 0700)); }
static void json_file(const char *base, const char *name, json_t *j) {
    char *path = NULL;
    assert(asprintf(&path, "%s/%s", base, name) > 0);
    assert(!json_dump_file(j, path, JSON_INDENT(2) | JSON_SORT_KEYS));
    free(path);
}
static void v1_blob(world *w, const uint8_t key[32], const char *value, char hex[65]) {
    uint8_t *wire = NULL, hash[32]; size_t len = 0;
    assert(!porthome_encrypt_chunk(key, (const uint8_t *)value, strlen(value), &wire, &len, hash));
    assert(!porthome_sha256(wire, len, hash)); store_blob(w, wire, len, hash); free(wire);
    porthome_hex64(hash, hex);
}
static json_t *snapshot(world *w, size_t writer, uint64_t seq, const porthome_head_parent *parent,
                        NostrEvent **event_out, vault_tree **tree_out) {
    porthome_ring_t ring = vault_ring_view(&w->ring);
    const porthome_ring_entry_t *latest = &ring.entries[ring.count - 1];
    w->node.epoch = latest->epoch;
    uint8_t *sealed = NULL, root[32]; size_t len = 0;
    assert(!porthome_dir_node_seal(&w->node, latest->node_key, &sealed, &len, root));
    store_blob(w, sealed, len, root); free(sealed);
    porthome_head head = {.epoch = latest->epoch, .seq = seq, .grants_rev = w->vault.grants_rev,
        .written_at = seq + 100, .parents = (porthome_head_parent *)parent, .parents_len = parent ? 1 : 0};
    memcpy(head.vault_id, w->vault.vault_id, 32); memcpy(head.root_manifest, root, 32);
    NostrEvent *event = NULL;
    assert(!porthome_head_event_new(&head, latest->node_key, w->pub[writer], &event));
    assert(!nostr_event_sign(event, w->secret[writer]));
    vault_blob_io io = {w, read_blob, put_blob};
    vault_record *record = NULL; vault_tree *tree = NULL;
    assert(!vault_record_fetch(event, w->vault.vault_id, w->node.node_id, &ring, latest->epoch,
                               NULL, NULL, &io, &record, &tree));
    json_t *j = NULL;
    assert(!nh_vault_snapshot_new(event, tree, &j));
    vault_record_free(record);
    if (tree_out) *tree_out = tree; else vault_tree_free(tree);
    if (event_out) *event_out = event; else nostr_event_free(event);
    return j;
}
static porthome_head_parent parent_from(NostrEvent *event, world *w, uint64_t epoch) {
    porthome_head *head = NULL;
    porthome_ring_t ring = vault_ring_view(&w->ring);
    assert(!porthome_head_event_open(event, w->vault.vault_id, &ring, epoch, &head));
    porthome_head_parent p = {.seq = head->seq};
    assert(!porthome_from_hex64(nostr_event_get_pubkey(event), p.writer));
    memcpy(p.root_manifest, head->root_manifest, 32);
    porthome_head_dispose(head); free(head);
    return p;
}
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--due")) {
        printf("%d\n", nh_syncd_gc_due(argv[2], strtoull(argv[3], NULL, 10)));
        return 0;
    }
    if (argc >= 3 && (!strcmp(argv[1], "--expire-budget") || !strcmp(argv[1], "--receipt-invalidated"))) {
        uint8_t seed[32] = {9}, key[32], id[32] = {0x21};
        assert(!porthome_key_derive(seed, key));
        if (!strcmp(argv[1], "--expire-budget")) {
            int dir = open(argv[2], O_RDONLY | O_DIRECTORY); assert(dir >= 0);
            uint64_t start, used; assert(!nh_syncd_retention_load(dir, key, &start, &used));
            assert(!nh_syncd_retention_write(dir, key, (uint64_t)time(NULL) - 604801, used)); close(dir);
        } else {
            assert(argc == 4);
            nh_vault_session *disk = NULL; assert(!nh_vault_session_load(argv[2], id, key, &disk));
            assert(!json_object_get(json_object_get(disk->doc, "puts"), argv[3]));
            assert(json_is_true(json_object_get(json_object_get(disk->doc, "gc_candidates"), argv[3])));
            nh_vault_session_close(disk);
        }
        return 0;
    }
    assert(argc == 2);
    const char *base = argv[1];
    char *home = NULL, *state = NULL, *blobs = NULL, *pin_path = NULL, *vault_pin = NULL, *xdg = NULL;
    assert(asprintf(&home, "%s/home", base) > 0 && asprintf(&xdg, "%s/state", base) > 0);
    assert(asprintf(&state, "%s/nostr-homed", xdg) > 0 && asprintf(&blobs, "%s/blobs", base) > 0);
    assert(asprintf(&pin_path, "%s/pinned.json", state) > 0 && asprintf(&vault_pin, "%s/vault-pinned.json", state) > 0);
    mkdir_at(home); mkdir_at(xdg); mkdir_at(state); mkdir_at(blobs);
    int state_fd = open(state, O_RDONLY | O_DIRECTORY), blob_fd = open(blobs, O_RDONLY | O_DIRECTORY);
    assert(state_fd >= 0 && blob_fd >= 0);
    int lock = openat(state_fd, "sync.lock", O_WRONLY | O_CREAT, 0600); assert(lock >= 0); close(lock);
    world w; world_init(&w);
    uint8_t seed[32] = {9}, home_key[32], root_id[32] = {18};
    assert(!porthome_key_derive(seed, home_key));
    char pub[65]; porthome_hex64(w.pub[0], pub);
    const char *d_tag = "nostr-homed.home.v1:personal";
    nh_syncd_state *local = NULL;
    assert(!nh_syncd_state_new(home, d_tag, pub, root_id, &local));
    char current[65], retained[65], conflict[65], orphan[65], unrelated[65], remote_only[65];
    v1_blob(&w, home_key, "v1 current", current);
    v1_blob(&w, home_key, "v1 retained generation", retained);
    v1_blob(&w, home_key, "v1 preserved conflict", conflict);
    v1_blob(&w, home_key, "v1 retired garbage", orphan);
    v1_blob(&w, home_key, "unrelated account upload", unrelated);
    v1_blob(&w, home_key, "v1 remote only", remote_only);
    const char *hashes[] = {current};
    assert(!nh_syncd_state_upsert_file_(local, "file", 0600, 0, 0, 1, 10, current, hashes, 1));
    hashes[0] = conflict;
    assert(!nh_syncd_state_upsert_file_(local, "file.conflict-other", 0600, 0, 0, 1, 10, conflict, hashes, 1));
    /* A prior generation had a lazy Archive. The current snapshot drops its
     * child, but the retained generation must still protect that blob. */
    assert(!nh_syncd_state_upsert_dir_(local, "Archive", 0700, 0, 0, 1));
    assert(!nh_syncd_state_set_lazy_dir_(local, "Archive", true));
    hashes[0] = retained;
    assert(!nh_syncd_state_upsert_file_(local, "Archive/cold.txt", 0600, 0, 0, 1, 10, retained, hashes, 1));
    nh_syncd_state_bump_generation_(local);
    nh_syncd_state_bump_generation_(local);
    assert(!nh_syncd_state_save(local, state));
    char *snapshot_path = NULL;
    assert(asprintf(&snapshot_path, "%s/snapshot.json", state) > 0);
    json_t *archive_prior = json_load_file(snapshot_path, JSON_REJECT_DUPLICATES, NULL);
    assert(archive_prior);
    json_file(base, "lazy-archive-prior.json", archive_prior);
    json_decref(archive_prior);
    free(snapshot_path);
    nh_syncd_pin_ring *pins = NULL;
    assert(!nh_syncd_pin_ring_open(pin_path, &pins));
    hashes[0] = orphan; assert(!nh_syncd_pin_ring_promote(pins, 1, hashes, 1));
    assert(!nh_syncd_pin_ring_promote_from_snapshot(pins, state));
    assert(!nh_syncd_state_delete_(local, "Archive/cold.txt"));
    nh_syncd_state_bump_generation_(local);
    assert(!nh_syncd_state_save(local, state));
    nh_syncd_state_free(local);
    porthome_manifest m;
    assert(!porthome_manifest_init(&m, root_id));
    porthome_chunk c = {.size = 43}; assert(!porthome_from_hex64(remote_only, c.sha256));
    char *path_enc = NULL; assert(!porthome_encrypt_path(home_key, "remote", &path_enc));
    assert(!porthome_manifest_add_file(&m, path_enc, 0600, 0, 0, 1, 14, &c, 1));
    uint8_t *wire = NULL; size_t len = 0;
    assert(!porthome_manifest_encode_sealed(&m, home_key, &wire, &len)); porthome_manifest_dispose(&m);
    char *content = malloc(2 * len + 1); assert(content);
    for (size_t i = 0; i < len; ++i) sprintf(content + 2 * i, "%02x", wire[i]);
    free(wire);
    NostrEvent *v1 = nostr_event_new(); assert(v1);
    nostr_event_set_kind(v1, 30078); nostr_event_set_pubkey(v1, pub);
    nostr_event_set_content(v1, content); free(content); nostr_event_set_created_at(v1, 100);
    v1->tags = nostr_tags_new(1, nostr_tag_new("d", d_tag, NULL));
    assert(!nostr_event_sign(v1, w.secret[0]));
    json_t *events = json_array();
    char *serialized = nostr_event_serialize(v1);
    assert(serialized); assert(!json_array_append_new(events, json_loads(serialized, 0, NULL)));
    free(serialized); nostr_event_free(v1);

    nh_vault_session *disk = NULL;
    assert(!nh_vault_session_open(state, w.vault.vault_id, home_key, &disk));
    NostrEvent *old_event = NULL, *owner_event = NULL, *writer_event = NULL;
    json_t *old = snapshot(&w, 1, 1, NULL, &old_event, NULL);
    porthome_head_parent parent = parent_from(old_event, &w, 1);
    uint8_t next_key[32] = {42}; uint64_t epoch = 0;
    assert(!vault_ring_append(&w.ring, next_key, &epoch) && epoch == 2);
    w.chunks[0] = chunk(&w, next_key, 2, (const uint8_t *)"new epoch", 9);
    w.entries[0].size_plain = 9;
    vault_tree *owner_tree = NULL;
    json_t *owner = snapshot(&w, 0, 2, &parent, &owner_event, &owner_tree);
    porthome_head_parent owner_parent = parent_from(owner_event, &w, 2);
    char missing_manifest[65]; porthome_hex64(owner_parent.root_manifest, missing_manifest);
    w.chunks[0] = chunk(&w, next_key, 2, (const uint8_t *)"writer conflict", 15);
    w.entries[0].size_plain = 15;
    json_t *writer = snapshot(&w, 1, 2, &parent, &writer_event, NULL);
    w.chunks[0] = chunk(&w, next_key, 2, (const uint8_t *)"pending reseal view", 19);
    w.entries[0].size_plain = 19;
    json_t *pending = snapshot(&w, 0, 3, &owner_parent, NULL, NULL);
    w.chunks[0] = chunk(&w, next_key, 2, (const uint8_t *)"archive only", 12);
    w.entries[0].size_plain = 12;
    json_t *archive = snapshot(&w, 1, 1, &parent, NULL, NULL);
    vault_grant grant = {.cap = VAULT_CAP_WRITE, .issued_at = 1};
    memcpy(grant.vault_id, w.vault.vault_id, 32); memcpy(grant.node_id, w.node.node_id, 32);
    memcpy(grant.owner, w.pub[0], 32); memcpy(grant.root_manifest, owner_parent.root_manifest, 32);
    porthome_ring_t ring = vault_ring_view(&w.ring);
    assert(!vault_ring_copy(&grant.ring, &ring));
    char *grant_wire = NULL; assert(!vault_grant_encode(&grant, &grant_wire));
    assert(!json_object_set_new(disk->doc, "grant", json_string(grant_wire))); free(grant_wire);
    assert(!json_array_append(json_object_get(disk->doc, "heads"), owner));
    assert(!json_array_append(json_object_get(disk->doc, "heads"), writer));
    assert(!json_array_append(json_object_get(disk->doc, "retained"), old));
    assert(!json_object_set(disk->doc, "view", owner) && !json_object_set(disk->doc, "own", owner));
    assert(!json_object_set(disk->doc, "pending", pending));
    assert(!json_object_set_new(disk->doc, "views", json_pack("{s:O}", "9", archive)));
    assert(!json_array_append_new(json_object_get(disk->doc, "conflicts"), json_pack("{s:s}", "head", old_event->id)));
    for (size_t i = 0; i < 2; ++i) {
        NostrEvent *e = i ? writer_event : owner_event;
        serialized = nostr_event_serialize(e); assert(serialized);
        assert(!json_array_append_new(events, json_loads(serialized, 0, NULL))); free(serialized);
    }
    /* Real lazy reseal task: remote accepts bytes but reports failure. Its
     * pending address and durable session put-intent must both be protected. */
    disk->remote = (vault_blob_io){&w, read_blob, put_blob};
    memset(disk->remote_scope, 'a', 64); disk->remote_scope[64] = 0;
    vault_reseal_item item = {.node = &w.node, .ring = &w.ring, .entry = 1, .chunk = 0};
    vault_reseal_task *task = NULL; assert(!vault_reseal_task_new(&item, 1, 0, &task));
    w.fail_put = 1;
    assert(vault_reseal_task_run(task, 1024 * 1024, &disk->io) == VAULT_ERR_IO);
    const uint8_t *pending_hash = vault_reseal_task_pending_address(task); assert(pending_hash);
    char inflight[65]; porthome_hex64(pending_hash, inflight);
    w.fail_put = 0;
    char vault_garbage[65]; v1_blob(&w, home_key, "orphan vault upload", vault_garbage);
    json_t *puts = json_object();
    for (size_t i = 0; i < w.stored; ++i) {
        char hash[65]; porthome_hex64(w.store[i].hash, hash);
        if (strcmp(hash, unrelated)) assert(!json_object_set_new(puts, hash, json_string(disk->remote_scope)));
        assert(!nh_vault_file_write(blob_fd, hash, w.store[i].bytes, w.store[i].len));
    }
    assert(!json_object_set_new(disk->doc, "puts", puts));
    assert(!nh_vault_session_save(disk));
    json_t *marked = json_object(), *roots = json_object(), *parents = json_object();
    disk->io = (vault_blob_io){&w, read_blob, put_blob};
    assert(!nh_syncd_gc_mark_snapshot(disk, &grant, owner, marked, roots, parents));
    char old_hash[65]; porthome_hex64(w.chunks[1].sha256, old_hash);
    assert(json_object_get(marked, old_hash)); /* epoch 1 remains live at epoch 2 */
    json_decref(marked); json_decref(roots); json_decref(parents);
    /* Generation 2 is the only retained slot containing Archive/cold.txt;
     * generation 1 is evicted at capacity, while 2 remains the oldest slot. */
    for (uint64_t i = 3; i <= 11; ++i) { hashes[0] = current; assert(!nh_syncd_pin_ring_promote(pins, i, hashes, 1)); }
    hashes[0] = conflict; assert(!nh_syncd_pin_ring_promote(pins, 11, hashes, 1));
    nh_syncd_pin_ring_close(pins);
    assert(!nh_syncd_pin_ring_open(vault_pin, &pins));
    hashes[0] = missing_manifest; assert(!nh_syncd_pin_ring_promote(pins, 11, hashes, 1)); nh_syncd_pin_ring_close(pins);
    char id[65]; porthome_hex64(w.vault.vault_id, id);
    json_t *config = json_pack("{s:[{s:s,s:s,s:b}]}", "vaults", "path", "Team", "vault_id", id, "is_owner", 1);
    json_file(base, "syncd.json", config); json_decref(config);
    json_file(base, "events.json", events); json_decref(events);
    json_t *expected = json_pack("{s:[s,s],s:s,s:s,s:s,s:s,s:s}", "garbage", orphan, vault_garbage,
        "unrelated", unrelated, "inflight", inflight, "old_epoch", old_hash, "missing_manifest", missing_manifest,
        "lazy_archive_retained", retained);
    json_file(base, "expected.json", expected); json_decref(expected);
    json_decref(old); json_decref(owner); json_decref(writer); json_decref(pending); json_decref(archive);
    nostr_event_free(old_event); nostr_event_free(owner_event); nostr_event_free(writer_event);
    vault_tree_free(owner_tree); vault_ring_dispose(&grant.ring); vault_reseal_task_free(task);
    nh_vault_session_close(disk); close(state_fd); close(blob_fd); world_dispose(&w);
    free(home); free(xdg); free(state); free(blobs); free(pin_path); free(vault_pin);
    return 0;
}
