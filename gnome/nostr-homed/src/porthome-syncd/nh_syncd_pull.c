/*
 * nh_syncd_pull.c — glue between the I1 subscription seam and the I2
 * three-way reconciler (design §6.3).
 *
 * SPDX-License-Identifier: MIT
 *
 * The subscription verifies kind, author, d-tag and signature before
 * delivering a pointer on the daemon's state-owning thread. This callback
 * decodes the sealed manifest and reconciles the latest addressable pointer.
 * It returns success only after reconciliation and persistence succeed, so
 * replay deduplication does not acknowledge a failed fetch. Equal-timestamp
 * pointers use NIP-01 event-ID ordering; the winning ID is persisted with
 * the snapshot cursor.
 */

#define _GNU_SOURCE

#include "nh_syncd.h"
/* Do NOT pull in nh_syncd_cache.h here: it redefines nh_syncd_notify_fn
 * with a different signature (a design wart, kept for source-compat).
 * We only need one function + one status constant — declare locally. */
struct nh_syncd_pin_ring;
#define NH_SYNCD_CACHE_OK_LOCAL 0
extern int nh_syncd_pin_ring_promote_from_snapshot(struct nh_syncd_pin_ring *r,
                                                   const char *state_dir);
#include "nh_porthome_crypto.h"
#include "nh_porthome_manifest.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct nh_syncd_pull_ctx {
    nh_syncd_pull_opts opts;
    pthread_mutex_t    mu;
    uint64_t           last_applied_generation;
    _Atomic size_t     total_conflicts;
    _Atomic size_t     total_events;
};

int nh_syncd_pull_ctx_new(const nh_syncd_pull_opts *opts, nh_syncd_pull_ctx **out) {
    if (!opts || !opts->state || !opts->root_dir || !opts->account_pubkey_hex ||
        !opts->d_tag || !out)
        return NH_SYNCD_ERR_ARG;
    nh_syncd_pull_ctx *c = calloc(1, sizeof *c);
    if (!c) return NH_SYNCD_ERR_OOM;
    c->opts = *opts;
    memcpy(c->opts.home_key, opts->home_key, NH_PORTHOME_KEY_LEN);
    pthread_mutex_init(&c->mu, NULL);
    *out = c;
    return NH_SYNCD_OK;
}

void nh_syncd_pull_ctx_free(nh_syncd_pull_ctx *c) {
    if (!c) return;
    /* Best-effort key wipe (defensive; the caller may re-use the opts
     * struct after us). */
    memset(c->opts.home_key, 0, sizeof c->opts.home_key);
    pthread_mutex_destroy(&c->mu);
    free(c);
}

uint64_t nh_syncd_pull_ctx_last_applied_generation(const nh_syncd_pull_ctx *c) {
    return c ? c->last_applied_generation : 0;
}
size_t nh_syncd_pull_ctx_total_conflicts(const nh_syncd_pull_ctx *c) {
    return c ? atomic_load(&c->total_conflicts) : 0;
}
size_t nh_syncd_pull_ctx_total_events(const nh_syncd_pull_ctx *c) {
    return c ? atomic_load(&c->total_events) : 0;
}

/* Hex-decode `hex` (must be lowercase, even length). Returns 0 on
 * success; caller frees. */
static int hex_decode(const char *hex, uint8_t **out, size_t *out_len) {
    if (!hex) return -1;
    size_t n = strlen(hex);
    if (n % 2u) return -1;
    for (size_t i = 0; i < n; i++) {
        char ch = hex[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return -1;
    }
    size_t bn = n / 2u;
    uint8_t *buf = malloc(bn ? bn : 1);
    if (!buf) return -1;
    for (size_t i = 0; i < bn; i++) {
        unsigned h = (unsigned)hex[2 * i], l = (unsigned)hex[2 * i + 1];
        unsigned dh = h <= '9' ? h - '0' : h - 'a' + 10;
        unsigned dl = l <= '9' ? l - '0' : l - 'a' + 10;
        buf[i] = (uint8_t)((dh << 4) | dl);
    }
    *out = buf; *out_len = bn;
    return 0;
}

extern const char *nh_syncd_state_pointer_event_id_(const nh_syncd_state *s);
extern void nh_syncd_state_set_pointer_event_id_(nh_syncd_state *s, const char *id);

int nh_syncd_pull_on_pointer(void *ud,
                              const char *event_id_hex,
                              int64_t     created_at,
                              const char *d_tag,
                              const char *content_hex_or_b64)
{
    nh_syncd_pull_ctx *c = (nh_syncd_pull_ctx *)ud;
    if (!c || !event_id_hex) return NH_SYNCD_ERR_ARG;
    atomic_fetch_add(&c->total_events, 1);

    /* d-tag check (belt-and-braces — the subscription filter also has
     * this, but a hostile pool implementation could still hand us
     * something else). */
    if (!d_tag || !c->opts.d_tag || strcmp(d_tag, c->opts.d_tag) != 0)
        return NH_SYNCD_ERR_ARG;

    /* Content is expected to be the AEAD-sealed CBOR bytes rendered as
     * lowercase hex (matches operator publisher + fetch helper). */
    uint8_t *sealed = NULL; size_t sealed_len = 0;
    if (hex_decode(content_hex_or_b64, &sealed, &sealed_len) != 0)
        return NH_SYNCD_ERR_MANIFEST;

    nh_porthome_manifest *m = NULL;
    if (nh_porthome_manifest_decode_sealed(sealed, sealed_len, c->opts.home_key, &m) != 0) {
        free(sealed);
        return NH_SYNCD_ERR_MANIFEST;
    }
    free(sealed);
    if (!m) return NH_SYNCD_ERR_MANIFEST;

    /* Generation ordering — design §2.2 uses event.created_at as the
     * monotone generation stamp for the pointer. */
    pthread_mutex_lock(&c->mu);
    uint64_t remote_gen = (uint64_t)(created_at > 0 ? created_at : 0);
    uint64_t local_gen  = nh_syncd_state_get_local_generation(c->opts.state);
    const char *current_id = nh_syncd_state_pointer_event_id_(c->opts.state);
    if (remote_gen < local_gen ||
        (remote_gen == local_gen && current_id[0] &&
         strcmp(event_id_hex, current_id) <= 0)) {
        pthread_mutex_unlock(&c->mu);
        nh_porthome_manifest_dispose(m); free(m);
        return NH_SYNCD_OK;
    }
    char previous_id[65];
    snprintf(previous_id, sizeof previous_id, "%s", current_id);
    nh_syncd_state_set_pointer_event_id_(c->opts.state, event_id_hex);

    nh_syncd_reconcile_cfg rc = {0};
    rc.manifest           = m;
    rc.remote_generation  = remote_gen;
    rc.lazy               = c->opts.lazy;
    memcpy(rc.home_key, c->opts.home_key, NH_PORTHOME_KEY_LEN);
    rc.fetch_chunk        = c->opts.fetch_chunk;
    rc.fetch_chunk_ctx    = c->opts.fetch_chunk_ctx;
    rc.device_name        = c->opts.device_name;
    rc.push_queue         = c->opts.push_queue;
    rc.notify             = c->opts.notify;
    rc.notify_ud          = c->opts.notify_ud;

    nh_syncd_reconcile_result *res = NULL;
    char *emsg = NULL;
    int rrc = nh_syncd_reconcile_from_manifest(&rc, c->opts.state,
                                               c->opts.root_dir,
                                               c->opts.ignore,
                                               c->opts.state_dir_for_persist,
                                               &res, &emsg);
    if (rrc == NH_SYNCD_OK && res) {
        c->last_applied_generation = remote_gen;
        atomic_fetch_add(&c->total_conflicts,
                         nh_syncd_reconcile_result_conflicts(res));
        /* W(1)(b): promote from snapshot on the post-reconcile state.
         * The reconciler has already persisted snapshot.json via
         * state_dir_for_persist, so promote_from_snapshot reads the
         * fresh generation + blob set. Warn-only on failure. */
        if (c->opts.pin_ring && c->opts.state_dir_for_persist) {
            int prc = nh_syncd_pin_ring_promote_from_snapshot(
                (struct nh_syncd_pin_ring *)c->opts.pin_ring,
                c->opts.state_dir_for_persist);
            if (prc != NH_SYNCD_CACHE_OK_LOCAL)
                fprintf(stderr, "syncd/pull: pin_ring promote rc=%d\n", prc);
        }
    } else {
        nh_syncd_state_set_pointer_event_id_(c->opts.state, previous_id);
        fprintf(stderr, "syncd/pull: reconcile rc=%d msg=%s\n", rrc,
                emsg ? emsg : "unknown error");
    }
    pthread_mutex_unlock(&c->mu);

    if (res) nh_syncd_reconcile_result_free(res);
    free(emsg);
    nh_porthome_manifest_dispose(m);
    free(m);
    return rrc == NH_SYNCD_OK && res ? NH_SYNCD_OK : (rrc ? rrc : NH_SYNCD_ERR_IO);
}
