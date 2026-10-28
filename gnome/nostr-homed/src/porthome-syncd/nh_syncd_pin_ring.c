/*
 * nh_syncd_pin_ring.c — persistent ring of the last N generations'
 *                       blob-hash sets (I3, §6.5).
 *
 * SPDX-License-Identifier: MIT
 *
 * A blob is "pinned" iff it appears in the union of any generation
 * still held by the ring. Persisted at (default)
 *   ${XDG_STATE_HOME:-~/.local/state}/nostr-homed/pinned.json
 *
 * The file is retention evidence for remote GC as well as cache pinning.
 * Malformed or missing evidence must never be repaired to empty for GC.
 * `seen` is an additive candidate inventory, not a reachability set: unknown
 * account blobs are never candidates, and every retained slot stays marked.
 *
 * Bead: nostrc-p6qp.
 */

#define _GNU_SOURCE
#include "nh_syncd_cache.h"
#include "nh_syncd.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    uint64_t generation;
    char   **hashes;
    size_t   n_hashes;
} ring_slot;

struct nh_syncd_pin_ring {
    char       *path;
    ring_slot  *slots;
    size_t      size;
    size_t      capacity;
    json_t     *seen;
};

static void slot_dispose(ring_slot *s) {
    if (!s) return;
    for (size_t i = 0; i < s->n_hashes; ++i) free(s->hashes[i]);
    free(s->hashes);
    s->hashes = NULL;
    s->n_hashes = 0;
    s->generation = 0;
}

static bool is_hex64(const char *s) {
    if (!s) return false;
    for (size_t i = 0; i < 64; ++i) {
        char c = s[i];
        if (!c) return false;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return s[64] == '\0';
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static int mkdirp_parent(const char *path) {
    char *dup = xstrdup(path);
    if (!dup) return NH_SYNCD_CACHE_ERR_OOM;
    char *slash = strrchr(dup, '/');
    if (!slash || slash == dup) { free(dup); return NH_SYNCD_CACHE_OK; }
    *slash = '\0';
    for (char *p = dup + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(dup, 0700) != 0 && errno != EEXIST) {
                free(dup); return NH_SYNCD_CACHE_ERR_IO;
            }
            *p = '/';
        }
    }
    if (mkdir(dup, 0700) != 0 && errno != EEXIST) {
        free(dup); return NH_SYNCD_CACHE_ERR_IO;
    }
    free(dup);
    return NH_SYNCD_CACHE_OK;
}

static int load_from_disk(nh_syncd_pin_ring *r) {
    struct stat st;
    if (stat(r->path, &st) != 0)
        return errno == ENOENT ? NH_SYNCD_CACHE_OK : NH_SYNCD_CACHE_ERR_IO;
    json_error_t je;
    json_t *root = json_load_file(r->path, JSON_REJECT_DUPLICATES, &je);
    if (!root) return NH_SYNCD_CACHE_ERR_JSON;
    if (!json_is_object(root)) { json_decref(root); return NH_SYNCD_CACHE_ERR_JSON; }

    json_t *cap = json_object_get(root, "capacity");
    if (json_is_integer(cap)) {
        json_int_t c = json_integer_value(cap);
        if (c > 0 && c <= 1024) r->capacity = (size_t)c;
    }

    json_t *gens = json_object_get(root, "generations");
    json_t *seen = json_object_get(root, "seen");
    if (json_integer_value(json_object_get(root, "schema")) != 1 ||
        !json_is_integer(cap) || json_integer_value(cap) <= 0 || json_integer_value(cap) > 1024 ||
        !json_is_array(gens) || json_array_size(gens) > r->capacity ||
        (seen && !json_is_object(seen))) { json_decref(root); return NH_SYNCD_CACHE_ERR_JSON; }
    const char *key; json_t *value;
    json_object_foreach(seen, key, value) {
        if (!is_hex64(key) || !json_is_true(value)) { json_decref(root); return NH_SYNCD_CACHE_ERR_JSON; }
        if (json_object_set(r->seen, key, value)) { json_decref(root); return NH_SYNCD_CACHE_ERR_OOM; }
    }
    size_t n = json_array_size(gens);
    r->slots = (ring_slot *)calloc(r->capacity, sizeof *r->slots);
    if (!r->slots) { json_decref(root); return NH_SYNCD_CACHE_ERR_OOM; }
    r->size = 0;

    for (size_t i = 0; i < n; ++i) {
        json_t *g = json_array_get(gens, i);
        if (!json_is_object(g)) { json_decref(root); return NH_SYNCD_CACHE_ERR_JSON; }
        json_t *jgen = json_object_get(g, "gen");
        json_t *jhashes = json_object_get(g, "hashes");
        if (!json_is_integer(jgen) || json_integer_value(jgen) < 0 || !json_is_array(jhashes)) {
            json_decref(root); return NH_SYNCD_CACHE_ERR_JSON;
        }
        ring_slot *slot = &r->slots[r->size++];
        slot->generation = (uint64_t)json_integer_value(jgen);
        size_t hn = json_array_size(jhashes);
        slot->hashes = (char **)calloc(hn + 1, sizeof(char *));
        if (!slot->hashes) { json_decref(root); return NH_SYNCD_CACHE_ERR_OOM; }
        for (size_t j = 0; j < hn; ++j) {
            json_t *h = json_array_get(jhashes, j);
            const char *hex = json_string_value(h);
            if (!is_hex64(hex) || json_string_length(h) != 64) {
                json_decref(root); return NH_SYNCD_CACHE_ERR_JSON;
            }
            if (json_object_set_new(r->seen, hex, json_true())) { json_decref(root); return NH_SYNCD_CACHE_ERR_OOM; }
            slot->hashes[slot->n_hashes] = xstrdup(hex);
            if (!slot->hashes[slot->n_hashes]) {
                json_decref(root); return NH_SYNCD_CACHE_ERR_OOM;
            }
            slot->n_hashes++;
        }
    }

    json_decref(root);
    return NH_SYNCD_CACHE_OK;
}

static int save_to_disk(const nh_syncd_pin_ring *r) {
    for (size_t i = 0; i < r->size; ++i)
        for (size_t k = 0; k < r->slots[i].n_hashes; ++k)
            if (json_object_set_new(r->seen, r->slots[i].hashes[k], json_true()))
                return NH_SYNCD_CACHE_ERR_OOM;
    json_t *root = json_pack("{s:i,s:I,s:O,s:[]}", "schema", 1,
        "capacity", (json_int_t)r->capacity, "seen", r->seen, "generations");
    if (!root) return NH_SYNCD_CACHE_ERR_OOM;
    json_t *gens = json_object_get(root, "generations");
    for (size_t i = 0; i < r->size; ++i) {
        const ring_slot *s = &r->slots[i];
        json_t *g = json_pack("{s:I,s:[]}", "gen", (json_int_t)s->generation, "hashes");
        if (!g) { json_decref(root); return NH_SYNCD_CACHE_ERR_OOM; }
        json_t *arr = json_object_get(g, "hashes");
        for (size_t j = 0; j < s->n_hashes; ++j) {
            if (json_array_append_new(arr, json_string(s->hashes[j]))) {
                json_decref(g); json_decref(root); return NH_SYNCD_CACHE_ERR_OOM;
            }
        }
        if (json_array_append_new(gens, g)) { json_decref(root); return NH_SYNCD_CACHE_ERR_OOM; }
    }

    char *rendered = json_dumps(root, JSON_INDENT(2));
    json_decref(root);
    if (!rendered) return NH_SYNCD_CACHE_ERR_OOM;

    if (mkdirp_parent(r->path) != NH_SYNCD_CACHE_OK) {
        free(rendered); return NH_SYNCD_CACHE_ERR_IO;
    }

    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.%d", r->path, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof tmp) { free(rendered); return NH_SYNCD_CACHE_ERR_IO; }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { free(rendered); return NH_SYNCD_CACHE_ERR_IO; }
    size_t rlen = strlen(rendered), off = 0;
    while (off < rlen) {
        ssize_t w = write(fd, rendered + off, rlen - off);
        if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); free(rendered); return NH_SYNCD_CACHE_ERR_IO; }
        off += (size_t)w;
    }
    (void)fchmod(fd, 0600);
    if (fsync(fd) != 0) { close(fd); unlink(tmp); free(rendered); return NH_SYNCD_CACHE_ERR_IO; }
    close(fd);
    if (rename(tmp, r->path) != 0) { unlink(tmp); free(rendered); return NH_SYNCD_CACHE_ERR_IO; }
    free(rendered);
    char *parent = xstrdup(r->path);
    if (!parent) return NH_SYNCD_CACHE_ERR_OOM;
    char *slash = strrchr(parent, '/');
    if (slash == parent) slash[1] = '\0';
    else if (slash) *slash = '\0';
    else strcpy(parent, ".");
    int dir = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(parent);
    int rc = dir < 0 || fsync(dir) ? NH_SYNCD_CACHE_ERR_IO : NH_SYNCD_CACHE_OK;
    if (dir >= 0) close(dir);
    return rc;
}

int nh_syncd_pin_ring_open(const char *path, nh_syncd_pin_ring **out) {
    if (!path || !out) return NH_SYNCD_CACHE_ERR_ARG;
    nh_syncd_pin_ring *r = (nh_syncd_pin_ring *)calloc(1, sizeof *r);
    if (!r) return NH_SYNCD_CACHE_ERR_OOM;
    r->path = xstrdup(path);
    r->seen = json_object();
    r->capacity = NH_SYNCD_CACHE_RETENTION_GENERATIONS;
    if (!r->path || !r->seen) { nh_syncd_pin_ring_close(r); return NH_SYNCD_CACHE_ERR_OOM; }
    int rc = load_from_disk(r);
    if (rc == NH_SYNCD_CACHE_ERR_OOM || rc == NH_SYNCD_CACHE_ERR_IO) {
        nh_syncd_pin_ring_close(r);
        return rc;
    }
    if (rc == NH_SYNCD_CACHE_ERR_JSON) {
        /* Preserve corrupt retention evidence. Never recreate it as empty. */
        nh_syncd_pin_ring_close(r);
        return NH_SYNCD_CACHE_ERR_JSON;
    }
    /* Ensure slots array is allocated even when the file was absent. */
    if (!r->slots) {
        r->slots = (ring_slot *)calloc(r->capacity, sizeof *r->slots);
        if (!r->slots) { nh_syncd_pin_ring_close(r); return NH_SYNCD_CACHE_ERR_OOM; }
    }
    *out = r;
    return NH_SYNCD_CACHE_OK;
}

void nh_syncd_pin_ring_close(nh_syncd_pin_ring *r) {
    if (!r) return;
    if (r->slots) {
        for (size_t i = 0; i < r->size; ++i) slot_dispose(&r->slots[i]);
        free(r->slots);
    }
    free(r->path);
    json_decref(r->seen);
    free(r);
}

size_t nh_syncd_pin_ring_size(const nh_syncd_pin_ring *r) {
    return r ? r->size : 0;
}
size_t nh_syncd_pin_ring_capacity(const nh_syncd_pin_ring *r) {
    return r ? r->capacity : 0;
}

int nh_syncd_pin_ring_promote(nh_syncd_pin_ring *r,
                              uint64_t generation,
                              const char *const *hashes,
                              size_t n_hashes) {
    if (!r || generation > INT64_MAX || (!hashes && n_hashes)) return NH_SYNCD_CACHE_ERR_ARG;

    /* Dedup hashes (case-insensitive would be nice, but our whole
     * pipeline emits lowercase). Reject non-hex64 entries. */
    char **uniq = (char **)calloc(n_hashes + 1, sizeof(char *));
    if (!uniq && n_hashes) return NH_SYNCD_CACHE_ERR_OOM;
    size_t un = 0;
    for (size_t i = 0; i < n_hashes; ++i) {
        if (!is_hex64(hashes[i])) {
            for (size_t k = 0; k < un; ++k) free(uniq[k]);
            free(uniq);
            return NH_SYNCD_CACHE_ERR_ARG;
        }
        int dup_seen = 0;
        for (size_t j = 0; j < un; ++j) {
            if (memcmp(uniq[j], hashes[i], 64) == 0) { dup_seen = 1; break; }
        }
        if (dup_seen) continue;
        uniq[un] = xstrdup(hashes[i]);
        if (!uniq[un]) {
            for (size_t k = 0; k < un; ++k) free(uniq[k]);
            free(uniq);
            return NH_SYNCD_CACHE_ERR_OOM;
        }
        un++;
    }

    /* A repeated generation can include conflict/installation snapshots.
     * Union it rather than dropping the earlier references from that slot. */
    if (r->size > 0 && r->slots[r->size - 1].generation == generation) {
        ring_slot *old = &r->slots[r->size - 1];
        char **joined = realloc(uniq, (un + old->n_hashes + 1) * sizeof *uniq);
        if (!joined) {
            for (size_t k = 0; k < un; ++k) free(uniq[k]);
            free(uniq); return NH_SYNCD_CACHE_ERR_OOM;
        }
        uniq = joined;
        for (size_t i = 0; i < old->n_hashes; ++i) {
            int found = 0;
            for (size_t k = 0; k < un; ++k) if (!strcmp(uniq[k], old->hashes[i])) found = 1;
            if (found) continue;
            uniq[un] = xstrdup(old->hashes[i]);
            if (!uniq[un]) {
                for (size_t k = 0; k < un; ++k) free(uniq[k]);
                free(uniq); return NH_SYNCD_CACHE_ERR_OOM;
            }
            ++un;
        }
        slot_dispose(&r->slots[r->size - 1]);
        r->slots[r->size - 1].generation = generation;
        r->slots[r->size - 1].hashes = uniq;
        r->slots[r->size - 1].n_hashes = un;
        return save_to_disk(r);
    }

    /* Rotate if full. */
    if (r->size == r->capacity) {
        slot_dispose(&r->slots[0]);
        memmove(&r->slots[0], &r->slots[1],
                (r->capacity - 1) * sizeof(ring_slot));
        r->size--;
    }
    r->slots[r->size].generation = generation;
    r->slots[r->size].hashes = uniq;
    r->slots[r->size].n_hashes = un;
    r->size++;
    return save_to_disk(r);
}

int nh_syncd_pin_ring_effective_pins(const nh_syncd_pin_ring *r,
                                     char ***out_hashes,
                                     size_t  *out_n) {
    if (!r || !out_hashes || !out_n) return NH_SYNCD_CACHE_ERR_ARG;
    /* Union via linear scan (n^2 in the number of blob hashes across
     * 10 generations — for the design target size this is trivial). */
    size_t max = 0;
    for (size_t i = 0; i < r->size; ++i) max += r->slots[i].n_hashes;
    char **arr = (char **)calloc(max + 1, sizeof(char *));
    if (!arr) return NH_SYNCD_CACHE_ERR_OOM;
    size_t n = 0;
    for (size_t i = 0; i < r->size; ++i) {
        for (size_t j = 0; j < r->slots[i].n_hashes; ++j) {
            const char *h = r->slots[i].hashes[j];
            int seen = 0;
            for (size_t k = 0; k < n; ++k)
                if (memcmp(arr[k], h, 64) == 0) { seen = 1; break; }
            if (seen) continue;
            arr[n] = xstrdup(h);
            if (!arr[n]) {
                for (size_t k = 0; k < n; ++k) free(arr[k]);
                free(arr);
                return NH_SYNCD_CACHE_ERR_OOM;
            }
            n++;
        }
    }
    *out_hashes = arr;
    *out_n = n;
    return NH_SYNCD_CACHE_OK;
}

int nh_syncd_pin_ring_apply(const nh_syncd_pin_ring *r,
                            nh_syncd_cache *cache) {
    if (!r || !cache) return NH_SYNCD_CACHE_ERR_ARG;
    char **hashes = NULL;
    size_t n = 0;
    int rc = nh_syncd_pin_ring_effective_pins(r, &hashes, &n);
    if (rc != NH_SYNCD_CACHE_OK) return rc;
    for (size_t i = 0; i < n; ++i) {
        (void)nh_syncd_cache_pin(cache, hashes[i]);
        free(hashes[i]);
    }
    free(hashes);
    return NH_SYNCD_CACHE_OK;
}

/* Do not use the display getters here: they intentionally default missing
 * fields, which would turn a damaged file row into an empty GC reference set. */
int nh_syncd_state_iter_(const nh_syncd_state *, int (*)(void *, const char *, json_t *), void *);
static int snapshot_row(void *ctx, const char *path, json_t *row) {
    (void)path;
    json_t *set = ctx;
    const char *kind = json_string_value(json_object_get(row, "kind"));
    json_t *size = json_object_get(row, "size"), *chunks = json_object_get(row, "chunk_addrs_hex");
    if (!json_is_object(row) || !kind ||
        (strcmp(kind, "file") && strcmp(kind, "dir") && strcmp(kind, "symlink")) ||
        !json_is_integer(size) || json_integer_value(size) < 0)
        return NH_SYNCD_CACHE_ERR_JSON;
    if (strcmp(kind, "file"))
        return chunks && (!json_is_array(chunks) || json_array_size(chunks)) ? NH_SYNCD_CACHE_ERR_JSON : 0;
    if (!json_is_array(chunks) || (json_integer_value(size) && !json_array_size(chunks)))
        return NH_SYNCD_CACHE_ERR_JSON;
    size_t i; json_t *value;
    json_array_foreach(chunks, i, value) {
        const char *hash = json_string_value(value);
        if (!is_hex64(hash) || json_string_length(value) != 64) return NH_SYNCD_CACHE_ERR_JSON;
        if (json_object_set_new(set, hash, json_true())) return NH_SYNCD_CACHE_ERR_OOM;
    }
    return 0;
}
/* Enumerate every chunk_addrs_hex from snapshot.json (dedup). */
static int hashes_from_snapshot(const char *state_dir,
                                uint64_t *out_generation,
                                char ***out_hashes,
                                size_t *out_n) {
    if (!state_dir || !out_hashes || !out_n) return NH_SYNCD_CACHE_ERR_ARG;
    nh_syncd_state *state = NULL;
    bool unknown = true;
    if (nh_syncd_state_load(state_dir, &state, &unknown) || unknown) {
        nh_syncd_state_free(state);
        return NH_SYNCD_CACHE_ERR_JSON;
    }
    json_t *set = json_object();
    if (!set) { nh_syncd_state_free(state); return NH_SYNCD_CACHE_ERR_OOM; }
    int rc = nh_syncd_state_get_local_generation(state) > INT64_MAX ? NH_SYNCD_CACHE_ERR_JSON :
        nh_syncd_state_iter_(state, snapshot_row, set);
    size_t n = json_object_size(set), at = 0;
    char **arr = calloc(n + 1, sizeof *arr);
    if (!arr) rc = NH_SYNCD_CACHE_ERR_OOM;
    const char *key; json_t *value;
    json_object_foreach(set, key, value) {
        if (rc) break;
        arr[at] = xstrdup(key);
        if (!arr[at]) rc = NH_SYNCD_CACHE_ERR_OOM;
        else ++at;
    }
    if (out_generation) *out_generation = nh_syncd_state_get_local_generation(state);
    nh_syncd_state_free(state);
    json_decref(set);
    if (rc) {
        for (size_t i = 0; i < at; ++i) free(arr[i]);
        free(arr); return rc;
    }
    *out_hashes = arr;
    *out_n = n;
    return NH_SYNCD_CACHE_OK;
}

int nh_syncd_pin_ring_promote_from_snapshot(nh_syncd_pin_ring *r,
                                            const char *state_dir) {
    if (!r || !state_dir) return NH_SYNCD_CACHE_ERR_ARG;
    uint64_t gen = 0;
    char **hashes = NULL;
    size_t n = 0;
    int rc = hashes_from_snapshot(state_dir, &gen, &hashes, &n);
    if (rc != NH_SYNCD_CACHE_OK) return rc;
    rc = nh_syncd_pin_ring_promote(r, gen, (const char *const *)hashes, n);
    for (size_t i = 0; i < n; ++i) free(hashes[i]);
    free(hashes);
    return rc;
}

/* Exported so nh_syncd_sweep.c can share the enumerator without
 * re-decoding snapshot.json (kept file-static in the header only for
 * intra-library use — no ABI). */
int nh_syncd__snapshot_hashes(const char *state_dir,
                              uint64_t *out_generation,
                              char ***out_hashes,
                              size_t *out_n);
int nh_syncd__snapshot_hashes(const char *state_dir,
                              uint64_t *out_generation,
                              char ***out_hashes,
                              size_t *out_n) {
    return hashes_from_snapshot(state_dir, out_generation, out_hashes, out_n);
}
