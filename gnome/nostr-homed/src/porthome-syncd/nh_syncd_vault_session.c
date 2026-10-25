/* SPDX-License-Identifier: MIT */
#include "nh_syncd_vault_session.h"
#include <porthome/wrapkey.h>
#include <json.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOCAL_CAP (64u * 1024u * 1024u)
static void wipe_json(json_t *j) {
    if (!j || j->refcount != 1)
        return;
    if (json_is_string(j))
        OPENSSL_cleanse((char *)json_string_value(j), json_string_length(j));
    else if (json_is_array(j)) {
        size_t i;
        json_t *v;
        json_array_foreach(j, i, v) wipe_json(v);
    } else if (json_is_object(j)) {
        const char *key;
        json_t *v;
        json_object_foreach(j, key, v) wipe_json(v);
    }
}
void nh_vault_secret_json_free(json_t *j) {
    wipe_json(j);
    json_decref(j);
}

int nh_vault_file_read(int dir, const char *name, uint8_t **out, size_t *len) {
    *out = NULL;
    *len = 0;
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? VAULT_ERR_INCOMPLETE : VAULT_ERR_IO;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022) ||
        st.st_size < 0 || (uint64_t)st.st_size > LOCAL_CAP) {
        close(fd);
        return VAULT_ERR_IO;
    }
    size_t n = (size_t)st.st_size, done = 0;
    uint8_t *p = malloc(n + 1);
    if (!p) {
        close(fd);
        return VAULT_ERR_OOM;
    }
    while (done < n) {
        ssize_t r = read(fd, p + done, n - done);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        done += (size_t)r;
    }
    uint8_t extra;
    int rc = done == n && read(fd, &extra, 1) == 0 ? 0 : VAULT_ERR_IO;
    close(fd);
    if (rc) {
        OPENSSL_cleanse(p, n);
        free(p);
        return rc;
    }
    p[n] = 0;
    *out = p;
    *len = n;
    return 0;
}
int nh_vault_file_write(int dir, const char *name, const uint8_t *p, size_t n) {
    if (n > LOCAL_CAP)
        return PORTHOME_ERR_TOO_LARGE;
    char tmp[96];
    uint8_t random[32];
    char hex[65];
    if (porthome_wrap_seed_random(random))
        return PORTHOME_ERR_CRYPTO;
    porthome_hex64(random, hex);
    snprintf(tmp, sizeof tmp, ".tmp-%s", hex);
    int fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return VAULT_ERR_IO;
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break;
        done += (size_t)w;
    }
    int rc = done == n && fsync(fd) == 0 ? 0 : VAULT_ERR_IO;
    if (close(fd) && !rc)
        rc = VAULT_ERR_IO;
    if (!rc && renameat(dir, tmp, dir, name))
        rc = VAULT_ERR_IO;
    if (!rc && fsync(dir))
        rc = VAULT_ERR_IO;
    if (rc)
        unlinkat(dir, tmp, 0);
    return rc;
}
static int directory(int parent, const char *name, int create) {
    if (create && mkdirat(parent, name, 0700) && errno != EEXIST)
        return -1;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 0077))) {
        close(fd);
        return -1;
    }
    return fd;
}
static int cache_read(void *ctx, const uint8_t address[32], uint8_t **out, size_t *len) {
    nh_vault_session *s = ctx;
    char hex[65];
    porthome_hex64(address, hex);
    char *cached = s->cache ? nh_syncd_cache_get_path(s->cache, hex) : NULL;
    int rc = cached ? nh_vault_file_read(AT_FDCWD, cached, out, len) : VAULT_ERR_INCOMPLETE;
    free(cached);
    if (rc == VAULT_ERR_INCOMPLETE && s->objects >= 0)
        rc = nh_vault_file_read(s->objects, hex, out, len);
    if (rc == VAULT_ERR_INCOMPLETE && s->remote.read) {
        rc = s->remote.read(s->remote.ctx, address, out, len);
        if (!rc) {
            uint8_t hash[32];
            SHA256(*out, *len, hash);
            if (CRYPTO_memcmp(hash, address, 32))
                rc = PORTHOME_ERR_HASH_MISMATCH;
            if (!rc) {
                if (s->cache)
                    rc = nh_syncd_cache_put(s->cache, hex, *out, *len) ? VAULT_ERR_BUDGET : 0;
                else
                    rc = nh_vault_file_write(s->objects, hex, *out, *len);
            }
            if (rc) {
                free(*out);
                *out = NULL;
                *len = 0;
            }
        }
    }
    return rc;
}
static int cache_put(void *ctx, const uint8_t *p, size_t n, const uint8_t address[32]) {
    nh_vault_session *s = ctx;
    char hex[65], intent[80];
    uint8_t hash[32];
    SHA256(p, n, hash);
    if (CRYPTO_memcmp(hash, address, 32))
        return PORTHOME_ERR_HASH_MISMATCH;
    porthome_hex64(address, hex);
    snprintf(intent, sizeof intent, "put-%s", hex);
    json_t *receipts = json_object_get(s->doc, "puts");
    const char *ack = json_string_value(json_object_get(receipts, hex));
    int acknowledged = *s->remote_scope && ack && !strcmp(ack, s->remote_scope);
    if (acknowledged && s->cache && nh_syncd_cache_has(s->cache, hex))
        return 0;
    uint8_t binding[97] = {0};
    memcpy(binding, address, 32);
    size_t scope_len = strnlen(s->remote_scope, sizeof s->remote_scope);
    if (scope_len >= sizeof s->remote_scope)
        return VAULT_ERR_ARG;
    memcpy(binding + 32, s->remote_scope, scope_len);
    uint8_t *prior = NULL;
    size_t prior_len = 0;
    int found = nh_vault_file_read(s->dir, intent, &prior, &prior_len);
    if (found != 0 && found != VAULT_ERR_INCOMPLETE) {
        free(prior);
        return found;
    }
    if (!found && !acknowledged) {
        int matches = prior_len == sizeof binding && !CRYPTO_memcmp(prior, binding, sizeof binding);
        free(prior);
        if (!matches || !*s->remote_scope || !s->reconcile ||
            s->reconcile(s->remote.ctx, address, p, n))
            return VAULT_ERR_INCOMPLETE;
        acknowledged = 1; /* Matching readback on the required replica quorum. */
    } else
        free(prior);
    int rc = nh_vault_file_write(s->objects, hex, p, n);
    if (!rc && found == VAULT_ERR_INCOMPLETE)
        rc = nh_vault_file_write(s->dir, intent, binding, sizeof binding);
    if (rc)
        return rc;
    if (!s->remote.put && !acknowledged)
        return VAULT_ERR_IO;
    rc = acknowledged ? 0 : s->remote.put(s->remote.ctx, p, n, address);
    if (!rc && *s->remote_scope) {
        if (!json_is_object(receipts)) {
            receipts = json_object();
            if (!receipts || json_object_set_new(s->doc, "puts", receipts))
                return VAULT_ERR_OOM;
        }
        if (json_object_set_new(receipts, hex, json_string(s->remote_scope)))
            return VAULT_ERR_OOM;
        rc = nh_vault_session_save(s);
    }
    if (!rc && s->cache) {
        rc = nh_syncd_cache_put(s->cache, hex, p, n) ? VAULT_ERR_BUDGET : 0;
        if (!rc && (unlinkat(s->objects, hex, 0) || fsync(s->objects)))
            rc = VAULT_ERR_IO;
    }
    if (!rc) {
        if (unlinkat(s->dir, intent, 0) || fsync(s->dir))
            rc = VAULT_ERR_IO;
    }
    return rc;
}
static int session_open(const char *state, const uint8_t id[32], const uint8_t key[32], int create,
                        nh_vault_session **out) {
    *out = NULL;
    nh_vault_session *s = calloc(1, sizeof *s);
    if (!s)
        return VAULT_ERR_OOM;
    s->dir = s->objects = -1;
    int root = open(state, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int vaults = root >= 0 ? directory(root, "vaults", create) : -1;
    if (root >= 0)
        close(root);
    char hex[65];
    porthome_hex64(id, hex);
    if (vaults >= 0) {
        s->dir = directory(vaults, hex, create);
        close(vaults);
    }
    if (s->dir >= 0)
        s->objects = directory(s->dir, "objects", create);
    if (s->objects < 0) {
        nh_vault_session_close(s);
        return VAULT_ERR_IO;
    }
    memcpy(s->home_key, key, 32);
    s->io = (vault_blob_io){s, cache_read, cache_put};
    uint8_t *sealed = NULL, *plain = NULL;
    size_t n = 0, m = 0;
    int rc = nh_vault_file_read(s->dir, "session.sealed", &sealed, &n);
    if (create && rc == VAULT_ERR_INCOMPLETE) {
        s->doc = json_pack("{s:i,s:[],s:[],s:[]}", "version", 1, "heads", "retained", "conflicts");
        rc = s->doc ? 0 : VAULT_ERR_OOM;
    } else if (!rc) {
        rc = porthome_decrypt_manifest(key, sealed, n, &plain, &m);
        if (!rc) {
            s->doc = json_loadb((char *)plain, m, JSON_REJECT_DUPLICATES, NULL);
            if (!json_is_object(s->doc) ||
                json_integer_value(json_object_get(s->doc, "version")) != 1 ||
                !json_is_array(json_object_get(s->doc, "heads")) ||
                !json_is_array(json_object_get(s->doc, "retained")) ||
                !json_is_array(json_object_get(s->doc, "conflicts")))
                rc = VAULT_ERR_ARG;
        }
    }
    if (plain) {
        OPENSSL_cleanse(plain, m);
        free(plain);
    }
    free(sealed);
    if (rc) {
        nh_vault_session_close(s);
        return rc;
    }
    *out = s;
    return 0;
}
int nh_vault_session_open(const char *state, const uint8_t id[32], const uint8_t key[32],
                          nh_vault_session **out) {
    return session_open(state, id, key, 1, out);
}
int nh_vault_session_load(const char *state, const uint8_t id[32], const uint8_t key[32],
                          nh_vault_session **out) {
    return session_open(state, id, key, 0, out);
}
int nh_vault_session_save(nh_vault_session *s) {
    char *plain = json_dumps(s->doc, JSON_COMPACT | JSON_SORT_KEYS);
    if (!plain)
        return VAULT_ERR_OOM;
    uint8_t *sealed = NULL;
    size_t n = 0, len = strlen(plain);
    int rc = len > LOCAL_CAP - PORTHOME_SEAL_OVERHEAD
                 ? PORTHOME_ERR_TOO_LARGE
                 : porthome_encrypt_manifest(s->home_key, (uint8_t *)plain, len, &sealed, &n);
    OPENSSL_cleanse(plain, len);
    free(plain);
    if (!rc)
        rc = nh_vault_file_write(s->dir, "session.sealed", sealed, n);
    free(sealed);
    return rc;
}
void nh_vault_session_close(nh_vault_session *s) {
    if (!s)
        return;
    nh_vault_secret_json_free(s->doc);
    vault_ring_dispose(&s->held_scratch);
    if (s->objects >= 0)
        close(s->objects);
    if (s->dir >= 0)
        close(s->dir);
    OPENSSL_cleanse(s, sizeof *s);
    free(s);
}
static int epoch_order(const void *a, const void *b) {
    uint64_t x = ((const porthome_ring_entry_t *)a)->epoch;
    uint64_t y = ((const porthome_ring_entry_t *)b)->epoch;
    return (x > y) - (x < y);
}
static int collect_keys(nh_vault_session *s, const json_t *snapshot, const char *id) {
    json_t *rings = json_object_get(snapshot, "rings"), *row;
    size_t i;
    json_array_foreach(rings, i, row) {
        const char *node = json_string_value(json_object_get(row, "id"));
        if (!node || strcmp(node, id))
            continue;
        json_t *keys = json_object_get(row, "keys");
        size_t old = s->held_scratch.count, add = json_array_size(keys);
        if (!json_is_array(keys) || !add || old > 4096 || add > 4096)
            return VAULT_ERR_ARG;
        porthome_ring_entry_t *all = calloc(old + add, sizeof *all);
        if (!all)
            return VAULT_ERR_OOM;
        if (old)
            memcpy(all, s->held_scratch.entries, old * sizeof *all);
        int rc = 0;
        for (size_t k = 0; k < add; ++k) {
            json_t *pair = json_array_get(keys, k), *epoch = json_array_get(pair, 0);
            const char *hex = json_string_value(json_array_get(pair, 1));
            if (!json_is_array(pair) || json_array_size(pair) != 2 || !json_is_integer(epoch) ||
                json_integer_value(epoch) <= 0 || !hex ||
                porthome_from_hex64(hex, all[old + k].node_key)) {
                rc = VAULT_ERR_ARG;
                break;
            }
            all[old + k].epoch = (uint64_t)json_integer_value(epoch);
        }
        size_t count = 0;
        if (!rc) {
            qsort(all, old + add, sizeof *all, epoch_order);
            for (size_t k = 0; k < old + add; ++k) {
                if (count && all[k].epoch == all[count - 1].epoch) {
                    if (CRYPTO_memcmp(all[k].node_key, all[count - 1].node_key, 32)) {
                        rc = VAULT_ERR_AUTH;
                        break;
                    }
                } else
                    all[count++] = all[k];
            }
        }
        vault_ring merged = {0};
        porthome_ring_t view = {all, count};
        if (!rc && count > 4096)
            rc = PORTHOME_ERR_TOO_LARGE;
        if (!rc)
            rc = vault_ring_copy(&merged, &view);
        OPENSSL_cleanse(all, (old + add) * sizeof *all);
        free(all);
        if (rc)
            return rc;
        vault_ring_dispose(&s->held_scratch);
        s->held_scratch = merged;
    }
    return 0;
}
int nh_vault_session_held(void *ctx, const uint8_t id[32], uint64_t epoch, porthome_ring_t *out) {
    nh_vault_session *s = ctx;
    (void)epoch;
    *out = (porthome_ring_t){0};
    vault_ring_dispose(&s->held_scratch);
    char hex[65];
    porthome_hex64(id, hex);
    int rc = 0;
    const char *single[] = {"view", "own", "pending", "local_pending"};
    for (size_t i = 0; !rc && i < sizeof single / sizeof *single; ++i)
        rc = collect_keys(s, json_object_get(s->doc, single[i]), hex);
    const char *arrays[] = {"heads", "retained"};
    for (size_t i = 0; !rc && i < sizeof arrays / sizeof *arrays; ++i) {
        size_t k;
        json_t *snapshot;
        json_array_foreach(json_object_get(s->doc, arrays[i]), k, snapshot) {
            rc = collect_keys(s, snapshot, hex);
            if (rc)
                break;
        }
    }
    if (!rc && !s->held_scratch.count)
        rc = VAULT_ERR_EPOCH;
    if (rc) {
        vault_ring_dispose(&s->held_scratch);
        return rc;
    }
    *out = vault_ring_view(&s->held_scratch);
    return 0;
}
int nh_vault_snapshot_held(void *ctx, const uint8_t id[32], uint64_t epoch, porthome_ring_t *out) {
    const vault_tree *tree = ctx;
    (void)epoch;
    *out = (porthome_ring_t){0};
    for (size_t i = 0; i < vault_tree_count(tree); ++i) {
        const porthome_dir_node *n = vault_tree_node_at(tree, i, out);
        if (!memcmp(n->node_id, id, 32))
            return 0;
    }
    *out = (porthome_ring_t){0};
    return VAULT_ERR_EPOCH;
}
int nh_vault_snapshot_new(const NostrEvent *event, const vault_tree *tree, json_t **out) {
    *out = NULL;
    char *wire = nostr_event_serialize(event);
    const porthome_dir_node *root = vault_tree_node_at(tree, 0, NULL);
    if (!wire || !root || root->epoch > INT64_MAX) {
        free(wire);
        return VAULT_ERR_ARG;
    }
    json_t *j =
        json_pack("{s:s,s:I,s:[]}", "event", wire, "epoch", (json_int_t)root->epoch, "rings");
    free(wire);
    if (!j)
        return VAULT_ERR_OOM;
    for (size_t i = 0; i < vault_tree_count(tree); ++i) {
        porthome_ring_t ring;
        const porthome_dir_node *node = vault_tree_node_at(tree, i, &ring);
        char id[65];
        porthome_hex64(node->node_id, id);
        json_t *r = json_pack("{s:s,s:[]}", "id", id, "keys");
        if (!r) {
            nh_vault_secret_json_free(j);
            return VAULT_ERR_OOM;
        }
        for (size_t k = 0; k < ring.count; ++k) {
            char key[65];
            porthome_hex64(ring.entries[k].node_key, key);
            json_t *entry = ring.entries[k].epoch <= INT64_MAX
                                ? json_pack("[I,s]", (json_int_t)ring.entries[k].epoch, key)
                                : NULL;
            OPENSSL_cleanse(key, sizeof key);
            if (!entry || json_array_append_new(json_object_get(r, "keys"), entry)) {
                nh_vault_secret_json_free(r);
                nh_vault_secret_json_free(j);
                return VAULT_ERR_OOM;
            }
        }
        if (json_array_append_new(json_object_get(j, "rings"), r)) {
            nh_vault_secret_json_free(j);
            return VAULT_ERR_OOM;
        }
    }
    *out = j;
    return 0;
}
typedef struct {
    uint8_t id[32];
    vault_ring ring;
} held_node;
typedef struct {
    held_node *nodes;
    size_t len;
} held_map;
static int map_held(void *ctx, const uint8_t id[32], uint64_t epoch, porthome_ring_t *out) {
    held_map *m = ctx;
    (void)epoch;
    for (size_t i = 0; i < m->len; ++i)
        if (!memcmp(id, m->nodes[i].id, 32)) {
            *out = vault_ring_view(&m->nodes[i].ring);
            return 0;
        }
    *out = (porthome_ring_t){0};
    return VAULT_ERR_EPOCH;
}
int nh_vault_snapshot_open(nh_vault_session *s, const vault_grant *grant, const json_t *j,
                           vault_record **record, vault_tree **tree) {
    *record = NULL;
    *tree = NULL;
    const char *wire = json_string_value(json_object_get(j, "event"));
    json_int_t epoch = json_integer_value(json_object_get(j, "epoch"));
    json_t *rings = json_object_get(j, "rings");
    if (!wire || epoch <= 0 || !json_is_array(rings) || json_array_size(rings) > VAULT_MAX_OBJECTS)
        return VAULT_ERR_ARG;
    held_map m = {.len = json_array_size(rings)};
    m.nodes = calloc(m.len ? m.len : 1, sizeof *m.nodes);
    if (!m.nodes)
        return VAULT_ERR_OOM;
    int rc = VAULT_ERR_ARG;
    NostrEvent *event = nostr_event_new();
    if (!event) {
        free(m.nodes);
        return VAULT_ERR_OOM;
    }
    if (nostr_event_deserialize(event, wire))
        goto done;
    for (size_t i = 0; i < m.len; ++i) {
        json_t *row = json_array_get(rings, i), *keys = json_object_get(row, "keys");
        const char *id = json_string_value(json_object_get(row, "id"));
        size_t n = json_array_size(keys);
        if (!id || porthome_from_hex64(id, m.nodes[i].id) || !json_is_array(keys) || !n || n > 4096)
            goto done;
        for (size_t k = 0; k < i; ++k)
            if (!memcmp(m.nodes[i].id, m.nodes[k].id, 32))
                goto done;
        m.nodes[i].ring.entries = calloc(n, sizeof *m.nodes[i].ring.entries);
        if (!m.nodes[i].ring.entries) {
            rc = VAULT_ERR_OOM;
            goto done;
        }
        m.nodes[i].ring.count = n;
        for (size_t k = 0; k < n; ++k) {
            json_t *key = json_array_get(keys, k);
            json_int_t e = json_integer_value(json_array_get(key, 0));
            const char *hex = json_string_value(json_array_get(key, 1));
            if (!json_is_array(key) || json_array_size(key) != 2 || e <= 0 || !hex ||
                (k && (uint64_t)e <= m.nodes[i].ring.entries[k - 1].epoch) ||
                porthome_from_hex64(hex, m.nodes[i].ring.entries[k].node_key))
                goto done;
            m.nodes[i].ring.entries[k].epoch = (uint64_t)e;
        }
    }
    porthome_ring_t root;
    rc = map_held(&m, grant->node_id, (uint64_t)epoch, &root);
    if (!rc)
        rc = vault_record_fetch(event, grant->vault_id, grant->node_id, &root, (uint64_t)epoch,
                                map_held, &m, &s->io, record, tree);
done:
    nostr_event_free(event);
    for (size_t i = 0; i < m.len; ++i)
        vault_ring_dispose(&m.nodes[i].ring);
    free(m.nodes);
    return rc;
}
