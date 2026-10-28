/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "nh_syncd_vault_engine.h"
#include "nh_syncd_vault_reader.h"
#include "nh_syncd_vault_net.h"
#include "nh_syncd_vault_fs.h"
#include <vault/nip59.h>
#include <time.h>
#include <json.h>
#include <nostr-keys.h>
#include <assert.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
int nh_syncd_state_upsert_file_(nh_syncd_state *, const char *, uint32_t, uint32_t, uint32_t,
                                uint64_t, uint64_t, const char *, const char *const *, size_t);
int nh_syncd_state_upsert_dir_(nh_syncd_state *, const char *, uint32_t, uint32_t, uint32_t,
                               uint64_t);
static const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";
static int cleanup(const char *p, const struct stat *s, int t, struct FTW *f) {
    (void)s;
    (void)t;
    (void)f;
    return remove(p);
}
static void write_file(const char *path, const void *bytes, size_t n) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
    assert(fd >= 0);
    assert(write(fd, bytes, n) == (ssize_t)n);
    assert(!close(fd));
}
typedef struct {
    int dir, uncertain, fail_reconcile;
    size_t reads, puts, chunks;
    uint8_t old[32];
} store;
static int get(void *ctx, const uint8_t hash[32], uint8_t **p, size_t *n) {
    store *s = ctx;
    ++s->reads;
    char h[65];
    porthome_hex64(hash, h);
    return nh_vault_file_read(s->dir, h, p, n);
}
static int put(void *ctx, const uint8_t *p, size_t n, const uint8_t hash[32]) {
    store *s = ctx;
    ++s->puts;
    if (!memcmp(hash, s->old, 32))
        ++s->chunks;
    char h[65];
    porthome_hex64(hash, h);
    int rc = nh_vault_file_write(s->dir, h, p, n);
    if (!rc && s->uncertain) {
        s->uncertain = 0;
        return VAULT_ERR_IO;
    }
    return rc;
}
static int reconcile(void *ctx, const uint8_t hash[32], const uint8_t *expected, size_t n) {
    store *s = ctx;
    if (s->fail_reconcile)
        return VAULT_ERR_IO;
    uint8_t *p = NULL;
    size_t len = 0;
    int rc = get(ctx, hash, &p, &len);
    if (!rc && (len != n || memcmp(p, expected, n)))
        rc = VAULT_ERR_AUTH;
    free(p);
    return rc;
}
static int publish(void *ctx, NostrEvent *e) {
    size_t *n = ctx;
    assert(nostr_event_check_signature(e));
    ++*n;
    return 0;
}
static void make_v1(const char *home, const char *state_dir, const uint8_t key[32],
                    const vault_blob_io *io, uint8_t hash[32]) {
    char path[2048];
    snprintf(path, sizeof path, "%s/Portable", home);
    assert(!mkdir(path, 0700));
    snprintf(path, sizeof path, "%s/Portable/Team", home);
    assert(!mkdir(path, 0700));
    snprintf(path, sizeof path, "%s/Portable/Team/nested", home);
    assert(!mkdir(path, 0700));
    const char bytes[] = "v1 migration preserves these exact old sealed bytes\n";
    snprintf(path, sizeof path, "%s/Portable/Team/nested/old", home);
    write_file(path, bytes, sizeof bytes - 1);
    uint8_t *sealed = NULL;
    size_t n = 0;
    assert(!porthome_encrypt_chunk(key, (uint8_t *)bytes, sizeof bytes - 1, &sealed, &n, hash));
    assert(!porthome_sha256(sealed, n, hash));
    assert(!io->put(io->ctx, sealed, n, hash));
    free(sealed);
    uint8_t root[32] = {1}, digest[32];
    char hex[65], plain[65];
    porthome_hex64(hash, hex);
    assert(!porthome_sha256((const uint8_t *)bytes, sizeof bytes - 1, digest));
    porthome_hex64(digest, plain);
    const char *chunks[] = {hex};
    nh_syncd_state *state = NULL;
    assert(!nh_syncd_state_new(home, "migration", "", root, &state));
    assert(!nh_syncd_state_upsert_dir_(state, "Portable/Team/nested", S_IFDIR | 0700, getuid(),
                                       getgid(), 1000000000));
    assert(!nh_syncd_state_upsert_file_(state, "Portable/Team/nested/old", S_IFREG | 0600, getuid(),
                                        getgid(), 1000000000, sizeof bytes - 1, plain, chunks, 1));
    assert(!nh_syncd_state_save(state, state_dir));
    nh_syncd_state_free(state);
}
static int send_wrap(void *ctx, const NostrEvent *event) {
    return nh_vault_net_publish(ctx, (NostrEvent *)event);
}
static int admin(int argc, char **argv) {
    assert(argc == 10);
    uint8_t id[32], seed[32], key[32], actor[32], member[32], secret[32];
    assert(!porthome_from_hex64(argv[4], id) && !porthome_from_hex64(argv[5], seed) &&
           !porthome_key_derive(seed, key));
    assert(!porthome_from_hex64(argv[6], secret) && !porthome_from_hex64(argv[7], member));
    char *pub = nostr_key_get_public(argv[6]);
    assert(pub && !porthome_from_hex64(pub, actor));
    free(pub);
    nh_vault_session *disk = NULL;
    assert(!nh_vault_session_load(argv[3], id, key, &disk));
    const char *relays[] = {argv[9]}, *servers[] = {argv[8]};
    nh_vault_net *net = NULL;
    assert(!nh_vault_net_open(argv[6], relays, 1, servers, 1, 1, 1, &net));
    disk->remote = nh_vault_net_blobs(net);
    NostrEvent **events = NULL;
    size_t count = 0;
    assert(!nh_vault_net_catchup(net, id, (const uint8_t(*)[32]) & actor, 1, &events, &count));
    for (size_t i = 0; i < count; ++i) {
        nostr_event_free(events[i]);
    }
    free(events);
    vault_grant *grant = NULL;
    assert(!vault_grant_decode(json_string_value(json_object_get(disk->doc, "grant")), &grant));
    vault_record *record = NULL;
    vault_tree *tree = NULL;
    assert(
        !nh_vault_snapshot_open(disk, grant, json_object_get(disk->doc, "view"), &record, &tree));
    const porthome_head *prev = vault_record_head(record);
    if (!strcmp(argv[2], "grant"))
        assert(!vault_tree_add_member(tree, actor, member, VAULT_CAP_WRITE));
    else {
        vault_tree *rotated = NULL;
        assert(!strcmp(argv[2], "revoke"));
        assert(!vault_tree_revoke(tree, prev, actor, member, prev->written_at + 1, &rotated));
        vault_tree_free(tree);
        tree = rotated;
    }
    uint8_t address[32];
    assert(!vault_tree_seal(tree, &disk->io, address));
    porthome_head *head = NULL;
    assert(!vault_tree_head_new(tree, prev, actor, address, prev->written_at + 1, &head));
    porthome_ring_t ring;
    const porthome_dir_node *root = vault_tree_node_at(tree, 0, &ring);
    const uint8_t *current = ring.entries[ring.count - 1].node_key;
    NostrEvent *event = NULL;
    assert(!porthome_head_event_new(head, current, actor, &event) &&
           !nostr_event_sign(event, argv[6]));
    assert(!nh_vault_net_publish(net, event));
    vault_ring_dispose(&grant->ring);
    assert(!vault_ring_init(&grant->ring, root->epoch, current));
    memcpy(grant->root_manifest, address, 32);
    grant->cap = VAULT_CAP_WRITE;
    grant->issued_at = (uint64_t)time(NULL);
    vault_nip59_transport transport = {.ctx = net, .publish = send_wrap};
    vault_nip59_carrier *carrier = NULL;
    assert(!vault_nip59_carrier_new(secret, &transport, &carrier));
    assert(!vault_grant_send(vault_nip59_carrier_interface(carrier),
                             !strcmp(argv[2], "grant") ? member : actor, grant));
    printf("ADMIN epoch=%llu\n", (unsigned long long)root->epoch);
    vault_nip59_carrier_free(carrier);
    nostr_event_free(event);
    porthome_head_dispose(head);
    free(head);
    vault_record_free(record);
    vault_tree_free(tree);
    vault_grant_free(grant);
    nh_vault_net_close(net);
    nh_vault_session_close(disk);
    return 0;
}
static int online(int argc, char **argv) {
    assert(argc >= 3);
    if (!strcmp(argv[1], "admin"))
        return admin(argc, argv);
    if (!strcmp(argv[1], "publish-auth")) {
        assert(argc == 4);
        const char *relays[] = {argv[2]}, *servers[] = {argv[3]};
        nh_vault_net *net = NULL;
        assert(!nh_vault_net_open(sk, relays, 1, servers, 1, 1, 1, &net));
        uint8_t actor[32], key[32] = {1};
        char *pub = nostr_key_get_public(sk);
        assert(pub && !porthome_from_hex64(pub, actor));
        free(pub);
        porthome_head head = {
            .epoch = 1, .seq = 1, .grants_rev = 1, .written_at = (uint64_t)time(NULL)};
        head.vault_id[0] = 1;
        head.root_manifest[0] = 2;
        NostrEvent **events = NULL;
        size_t count = 0;
        assert(!nh_vault_net_catchup(net, head.vault_id, (const uint8_t(*)[32]) & actor, 1, &events,
                                     &count));
        for (size_t i = 0; i < count; ++i) {
            nostr_event_free(events[i]);
        }
        free(events);
        NostrEvent *event = NULL;
        assert(!porthome_head_event_new(&head, key, actor, &event) && !nostr_event_sign(event, sk));
        assert(!nh_vault_net_publish(net, event));
        nostr_event_free(event);
        nh_vault_net_close(net);
        puts("PASS publish waited for matching OK while servicing reauthentication");
        return 0;
    }
    if (!strcmp(argv[1], "inspect")) {
        assert(argc == 5);
        uint8_t id[32], seed[32], key[32];
        assert(!porthome_from_hex64(argv[3], id) && !porthome_from_hex64(argv[4], seed) &&
               !porthome_key_derive(seed, key));
        nh_vault_session *disk = NULL;
        assert(!nh_vault_session_load(argv[2], id, key, &disk));
        char *cache_dir = nh_syncd_cache_default_dir();
        assert(cache_dir);
        nh_syncd_cache *cache = NULL;
        assert(!nh_syncd_cache_open(cache_dir, 0, &cache));
        free(cache_dir);
        disk->cache = cache;
        vault_grant *grant = NULL;
        assert(!vault_grant_decode(json_string_value(json_object_get(disk->doc, "grant")), &grant));
        vault_record *record = NULL;
        vault_tree *tree = NULL;
        assert(!nh_vault_snapshot_open(disk, grant, json_object_get(disk->doc, "view"), &record,
                                       &tree));
        const porthome_head *head = vault_record_head(record);
        size_t remaining = 0;
        for (size_t i = 0; i < vault_tree_count(tree); ++i) {
            const porthome_dir_node *node = vault_tree_node_at(tree, i, NULL);
            for (size_t k = 0; k < node->entries_len; ++k)
                for (size_t c = 0; c < node->entries[k].chunks_len; ++c)
                    if (node->entries[k].chunks[c].chunk_key_id != node->epoch)
                        ++remaining;
        }
        NostrEvent *event = nostr_event_new();
        assert(!nostr_event_deserialize(event, json_string_value(json_object_get(
                                                   json_object_get(disk->doc, "view"), "event"))));
        const porthome_dir_node *root = vault_tree_node_at(tree, 0, NULL);
        const porthome_node_chunk *chunk = NULL;
        for (size_t k = 0; k < root->entries_len; ++k)
            if (!strcmp(root->entries[k].name_plain, "file") && root->entries[k].chunks_len) chunk = &root->entries[k].chunks[0];
        char chunk_hex[65] = {0}; if (chunk) porthome_hex64(chunk->sha256, chunk_hex);
        printf("{\"head\":\"%s\",\"epoch\":%llu,\"remaining\":%zu,\"chunk_hash\":\"%s\",\"size_plain\":%llu}\n", event->id,
                 (unsigned long long)head->epoch, remaining, chunk_hex, (unsigned long long)(chunk ? chunk->size_plain : 0));
        nostr_event_free(event);
        vault_record_free(record);
        vault_tree_free(tree);
        vault_grant_free(grant);
        nh_vault_session_close(disk);
        nh_syncd_cache_close(cache);
        return 0;
    }
    if (!strcmp(argv[1], "seed-v1")) {
        assert(argc == 7);
        uint8_t seed[32], key[32], hash[32];
        assert(!porthome_from_hex64(argv[6], seed));
        assert(!porthome_key_derive(seed, key));
        const char *relays[] = {"ws://127.0.0.1:17777"}, *servers[] = {argv[4]};
        nh_vault_net *net = NULL;
        assert(!nh_vault_net_open(argv[5], relays, 1, servers, 1, 1, 1, &net));
        vault_blob_io io = nh_vault_net_blobs(net);
        make_v1(argv[2], argv[3], key, &io, hash);
        char h[65];
        porthome_hex64(hash, h);
        puts(h);
        nh_vault_net_close(net);
        return 0;
    }
    if (!strcmp(argv[1], "get")) {
        assert(argc == 5);
        uint8_t hash[32], *p = NULL;
        size_t n = 0;
        assert(!porthome_from_hex64(argv[4], hash));
        const char *relays[] = {"ws://127.0.0.1:17777"}, *servers[] = {argv[2]};
        nh_vault_net *net = NULL;
        assert(!nh_vault_net_open(argv[3], relays, 1, servers, 1, 1, 1, &net));
        vault_blob_io io = nh_vault_net_blobs(net);
        int rc = io.read(io.ctx, hash, &p, &n);
        printf("GET rc=%d bytes=%zu\n", rc, n);
        assert(rc == VAULT_ERR_AUTH && !p && !n);
        nh_vault_net_close(net);
        return 0;
    }
    if (!strcmp(argv[1], "ring-refusal")) {
        assert(argc == 12);
        uint8_t id[32], seed[32], key[32];
        assert(!porthome_from_hex64(argv[3], id));
        nh_vault_session *member = NULL;
        assert(!porthome_from_hex64(argv[4], seed));
        assert(!porthome_key_derive(seed, key));
        assert(!nh_vault_session_load(argv[2], id, key, &member));
        vault_grant *g = NULL;
        assert(!vault_grant_decode(json_string_value(json_object_get(member->doc, "grant")), &g));
        const char *servers[] = {argv[6]}, *relays[] = {argv[7]};
        nh_vault_net *net = NULL;
        assert(!nh_vault_net_open(argv[5], relays, 1, servers, 1, 1, 1, &net));
        NostrEvent **events = NULL;
        size_t count = 0;
        assert(
            !nh_vault_net_catchup(net, id, (const uint8_t(*)[32]) & g->owner, 1, &events, &count));
        int tested = 0;
        porthome_ring_t ring = vault_ring_view(&g->ring);
        for (size_t i = 0; i < count; ++i) {
            if (nostr_event_get_kind(events[i]) == PORTHOME_HEAD_KIND &&
                !strcmp(events[i]->id, argv[9])) {
                porthome_head *head = NULL;
                assert(nostr_event_check_signature(events[i]));
                assert(porthome_head_event_open(events[i], id, &ring, strtoull(argv[8], NULL, 10),
                                                &head) == PORTHOME_ERR_EPOCH &&
                       !head);
                ++tested;
            }
            nostr_event_free(events[i]);
        }
        assert(tested);
        free(events);
        uint8_t hash[32], *sealed = NULL, *plain = NULL; size_t n = 0, len = 0;
        assert(!porthome_from_hex64(argv[10], hash));
        vault_blob_io blobs = nh_vault_net_blobs(net);
        assert(!blobs.read(blobs.ctx, hash, &sealed, &n));
        assert(porthome_decrypt_chunk_v2(&ring, strtoull(argv[8], NULL, 10), sealed, n,
            (size_t)strtoull(argv[11], NULL, 10), &plain, &len) == PORTHOME_ERR_EPOCH && !plain && !len);
        free(sealed);
        puts("PASS epoch-only head AND new chunk refusal: PORTHOME_ERR_EPOCH, no output, no alternate key trials");
        vault_grant_free(g);
        nh_vault_net_close(net);
        nh_vault_session_close(member);
        return 0;
    }
    return 2;
}
static void fuse_check(const char *binary, const char *home, const char *dir, const char *work,
                       const nh_syncd_vault_config_entry *cfg, int expected) {
    char config[2048], status[2048], cache[2048], id[65], body[512], seed[65] = {0};
    snprintf(config, sizeof config, "%s/config.json", work);
    porthome_hex64(cfg->vault_id, id);
    snprintf(body, sizeof body,
             "{\"vaults\":[{\"path\":\"Portable/Team\",\"vault_id\":\"%s\",\"is_owner\":true}]}",
             id);
    write_file(config, body, strlen(body));
    snprintf(status, sizeof status, "%s/status", work);
    snprintf(cache, sizeof cache, "%s/cache", work);
    uint8_t raw[32] = {9};
    porthome_hex64(raw, seed);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        assert(!setenv("HOME", home, 1) && !setenv("NH_FUSE_STATE_DIR", dir, 1) &&
               !setenv("NOSTR_HOMED_SYNCD_CONFIG", config, 1));
        assert(!setenv("XDG_CACHE_HOME", cache, 1) && !setenv("XDG_STATE_HOME", status, 1));
        assert(!setenv("NH_FUSE_TEST_HARNESS", "1", 1) &&
               !setenv("NH_FUSE_TEST_READ", "Portable/Team/nested/old", 1));
        assert(!setenv("NH_FUSE_SEED_HEX", seed, 1) && !setenv("NOSTR_HOMED_SYNCD_NOTIFY", "0", 1));
        execl(binary, binary, (char *)NULL);
        _exit(127);
    }
    int code;
    assert(waitpid(pid, &code, 0) == pid && WIFEXITED(code) && WEXITSTATUS(code) == expected);
}
static porthome_dir_node *copy_node(const porthome_dir_node *node) {
    uint8_t *wire = NULL;
    size_t n = 0;
    porthome_dir_node *copy = NULL;
    assert(!porthome_dir_node_encode(node, &wire, &n));
    assert(!porthome_dir_node_decode(wire, n, &copy));
    free(wire);
    return copy;
}
static void node_free(porthome_dir_node *node) {
    porthome_dir_node_dispose(node);
    free(node);
}
static NostrEvent *signed_tree(nh_vault_session *disk, nh_vault_engine *engine, vault_tree *tree,
                               const uint8_t *wrong_key) {
    const vault_grant *grant = nh_vault_engine_grant(engine);
    vault_record *prev = NULL;
    vault_tree *old = NULL;
    assert(!nh_vault_snapshot_open(disk, grant, json_object_get(disk->doc, "view"), &prev, &old));
    uint8_t address[32], actor[32];
    char *pub = nostr_key_get_public(sk);
    assert(pub && !porthome_from_hex64(pub, actor));
    free(pub);
    assert(!vault_tree_seal(tree, &disk->io, address));
    porthome_head *head = NULL;
    assert(!vault_tree_head_new(tree, vault_record_head(prev), actor, address,
                                vault_record_head(prev)->written_at + 1, &head));
    porthome_ring_t ring;
    vault_tree_node_at(tree, 0, &ring);
    NostrEvent *event = NULL;
    assert(!porthome_head_event_new(
        head, wrong_key ? wrong_key : ring.entries[ring.count - 1].node_key, actor, &event));
    assert(!nostr_event_sign(event, sk));
    porthome_head_dispose(head);
    free(head);
    vault_record_free(prev);
    vault_tree_free(old);
    return event;
}
static void runtime_recovery(nh_vault_session *disk, nh_vault_engine **engine,
                             nh_syncd_state **state, const nh_syncd_vault_config_entry *cfg,
                             const nh_syncd_ignore *ignore, int home, const char *dir) {
    vault_record *record = NULL;
    vault_tree *original = NULL;
    assert(!nh_vault_snapshot_open(disk, nh_vault_engine_grant(*engine),
                                   json_object_get(disk->doc, "view"), &record, &original));
    uint8_t wrong[32] = {0x42};
    NostrEvent *event = signed_tree(disk, *engine, original, wrong);
    const vault_tree *before = nh_vault_engine_view(*engine);
    assert(!nh_vault_engine_can_capture(*engine, &event, 1));
    assert(nh_vault_engine_events(*engine, &event, 1) == VAULT_ERR_INCOMPLETE);
    assert(nh_vault_engine_view(*engine) == before);
    nostr_event_free(event);
    porthome_ring_t root_ring, child_ring;
    const porthome_dir_node *root = vault_tree_node_at(original, 0, &root_ring);
    const porthome_dir_node *child = vault_tree_node_at(original, 1, &child_ring);
    assert(root->entries_len == 1 && root->entries[0].kind == PORTHOME_KIND_DIR &&
           child_ring.count == 1);
    porthome_dir_node *empty = copy_node(root);
    empty->entries_len = 0;
    vault_tree_node only = {empty, root_ring};
    vault_tree *pruned = NULL;
    assert(!vault_tree_new(&only, 1, &pruned));
    empty->entries_len = root->entries_len;
    node_free(empty);
    event = signed_tree(disk, *engine, pruned, NULL);
    assert(!nh_vault_engine_events(*engine, &event, 1));
    nostr_event_free(event);
    assert(!nh_vault_engine_fold_apply(*engine) &&
           vault_tree_count(nh_vault_engine_view(*engine)) == 1);
    porthome_dir_node *re_root = copy_node(root), *re_child = copy_node(child);
    re_child->epoch = 2;
    re_root->entries[0].child_epoch = 2;
    re_root->entries[0].has_key_override = 1;
    memset(re_root->entries[0].key_override, 0x88, 32);
    porthome_ring_entry_t history[2] = {child_ring.entries[0], {.epoch = 2}};
    memcpy(history[1].node_key, re_root->entries[0].key_override, 32);
    vault_tree_node nodes[] = {{re_root, root_ring}, {re_child, {history, 2}}};
    vault_tree *reappeared = NULL;
    assert(!vault_tree_new(nodes, 2, &reappeared));
    event = signed_tree(disk, *engine, reappeared, NULL);
    assert(!nh_vault_engine_events(*engine, &event, 1));
    nostr_event_free(event);
    assert(!nh_vault_engine_fold_apply(*engine));
    porthome_ring_t held;
    const porthome_dir_node *restored = vault_tree_node_at(nh_vault_engine_view(*engine), 1, &held);
    assert(restored->epoch == 2 && held.count == 2 && held.entries[0].epoch == 1 &&
           held.entries[1].epoch == 2);
    assert(!nh_vault_session_held(disk, child->node_id, 2, &held) && held.count == 2);
    /* Conflicting node/epoch archive keys are refused, not selected by order. */
    json_t *bad = json_deep_copy(json_object_get(disk->doc, "view"));
    assert(bad);
    json_t *bad_keys = json_object_get(json_array_get(json_object_get(bad, "rings"), 1), "keys");
    char badhex[65];
    porthome_hex64(wrong, badhex);
    assert(!json_array_set_new(json_array_get(bad_keys, 0), 1, json_string(badhex)));
    json_t *retained = json_object_get(disk->doc, "retained");
    assert(!json_array_append_new(retained, bad));
    assert(nh_vault_session_held(disk, child->node_id, 2, &held) == VAULT_ERR_AUTH && !held.count);
    assert(!json_array_remove(retained, json_array_size(retained) - 1));
    const char *destination = "Portable/.conflicts/directory@writer";
    assert(!nh_syncd_state_save(*state, dir)); /* Last committed namespace lacks the conflict. */
    assert(!nh_syncd_vault_materialize_one(home, destination, reappeared,
                                           re_root->entries[0].name_enc, *state, &disk->io));
    json_t *view = json_object_get(disk->doc, "view");
    assert(!json_array_append_new(retained, json_deep_copy(view)));
    NostrEvent *at = nostr_event_new();
    assert(!nostr_event_deserialize(at, json_string_value(json_object_get(view, "event"))));
    json_t *row = json_pack("{s:s,s:s,s:s,s:s,s:b,s:b}", "head", at->id, "path",
                            "Portable/Team/nested", "destination", destination, "source_path",
                            re_root->entries[0].name_enc, "present", 1, "materialized", 1);
    assert(row && !json_array_append_new(json_object_get(disk->doc, "conflicts"), row));
    nostr_event_free(at);
    assert(!nh_vault_session_save(disk));
    nh_vault_engine_close(*engine);
    nh_syncd_state_free(*state);
    bool unknown = true;
    assert(!nh_syncd_state_load(dir, state, &unknown) && !unknown);
    assert(!nh_syncd_state_find(*state, "Portable/.conflicts/directory@writer/old"));
    assert(!nh_vault_engine_open(cfg, disk, home, *state, ignore, sk, engine));
    assert(!nh_vault_engine_fold_apply(*engine));
    assert(nh_syncd_state_find(*state, "Portable/.conflicts/directory@writer/old"));
    /* Reusing a directory suffix removes absent TRACKED children only. */
    int fd = openat(home, destination, O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);
    int check = openat(fd, "old", O_RDONLY);
    assert(check >= 0 && !close(check));
    int untracked = openat(fd, "untracked", O_CREAT | O_WRONLY, 0600);
    assert(untracked >= 0 && !close(untracked));
    size_t saved = re_child->entries_len;
    re_child->entries_len = 0;
    vault_tree *newer = NULL;
    assert(!vault_tree_new(nodes, 2, &newer));
    re_child->entries_len = saved;
    assert(!nh_syncd_vault_materialize_one(home, destination, newer, re_root->entries[0].name_enc,
                                           *state, &disk->io));
    struct stat st;
    assert(fstatat(fd, "old", &st, AT_SYMLINK_NOFOLLOW) < 0);
    assert(!fstatat(fd, "untracked", &st, AT_SYMLINK_NOFOLLOW));
    close(fd);
    vault_tree_free(newer);
    vault_tree_free(pruned);
    vault_tree_free(reappeared);
    vault_tree_free(original);
    vault_record_free(record);
    node_free(re_root);
    node_free(re_child);
}
/* ── nostrc-h10m.6: conflict resolution through the engine ──
 *
 * V1 of Portable/Shared is published, then a.txt changes and V2 is
 * published, so V1 is a retained signed snapshot. Conflict rows pointing at
 * V1 (as conflicts() records a losing writer's version) are resolved with
 * each choice; every resolution must sign + publish a head, leave HOME
 * matching that head (fold_apply's dirty preflight passes) and drop the
 * saved copy only after publication. */
#define OTHER_WRITER "abababababababababababababababababababababababababababababababab"
static int publish_offline(void *ctx, NostrEvent *e) {
    (void)ctx;
    (void)e;
    return VAULT_ERR_IO;
}
static void put_text(int home, const char *rel, const char *text) {
    int fd = openat(home, rel, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
    assert(fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text) && !close(fd));
}
static int has_text(int home, const char *rel, const char *text) {
    char buf[128] = {0};
    int fd = openat(home, rel, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    return n == (ssize_t)strlen(text) && !memcmp(buf, text, (size_t)n);
}
static int present(int home, const char *rel) {
    struct stat st;
    return !fstatat(home, rel, &st, AT_SYMLINK_NOFOLLOW);
}
static void inject(nh_vault_session *disk, const char *head, const char *leaf, const char *enc,
                   int saved) {
    char path[128], dest[256];
    snprintf(path, sizeof path, "Portable/Shared/%s", leaf);
    snprintf(dest, sizeof dest, "Portable/.conflicts/%s@%s", path, OTHER_WRITER);
    json_t *row = saved ? json_pack("{s:s,s:s,s:s,s:b,s:b,s:s,s:s}", "path", path, "writer",
                                    OTHER_WRITER, "head", head, "present", 1, "materialized", 0,
                                    "destination", dest, "source_path", enc)
                        : json_pack("{s:s,s:s,s:s,s:b,s:b}", "path", path, "writer", OTHER_WRITER,
                                    "head", head, "present", 0, "materialized", 0);
    assert(row && !json_array_append_new(json_object_get(disk->doc, "conflicts"), row));
    assert(!nh_vault_session_save(disk));
}
static json_t *projected(nh_vault_engine *e, const char *leaf, char id[65], char token[65]) {
    json_t *rows = json_array();
    assert(!nh_vault_engine_project_conflicts(e, rows));
    char path[128];
    snprintf(path, sizeof path, "Portable/Shared/%s", leaf);
    size_t i;
    json_t *row, *hit = NULL;
    json_array_foreach(rows, i, row) if (!strcmp(json_string_value(json_object_get(row, "path")),
                                                 path)) hit = json_incref(row);
    if (hit) {
        snprintf(id, 65, "%s", json_string_value(json_object_get(hit, "id")));
        snprintf(token, 65, "%s", json_string_value(json_object_get(hit, "expect")));
    }
    json_decref(rows);
    return hit;
}
static void resolve_flow(const char *dir, int home, nh_syncd_state *state,
                         const nh_syncd_ignore *ignore, const vault_blob_io *io,
                         nh_syncd_cache *cache, const uint8_t key[32], const uint8_t rootkey[32]) {
    static const char *names[] = {"a.txt", "b.txt", "c.txt", "d.txt", "e.txt", "f.txt"};
    assert(!mkdirat(home, "Portable/Shared", 0700));
    for (size_t i = 0; i < 6; ++i) {
        char rel[64], text[32];
        snprintf(rel, sizeof rel, "Portable/Shared/%s", names[i]);
        snprintf(text, sizeof text, "v1 %s\n", names[i]);
        put_text(home, rel, text);
    }
    nh_syncd_vault_config_entry cfg = {.path = "Portable/Shared", .is_owner = 1};
    cfg.vault_id[0] = 99;
    nh_vault_session *disk = NULL;
    nh_vault_engine *e = NULL;
    assert(!nh_vault_session_open(dir, cfg.vault_id, key, &disk));
    disk->remote = *io;
    disk->cache = cache;
    strcpy(disk->remote_scope, "fixture");
    assert(!nh_vault_engine_open(&cfg, disk, home, state, ignore, sk, &e));
    assert(!nh_vault_engine_owner_init(e, 0, rootkey));
    size_t pubs = 0;
    assert(!nh_vault_engine_publish_pending(e, publish, &pubs) && pubs == 1);
    assert(!nh_vault_engine_fold_apply(e));

    /* V1: remember its signed snapshot and every file's encrypted name. */
    json_t *v1 = json_deep_copy(json_object_get(disk->doc, "view"));
    NostrEvent *v1_event = nostr_event_new();
    assert(v1 && !nostr_event_deserialize(v1_event, json_string_value(json_object_get(v1, "event"))));
    char v1_id[65];
    snprintf(v1_id, sizeof v1_id, "%s", v1_event->id);
    nostr_event_free(v1_event);
    char enc[6][256] = {{0}};
    const porthome_dir_node *root = vault_tree_node_at(nh_vault_engine_view(e), 0, NULL);
    for (size_t i = 0; i < root->entries_len; ++i)
        for (size_t k = 0; k < 6; ++k)
            if (!strcmp(root->entries[i].name_plain, names[k]))
                snprintf(enc[k], sizeof enc[k], "%s", root->entries[i].name_enc);
    for (size_t k = 0; k < 6; ++k)
        assert(enc[k][0]);

    /* V2: a.txt changes and is published; V1 stays retained. */
    put_text(home, "Portable/Shared/a.txt", "v2 a.txt\n");
    assert(!nh_vault_engine_capture(e, NULL, publish, &pubs) && pubs == 2);
    assert(!nh_vault_engine_fold_apply(e));
    assert(!json_array_append_new(json_object_get(disk->doc, "retained"), v1));

    inject(disk, v1_id, "a.txt", enc[0], 1);
    inject(disk, v1_id, "b.txt", enc[1], 1);
    inject(disk, v1_id, "c.txt", enc[2], 1);
    inject(disk, v1_id, "d.txt", NULL, 0); /* deletion-only: the other writer removed it */
    inject(disk, v1_id, "e.txt", enc[4], 1);
    /* The normal fold materialises the saved copies from the signed V1. */
    assert(!nh_vault_engine_fold_apply(e));
    const char *copy_a = "Portable/.conflicts/Portable/Shared/a.txt@" OTHER_WRITER;
    assert(has_text(home, copy_a, "v1 a.txt\n"));

    char id[65], token[65];
    json_t *row = projected(e, "a.txt", id, token);
    assert(row && strlen(id) == 64 && strcmp(token, "absent"));
    assert(!strcmp(json_string_value(json_object_get(row, "mine")), "current")); /* owner view */
    assert(json_array_size(json_object_get(row, "choices")) == 3);
    assert(!json_object_get(row, "source_path") && !json_object_get(row, "resolution"));
    json_decref(row);
    row = projected(e, "d.txt", id, token);
    assert(row && json_is_false(json_object_get(row, "present")) &&
           json_array_size(json_object_get(row, "choices")) == 2);
    json_decref(row);

    const char *why = NULL;
    /* Refusals publish nothing. */
    json_decref(projected(e, "a.txt", id, token));
    assert(nh_vault_engine_resolve(e, id, NH_CONFLICT_USE_SAVED, "0000", "11111111111111111111111111111111",
                                   publish, &pubs, &why) < 0 && !strcmp(why, "stale"));
    json_decref(projected(e, "d.txt", id, token));
    assert(nh_vault_engine_resolve(e, id, NH_CONFLICT_KEEP_BOTH, token, "22222222222222222222222222222222",
                                   publish, &pubs, &why) < 0 && !strcmp(why, "unsupported-choice"));
    assert(nh_vault_engine_resolve(e, "ff", NH_CONFLICT_KEEP_CURRENT, "", "33333333333333333333333333333333",
                                   publish, &pubs, &why) < 0 && !strcmp(why, "not-found"));
    assert(pubs == 2);

    /* use-saved: V1's a.txt comes back and is published as a new head. */
    json_decref(projected(e, "a.txt", id, token));
    assert(!nh_vault_engine_resolve(e, id, NH_CONFLICT_USE_SAVED, token, "44444444444444444444444444444444",
                                    publish, &pubs, &why));
    assert(pubs == 3 && has_text(home, "Portable/Shared/a.txt", "v1 a.txt\n"));
    assert(!present(home, copy_a));
    assert(!nh_vault_engine_fold_apply(e)); /* HOME matches the published head */
    /* The new view itself carries V1's bytes (same length as V2's). */
    uint8_t bytes[32];
    size_t got = 0;
    assert(!nh_syncd_vault_read(nh_vault_engine_view(e), "a.txt", 0, bytes, sizeof bytes,
                                &disk->io, &got) &&
           got == 9 && !memcmp(bytes, "v1 a.txt\n", 9));
    assert(!projected(e, "a.txt", id, token));
    /* Duplicate delivery of the finished request is a no-op. */
    assert(!nh_vault_engine_resolve(e, id, NH_CONFLICT_USE_SAVED, "", "44444444444444444444444444444444",
                                    publish, &pubs, &why) && pubs == 3);
    /* ...and a resolved row is never rematerialised by later folds. */
    assert(!nh_vault_engine_fold_apply(e) && !present(home, copy_a));

    /* keep-both: the saved version is added next to the current one. */
    json_decref(projected(e, "b.txt", id, token));
    assert(!nh_vault_engine_resolve(e, id, NH_CONFLICT_KEEP_BOTH, token, "55555555555555555555555555555555",
                                    publish, &pubs, &why));
    assert(pubs == 4 && has_text(home, "Portable/Shared/b.txt", "v1 b.txt\n"));
    assert(has_text(home, "Portable/Shared/b (conflicted copy from abababab).txt", "v1 b.txt\n"));
    assert(!nh_vault_engine_fold_apply(e));
    const porthome_node_entry *found = NULL;
    porthome_ring_t ring;
    assert(!nh_syncd_vault_lookup(nh_vault_engine_view(e), "b (conflicted copy from abababab).txt", 0,
                                  &found, &ring) && found);

    /* keep-current still signs a head (it covers the conflicting one). */
    json_decref(projected(e, "c.txt", id, token));
    assert(!nh_vault_engine_resolve(e, id, NH_CONFLICT_KEEP_CURRENT, token, "66666666666666666666666666666666",
                                    publish, &pubs, &why));
    assert(pubs == 5 && has_text(home, "Portable/Shared/c.txt", "v1 c.txt\n"));
    assert(!present(home, "Portable/.conflicts/Portable/Shared/c.txt@" OTHER_WRITER));

    /* Deletion-only, use-saved = delete the file, and that is published. */
    json_decref(projected(e, "d.txt", id, token));
    assert(!nh_vault_engine_resolve(e, id, NH_CONFLICT_USE_SAVED, token, "77777777777777777777777777777777",
                                    publish, &pubs, &why));
    assert(pubs == 6 && !present(home, "Portable/Shared/d.txt"));
    assert(!nh_vault_engine_fold_apply(e));
    assert(nh_syncd_vault_lookup(nh_vault_engine_view(e), "d.txt", 0, &found, &ring) != 0);

    /* Offline: signed and journalled, row pending, saved copy kept until a
     * later publication succeeds and commits it. */
    json_decref(projected(e, "e.txt", id, token));
    assert(nh_vault_engine_resolve(e, id, NH_CONFLICT_USE_SAVED, token, "88888888888888888888888888888888",
                                   publish_offline, NULL, &why) == NH_CONFLICT_QUEUED);
    assert(!strcmp(why, "offline"));
    assert(present(home, "Portable/.conflicts/Portable/Shared/e.txt@" OTHER_WRITER));
    row = projected(e, "e.txt", id, token);
    assert(row && !strcmp(json_string_value(json_object_get(row, "state")), "pending"));
    json_decref(row);
    assert(!nh_vault_engine_publish_pending(e, publish, &pubs) && pubs == 7);
    assert(!projected(e, "e.txt", id, token));
    assert(!present(home, "Portable/.conflicts/Portable/Shared/e.txt@" OTHER_WRITER));
    assert(has_text(home, "Portable/Shared/e.txt", "v1 e.txt\n"));
    assert(!nh_vault_engine_fold_apply(e));

    /* Every resolved row is still in the journal, with its resolution. */
    size_t i, resolved = 0;
    json_array_foreach(json_object_get(disk->doc, "conflicts"), i, row) {
        const char *st = json_string_value(json_object_get(json_object_get(row, "resolution"), "state"));
        if (st && !strcmp(st, "resolved")) {
            ++resolved;
            assert(json_integer_value(json_object_get(row, "resolved_generation")) > 0);
        }
    }
    assert(resolved == 5);
    nh_vault_engine_close(e);
    nh_vault_session_close(disk);
    puts("PASS vault conflict resolution: use-saved/keep-both/keep-current/delete publish a head; "
         "offline resolutions commit on later publication");
}
void nh_syncd_state_bump_generation_(nh_syncd_state *);
int main(int argc, char **argv) {
    int fuse_mode = argc == 3 && !strcmp(argv[1], "--fuse");
    if (argc > 1 && !fuse_mode)
        return online(argc, argv);
    char work[] = "/tmp/nostrc-vault-runtime-XXXXXX";
    assert(mkdtemp(work));
    char home[1024], dir[1024], remote[1024], cache_dir[1024], path[2048];
    snprintf(home, sizeof home, "%s/home", work);
    snprintf(dir, sizeof dir, "%s/state", work);
    snprintf(remote, sizeof remote, "%s/remote", work);
    snprintf(cache_dir, sizeof cache_dir, "%s/cache/nostr-homed/blobs", work);
    assert(!mkdir(home, 0700) && !mkdir(dir, 0700) && !mkdir(remote, 0700));
    store storage = {.dir = open(remote, O_RDONLY | O_DIRECTORY)};
    assert(storage.dir >= 0);
    vault_blob_io io = {&storage, get, put};
    uint8_t seed[32] = {9}, key[32], rootkey[32] = {10};
    assert(!porthome_key_derive(seed, key));
    make_v1(home, dir, key, &io, storage.old);
    storage.reads = storage.puts = storage.chunks = 0;
    nh_syncd_state *state = NULL;
    bool unknown = true;
    assert(!nh_syncd_state_load(dir, &state, &unknown) && !unknown);
    nh_syncd_ignore *ignore = NULL;
    assert(!nh_syncd_ignore_new(home, &ignore));
    nh_syncd_vault_config_entry cfg = {.path = "Portable/Team", .is_owner = 1};
    cfg.vault_id[0] = 77;
    nh_vault_session *disk = NULL;
    assert(!nh_vault_session_open(dir, cfg.vault_id, key, &disk));
    disk->remote = io;
    strcpy(disk->remote_scope, "fixture");
    nh_syncd_cache *cache = NULL;
    assert(!nh_syncd_cache_open(cache_dir, 16u * 1024u * 1024u, &cache));
    disk->cache = cache;
    disk->reconcile = reconcile;
    uint8_t *uncertain = NULL, address[32];
    size_t uncertain_len = 0;
    assert(
        !porthome_encrypt_manifest(key, (const uint8_t *)"receipt", 7, &uncertain, &uncertain_len));
    assert(!porthome_sha256(uncertain, uncertain_len, address));
    storage.uncertain = 1;
    assert(disk->io.put(disk->io.ctx, uncertain, uncertain_len, address) == VAULT_ERR_IO);
    size_t attempts = storage.puts;
    nh_vault_session_close(disk);
    assert(!nh_vault_session_open(dir, cfg.vault_id, key, &disk));
    disk->cache = cache;
    disk->remote = io;
    disk->reconcile = reconcile;
    strcpy(disk->remote_scope, "fixture");
    storage.fail_reconcile = 1;
    assert(disk->io.put(disk->io.ctx, uncertain, uncertain_len, address) == VAULT_ERR_INCOMPLETE &&
           storage.puts == attempts);
    storage.fail_reconcile = 0;
    strcpy(disk->remote_scope, "other-scope");
    assert(disk->io.put(disk->io.ctx, uncertain, uncertain_len, address) == VAULT_ERR_INCOMPLETE &&
           storage.puts == attempts);
    strcpy(disk->remote_scope, "fixture");
    assert(!disk->io.put(disk->io.ctx, uncertain, uncertain_len, address) &&
           storage.puts == attempts);
    free(uncertain);
    storage.reads = storage.puts = storage.chunks = 0;
    int homefd = open(home, O_RDONLY | O_DIRECTORY);
    assert(homefd >= 0);
    nh_vault_engine *engine = NULL;
    assert(!nh_vault_engine_open(&cfg, disk, homefd, state, ignore, sk, &engine));
    assert(!nh_vault_engine_owner_init(engine, 1, rootkey));
    assert(!storage.chunks && !storage.reads); /* No old chunk download or upload. */
    size_t publications = 0;
    assert(!nh_vault_engine_publish_pending(engine, publish, &publications) && publications == 1);
    assert(!nh_vault_engine_fold_apply(engine));
    const vault_tree *tree = nh_vault_engine_view(engine);
    assert(vault_tree_count(tree) == 2);
    porthome_ring_t ring;
    const porthome_dir_node *node = vault_tree_node_at(tree, 0, &ring);
    assert(ring.count == 1 && ring.entries[0].epoch == 1 &&
           !memcmp(ring.entries[0].node_key, key, 32));
    int overrides = 0;
    for (size_t i = 0; i < node->entries_len; ++i)
        if (node->entries[i].kind == PORTHOME_KIND_DIR) {
            assert(node->entries[i].has_key_override &&
                   !memcmp(node->entries[i].key_override, key, 32));
            ++overrides;
        }
    assert(overrides == 1 && !storage.chunks);
    json_t *view = json_object_get(disk->doc, "view");
    assert(!json_object_set_new(disk->doc, "views", json_pack("{s:O}", "1", view)));
    assert(!json_object_set_new(disk->doc, "cache_only", json_true()));
    assert(!nh_vault_session_save(disk));
    /* Fill the real cache through the production vault reader, then cache-only FUSE reads. */
    uint8_t *sealed = NULL;
    size_t sealed_n = 0;
    assert(!get(&storage, storage.old, &sealed, &sealed_n));
    char hex[65];
    porthome_hex64(storage.old, hex);
    assert(!nh_syncd_cache_put(cache, hex, sealed, sealed_n));
    free(sealed);
    if (fuse_mode) {
        nh_syncd_state_bump_generation_(state);
        assert(!nh_syncd_state_save(state, dir));
        fuse_check(argv[2], home, dir, work, &cfg, 0);
        char *cached = nh_syncd_cache_get_path(cache, hex);
        assert(cached);
        char *held = NULL;
        assert(asprintf(&held, "%s.held", cached) > 0);
        assert(!rename(cached, held));
        fuse_check(argv[2], home, dir, work, &cfg, 6);
        assert(!rename(held, cached));
        free(held);
        free(cached);
    }
    const nh_syncd_entry *entry = nh_syncd_state_find(state, "Portable/Team/nested/old");
    assert(entry);
    const char *chunks[] = {hex};
    nh_syncd_vault_config config = {.vaults = &cfg, .len = 1};
    nh_vault_reader *reader = NULL;
    assert(!nh_vault_reader_open(&config, dir, key, cache, NULL, "Portable/Team/nested/old", 1,
                                 nh_syncd_entry_size(entry), chunks, 1, &reader));
    uint8_t output[200];
    size_t got = 0;
    assert(!nh_vault_reader_read(reader, 3, output, sizeof output, &got) && got > 10 &&
           !memcmp(output, "migration", 9));
    snprintf(path, sizeof path, "%s/Portable/Team/nested/old", home);
    write_file(path, "changed by this writer\n", 23);
    assert(nh_vault_engine_fold_apply(engine) ==
           VAULT_ERR_AGAIN); /* Dirty HOME is not overwritten by remote apply. */
    assert(!nh_vault_engine_capture(engine, NULL, publish, &publications) && publications == 2);
    assert(!nh_vault_engine_fold_apply(engine));
    if (fuse_mode) {
        assert(!json_object_set_new(json_object_get(disk->doc, "views"), "2", json_deep_copy(json_object_get(disk->doc, "view"))));
        assert(!nh_vault_session_save(disk)); nh_syncd_state_bump_generation_(state);
        assert(!nh_syncd_state_save(state, dir));
        fuse_check(argv[2], home, dir, work, &cfg, 0); /* New v2 bytes, not the old v1 HOME-key ladder. */
    }
    /* An already-open reader remains pinned to its old keys and CDC descriptors. */
    assert(!nh_vault_reader_read(reader, 0, output, sizeof output, &got) && got > 10 &&
           !memcmp(output, "v1 migration", 12));
    nh_vault_reader_close(reader);
    nh_vault_engine_close(engine);
    nh_vault_session_close(disk);
    assert(!nh_vault_session_open(dir, cfg.vault_id, key, &disk));
    disk->cache = cache;
    disk->remote = io;
    assert(!nh_vault_engine_open(&cfg, disk, homefd, state, ignore, sk, &engine));
    assert(!nh_vault_engine_capture(engine, NULL, publish, &publications) && publications == 2);
    runtime_recovery(disk, &engine, &state, &cfg, ignore, homefd, dir);
    uint8_t prior_key[32] = {0};
    for (unsigned k = 0; k < 2; ++k) {
        nh_syncd_vault_config_entry other = {.path = "Other", .is_owner = 1}; other.vault_id[0] = (uint8_t)(88 + k);
        nh_vault_session *journal = NULL; nh_vault_engine *fresh = NULL;
        assert(!nh_vault_session_open(dir, other.vault_id, key, &journal));
        journal->remote = io; journal->cache = cache; strcpy(journal->remote_scope, "fixture");
        assert(!nh_vault_engine_open(&other, journal, homefd, state, ignore, sk, &fresh));
        assert(!nh_vault_engine_owner_init(fresh, 0, rootkey));
        const vault_grant *grant = nh_vault_engine_grant(fresh); uint8_t expected[32];
        assert(!porthome_child_node_key(rootkey, grant->node_id, NULL, expected));
        assert(grant->ring.count == 1 && !memcmp(expected, grant->ring.entries[0].node_key, 32));
        assert(memcmp(expected, rootkey, 32) && (!k || memcmp(expected, prior_key, 32)));
        memcpy(prior_key, expected, 32);
        nh_vault_engine_close(fresh); nh_vault_session_close(journal);
    }
    if (!fuse_mode)
        resolve_flow(dir, homefd, state, ignore, &io, cache, key, rootkey);
    nh_vault_engine_close(engine);
    nh_vault_session_close(disk);
    nh_syncd_cache_close(cache);
    close(homefd);
    close(storage.dir);
    nh_syncd_ignore_free(ignore);
    nh_syncd_state_free(state);
    assert(!nftw(work, cleanup, 32, FTW_DEPTH | FTW_PHYS));
    puts("PASS engine migration/no-reupload, capture, immutable cache-only reader, restart "
         "idempotence");
    return 0;
}
