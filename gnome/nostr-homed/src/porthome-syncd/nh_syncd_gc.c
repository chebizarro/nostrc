/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "nh_syncd_gc.h"
#include "nh_syncd_vault_net.h"
#include "nh_porthome_status.h"
#include <porthome/manifest.h>
#include <nostr-keys.h>
#include <json.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int nh_syncd__snapshot_hashes(const char *, uint64_t *, char ***, size_t *);

/* A bounded pass may keep garbage, never truncate a reachability walk. */
#define GC_MAX_HASHES 262144u
#define GC_MAX_FILE (64u * 1024u * 1024u)
#define GC_DELETE_COST 1024u

typedef struct {
    nh_vault_session *disk;
    vault_grant *grant;
    uint64_t owner_seq;
    uint8_t owner_root[32];
} gc_vault;
typedef struct {
    const nh_syncd_gc_options *o;
    int dir;
    uint64_t used, limit;
    nh_vault_net *net;
    blossom_client_t *clients[VAULT_MAX_HINTS];
    gc_vault *vaults;
    size_t count;
    int require_home;
    json_t *marked, *candidates, *roots, *parents, *events, *fences;
    const char *reason;
} gc_walk;
static int hex64(const char *s) {
    return s && strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64;
}
static int add(json_t *set, const char *hash) {
    if (!hex64(hash)) return VAULT_ERR_ARG;
    if (!json_object_get(set, hash) && json_object_size(set) >= GC_MAX_HASHES)
        return VAULT_ERR_BUDGET;
    return json_object_set_new(set, hash, json_true()) ? VAULT_ERR_OOM : 0;
}
static int add_bytes(json_t *set, const uint8_t hash[32]) {
    char hex[65]; porthome_hex64(hash, hex); return add(set, hex);
}
static int charge(gc_walk *w, uint64_t bytes) {
    if (w->used > w->limit || bytes > w->limit - w->used) return VAULT_ERR_BUDGET;
    w->used += bytes;
    return 0;
}
int nh_syncd_retention_write(int dir, const uint8_t key[32], uint64_t start, uint64_t used) {
    if (start > INT64_MAX || used > INT64_MAX) return VAULT_ERR_ARG;
    json_t *j = json_pack("{s:I,s:I}", "start", (json_int_t)start, "used", (json_int_t)used);
    char *wire = j ? json_dumps(j, JSON_COMPACT) : NULL;
    json_decref(j);
    if (!wire) return VAULT_ERR_OOM;
    uint8_t *sealed = NULL; size_t n = 0;
    int rc = porthome_encrypt_manifest(key, (uint8_t *)wire, strlen(wire), &sealed, &n);
    free(wire);
    if (!rc) rc = nh_vault_file_write(dir, "vault-retention.sealed", sealed, n);
    free(sealed);
    return rc;
}
int nh_syncd_retention_load(int dir, const uint8_t key[32], uint64_t *out_start, uint64_t *out_used) {
    uint64_t now = (uint64_t)time(NULL), start = now, used = 0;
    uint8_t *sealed = NULL, *plain = NULL; size_t n = 0, len = 0;
    int rc = nh_vault_file_read(dir, "vault-retention.sealed", &sealed, &n);
    if (!rc) rc = porthome_decrypt_manifest(key, sealed, n, &plain, &len);
    if (!rc) {
        json_t *j = json_loadb((char *)plain, len, JSON_REJECT_DUPLICATES, NULL);
        json_int_t a = json_integer_value(json_object_get(j, "start"));
        json_int_t b = json_integer_value(json_object_get(j, "used"));
        if (!json_is_object(j) || json_object_size(j) != 2 ||
            !json_is_integer(json_object_get(j, "start")) ||
            !json_is_integer(json_object_get(j, "used")) || a <= 0 || b < 0)
            rc = VAULT_ERR_ARG;
        else { start = (uint64_t)a; used = (uint64_t)b; }
        json_decref(j);
    }
    if (rc == VAULT_ERR_INCOMPLETE) rc = 0;
    free(sealed);
    if (plain) { OPENSSL_cleanse(plain, len); free(plain); }
    if (rc) return rc;
    if (now >= start && now - start >= NH_SYNCD_CACHE_SWEEP_PERIOD_SECS) { start = now; used = 0; }
    *out_start = start; *out_used = used;
    return 0;
}
static int mark_record(const vault_record *record, json_t *marked, json_t *roots, json_t *parents) {
    const porthome_head *head = vault_record_head(record);
    /* Unknown reference-state codecs cannot be treated as empty graphs. */
    if (!head || head->has_ref_state) return VAULT_ERR_INCOMPLETE;
    int rc = add_bytes(roots, head->root_manifest);
    for (size_t i = 0; !rc && i < head->parents_len; ++i)
        rc = add_bytes(parents, head->parents[i].root_manifest);
    for (size_t i = 0; !rc && i < vault_record_manifest_count(record); ++i) {
        const uint8_t *address = NULL;
        const porthome_dir_node *node = vault_record_manifest_at(record, i, &address);
        if (!node || !address) return VAULT_ERR_INCOMPLETE;
        rc = add_bytes(marked, address);
        for (size_t e = 0; !rc && e < node->entries_len; ++e) {
            const porthome_node_entry *entry = &node->entries[e];
            if (entry->kind != PORTHOME_KIND_FILE) continue;
            /* Epoch is deliberately NOT filtered: lazy reseal needs old chunks. */
            for (size_t c = 0; !rc && c < entry->chunks_len; ++c)
                rc = add_bytes(marked, entry->chunks[c].sha256);
        }
    }
    return rc;
}
int nh_syncd_gc_mark_snapshot(nh_vault_session *disk, const vault_grant *grant, const json_t *snapshot,
                             json_t *marked, json_t *roots, json_t *parents) {
    vault_record *record = NULL; vault_tree *tree = NULL;
    int rc = nh_vault_snapshot_open(disk, grant, snapshot, &record, &tree);
    if (!rc) rc = mark_record(record, marked, roots, parents);
    vault_record_free(record); vault_tree_free(tree);
    return rc;
}
static int read_manifest(void *ctx, const uint8_t hash[32], uint8_t **out, size_t *len) {
    gc_walk *w = ctx;
    *out = NULL; *len = 0;
    uint64_t remaining = w->limit - w->used;
    if (!remaining) return VAULT_ERR_BUDGET;
    nh_vault_net_read_limit(w->net, remaining < GC_MAX_FILE ? (size_t)remaining : GC_MAX_FILE);
    vault_blob_io io = nh_vault_net_blobs(w->net);
    int rc = io.read(io.ctx, hash, out, len);
    if (!rc) rc = charge(w, *len);
    if (rc) { free(*out); *out = NULL; *len = 0; }
    return rc;
}
static void events_free(NostrEvent **events, size_t n) {
    for (size_t i = 0; i < n; ++i) nostr_event_free(events[i]);
    free(events);
}
static const char *event_tag(const NostrEvent *event) {
    const char *tag = NULL;
    for (size_t i = 0; i < nostr_tags_size(event->tags); ++i) {
        NostrTag *t = nostr_tags_get(event->tags, i);
        if (nostr_tag_size(t) < 2 || strcmp(nostr_tag_get(t, 0), "d")) continue;
        if (tag) return NULL;
        tag = nostr_tag_get(t, 1);
    }
    return tag;
}
static int query(gc_walk *w, const char *tag, const uint8_t (*authors)[32], size_t n,
                 NostrEvent ***out, size_t *count, int fence) {
    int rc = nh_vault_net_gc_heads(w->net, tag, authors, n, out, count);
    json_t *row = json_pack("{s:[],s:{}}", "authors", "ids");
    if (!row) return VAULT_ERR_OOM;
    if (tag) json_object_set_new(row, "tag", json_string(tag));
    for (size_t i = 0; !rc && i < n; ++i) {
        char hex[65]; porthome_hex64(authors[i], hex);
        if (json_array_append_new(json_object_get(row, "authors"), json_string(hex))) rc = VAULT_ERR_OOM;
    }
    for (size_t i = 0; !rc && i < *count; ++i) {
        NostrEvent *e = (*out)[i];
        uint8_t pub[32]; int allowed = 0;
        if (nostr_event_get_kind(e) != PORTHOME_HEAD_KIND || !e->id ||
            porthome_from_hex64(nostr_event_get_pubkey(e), pub) || !nostr_event_check_signature(e)) {
            rc = VAULT_ERR_AUTH; break;
        }
        for (size_t a = 0; a < n; ++a) if (!memcmp(pub, authors[a], 32)) allowed = 1;
        const char *d = event_tag(e);
        if (!allowed || (tag && (!d || strcmp(tag, d)))) { rc = VAULT_ERR_AUTH; break; }
        if (add(json_object_get(row, "ids"), e->id)) rc = VAULT_ERR_OOM;
        char *body = nostr_event_serialize(e);
        if (!rc) rc = body ? charge(w, strlen(body) + 512) : VAULT_ERR_OOM;
        free(body);
    }
    if (!rc && !fence && json_array_append(w->fences, row)) rc = VAULT_ERR_OOM;
    if (!rc && fence) {
        json_t *old = json_array_get(w->fences, (size_t)(fence - 1));
        if (!json_equal(old, row)) rc = VAULT_ERR_INCOMPLETE;
    }
    json_decref(row);
    return rc;
}
static int mark_v1(gc_walk *w, NostrEvent *e) {
    const char *hex = nostr_event_get_content(e);
    size_t n = hex ? strlen(hex) : 0;
    if (!n || n % 2 || n > 2u * (PORTHOME_MAX_CBOR_BYTES + 128u) ||
        strspn(hex, "0123456789abcdef") != n) return VAULT_ERR_INCOMPLETE;
    uint8_t *sealed = malloc(n / 2);
    if (!sealed) return VAULT_ERR_OOM;
    for (size_t i = 0; i < n / 2; ++i) {
        unsigned byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) { free(sealed); return VAULT_ERR_ARG; }
        sealed[i] = (uint8_t)byte;
    }
    porthome_manifest *m = NULL;
    int rc = porthome_manifest_decode_sealed(sealed, n / 2, w->o->home_key, &m);
    free(sealed);
    for (size_t i = 0; !rc && i < m->entries_len; ++i)
        for (size_t k = 0; !rc && k < m->entries[i].chunks_len; ++k)
            rc = add_bytes(w->marked, m->entries[i].chunks[k].sha256);
    if (m) { porthome_manifest_dispose(m); free(m); }
    return rc;
}
static int mark_refs(json_t *marked, const json_t *refs) {
    if (!json_is_object(refs)) return VAULT_ERR_INCOMPLETE;
    const char *hash; json_t *value;
    json_object_foreach((json_t *)refs, hash, value) {
        if ((!json_is_integer(value) || json_integer_value(value) < 0) && !json_is_true(value))
            return VAULT_ERR_INCOMPLETE;
        int rc = add(marked, hash); if (rc) return rc;
    }
    return 0;
}
static int mark_ring(gc_walk *w, const char *path, int required) {
    struct stat st;
    if (lstat(path, &st)) return errno == ENOENT && !required ? 0 : VAULT_ERR_INCOMPLETE;
    if (!S_ISREG(st.st_mode) || st.st_size > GC_MAX_FILE) return VAULT_ERR_INCOMPLETE;
    nh_syncd_pin_ring *ring = NULL;
    int rc = nh_syncd_pin_ring_open(path, &ring);
    char **hashes = NULL; size_t n = 0;
    if (!rc && nh_syncd_pin_ring_capacity(ring) < NH_SYNCD_CACHE_RETENTION_GENERATIONS) rc = VAULT_ERR_INCOMPLETE;
    if (!rc && !strcmp(path, w->o->pin_path) && nh_syncd_pin_ring_size(ring)) w->require_home = 1;
    if (!rc) rc = nh_syncd_pin_ring_effective_pins(ring, &hashes, &n);
    for (size_t i = 0; !rc && i < n; ++i) rc = add(w->marked, hashes[i]);
    for (size_t i = 0; i < n; ++i) free(hashes[i]);
    free(hashes); nh_syncd_pin_ring_close(ring);
    json_t *j = rc ? NULL : json_load_file(path, JSON_REJECT_DUPLICATES, NULL);
    if (!rc && !j) rc = VAULT_ERR_INCOMPLETE;
    const char *hash; json_t *value;
    json_object_foreach(json_object_get(j, "seen"), hash, value) {
        if (!rc) rc = add(w->candidates, hash);
    }
    json_decref(j);
    return rc;
}
static int mark_state(gc_walk *w) {
    nh_syncd_state *state = NULL; bool unknown = true;
    int rc = nh_syncd_state_load(w->o->state_dir, &state, &unknown);
    if (!rc && unknown) rc = VAULT_ERR_INCOMPLETE;
    char *pub = nostr_key_get_public(w->o->identity);
    const char *account = nh_syncd_state_get_account_pubkey_hex(state);
    if (!rc && (!pub || (account && *account && strcmp(pub, account)))) rc = VAULT_ERR_AUTH;
    free(pub);
    nh_syncd_state_free(state);
    char **hashes = NULL; size_t n = 0;
    if (!rc) rc = nh_syncd__snapshot_hashes(w->o->state_dir, NULL, &hashes, &n);
    for (size_t i = 0; !rc && i < n; ++i) rc = add(w->marked, hashes[i]);
    for (size_t i = 0; i < n; ++i) free(hashes[i]);
    free(hashes);
    if (!rc) rc = mark_ring(w, w->o->pin_path, 1);
    char *path = NULL;
    if (!rc && asprintf(&path, "%s/vault-pinned.json", w->o->state_dir) < 0) rc = VAULT_ERR_OOM;
    if (!rc) rc = mark_ring(w, path, w->o->vaults->len != 0);
    free(path);
    if (!rc && w->o->extra_refs) rc = mark_refs(w->marked, w->o->extra_refs);
    return rc;
}
static int configured(const nh_syncd_gc_options *o, const char *id) {
    for (size_t i = 0; i < o->vaults->len; ++i) {
        char hex[65]; porthome_hex64(o->vaults->vaults[i].vault_id, hex);
        if (!strcmp(hex, id)) return 1;
    }
    return 0;
}
static int vault_inventory(gc_walk *w) {
    int fd = openat(w->dir, "vaults", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT && !w->o->vaults->len ? 0 : VAULT_ERR_INCOMPLETE;
    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); return VAULT_ERR_IO; }
    struct dirent *entry; int rc = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (!configured(w->o, entry->d_name)) { rc = VAULT_ERR_INCOMPLETE; break; }
    }
    closedir(dir);
    return rc;
}
static int local_snapshot(gc_walk *w, gc_vault *v, json_t *snapshot, int committed) {
    if (!json_is_object(snapshot)) return VAULT_ERR_INCOMPLETE;
    vault_record *record = NULL; vault_tree *tree = NULL;
    int rc = nh_vault_snapshot_open(v->disk, v->grant, snapshot, &record, &tree);
    if (!rc) rc = mark_record(record, w->marked, w->roots, w->parents);
    const char *wire = json_string_value(json_object_get(snapshot, "event"));
    NostrEvent *e = wire ? nostr_event_new() : NULL;
    if (!rc && (!e || nostr_event_deserialize(e, wire) || !e->id)) rc = VAULT_ERR_INCOMPLETE;
    if (!rc) rc = add(w->events, e->id);
    uint8_t author[32];
    if (!rc && committed && !porthome_from_hex64(nostr_event_get_pubkey(e), author) && !memcmp(author, v->grant->owner, 32)) {
        const porthome_head *head = vault_record_head(record);
        if (v->owner_seq == head->seq && memcmp(v->owner_root, head->root_manifest, 32)) rc = VAULT_ERR_CONFLICT;
        if (!rc && head->seq > v->owner_seq) {
            v->owner_seq = head->seq; memcpy(v->owner_root, head->root_manifest, 32);
        }
    }
    nostr_event_free(e); vault_record_free(record); vault_tree_free(tree);
    return rc;
}
static int local_vault(gc_walk *w, gc_vault *v, size_t index) {
    const nh_syncd_vault_config_entry *config = &w->o->vaults->vaults[index];
    int rc = nh_vault_session_load(w->o->state_dir, config->vault_id, w->o->home_key, &v->disk);
    if (rc) return rc;
    const char *body = json_string_value(json_object_get(v->disk->doc, "grant"));
    rc = body ? vault_grant_decode(body, &v->grant) : VAULT_ERR_INCOMPLETE;
    if (!rc && memcmp(v->grant->vault_id, config->vault_id, 32)) rc = VAULT_ERR_AUTH;
    if (rc) return rc;
    /* Pure remote reads: neither a dry run nor GC fills/evicts the local cache. */
    v->disk->io = (vault_blob_io){.ctx = w, .read = read_manifest};
    const char *arrays[] = {"heads", "retained"};
    for (size_t f = 0; !rc && f < 2; ++f) {
        json_t *array = json_object_get(v->disk->doc, arrays[f]), *snapshot; size_t i;
        if (!json_is_array(array)) return VAULT_ERR_INCOMPLETE;
        json_array_foreach(array, i, snapshot) {
            if (!rc) rc = local_snapshot(w, v, snapshot, 1);
        }
    }
    const char *fields[] = {"view", "own", "pending", "local_pending"};
    for (size_t i = 0; !rc && i < sizeof fields / sizeof *fields; ++i) {
        json_t *snapshot = json_object_get(v->disk->doc, fields[i]);
        if (snapshot) rc = local_snapshot(w, v, snapshot, i < 2);
    }
    const char *hash; json_t *value;
    json_t *archives = json_object_get(v->disk->doc, "views");
    if (archives && !json_is_object(archives)) return VAULT_ERR_INCOMPLETE;
    json_object_foreach(archives, hash, value) {
        if (!rc) rc = local_snapshot(w, v, value, 1);
    }
    json_t *puts = json_object_get(v->disk->doc, "puts");
    if (puts && !json_is_object(puts)) return VAULT_ERR_INCOMPLETE;
    json_object_foreach(puts, hash, value) {
        if (!rc && (!hex64(json_string_value(value)) || json_string_length(value) != 64)) rc = VAULT_ERR_INCOMPLETE;
        if (!rc) rc = add(w->candidates, hash);
    }
    json_t *candidates = json_object_get(v->disk->doc, "gc_candidates");
    if (candidates && !json_is_object(candidates)) return VAULT_ERR_INCOMPLETE;
    json_object_foreach(candidates, hash, value) {
        if (!rc && !json_is_true(value)) rc = VAULT_ERR_INCOMPLETE;
        if (!rc) rc = add(w->candidates, hash);
    }
    /* Private cache-view and planner references remain pinned even if they are
     * not the materialised frontier (e.g. an interrupted installation). */
    json_t *views = json_object_get(v->disk->doc, "cache_views");
    if (views && !json_is_object(views)) return VAULT_ERR_INCOMPLETE;
    json_object_foreach(views, hash, value) { if (!rc) rc = mark_refs(w->marked, value); }
    DIR *dir = fdopendir(dup(v->disk->dir));
    if (!dir) return VAULT_ERR_IO;
    struct dirent *entry;
    while (!rc && (entry = readdir(dir)))
        if (!strncmp(entry->d_name, "put-", 4)) rc = add(w->marked, entry->d_name + 4);
    closedir(dir);
    return rc;
}
static int remote_vault(gc_walk *w, gc_vault *v) {
    char tag[PORTHOME_HEAD_D_LEN];
    if (porthome_head_d_tag(v->grant->vault_id, tag)) return VAULT_ERR_ARG;
    NostrEvent **events = NULL; size_t n = 0;
    int rc = query(w, tag, (const uint8_t(*)[32])&v->grant->owner, 1, &events, &n, 0);
    vault_record *owner = NULL; vault_tree *owner_tree = NULL;
    uint64_t seq = 0;
    porthome_ring_t ring = vault_ring_view(&v->grant->ring);
    uint64_t epoch = ring.entries[ring.count - 1].epoch;
    for (size_t i = 0; !rc && i < n; ++i) {
        vault_record *record = NULL; vault_tree *tree = NULL;
        rc = vault_record_fetch(events[i], v->grant->vault_id, v->grant->node_id, &ring, epoch,
            nh_vault_session_held, v->disk, &v->disk->io, &record, &tree);
        if (!rc) rc = mark_record(record, w->marked, w->roots, w->parents);
        const porthome_head *head = record ? vault_record_head(record) : NULL;
        if (!rc && owner && head->seq == seq &&
            memcmp(head->root_manifest, vault_record_head(owner)->root_manifest, 32)) rc = VAULT_ERR_CONFLICT;
        if (!rc && (!owner || head->seq > seq)) {
            seq = head->seq; vault_record_free(owner); vault_tree_free(owner_tree);
            owner = record; owner_tree = tree; record = NULL; tree = NULL;
        }
        vault_record_free(record); vault_tree_free(tree);
    }
    events_free(events, n); events = NULL; n = 0;
    if (!rc && (!owner || seq < v->owner_seq ||
        (seq == v->owner_seq && memcmp(vault_record_head(owner)->root_manifest, v->owner_root, 32))))
        rc = VAULT_ERR_INCOMPLETE;
    const porthome_vault_info *policy = owner ? vault_record_root(owner)->vault : NULL;
    if (!rc && (!policy || !policy->writers_len)) rc = VAULT_ERR_INCOMPLETE;
    if (!rc) rc = query(w, tag, (const uint8_t(*)[32])policy->writers, policy->writers_len, &events, &n, 0);
    vault_record **records = n ? calloc(n, sizeof *records) : NULL;
    size_t owner_index = SIZE_MAX;
    if (!rc && n && !records) rc = VAULT_ERR_OOM;
    for (size_t i = 0; !rc && i < n; ++i) {
        vault_tree *tree = NULL;
        rc = vault_record_fetch(events[i], v->grant->vault_id, v->grant->node_id, &ring, epoch,
            nh_vault_session_held, v->disk, &v->disk->io, &records[i], &tree);
        if (!rc) rc = mark_record(records[i], w->marked, w->roots, w->parents);
        uint8_t author[32];
        if (!rc && !porthome_from_hex64(nostr_event_get_pubkey(events[i]), author) &&
            !memcmp(author, v->grant->owner, 32) &&
            vault_record_head(records[i])->seq == seq &&
            !memcmp(vault_record_head(records[i])->root_manifest, vault_record_head(owner)->root_manifest, 32))
            owner_index = i;
        vault_tree_free(tree);
    }
    vault_fold_result *fold = NULL;
    if (!rc && owner_index == SIZE_MAX) rc = VAULT_ERR_INCOMPLETE;
    if (!rc) rc = vault_fold_checkpoint(v->grant, (const vault_record *const *)records, n, owner_index, &fold);
    vault_fold_result_free(fold);
    for (size_t i = 0; records && i < n; ++i) vault_record_free(records[i]);
    free(records); events_free(events, n); vault_record_free(owner); vault_tree_free(owner_tree);
    return rc;
}
static int collect(gc_walk *w) {
    w->reason = "retention-evidence-incomplete";
    int rc = nh_syncd_partial_state_is_set(w->o->state_dir) ? VAULT_ERR_INCOMPLETE : mark_state(w);
    if (!rc) rc = vault_inventory(w);
    if (rc) return rc;
    uint8_t author[1][32]; char *pub = nostr_key_get_public(w->o->identity);
    rc = pub ? porthome_from_hex64(pub, author[0]) : VAULT_ERR_AUTH;
    free(pub);
    NostrEvent **events = NULL; size_t n = 0;
    w->reason = "relay-walk-incomplete";
    if (!rc) rc = query(w, NULL, (const uint8_t(*)[32])author, 1, &events, &n, 0);
    int home_seen = 0;
    for (size_t i = 0; !rc && i < n; ++i) {
        const char *tag = event_tag(events[i]);
        if (!tag) { rc = VAULT_ERR_INCOMPLETE; break; }
        if (!strcmp(tag, w->o->d_tag) || !strncmp(tag, "nostr-homed.home.v1:", sizeof("nostr-homed.home.v1:") - 1)) {
            if (!strcmp(tag, w->o->d_tag)) home_seen = 1;
            rc = mark_v1(w, events[i]);
        } else if (!strncmp(tag, "porthome/v2/", 12) && !configured(w->o, tag + 12))
            rc = VAULT_ERR_INCOMPLETE;
    }
    events_free(events, n);
    if (!rc && !home_seen && (w->require_home || !w->o->vaults->len)) rc = VAULT_ERR_INCOMPLETE;
    for (size_t i = 0; !rc && i < w->o->vaults->len; ++i) {
        gc_vault *v = &w->vaults[w->count++];
        w->reason = "vault-manifest-walk-incomplete";
        rc = local_vault(w, v, i);
        if (!rc) { w->reason = "vault-relay-walk-incomplete"; rc = remote_vault(w, v); }
    }
    const char *hash; json_t *value;
    w->reason = rc ? w->reason : "parent-manifest-walk-incomplete";
    json_object_foreach(w->parents, hash, value)
        if (!rc && !json_object_get(w->roots, hash)) rc = VAULT_ERR_INCOMPLETE;
    for (size_t k = 0; !rc && k < w->count; ++k) {
        json_t *conflicts = json_object_get(w->vaults[k].disk->doc, "conflicts"), *row; size_t i;
        if (!json_is_array(conflicts)) { rc = VAULT_ERR_INCOMPLETE; break; }
        json_array_foreach(conflicts, i, row) {
            const char *id = json_string_value(json_object_get(row, "head"));
            if (!id || !json_object_get(w->events, id)) rc = VAULT_ERR_INCOMPLETE;
        }
    }
    return rc;
}
static int sign_json(const char *wire, char **out, void *ctx) {
    NostrEvent *e = nostr_event_new(); *out = NULL;
    int rc = e ? nostr_event_deserialize(e, wire) : -1;
    if (!rc) rc = nostr_event_sign(e, ctx);
    if (!rc) *out = nostr_event_serialize(e);
    nostr_event_free(e);
    return !rc && *out ? 0 : BLOSSOM_ERR_NOSTR;
}
static int inventory(gc_walk *w, json_t *rows) {
    char *pub = nostr_key_get_public(w->o->identity);
    if (!pub) return VAULT_ERR_AUTH;
    blossom_signer_t signer = {.pubkey = pub, .sign = sign_json, .user_data = (void *)w->o->identity};
    int rc = 0;
    for (size_t s = 0; !rc && s < w->o->servers_len; ++s) {
        if (w->used == w->limit) { rc = VAULT_ERR_BUDGET; break; }
        uint64_t remaining = w->limit - w->used;
        blossom_client_opts_t opts = {.endpoint = w->o->servers[s], .timeout_seconds = 15,
            .max_blob_bytes = remaining < GC_MAX_FILE ? (size_t)remaining : GC_MAX_FILE,
            .allow_insecure = w->o->test_mode != 0};
        rc = blossom_client_new(&opts, &signer, &w->clients[s]);
        char *wire = NULL; size_t len = 0;
        if (!rc) rc = blossom_client_list(w->clients[s], pub, &wire, &len);
        if (!rc) rc = charge(w, len);
        json_t *list = !rc ? json_loadb(wire, len, JSON_REJECT_DUPLICATES, NULL) : NULL;
        free(wire);
        if (!rc && (!json_is_array(list) || json_array_size(list) > GC_MAX_HASHES)) rc = VAULT_ERR_INCOMPLETE;
        json_t *seen = json_object(), *item; size_t i;
        if (!seen) rc = VAULT_ERR_OOM;
        json_array_foreach(list, i, item) {
            if (rc) break;
            json_t *h = json_object_get(item, "sha256"), *size = json_object_get(item, "size");
            const char *hash = json_string_value(h);
            if (!hex64(hash) || json_string_length(h) != 64 || !json_is_integer(size) || json_integer_value(size) < 0 ||
                json_object_get(seen, hash)) { rc = VAULT_ERR_INCOMPLETE; break; }
            rc = add(seen, hash);
            if (rc || !json_object_get(w->candidates, hash) || json_object_get(w->marked, hash)) continue;
            json_t *row = json_pack("{s:I,s:s,s:s,s:I,s:s}", "server_index", (json_int_t)s,
                "server", w->o->servers[s], "sha256", hash, "bytes", json_integer_value(size), "result", "kept-budget");
            if (!row || json_array_append_new(rows, row)) rc = VAULT_ERR_OOM;
        }
        json_decref(seen); json_decref(list);
    }
    /* blossom_client_new copies the signer, but borrows its public key. Keep
     * that allocation until every handle has been freed in the caller. */
    for (size_t s = 0; s < w->o->servers_len; ++s) {
        blossom_client_free(w->clients[s]); w->clients[s] = NULL;
    }
    free(pub);
    return rc;
}
static int fence(gc_walk *w) {
    size_t i; json_t *row;
    json_array_foreach(w->fences, i, row) {
        uint8_t authors[VAULT_MAX_HEADS][32];
        json_t *array = json_object_get(row, "authors"); size_t n = json_array_size(array);
        if (!n || n > VAULT_MAX_HEADS) return VAULT_ERR_INCOMPLETE;
        for (size_t k = 0; k < n; ++k)
            if (porthome_from_hex64(json_string_value(json_array_get(array, k)), authors[k])) return VAULT_ERR_ARG;
        NostrEvent **events = NULL; size_t count = 0;
        int rc = query(w, json_string_value(json_object_get(row, "tag")), (const uint8_t(*)[32])authors,
                       n, &events, &count, (int)i + 1);
        events_free(events, count);
        if (rc) return rc;
    }
    return 0;
}
static int receipt(int fd, const char *server, json_t *row, const char *result, long status) {
    json_t *j = json_deep_copy(row);
    if (!j) return VAULT_ERR_OOM;
    json_object_del(j, "server_index");
    json_object_set_new(j, "server", json_string(server));
    json_object_set_new(j, "result", json_string(result));
    json_object_set_new(j, "http_status", json_integer(status));
    json_object_set_new(j, "at", json_integer((json_int_t)time(NULL)));
    char *wire = json_dumps(j, JSON_COMPACT | JSON_SORT_KEYS);
    json_decref(j);
    if (!wire) return VAULT_ERR_OOM;
    size_t len = strlen(wire); wire[len] = '\n';
    size_t off = 0; int rc = 0;
    while (off < len + 1) {
        ssize_t n = write(fd, wire + off, len + 1 - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { rc = VAULT_ERR_IO; break; }
        off += (size_t)n;
    }
    free(wire);
    if (!rc && fsync(fd)) rc = VAULT_ERR_IO;
    return rc;
}
int nh_syncd_gc_forget_receipt(nh_vault_session *disk, const char *hash) {
    json_t *puts = json_object_get(disk->doc, "puts");
    if (!json_object_get(puts, hash)) return 0;
    json_t *candidates = json_object_get(disk->doc, "gc_candidates");
    if (!candidates) {
        candidates = json_object();
        if (!candidates || json_object_set_new(disk->doc, "gc_candidates", candidates)) return VAULT_ERR_OOM;
    }
    int rc = add(candidates, hash);
    if (!rc && json_object_del(puts, hash)) rc = VAULT_ERR_IO;
    return rc;
}
static int sweep(gc_walk *w, json_t *rows) {
    char *pub = nostr_key_get_public(w->o->identity);
    if (!pub) return VAULT_ERR_AUTH;
    blossom_signer_t signer = {.pubkey = pub, .sign = sign_json, .user_data = (void *)w->o->identity};
    int journal = -1, rc = 0;
    if (!w->o->dry_run) {
        journal = openat(w->dir, "blob-gc-deletions.jsonl", O_RDWR | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        struct stat st;
        if (journal < 0 || fstat(journal, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077) || fsync(w->dir))
            rc = VAULT_ERR_IO;
        if (!rc && st.st_size) {
            char last = 0;
            if (pread(journal, &last, 1, st.st_size - 1) != 1 || last != '\n') rc = VAULT_ERR_IO;
        }
    }
    size_t i; json_t *row;
    json_array_foreach(rows, i, row) {
        if (rc) break;
        size_t s = (size_t)json_integer_value(json_object_get(row, "server_index"));
        uint64_t bytes = (uint64_t)json_integer_value(json_object_get(row, "bytes"));
        if (bytes > UINT64_MAX - GC_DELETE_COST || charge(w, bytes + GC_DELETE_COST)) continue;
        const char *hash = json_string_value(json_object_get(row, "sha256"));
        json_object_set_new(row, "server", json_string(w->o->servers[s]));
        if (w->o->dry_run) { json_object_set_new(row, "result", json_string("would-delete")); continue; }
        /* Invalidate every durable PUT acknowledgement BEFORE remote mutation.
         * A later capture must re-upload even after a crash or partial failure. */
        for (size_t k = 0; !rc && k < w->count; ++k) {
            json_t *puts = json_object_get(w->vaults[k].disk->doc, "puts");
            if (json_object_get(puts, hash)) {
                rc = nh_syncd_gc_forget_receipt(w->vaults[k].disk, hash);
                if (!rc) rc = nh_vault_session_save(w->vaults[k].disk);
            }
        }
        if (!rc && w->o->forget) rc = w->o->forget(w->o->forget_ctx, hash);
        if (!rc) rc = receipt(journal, w->o->servers[s], row, "attempt", 0);
        if (rc) break;
        blossom_client_t *client = NULL;
        blossom_client_opts_t opts = {.endpoint = w->o->servers[s], .timeout_seconds = 15,
            .allow_insecure = w->o->test_mode != 0};
        int result = blossom_client_new(&opts, &signer, &client);
        if (!result) result = blossom_client_delete(client, hash);
        long status = blossom_client_http_status(client);
        const char *label = !result ? "deleted" : "delete-failed";
        json_object_set_new(row, "result", json_string(label));
        json_object_set_new(row, "http_status", json_integer(status));
        rc = receipt(journal, w->o->servers[s], row, label, status);
        blossom_client_free(client);
        if (!rc && result) rc = VAULT_ERR_IO;
    }
    if (journal >= 0) close(journal);
    free(pub);
    return rc;
}
int nh_syncd_gc_due(const char *state_dir, uint64_t now) {
    int dir = open(state_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return 0;
    uint8_t *wire = NULL; size_t len = 0;
    int rc = nh_vault_file_read(dir, "blob-gc-report.json", &wire, &len);
    close(dir);
    json_t *j = !rc ? json_loadb((char *)wire, len, JSON_REJECT_DUPLICATES, NULL) : NULL;
    free(wire);
    json_int_t at = json_integer_value(json_object_get(j, "at"));
    json_decref(j);
    if (rc == VAULT_ERR_INCOMPLETE) return 1;
    return !rc && at > 0 && now >= (uint64_t)at && now - (uint64_t)at >= NH_SYNCD_CACHE_SWEEP_PERIOD_SECS;
}
int nh_syncd_gc_run(const nh_syncd_gc_options *o, json_t **out) {
    if (out) *out = NULL;
    if (!o || !out || !o->state_dir || !o->pin_path || !o->identity || !o->d_tag || !o->vaults ||
        !o->relays_len || !o->servers_len || o->vaults->len > VAULT_MAX_HEADS ||
        o->servers_len > VAULT_MAX_HINTS || o->budget > INT64_MAX)
        return VAULT_ERR_ARG;
    gc_walk w = {.o = o, .dir = -1, .reason = "walk-incomplete"};
    json_t *report = json_pack("{s:i,s:I,s:b,s:s,s:[]}", "schema", 1, "at", (json_int_t)time(NULL),
        "dry_run", o->dry_run != 0, "result", "refused", "deletions");
    w.marked = json_object(); w.candidates = json_object(); w.roots = json_object();
    w.parents = json_object(); w.events = json_object(); w.fences = json_array();
    w.vaults = calloc(o->vaults->len + 1, sizeof *w.vaults);
    int rc = !report || !w.marked || !w.candidates || !w.roots || !w.parents || !w.events || !w.fences || !w.vaults ? VAULT_ERR_OOM : 0;
    if (!rc) w.dir = open(o->state_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!rc && w.dir < 0) rc = VAULT_ERR_IO;
    uint64_t start = 0, spent = 0;
    if (!rc) rc = nh_syncd_retention_load(w.dir, o->home_key, &start, &spent);
    w.limit = spent < o->budget ? o->budget - spent : 0;
    int reserved = 0;
    if (!rc && !w.limit) { rc = VAULT_ERR_BUDGET; w.reason = "retention-budget-exhausted"; }
    if (!rc && !o->dry_run) {
        rc = nh_syncd_retention_write(w.dir, o->home_key, start, o->budget);
        reserved = !rc;
    }
    if (!rc) w.reason = "relay-walk-incomplete";
    if (!rc) rc = nh_vault_net_open(o->identity, o->relays, o->relays_len, o->servers, o->servers_len,
                                   1, o->test_mode, &w.net);
    if (!rc) rc = collect(&w);
    if (!rc) { w.reason = "server-inventory-incomplete"; rc = inventory(&w, json_object_get(report, "deletions")); }
    if (!rc) { w.reason = "relay-heads-changed"; rc = fence(&w); }
    if (!rc) {
        w.reason = "delete-failed";
        json_object_set_new(report, "result", json_string(o->dry_run ? "dry-run" : "complete"));
        rc = sweep(&w, json_object_get(report, "deletions"));
        if (rc) json_object_set_new(report, "result", json_string("partial"));
    }
    if (rc) json_object_set_new(report, "reason", json_string(rc == VAULT_ERR_BUDGET ? "retention-budget-exhausted" : w.reason));
    json_object_set_new(report, "rc", json_integer(rc));
    json_object_set_new(report, "marked", json_integer((json_int_t)json_object_size(w.marked)));
    json_object_set_new(report, "candidates", json_integer((json_int_t)json_object_size(w.candidates)));
    json_object_set_new(report, "charged_bytes", json_integer((json_int_t)w.used));
    /* On failed reads keep the full reservation: bytes received before a
     * transport error are not reliably reported. Successful passes refund it. */
    if (reserved && !rc) {
        rc = nh_syncd_retention_write(w.dir, o->home_key, start, spent + w.used);
        if (rc) {
            json_object_set_new(report, "result", json_string("partial"));
            json_object_set_new(report, "reason", json_string("budget-record-failed"));
            json_object_set_new(report, "rc", json_integer(rc));
        }
    }
    if (!o->dry_run && report && w.dir >= 0) {
        char *wire = json_dumps(report, JSON_COMPACT | JSON_SORT_KEYS);
        int saved = wire ? nh_vault_file_write(w.dir, "blob-gc-report.json", (uint8_t *)wire, strlen(wire)) : VAULT_ERR_OOM;
        if (!saved) saved = nh_porthome_status_ensure_dir();
        if (!saved) saved = nh_porthome_status_write_key_default("gc", wire);
        free(wire);
        if (!rc && saved) {
            rc = saved;
            json_object_set_new(report, "result", json_string("partial"));
            json_object_set_new(report, "reason", json_string("status-record-failed"));
            json_object_set_new(report, "rc", json_integer(rc));
        }
    }
    for (size_t i = 0; i < w.count; ++i) { vault_grant_free(w.vaults[i].grant); nh_vault_session_close(w.vaults[i].disk); }
    free(w.vaults); nh_vault_net_close(w.net);
    if (w.dir >= 0) close(w.dir);
    json_decref(w.marked); json_decref(w.candidates); json_decref(w.roots); json_decref(w.parents); json_decref(w.events); json_decref(w.fences);
    *out = report;
    return rc;
}
