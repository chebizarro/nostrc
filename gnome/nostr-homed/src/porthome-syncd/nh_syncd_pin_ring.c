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
 * The file format is stable across restart: it's the source of truth
 * for retention, so a corrupted file is an outage. Callers may treat
 * NH_SYNCD_CACHE_ERR_JSON as "recreate as empty" if they have no
 * better recovery (design: a lost ring costs at most 10 generations
 * worth of eviction safety, not user data — the blobs still exist on
 * Blossom until the weekly sweep proves otherwise).
 *
 * Bead: nostrc-p6qp.
 */

#define _GNU_SOURCE
#include "nh_syncd_cache.h"

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
    if (stat(r->path, &st) != 0) return NH_SYNCD_CACHE_OK; /* absent -> empty */
    json_error_t je;
    json_t *root = json_load_file(r->path, 0, &je);
    if (!root) return NH_SYNCD_CACHE_ERR_JSON;
    if (!json_is_object(root)) { json_decref(root); return NH_SYNCD_CACHE_ERR_JSON; }

    json_t *cap = json_object_get(root, "capacity");
    if (json_is_integer(cap)) {
        json_int_t c = json_integer_value(cap);
        if (c > 0 && c <= 1024) r->capacity = (size_t)c;
    }

    json_t *gens = json_object_get(root, "generations");
    if (!json_is_array(gens)) { json_decref(root); return NH_SYNCD_CACHE_OK; }

    size_t n = json_array_size(gens);
    if (n > r->capacity) n = r->capacity;
    r->slots = (ring_slot *)calloc(r->capacity, sizeof *r->slots);
    if (!r->slots) { json_decref(root); return NH_SYNCD_CACHE_ERR_OOM; }
    r->size = 0;

    for (size_t i = 0; i < n; ++i) {
        json_t *g = json_array_get(gens, i);
        if (!json_is_object(g)) continue;
        json_t *jgen = json_object_get(g, "gen");
        json_t *jhashes = json_object_get(g, "hashes");
        if (!json_is_integer(jgen) || !json_is_array(jhashes)) continue;

        ring_slot *slot = &r->slots[r->size];
        slot->generation = (uint64_t)json_integer_value(jgen);
        size_t hn = json_array_size(jhashes);
        slot->hashes = (char **)calloc(hn, sizeof(char *));
        if (!slot->hashes) { json_decref(root); return NH_SYNCD_CACHE_ERR_OOM; }
        for (size_t j = 0; j < hn; ++j) {
            json_t *h = json_array_get(jhashes, j);
            if (!json_is_string(h)) continue;
            const char *hex = json_string_value(h);
            if (!is_hex64(hex)) continue;
            slot->hashes[slot->n_hashes] = xstrdup(hex);
            if (!slot->hashes[slot->n_hashes]) {
                json_decref(root); return NH_SYNCD_CACHE_ERR_OOM;
            }
            slot->n_hashes++;
        }
        r->size++;
    }

    json_decref(root);
    return NH_SYNCD_CACHE_OK;
}

static int save_to_disk(const nh_syncd_pin_ring *r) {
    json_t *root = json_object();
    if (!root) return NH_SYNCD_CACHE_ERR_OOM;
    json_object_set_new(root, "schema", json_integer(1));
    json_object_set_new(root, "capacity", json_integer((json_int_t)r->capacity));
    json_t *gens = json_array();
    for (size_t i = 0; i < r->size; ++i) {
        const ring_slot *s = &r->slots[i];
        json_t *g = json_object();
        json_object_set_new(g, "gen", json_integer((json_int_t)s->generation));
        json_t *arr = json_array();
        for (size_t j = 0; j < s->n_hashes; ++j)
            json_array_append_new(arr, json_string(s->hashes[j]));
        json_object_set_new(g, "hashes", arr);
        json_array_append_new(gens, g);
    }
    json_object_set_new(root, "generations", gens);

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
    return NH_SYNCD_CACHE_OK;
}

int nh_syncd_pin_ring_open(const char *path, nh_syncd_pin_ring **out) {
    if (!path || !out) return NH_SYNCD_CACHE_ERR_ARG;
    nh_syncd_pin_ring *r = (nh_syncd_pin_ring *)calloc(1, sizeof *r);
    if (!r) return NH_SYNCD_CACHE_ERR_OOM;
    r->path = xstrdup(path);
    r->capacity = NH_SYNCD_CACHE_RETENTION_GENERATIONS;
    if (!r->path) { free(r); return NH_SYNCD_CACHE_ERR_OOM; }
    int rc = load_from_disk(r);
    if (rc == NH_SYNCD_CACHE_ERR_OOM || rc == NH_SYNCD_CACHE_ERR_IO) {
        nh_syncd_pin_ring_close(r);
        return rc;
    }
    if (rc == NH_SYNCD_CACHE_ERR_JSON) {
        /* Callers treat this as "recreate empty" — but honour their
         * choice; we return the error and leave `r` half-initialised
         * so they can pin_ring_close() and re-open after deleting. */
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
    if (!r || (!hashes && n_hashes)) return NH_SYNCD_CACHE_ERR_ARG;

    /* Dedup hashes (case-insensitive would be nice, but our whole
     * pipeline emits lowercase). Reject non-hex64 entries. */
    char **uniq = (char **)calloc(n_hashes + 1, sizeof(char *));
    if (!uniq && n_hashes) return NH_SYNCD_CACHE_ERR_OOM;
    size_t un = 0;
    for (size_t i = 0; i < n_hashes; ++i) {
        if (!is_hex64(hashes[i])) continue;
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

    /* Idempotent replace: if the last slot already matches this gen,
     * overwrite it. */
    if (r->size > 0 && r->slots[r->size - 1].generation == generation) {
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

/* Enumerate every chunk_addrs_hex from snapshot.json (dedup). */
static int hashes_from_snapshot(const char *state_dir,
                                uint64_t *out_generation,
                                char ***out_hashes,
                                size_t *out_n) {
    if (!state_dir || !out_hashes || !out_n) return NH_SYNCD_CACHE_ERR_ARG;
    char snap[PATH_MAX];
    int nn = snprintf(snap, sizeof snap, "%s/snapshot.json", state_dir);
    if (nn < 0 || (size_t)nn >= sizeof snap) return NH_SYNCD_CACHE_ERR_IO;
    json_error_t je;
    json_t *root = json_load_file(snap, 0, &je);
    if (!root) return NH_SYNCD_CACHE_ERR_JSON;
    if (!json_is_object(root)) { json_decref(root); return NH_SYNCD_CACHE_ERR_JSON; }

    if (out_generation) {
        json_t *g = json_object_get(root, "generation");
        *out_generation = json_is_integer(g) ? (uint64_t)json_integer_value(g) : 0;
    }
    json_t *files = json_object_get(root, "files");
    char **arr = NULL;
    size_t n = 0, cap = 0;
    if (json_is_object(files)) {
        const char *key;
        json_t *val;
        json_object_foreach(files, key, val) {
            (void)key;
            json_t *addrs = json_object_get(val, "chunk_addrs_hex");
            if (!json_is_array(addrs)) continue;
            size_t hn = json_array_size(addrs);
            for (size_t i = 0; i < hn; ++i) {
                json_t *h = json_array_get(addrs, i);
                if (!json_is_string(h)) continue;
                const char *hex = json_string_value(h);
                if (!is_hex64(hex)) continue;
                int seen = 0;
                for (size_t k = 0; k < n; ++k)
                    if (memcmp(arr[k], hex, 64) == 0) { seen = 1; break; }
                if (seen) continue;
                if (n == cap) {
                    size_t nc = cap ? cap * 2 : 64;
                    char **nx = (char **)realloc(arr, nc * sizeof(char *));
                    if (!nx) { for (size_t k = 0; k < n; ++k) free(arr[k]); free(arr); json_decref(root); return NH_SYNCD_CACHE_ERR_OOM; }
                    arr = nx; cap = nc;
                }
                arr[n] = xstrdup(hex);
                if (!arr[n]) { for (size_t k = 0; k < n; ++k) free(arr[k]); free(arr); json_decref(root); return NH_SYNCD_CACHE_ERR_OOM; }
                n++;
            }
        }
    }
    json_decref(root);
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
