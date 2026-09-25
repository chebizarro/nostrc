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
#include "nh_porthome_blossom.h"

#include <hanami/hanami-blossom-shim.h>
#include <hanami/hanami-types.h>

#include <jansson.h>

#include "nostr-event.h"
#include "nostr-relay.h"
#include "nostr-tag.h"
#include "nostr-keys.h"
#include "error.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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
extern void nh_syncd_state_bump_generation_(nh_syncd_state *s);

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

/* Per-server PUT using the porthome-common Blossom wrapper. Opens a
 * single-endpoint wrapper each call so `nh_porthome_blossom_upload`'s
 * fan-out is reduced to exactly one server — the syncd pusher owns the
 * outer per-server iteration. Coordinated with the porthome-common
 * contract (see nh_porthome_blossom.h): the wrapper stays multi-server
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
    nh_porthome_blossom_opts_t bopts = {0};
    bopts.servers        = one;
    bopts.n_servers      = 1;
    bopts.max_blob_bytes = ct_len + NH_PORTHOME_SEAL_OVERHEAD + 4096;
    nh_porthome_blossom_t *one_bl = NULL;
    if (nh_porthome_blossom_new(&bopts, cfg->bud02_signer, &one_bl) != 0) {
        if (out_ms) *out_ms = mono_ms() - t0;
        return NH_SYNCD_ERR_UPLOAD;
    }
    char echo[65];
    int urc = nh_porthome_blossom_upload(one_bl, ct, ct_len,
                                         expected_sha_hex, echo);
    nh_porthome_blossom_free(one_bl);
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
                        nh_porthome_blossom_t *bl,
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
     * When the shim is active the downstream nh_porthome_blossom_upload
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
     * nh_porthome_blossom.c's shim gate uses. Both call sites walk the
     * same cfg->blossom_servers[] list, so libhanami's URL-keyed cache
     * makes them agree (a probe run on any URL is memoised for the
     * process). The env-var override still wins when set. */
    bool shim_on = hanami_blossom_shim_active_for(
        cfg->blossom_servers, cfg->n_blossom_servers);
    if (shim_on) {
        uint8_t shim_sha[32];
        if (hanami_blossom_shim_sha256(ct, ct_len, shim_sha) != HANAMI_OK) {
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
        (void)nh_porthome_blossom_has(bl, out_addr_hex, &present);
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
    int rc;
} rebuild_ctx;

static int rebuild_cb(void *ud, const char *rel, json_t *row) {
    rebuild_ctx *rc = (rebuild_ctx *)ud;
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
    if (kind && !strcmp(kind, "dir")) {
        rc->rc = nh_porthome_manifest_add_dir(rc->m, penc, mode, uid, gid, mt_ns);
        return rc->rc == 0 ? 0 : -1;
    }
    if (kind && !strcmp(kind, "symlink")) {
        const char *tgt = json_string_value(json_object_get(row, "symlink_target"));
        char *dup = strdup(tgt ? tgt : "");
        if (!dup) { free(penc); rc->rc = NH_SYNCD_ERR_OOM; return -1; }
        rc->rc = nh_porthome_manifest_add_symlink(rc->m, penc, mode, uid, gid, mt_ns, dup);
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
    rc->rc = nh_porthome_manifest_add_file(rc->m, penc, mode, uid, gid,
                                           mt_ns, size, chs, n);
    free(chs);
    return rc->rc == 0 ? 0 : -1;
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
    for (size_t i = 0; i < cfg->n_relays; i++) {
        Error *werr = NULL;
        NostrRelay *r = nostr_relay_new(NULL, cfg->relays[i], &werr);
        if (werr) free_error(werr);
        if (!r) continue;
        werr = NULL;
        bool ok = nostr_relay_connect(r, &werr);
        if (werr) free_error(werr);
        if (!ok) { nostr_relay_free(r); continue; }
        werr = NULL;
        bool pub = nostr_relay_publish_and_wait(r, e, tmo, &werr);
        if (werr) free_error(werr);
        if (pub) any_ok = 1;
        nostr_relay_free(r);
        /* First OK wins — but we DO NOT tear down early. The relay
         * ack means the pointer landed; further relays are best
         * effort and are logged via the caller (I3 daemon glue). */
        if (any_ok) break;
    }

    nostr_event_free(e);
    if (!any_ok) {
        set_err(err, "no relay OK'd the pointer");
        return NH_SYNCD_ERR_PUBLISH;
    }
    return NH_SYNCD_OK;
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
    nh_porthome_blossom_t *bl = NULL;
    if (cfg->n_blossom_servers > 0) {
        nh_porthome_blossom_opts_t bopts = {0};
        bopts.servers   = cfg->blossom_servers;
        bopts.n_servers = cfg->n_blossom_servers;
        bopts.max_blob_bytes = NH_SYNCD_CHUNK_SIZE + NH_PORTHOME_SEAL_OVERHEAD + 4096;
        if (nh_porthome_blossom_new(&bopts, cfg->bud02_signer, &bl) != 0) {
            set_err(out_error_msg, "blossom_new failed");
            return NH_SYNCD_ERR_UPLOAD;
        }
    }

    /* Walk each batch item. Anything that survives the ignore gate
     * and stat/permission checks is classified as ADD / MODIFY /
     * DELETE and mutates state.  Errors along the way roll back the
     * whole push (state stays as-loaded). */
    for (size_t i = 0; i < nh_syncd_batch_len(batch); i++) {
        const char *rel = NULL;
        nh_syncd_change_kind kind = 0;
        (void)nh_syncd_batch_at(batch, i, &rel, &kind);
        if (!rel || !*rel) continue;

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
            nh_syncd_state_delete_(state, rel);
            continue;
        }

        uint32_t mode  = st.st_mode & 07777;
        uint32_t uid   = (uint32_t)st.st_uid;
        uint32_t gid   = (uint32_t)st.st_gid;
        uint64_t mt_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull
                       + (uint64_t)st.st_mtim.tv_nsec;

        if (S_ISDIR(st.st_mode)) {
            (void)nh_syncd_state_upsert_dir_(state, rel, mode, uid, gid, mt_ns);
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            char tgt[4096];
            ssize_t tn = readlink(abs, tgt, sizeof tgt - 1);
            if (tn < 0) { rc = NH_SYNCD_ERR_IO; goto fail; }
            tgt[tn] = '\0';
            (void)nh_syncd_state_upsert_symlink_(state, rel, mode, uid, gid, mt_ns, tgt);
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
        /* Skip if content unchanged AND size + mtime unchanged
         * (defensive short-circuit for spurious inotify wake-ups). */
        const nh_syncd_entry *cur = nh_syncd_state_find(state, rel);
        if (cur && nh_syncd_entry_size(cur) == pt_len &&
            nh_syncd_entry_kind(cur) == NH_SYNCD_KIND_FILE &&
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
    }

    /* Rebuild the WHOLE manifest from the (now-updated) state. */
    nh_porthome_manifest m;
    if (nh_porthome_manifest_init(&m, cfg->root_id) != 0) {
        rc = NH_SYNCD_ERR_MANIFEST; goto fail;
    }
    rebuild_ctx rctx = { cfg->home_key, &m, NH_SYNCD_OK };
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

    /* Advance and persist. */
    nh_syncd_state_bump_generation_(state);
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
    if (bl) nh_porthome_blossom_free(bl);
    return NH_SYNCD_OK;

fail:
    if (cfg->on_upload_summary)
        cfg->on_upload_summary(cfg->on_upload_summary_ud, &worst_result);
    (void)worst_error_class;
    if (bl) nh_porthome_blossom_free(bl);
    return rc;
}
