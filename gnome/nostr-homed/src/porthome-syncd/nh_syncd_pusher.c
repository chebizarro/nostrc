/*
 * nh_syncd_pusher.c — push closure per design §6.2.
 *
 * SPDX-License-Identifier: MIT
 *
 * Pipeline (each step short-circuits on failure without touching state):
 *   1. Refuse if any interlock trips (limited-mode / partial state /
 *      unknown snapshot base).
 *   2. Walk the batch. For each item that survives the ignore filter
 *      and stat check, compare against the last-known state and
 *      classify as ADD | MODIFY | DELETE. Content-unchanged files
 *      (same size + same content hash) are skipped.
 *   3. For each ADD/MODIFY: read the file, chunk at 4 MiB, encrypt
 *      each chunk via nh_porthome_encrypt_chunk, HEAD each address
 *      on the configured servers and PUT the missing ones until at
 *      least min_replication servers report success.
 *   4. Apply the classified changes to the in-memory state, then
 *      rebuild the manifest from the FULL current state (§2.3: the
 *      pointer content is a whole-tree snapshot).
 *   5. Encode + AEAD-seal the manifest and publish the kind-30078
 *      pointer with generation = local_generation + 1. Wait for at
 *      least one relay to answer OK true within the timeout. When
 *      `event_publish_fn` is set, that overrides the libnostr path
 *      (test injection point).
 *   6. Advance local_generation, persist state, return OK.
 *
 * Everything else — inotify wiring, batch coalescing, the
 * subscription seam — lives in siblings. This file speaks a single
 * pure function that is unit-testable end-to-end against fakes.
 */

#include "nh_syncd.h"
#include "nh_syncd_auth.h"
#include <blossom/pool.h>

#include <blossom/shim.h>
#include <blossom/types.h>

#include <jansson.h>

#include "nostr-event.h"
#include "nostr-relay.h"
#include "nostr-tag.h"
#include "nostr-keys.h"
#include "error.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Symbols from nh_syncd_state.c that are not in the public header. */
extern int nh_syncd_state_upsert_file_(nh_syncd_state *s,
                                       const char *rel,
                                       uint32_t mode, uint32_t uid, uint32_t gid,
                                       uint64_t mtime_ns, uint64_t size,
                                       const char *content_hash_hex,
                                       const char *const *chunk_addrs_hex,
                                       size_t chunk_addrs_n);
extern int nh_syncd_state_set_lazy_dir_(nh_syncd_state *s, const char *rel, bool lazy);
extern int nh_syncd_state_upsert_dir_(nh_syncd_state *s,
                                      const char *rel,
                                      uint32_t mode, uint32_t uid, uint32_t gid,
                                      uint64_t mtime_ns);
extern int nh_syncd_state_upsert_symlink_(nh_syncd_state *s,
                                          const char *rel,
                                          uint32_t mode, uint32_t uid, uint32_t gid,
                                          uint64_t mtime_ns,
                                          const char *target);
extern int nh_syncd_state_delete_(nh_syncd_state *s, const char *rel);
extern void nh_syncd_state_set_local_generation_(nh_syncd_state *s, uint64_t generation);
extern void nh_syncd_state_set_pointer_event_id_(nh_syncd_state *s, const char *id);

typedef int (*nh_syncd_state_iter_fn_local)(void *ud, const char *rel, json_t *row);
/* nh_syncd_state.c defines the actual iter fn with the same shape but with json_t as the typedef.
 * We link against it via extern; the local typedef name is just here for readability. */
extern int nh_syncd_state_iter_(const nh_syncd_state *s,
                                nh_syncd_state_iter_fn_local cb, void *ud);

/* Small utility for error-message rendering. */
static void set_err(char **out, const char *fmt, ...) {
    if (!out) return;
    if (*out) { free(*out); *out = NULL; }
    va_list ap; va_start(ap, fmt);
    char *buf = NULL;
    if (vasprintf(&buf, fmt, ap) < 0) buf = NULL;
    va_end(ap);
    *out = buf;
}

/* Read whole file into memory. Fails if bigger than
 * NH_SYNCD_CHUNK_SIZE * MAX_CHUNKS_PER_ENTRY (design cap). */
static int slurp_file(const char *abs, uint8_t **out, size_t *out_len) {
    *out = NULL; *out_len = 0;
    int fd = open(abs, O_RDONLY);
    if (fd < 0) return NH_SYNCD_ERR_IO;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NH_SYNCD_ERR_IO; }
    uint64_t max_bytes = (uint64_t)NH_SYNCD_CHUNK_SIZE
                       * NH_PORTHOME_MAX_CHUNKS_PER_ENTRY;
    if ((uint64_t)st.st_size > max_bytes) { close(fd); return NH_SYNCD_ERR_PATH; }
    uint8_t *buf = malloc((size_t)st.st_size ? (size_t)st.st_size : 1);
    if (!buf) { close(fd); return NH_SYNCD_ERR_OOM; }
    size_t off = 0;
    while (off < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + off, (size_t)st.st_size - off);
        if (r < 0) { if (errno == EINTR) continue; free(buf); close(fd); return NH_SYNCD_ERR_IO; }
        if (r == 0) break;
        off += (size_t)r;
    }
    close(fd);
    *out = buf; *out_len = off;
    return NH_SYNCD_OK;
}

static void hex_of(const uint8_t *b, size_t n, char *out) {
    static const char lc[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = lc[(b[i] >> 4) & 0xf];
        out[i * 2 + 1] = lc[b[i] & 0xf];
    }
    out[n * 2] = '\0';
}

/* Milliseconds since the monotonic epoch (best-effort; wrap-safe
 * differences only). */
static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* Per-server PUT using the libblossom pool. Opens a
 * single-endpoint pool each call so `blossom_pool_upload`'s
 * fan-out is reduced to exactly one server — the syncd pusher owns the
 * outer per-server iteration. Coordinated with the libblossom
 * contract (see blossom/pool.h): the pool stays multi-server
 * capable; syncd just narrows its input list.
 *
 * Returns 0 on accept, an NH_SYNCD_ERR_* on refusal. */
static int upload_chunk_to_one_server(const nh_syncd_push_cfg *cfg,
                                      const char *server_url,
                                      const uint8_t *ct, size_t ct_len,
                                      const char *expected_sha_hex,
                                      int *out_http_status,
                                      uint64_t *out_ms) {
    if (out_http_status) *out_http_status = 0;
    uint64_t t0 = mono_ms();

    /* Test seam wins. */
    if (cfg->upload_to_server_fn) {
        int rc = cfg->upload_to_server_fn(cfg->upload_to_server_ud,
                                          server_url, ct, ct_len,
                                          expected_sha_hex,
                                          out_http_status, out_ms);
        if (out_ms && *out_ms == 0) *out_ms = mono_ms() - t0;
        return rc == 0 ? NH_SYNCD_OK : NH_SYNCD_ERR_UPLOAD;
    }

    /* Real path: single-endpoint wrapper. */
    const char *one[1] = { server_url };
    blossom_pool_opts_t bopts = {0};
    const char *allow_insecure = getenv("NH_PORTHOME_ALLOW_INSECURE");
    bopts.allow_insecure = allow_insecure && !strcmp(allow_insecure, "1");
    bopts.servers        = one;
    bopts.n_servers      = 1;
    bopts.max_blob_bytes = ct_len + NH_PORTHOME_SEAL_OVERHEAD + 4096;
    blossom_pool_t *one_bl = NULL;
    if (blossom_pool_new(&bopts, cfg->bud02_signer, &one_bl) != 0) {
        if (out_ms) *out_ms = mono_ms() - t0;
        return NH_SYNCD_ERR_UPLOAD;
    }
    char echo[65];
    int urc = blossom_pool_upload(one_bl, ct, ct_len,
                                         expected_sha_hex, echo);
    blossom_pool_free(one_bl);
    if (out_ms) *out_ms = mono_ms() - t0;
    /* We don't get a raw HTTP status back through the wrapper's
     * boundary; leave 0 unless a test seam supplied one. */
    return urc == 0 ? NH_SYNCD_OK : NH_SYNCD_ERR_UPLOAD;
}

/* Upload one plaintext chunk after encryption, iterating the configured
 * server list explicitly so `min_replication` is a HARD requirement.
 *
 * On success writes the 64-hex sealed address to `out_addr_hex` and
 * fills `*out_result` with per-server outcomes.
 *
 * Returns:
 *   NH_SYNCD_OK                              — quorum met
 *   NH_SYNCD_ERR_INSUFFICIENT_REPLICATION    — < min_replication accepts
 *   NH_SYNCD_ERR_UPLOAD                      — zero accepts
 *   NH_SYNCD_ERR_CRYPTO                      — encryption failure
 *
 * `bl` is retained for HEAD-set probing when the caller has already
 * built one. May be NULL — the daemon still records the sealed address
 * in the snapshot in that case (test-injection path when
 * `n_blossom_servers == 0`). */
static int upload_chunk(const nh_syncd_push_cfg *cfg,
                        blossom_pool_t *bl,
                        const uint8_t *pt, size_t pt_len,
                        char out_addr_hex[65],
                        nh_syncd_upload_result *out_result,
                        char **err) {
    if (out_result) memset(out_result, 0, sizeof *out_result);

    uint8_t *ct = NULL; size_t ct_len = 0; uint8_t sha[32];
    int rc = nh_porthome_encrypt_chunk(cfg->home_key, pt, pt_len,
                                       &ct, &ct_len, sha);
    if (rc != 0) { set_err(err, "encrypt_chunk rc=%d", rc); return NH_SYNCD_ERR_CRYPTO; }

    /* PNG-shim consistency (nostrc-wmb5) + auto-decide (nostrc-si30).
     * When the shim is active the downstream blossom_pool_upload
     * will hash sha256(shim||ct) and PUT that address. The "sealed
     * address" this function publishes (out_addr_hex, stored in the
     * syncd state and rebuilt into every future manifest for this
     * chunk) MUST match, or the eventual pull looks up sha256(ct) and
     * 404s on every chunk. Swap sha to the shim-inclusive hash BEFORE
     * we serialise it and BEFORE we pass it as `expected_sha_hex` to
     * the single-blob uploader — otherwise the uploader's own
     * consistency check trips HASH_MISMATCH once it wraps.
     *
     * We consult the SAME auto-decide helper that
     * libblossom's pool shim gate uses. Both call sites walk the
     * same cfg->blossom_servers[] list, so libhanami's URL-keyed cache
     * makes them agree (a probe run on any URL is memoised for the
     * process). The env-var override still wins when set. */
    bool shim_on = blossom_shim_active_for(
        cfg->blossom_servers, cfg->n_blossom_servers);
    if (shim_on) {
        uint8_t shim_sha[32];
        if (blossom_shim_sha256(ct, ct_len, shim_sha) != BLOSSOM_OK) {
            free(ct);
            set_err(err, "shim_sha256 failed");
            return NH_SYNCD_ERR_CRYPTO;
        }
        memcpy(sha, shim_sha, 32);
    }
    hex_of(sha, 32, out_addr_hex);

    size_t n_servers = cfg->n_blossom_servers;
    size_t need = cfg->min_replication ? cfg->min_replication
                                       : NH_SYNCD_DEFAULT_MIN_REPLICATION;

    /* Test-injection path: no servers configured. Sealed address is
     * still recorded in the snapshot so a later live-Blossom sweep
     * (I3) can re-upload if necessary. Treat this as "quorum vacuously
     * met" — matches the pre-xnxd behaviour driven by test_syncd_push_e2e. */
    if (n_servers == 0) {
        free(ct);
        if (out_result) { out_result->total = 0; out_result->succeeded = 0; }
        return NH_SYNCD_OK;
    }

    /* Optional HEAD fast-path for the common "need == 1, blob already
     * present on some server" case. Skipped when the test seam is in
     * play (the seam simulates fresh uploads and would break on a
     * real-transport HEAD to a fake URL) and skipped when we can't tell
     * per-server which server responded (any need > 1 forces a full
     * per-server PUT loop for accurate accounting anyway). */
    if (bl && need == 1 && !cfg->upload_to_server_fn) {
        bool present = false;
        (void)blossom_pool_has(bl, out_addr_hex, &present);
        if (present) {
            if (out_result) {
                out_result->total = n_servers;
                out_result->succeeded = 1;
                out_result->per_server[0].server_url    = cfg->blossom_servers[0];
                out_result->per_server[0].http_status   = 200;
                out_result->per_server[0].bytes_uploaded = 0;
                out_result->per_server[0].error_class   = 0;
            }
            free(ct);
            return NH_SYNCD_OK;
        }
    }

    /* Per-server iteration. */
    if (out_result) out_result->total = n_servers;
    size_t successes = 0;
    size_t recorded = n_servers > NH_SYNCD_MAX_SERVERS_PER_UPLOAD
                    ? NH_SYNCD_MAX_SERVERS_PER_UPLOAD
                    : n_servers;
    for (size_t si = 0; si < n_servers; si++) {
        int http = 0;
        uint64_t ms = 0;
        int urc = upload_chunk_to_one_server(cfg, cfg->blossom_servers[si],
                                             ct, ct_len, out_addr_hex,
                                             &http, &ms);
        if (si < recorded && out_result) {
            out_result->per_server[si].server_url     = cfg->blossom_servers[si];
            out_result->per_server[si].http_status    = http;
            out_result->per_server[si].bytes_uploaded = (urc == NH_SYNCD_OK) ? ct_len : 0;
            out_result->per_server[si].elapsed_ms     = ms;
            out_result->per_server[si].error_class    = (urc == NH_SYNCD_OK) ? 0 : urc;
        }
        if (urc == NH_SYNCD_OK) successes++;
    }
    if (out_result) out_result->succeeded = successes;
    free(ct);

    if (successes == 0) {
        set_err(err, "no server accepted (0/%zu)", n_servers);
        return NH_SYNCD_ERR_UPLOAD;
    }
    if (successes < need) {
        set_err(err, "insufficient replication (%zu/%zu, need %zu)",
                successes, n_servers, need);
        return NH_SYNCD_ERR_INSUFFICIENT_REPLICATION;
    }
    return NH_SYNCD_OK;
}

/* Compute sha256 of a whole plaintext buffer as 64-hex. */
static int sha256_hex(const uint8_t *buf, size_t len, char out[65]) {
    uint8_t h[32];
    int rc = nh_porthome_sha256(buf, len, h);
    if (rc != 0) return rc;
    hex_of(h, 32, out);
    return 0;
}

/* Iterator callback that appends every recorded entry to a manifest
 * in the state's insertion order. */
typedef struct {
    const uint8_t *home_key;
    nh_porthome_manifest *m;
    const nh_syncd_push_cfg *cfg;
    int rc;
} rebuild_ctx;

static int rebuild_cb(void *ud, const char *rel, json_t *row) {
    rebuild_ctx *rc = (rebuild_ctx *)ud;
    if (rc->cfg->exclude_path && rc->cfg->exclude_path(rc->cfg->exclude_path_ud, rel)) return 0;
    const char *kind = json_string_value(json_object_get(row, "kind"));
    uint32_t mode    = (uint32_t)json_integer_value(json_object_get(row, "mode"));
    uint32_t uid     = (uint32_t)json_integer_value(json_object_get(row, "uid"));
    uint32_t gid     = (uint32_t)json_integer_value(json_object_get(row, "gid"));
    uint64_t mt_ns   = (uint64_t)json_integer_value(json_object_get(row, "mtime_ns"));
    uint64_t size    = (uint64_t)json_integer_value(json_object_get(row, "size"));

    char *penc = NULL;
    if (nh_porthome_encrypt_path(rc->home_key, rel, &penc) != 0) {
        rc->rc = NH_SYNCD_ERR_CRYPTO; return -1;
    }
    const char *base = strrchr(rel, '/');
    char *name = strdup(base ? base + 1 : rel);
    if (!name) { free(penc); rc->rc = NH_SYNCD_ERR_OOM; return -1; }
    if (kind && !strcmp(kind, "dir")) {
        rc->rc = nh_porthome_manifest_add_dir_v2(rc->m, penc, name, mode, uid, gid, mt_ns);
        if (rc->rc == 0 && (json_is_true(json_object_get(row, "lazy")) ||
                            (rc->cfg->lazy && nh_syncd_lazy_covers(rc->cfg->lazy, rel))))
            rc->m->entries[rc->m->entries_len - 1].lazy = true;
        return rc->rc == 0 ? 0 : -1;
    }
    if (kind && !strcmp(kind, "symlink")) {
        const char *tgt = json_string_value(json_object_get(row, "symlink_target"));
        char *dup = strdup(tgt ? tgt : "");
        if (!dup) { free(penc); rc->rc = NH_SYNCD_ERR_OOM; return -1; }
        rc->rc = nh_porthome_manifest_add_symlink_v2(rc->m, penc, name, mode, uid, gid, mt_ns, dup);
        return rc->rc == 0 ? 0 : -1;
    }
    /* file */
    json_t *addrs = json_object_get(row, "chunk_addrs_hex");
    size_t n = json_array_size(addrs);
    nh_porthome_chunk *chs = NULL;
    if (n) {
        chs = calloc(n, sizeof *chs);
        if (!chs) { free(penc); rc->rc = NH_SYNCD_ERR_OOM; return -1; }
    }
    for (size_t i = 0; i < n; i++) {
        const char *ah = json_string_value(json_array_get(addrs, i));
        if (!ah || nh_porthome_from_hex64(ah, chs[i].sha256) != 0) {
            free(chs); free(penc); rc->rc = NH_SYNCD_ERR_MANIFEST; return -1;
        }
        /* We do not persist per-chunk sealed size in snapshot.json;
         * recompute conservatively as full-chunk + overhead. This is
         * fine for the manifest's "hint" size field. */
        chs[i].size = NH_SYNCD_CHUNK_SIZE + NH_PORTHOME_SEAL_OVERHEAD;
        chs[i].chunk_key_id = 0;
    }
    rc->rc = nh_porthome_manifest_add_file_v2(rc->m, penc, name, mode, uid, gid,
                                              mt_ns, size, chs, n);
    free(chs);
    return rc->rc == 0 ? 0 : -1;
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    char *challenge;
    char auth_id[65];
    char *auth_reason;
    bool auth_done;
    bool auth_ok;
} publish_auth;

static struct timespec auth_deadline(uint32_t timeout_ms) {
    struct timespec end;
#ifdef __linux__
    clock_gettime(CLOCK_MONOTONIC, &end);
#else
    clock_gettime(CLOCK_REALTIME, &end);
#endif
    end.tv_sec += timeout_ms / 1000;
    end.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (end.tv_nsec >= 1000000000L) { end.tv_sec++; end.tv_nsec -= 1000000000L; }
    return end;
}

static void publish_auth_challenge(NostrRelay *relay, const char *challenge, void *ud) {
    publish_auth *a = ud;
    (void)relay;
    if (!challenge) return;
    char *copy = strdup(challenge);
    if (!copy) return;
    pthread_mutex_lock(&a->mu);
    free(a->challenge);
    a->challenge = copy;
    pthread_cond_signal(&a->cv);
    pthread_mutex_unlock(&a->mu);
}

static void publish_auth_ok(const char *id, bool accepted, const char *reason, void *ud) {
    publish_auth *a = ud;
    if (!id) return;
    pthread_mutex_lock(&a->mu);
    if (a->auth_id[0] && !strcmp(a->auth_id, id)) {
        a->auth_done = true;
        a->auth_ok = accepted;
        free(a->auth_reason);
        a->auth_reason = reason ? strdup(reason) : NULL;
        pthread_cond_signal(&a->cv);
    }
    pthread_mutex_unlock(&a->mu);
}

/* The pull subscription uses this same kind-22242 signer/send path. The
 * callback records the correlated OK; neither an AUTH write nor the
 * pointer's initial auth-required response counts as acceptance. */
static bool authenticate_publish(NostrRelay *relay, const char *secret,
                                 publish_auth *a, uint32_t timeout_ms,
                                 char **reason) {
    struct timespec end = auth_deadline(timeout_ms);
    pthread_mutex_lock(&a->mu);
    while (!a->challenge) {
        if (pthread_cond_timedwait(&a->cv, &a->mu, &end) == ETIMEDOUT) {
            pthread_mutex_unlock(&a->mu);
            set_err(reason, "auth-required: timed out waiting for relay AUTH challenge");
            return false;
        }
    }
    char *challenge = a->challenge;
    a->challenge = NULL;
    a->auth_done = false;
    a->auth_id[0] = '\0';
    bool sent = nh_syncd_send_auth(relay, secret, challenge, a->auth_id);
    free(challenge);
    if (!sent) {
        pthread_mutex_unlock(&a->mu);
        set_err(reason, "auth-required: signing or sending relay AUTH failed");
        return false;
    }
    while (!a->auth_done) {
        if (pthread_cond_timedwait(&a->cv, &a->mu, &end) == ETIMEDOUT) {
            pthread_mutex_unlock(&a->mu);
            set_err(reason, "auth-required: timed out waiting for relay AUTH OK");
            return false;
        }
    }
    bool accepted = a->auth_ok;
    if (!accepted) set_err(reason, "%s", a->auth_reason ? a->auth_reason : "relay rejected AUTH");
    pthread_mutex_unlock(&a->mu);
    return accepted;
}

/* Sign + publish the kind-30078 pointer via libnostr, waiting for at
 * least one OK true. Timeout is per-relay. */
static int publish_via_libnostr(const nh_syncd_push_cfg *cfg,
                                const char *pubkey_hex,
                                int64_t created_at,
                                const uint8_t *sealed, size_t sealed_len,
                                char out_event_id[65],
                                char **err) {
    /* Encode sealed as lowercase hex — same shape the operator
     * publisher and fetch helper agree on. */
    char *content = malloc(sealed_len * 2 + 1);
    if (!content) return NH_SYNCD_ERR_OOM;
    hex_of(sealed, sealed_len, content);

    NostrEvent *e = nostr_event_new();
    if (!e) { free(content); return NH_SYNCD_ERR_OOM; }

    nostr_event_set_pubkey(e, pubkey_hex);
    nostr_event_set_kind(e, 30078);
    nostr_event_set_created_at(e, created_at);
    nostr_event_set_content(e, content);
    const char *d_tag = cfg->d_tag ? cfg->d_tag : "nostr-homed.home.v1:personal";
    NostrTags *tags = nostr_tags_new(2,
        nostr_tag_new("d", d_tag, NULL),
        nostr_tag_new("client", "nostr-home-syncd", NULL));
    if (!tags) { nostr_event_free(e); free(content); return NH_SYNCD_ERR_OOM; }
    e->tags = tags;

    if (nostr_event_sign(e, cfg->event_signer_nsec_hex) != 0) {
        nostr_event_free(e); free(content);
        set_err(err, "event_sign failed");
        return NH_SYNCD_ERR_PUBLISH;
    }
    free(content);
    if (e->id) {
        strncpy(out_event_id, e->id, 64);
        out_event_id[64] = '\0';
    } else {
        out_event_id[0] = '\0';
    }

    uint32_t tmo = cfg->relay_publish_timeout_ms ?
                   cfg->relay_publish_timeout_ms :
                   NH_SYNCD_DEFAULT_PUBLISH_TIMEOUT_MS;

    int any_ok = 0;
    char *last_failure = NULL, *restricted_failure = NULL;
    for (size_t i = 0; i < cfg->n_relays; i++) {
        const char *url = cfg->relays[i];
        Error *werr = NULL;
        NostrRelay *r = nostr_relay_new(NULL, url, &werr);
        if (!r) {
            set_err(&last_failure, "%s: %s", url,
                    werr && werr->message ? werr->message : "relay creation failed");
            free_error(werr);
            continue;
        }
        free_error(werr);
        publish_auth auth = {0};
        if (pthread_mutex_init(&auth.mu, NULL) != 0) {
            set_err(&last_failure, "%s: AUTH mutex initialization failed", url);
            nostr_relay_free(r);
            continue;
        }
        pthread_condattr_t attr;
        int attr_ok = pthread_condattr_init(&attr);
        int cond_ok = attr_ok;
#ifdef __linux__
        if (cond_ok == 0) cond_ok = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
        if (cond_ok == 0) cond_ok = pthread_cond_init(&auth.cv, &attr);
        if (attr_ok == 0) pthread_condattr_destroy(&attr);
        if (cond_ok != 0) {
            set_err(&last_failure, "%s: AUTH condition initialization failed", url);
            pthread_mutex_destroy(&auth.mu);
            nostr_relay_free(r);
            continue;
        }
        nostr_relay_set_auth_callback(r, publish_auth_challenge, &auth);
        nostr_relay_set_ok_callback(r, publish_auth_ok, &auth);
        werr = NULL;
        bool connected = nostr_relay_connect(r, &werr);
        if (!connected) {
            set_err(&last_failure, "%s: %s", url,
                    werr && werr->message ? werr->message : "relay connect failed");
        }
        free_error(werr);
        if (connected) {
            pthread_mutex_lock(&auth.mu);
            bool challenged = auth.challenge != NULL;
            pthread_mutex_unlock(&auth.mu);
            char *reason = NULL;
            bool authenticated = !challenged ||
                authenticate_publish(r, cfg->event_signer_nsec_hex, &auth, tmo, &reason);
            bool pub = false;
            if (authenticated) {
                werr = NULL;
                pub = nostr_relay_publish_and_wait(r, e, tmo, &werr);
                if (!pub && werr && werr->message)
                    set_err(&reason, "%s", werr->message);
                free_error(werr);
                pthread_mutex_lock(&auth.mu);
                bool late_challenge = auth.challenge != NULL;
                pthread_mutex_unlock(&auth.mu);
                if (!pub &&
                    ((reason && strstr(reason, "auth-required:")) ||
                     (!challenged && late_challenge &&
                      (!reason || strstr(reason, "timed out")))) &&
                    !(reason && strstr(reason, "restricted:"))) {
                    free(reason); reason = NULL;
                    if (authenticate_publish(r, cfg->event_signer_nsec_hex, &auth, tmo, &reason)) {
                        werr = NULL;
                        pub = nostr_relay_publish_and_wait(r, e, tmo, &werr);
                        if (!pub && werr && werr->message)
                            set_err(&reason, "%s", werr->message);
                        free_error(werr);
                    }
                }
            }
            if (pub) any_ok = 1;
            else {
                set_err(&last_failure, "%s: %s", url,
                        reason ? reason : "no relay OK for pointer");
                if (reason && strstr(reason, "restricted:") && !restricted_failure)
                    restricted_failure = strdup(last_failure);
                fprintf(stderr, "syncd/pusher: %s\n", last_failure);
            }
            free(reason);
        }
        nostr_relay_free(r);
        pthread_cond_destroy(&auth.cv);
        pthread_mutex_destroy(&auth.mu);
        free(auth.challenge);
        free(auth.auth_reason);
        /* First OK true wins; other configured relays remain best effort. */
        if (any_ok) break;
    }

    nostr_event_free(e);
    if (!any_ok) {
        set_err(err, "no relay OK'd the pointer: %s",
                restricted_failure ? restricted_failure :
                last_failure ? last_failure : "no relays configured");
    }
    free(last_failure);
    free(restricted_failure);
    return any_ok ? NH_SYNCD_OK : NH_SYNCD_ERR_PUBLISH;
}

/* Callback for nh_syncd_state_save (state_dir handed in via user
 * data). Forward declaration from nh_syncd_state.c. */
extern int nh_syncd_state_save(const nh_syncd_state *s, const char *state_dir);

int nh_syncd_push_batch(const nh_syncd_push_cfg *cfg,
                        nh_syncd_state           *state,
                        const nh_syncd_batch     *batch,
                        const char               *root_dir,
                        const nh_syncd_ignore    *ignore,
                        const nh_syncd_interlocks *ilk,
                        const char               *state_dir_for_persist,
                        char                    **out_error_msg)
{
    if (out_error_msg) *out_error_msg = NULL;
    if (!cfg || !state || !batch || !root_dir || !ignore || !ilk)
        return NH_SYNCD_ERR_ARG;

    /* xnxd part 1: track worst per-server outcome across every chunk we
     * upload in this batch. On success the pusher hands `worst_result`
     * to the summary callback so status readers see the tightest quorum
     * the batch achieved. `worst_error_class` is empty on OK. */
    nh_syncd_upload_result worst_result = {0};
    const char *worst_error_class = NULL;

    /* Interlock 1: limited-mode / partial. */
    int rc = nh_syncd_interlocks_check(ilk);
    if (rc != NH_SYNCD_OK) return rc;

    /* Interlock 2: snapshot base known. `root_id` is all zeros in a
     * freshly-loaded empty state (see nh_syncd_state_load "missing"
     * branch); reject push in that case. Callers that want to seed
     * a first-ever snapshot should call nh_syncd_state_new first
     * (with a real root_id) — the daemon main does so. */
    uint8_t zero[NH_PORTHOME_SHA256_LEN] = {0};
    /* We can't read state's root_id from public API, so use the fact
     * that a freshly-loaded state without a snapshot.json also has
     * an empty account_pubkey_hex + empty d_tag. */
    const char *root = nh_syncd_state_get_root(state);
    const char *dtag = nh_syncd_state_get_d_tag(state);
    (void)zero;
    if (!root || !*root || !dtag || !*dtag) return NH_SYNCD_ERR_SNAPSHOT_UNKNOWN;

    /* Interlock 3: signer wiring. */
    if (!cfg->event_signer_nsec_hex && !cfg->event_publish_fn)
        return NH_SYNCD_ERR_ARG;

    /* Derive account pubkey. */
    char pubkey_hex[65] = {0};
    if (cfg->account_pubkey_hex_override) {
        strncpy(pubkey_hex, cfg->account_pubkey_hex_override, 64);
        pubkey_hex[64] = '\0';
    } else {
        char *pk = nostr_key_get_public(cfg->event_signer_nsec_hex);
        if (!pk) { set_err(out_error_msg, "nostr_key_get_public failed"); return NH_SYNCD_ERR_ARG; }
        strncpy(pubkey_hex, pk, 64);
        pubkey_hex[64] = '\0';
        free(pk);
    }

    /* Open Blossom wrapper (only if we actually need to upload). */
    blossom_pool_t *bl = NULL;
    if (cfg->n_blossom_servers > 0) {
        blossom_pool_opts_t bopts = {0};
        const char *allow_insecure = getenv("NH_PORTHOME_ALLOW_INSECURE");
        bopts.allow_insecure = allow_insecure && !strcmp(allow_insecure, "1");
        bopts.servers   = cfg->blossom_servers;
        bopts.n_servers = cfg->n_blossom_servers;
        bopts.max_blob_bytes = NH_SYNCD_CHUNK_SIZE + NH_PORTHOME_SEAL_OVERHEAD + 4096;
        if (blossom_pool_new(&bopts, cfg->bud02_signer, &bl) != 0) {
            set_err(out_error_msg, "blossom_new failed");
            return NH_SYNCD_ERR_UPLOAD;
        }
    }

    bool changed = false;
    bool excluded_in_batch = false;
    /* Walk each batch item. Anything that survives the ignore gate
     * and stat/permission checks is classified as ADD / MODIFY /
     * DELETE and mutates state.  Errors along the way roll back the
     * whole push (state stays as-loaded). */
    for (size_t i = 0; i < nh_syncd_batch_len(batch); i++) {
        const char *rel = NULL;
        nh_syncd_change_kind kind = 0;
        (void)nh_syncd_batch_at(batch, i, &rel, &kind);
        if (!rel || !*rel) continue;
        if (cfg->exclude_path && cfg->exclude_path(cfg->exclude_path_ud, rel)) {
            excluded_in_batch = true;
            continue;
        }

        /* Ignore gate (path-only + stat). */
        char abs[PATH_MAX];
        int an = snprintf(abs, sizeof abs, "%s/%s", root_dir, rel);
        if (an < 0 || an >= (int)sizeof abs) continue;
        struct stat st;
        int have_st = (lstat(abs, &st) == 0);
        nh_syncd_ignore_kind ik =
            have_st ? nh_syncd_ignore_check(ignore, rel,
                                            (uint64_t)st.st_dev,
                                            (uint32_t)st.st_mode)
                    : nh_syncd_ignore_check_path(ignore, rel);
        if (ik != NH_SYNCD_IGNORE_PASS) continue;

        /* DELETE (or ADD/MODIFY where the file is now gone). */
        if (!have_st || kind == NH_SYNCD_CHANGE_DELETE) {
            if (nh_syncd_state_find(state, rel)) {
                nh_syncd_state_delete_(state, rel);
                changed = true;
            }
            continue;
        }

        uint32_t mode  = st.st_mode & 07777;
        uint32_t uid   = (uint32_t)st.st_uid;
        uint32_t gid   = (uint32_t)st.st_gid;
        uint64_t mt_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull
                       + (uint64_t)st.st_mtim.tv_nsec;

        if (S_ISDIR(st.st_mode)) {
            const nh_syncd_entry *cur = nh_syncd_state_find(state, rel);
            if (cur && nh_syncd_entry_kind(cur) == NH_SYNCD_KIND_DIR &&
                nh_syncd_entry_mode(cur) == mode &&
                nh_syncd_entry_uid(cur) == uid && nh_syncd_entry_gid(cur) == gid &&
                nh_syncd_entry_mtime_ns(cur) == mt_ns) continue;
            (void)nh_syncd_state_upsert_dir_(state, rel, mode, uid, gid, mt_ns);
            changed = true;
            if (cfg->lazy && nh_syncd_lazy_covers(cfg->lazy, rel))
                (void)nh_syncd_state_set_lazy_dir_(state, rel, true);
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            char tgt[4096];
            ssize_t tn = readlink(abs, tgt, sizeof tgt - 1);
            if (tn < 0) { rc = NH_SYNCD_ERR_IO; goto fail; }
            tgt[tn] = '\0';
            const nh_syncd_entry *cur = nh_syncd_state_find(state, rel);
            if (cur && nh_syncd_entry_kind(cur) == NH_SYNCD_KIND_SYMLINK &&
                nh_syncd_entry_mode(cur) == mode &&
                nh_syncd_entry_uid(cur) == uid && nh_syncd_entry_gid(cur) == gid &&
                nh_syncd_entry_mtime_ns(cur) == mt_ns &&
                nh_syncd_entry_symlink_target(cur) &&
                !strcmp(nh_syncd_entry_symlink_target(cur), tgt)) continue;
            (void)nh_syncd_state_upsert_symlink_(state, rel, mode, uid, gid, mt_ns, tgt);
            changed = true;
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue; /* defence-in-depth */

        uint8_t *pt = NULL; size_t pt_len = 0;
        rc = slurp_file(abs, &pt, &pt_len);
        if (rc != NH_SYNCD_OK) { set_err(out_error_msg, "read %s failed", rel); goto fail; }

        char content_hash_hex[65];
        if (sha256_hex(pt, pt_len, content_hash_hex) != 0) {
            free(pt); rc = NH_SYNCD_ERR_CRYPTO; goto fail;
        }
        /* Skip exact unchanged entries caused by spurious inotify wake-ups. */
        const nh_syncd_entry *cur = nh_syncd_state_find(state, rel);
        size_t expected_chunks = pt_len ?
            (pt_len + NH_SYNCD_CHUNK_SIZE - 1) / NH_SYNCD_CHUNK_SIZE : 0;
        if (cur && nh_syncd_entry_size(cur) == pt_len &&
            nh_syncd_entry_chunk_count(cur) == expected_chunks &&
            nh_syncd_entry_kind(cur) == NH_SYNCD_KIND_FILE &&
            nh_syncd_entry_mode(cur) == mode &&
            nh_syncd_entry_uid(cur) == uid && nh_syncd_entry_gid(cur) == gid &&
            nh_syncd_entry_mtime_ns(cur) == mt_ns &&
            !strcmp(nh_syncd_entry_content_hash_hex(cur), content_hash_hex)) {
            free(pt);
            continue;
        }

        /* Chunk + upload. */
        size_t n_chunks = pt_len ? (pt_len + NH_SYNCD_CHUNK_SIZE - 1) / NH_SYNCD_CHUNK_SIZE : 0;
        char **addrs = n_chunks ? calloc(n_chunks, sizeof(char *)) : NULL;
        if (n_chunks && !addrs) { free(pt); rc = NH_SYNCD_ERR_OOM; goto fail; }
        size_t off = 0;
        for (size_t ci = 0; ci < n_chunks; ci++) {
            size_t want = pt_len - off;
            if (want > NH_SYNCD_CHUNK_SIZE) want = NH_SYNCD_CHUNK_SIZE;
            char hex[65];
            nh_syncd_upload_result ur = {0};
            int urc = upload_chunk(cfg, bl, pt + off, want, hex, &ur, out_error_msg);
            /* Track worst-observed replication so the summary callback
             * reports the tightest quorum this batch achieved. First
             * chunk seeds; subsequent chunks demote by min(). */
            if (worst_result.total == 0 && ur.total > 0) {
                worst_result = ur;
            } else if (ur.total > 0 && ur.succeeded < worst_result.succeeded) {
                worst_result = ur;
            }
            if (urc != NH_SYNCD_OK) {
                worst_error_class = (urc == NH_SYNCD_ERR_INSUFFICIENT_REPLICATION)
                                    ? "insufficient-replication"
                                    : "upload-failed";
                for (size_t j = 0; j < ci; j++) free(addrs[j]);
                free(addrs); free(pt);
                rc = urc; goto fail;
            }
            addrs[ci] = strdup(hex);
            if (!addrs[ci]) {
                for (size_t j = 0; j < ci; j++) free(addrs[j]);
                free(addrs); free(pt);
                rc = NH_SYNCD_ERR_OOM; goto fail;
            }
            off += want;
        }
        /* Update state. */
        (void)nh_syncd_state_upsert_file_(state, rel, mode, uid, gid,
                                          mt_ns, pt_len,
                                          content_hash_hex,
                                          (const char *const *)addrs,
                                          n_chunks);
        for (size_t j = 0; j < n_chunks; j++) free(addrs[j]);
        free(addrs);
        free(pt);
        changed = true;
    }
    if (!changed && !excluded_in_batch) {
        if (cfg->on_upload_summary)
            cfg->on_upload_summary(cfg->on_upload_summary_ud, &worst_result);
        if (bl) blossom_pool_free(bl);
        return NH_SYNCD_OK;
    }

    /* Rebuild the WHOLE manifest from the (now-updated) state. */
    nh_porthome_manifest m;
    if (nh_porthome_manifest_init_v2(&m, cfg->root_id) != 0) {
        rc = NH_SYNCD_ERR_MANIFEST; goto fail;
    }
    rebuild_ctx rctx = { cfg->home_key, &m, cfg, NH_SYNCD_OK };
    (void)nh_syncd_state_iter_(state, rebuild_cb, &rctx);
    if (rctx.rc != NH_SYNCD_OK) {
        nh_porthome_manifest_dispose(&m);
        rc = rctx.rc; goto fail;
    }
    uint8_t *sealed = NULL; size_t sealed_len = 0;
    if (nh_porthome_manifest_encode_sealed(&m, cfg->home_key,
                                           &sealed, &sealed_len) != 0) {
        nh_porthome_manifest_dispose(&m);
        rc = NH_SYNCD_ERR_MANIFEST; goto fail;
    }
    nh_porthome_manifest_dispose(&m);

    /* Publish. */
    char event_id[65] = {0};
    int64_t created_at = (int64_t)time(NULL);
    uint64_t previous = nh_syncd_state_get_local_generation(state);
    if (previous >= INT64_MAX) {
        free(sealed);
        set_err(out_error_msg, "local generation exhausted");
        rc = NH_SYNCD_ERR_LIMITED_MODE; goto fail;
    }
    if (created_at <= (int64_t)previous) created_at = (int64_t)(previous + 1);
    if (cfg->event_publish_fn) {
        int prc = cfg->event_publish_fn(cfg->event_publish_ud,
                                        pubkey_hex, created_at,
                                        cfg->d_tag ? cfg->d_tag : "nostr-homed.home.v1:personal",
                                        sealed, sealed_len, event_id);
        if (prc != 0) {
            free(sealed);
            set_err(out_error_msg, "test publish_fn returned %d", prc);
            rc = NH_SYNCD_ERR_PUBLISH; goto fail;
        }
    } else {
        int prc = publish_via_libnostr(cfg, pubkey_hex, created_at,
                                       sealed, sealed_len, event_id,
                                       out_error_msg);
        if (prc != NH_SYNCD_OK) {
            free(sealed);
            rc = prc; goto fail;
        }
    }
    free(sealed);

    /* The signed pointer's timestamp is the generation stamp. Keep the
     * durable state at that same value so own relay echoes are no-ops. */
    nh_syncd_state_set_local_generation_(state, (uint64_t)created_at);
    /* OK confirms receipt, not that this addressable event won every relay's
     * replaceable-head ordering. Keep the last reconciled remote cursor until
     * a readback, so a concurrent remote head cannot delete local-only work. */
    nh_syncd_state_set_pointer_event_id_(state, event_id);
    if (state_dir_for_persist) {
        int srv = nh_syncd_state_save(state, state_dir_for_persist);
        if (srv != NH_SYNCD_OK) {
            /* State advanced in memory but wasn't persisted — mark
             * as a warning; the pointer is already on the relay so
             * generation IS effectively advanced. Return OK; caller
             * logs via out_error_msg. */
            set_err(out_error_msg, "state_save rc=%d (pointer already published)", srv);
        }
    }

    if (cfg->on_upload_summary)
        cfg->on_upload_summary(cfg->on_upload_summary_ud, &worst_result);
    if (bl) blossom_pool_free(bl);
    return NH_SYNCD_OK;

fail:
    if (cfg->on_upload_summary)
        cfg->on_upload_summary(cfg->on_upload_summary_ud, &worst_result);
    (void)worst_error_class;
    if (bl) blossom_pool_free(bl);
    return rc;
}
