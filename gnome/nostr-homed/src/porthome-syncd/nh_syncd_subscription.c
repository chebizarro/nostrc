/*
 * nh_syncd_subscription.c — long-lived REQ on kind-30078 (§6.2 §6.3).
 *
 * SPDX-License-Identifier: MIT
 *
 * The I1 subscription is deliberately minimal: dial the pool, ensure
 * every relay, install an EVENT middleware that forwards the event
 * facts (id, created_at, d-tag, content-hex) to a caller-supplied
 * callback. It NEVER polls or sleeps; the SimplePool's own
 * background threads dispatch events as they arrive.
 *
 * I2 (pull path) replaces the default callback with a real decoder
 * that decrypts the pointer content and drives the reconciler. I1
 * ships `nh_syncd_default_remote_pointer_recorder` which persists
 * the observed `created_at` to `${state_dir}/remote.json` so I2 can
 * bootstrap its "last seen" cursor on cold start.
 *
 * The subscription auto-unsub-on-eose is disabled: this is a live
 * feed, not a one-shot query. The pool tears down on _stop().
 */

#include "nh_syncd.h"

#include "nostr-simple-pool.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr-tag.h"

#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    nh_syncd_on_remote_pointer_fn cb;
    void *cb_ud;
} sub_ctx;

struct nh_syncd_subscription {
    NostrSimplePool *pool;
    NostrFilters    *filters; /* kept alive for the pool's lifetime */
    sub_ctx          ctx;
};

static const char *find_d_tag(NostrEvent *e) {
    if (!e) return NULL;
    void *tags = nostr_event_get_tags(e);
    if (!tags) return NULL;
    NostrTags *t = (NostrTags *)tags;
    size_t n = nostr_tags_size(t);
    for (size_t i = 0; i < n; i++) {
        NostrTag *tg = nostr_tags_get(t, i);
        if (!tg) continue;
        size_t sz = nostr_tag_size(tg);
        if (sz < 2) continue;
        const char *k = nostr_tag_get_key(tg);
        if (k && !strcmp(k, "d"))
            return nostr_tag_get(tg, 1);
    }
    return NULL;
}

static void middleware(NostrIncomingEvent *ie, void *ud) {
    if (!ie || !ie->event || !ud) return;
    sub_ctx *c = (sub_ctx *)ud;
    NostrEvent *e = ie->event;
    if (!c->cb) return;
    const char *id  = nostr_event_get_id(e);
    int64_t     cat = nostr_event_get_created_at(e);
    const char *ct  = nostr_event_get_content(e);
    const char *dt  = find_d_tag(e);
    c->cb(c->cb_ud, id, cat, dt, ct);
}

int nh_syncd_subscription_start(const char *const *relays, size_t n_relays,
                                const char *account_pubkey_hex,
                                const char *d_tag,
                                nh_syncd_on_remote_pointer_fn cb,
                                void *cb_ud,
                                nh_syncd_subscription **out)
{
    if (!relays || n_relays == 0 || !account_pubkey_hex || !d_tag || !cb || !out)
        return NH_SYNCD_ERR_ARG;

    NostrSimplePool *pool = nostr_simple_pool_new();
    if (!pool) return NH_SYNCD_ERR_OOM;
    /* This is a LIVE feed. Do not auto-unsub on EOSE. */
    nostr_simple_pool_set_auto_unsub_on_eose(pool, false);

    nh_syncd_subscription *s = calloc(1, sizeof *s);
    if (!s) { nostr_simple_pool_stop(pool); return NH_SYNCD_ERR_OOM; }
    s->pool = pool;
    s->ctx.cb = cb;
    s->ctx.cb_ud = cb_ud;
    nostr_simple_pool_set_event_middleware_ex(pool, middleware, &s->ctx);
    nostr_simple_pool_start(pool);

    for (size_t i = 0; i < n_relays; i++)
        nostr_simple_pool_ensure_relay(pool, relays[i]);

    NostrFilter *f = nostr_filter_new();
    nostr_filter_add_kind(f, 30078);
    nostr_filter_add_author(f, account_pubkey_hex);
    nostr_filter_tags_append(f, "d", d_tag, NULL);
    NostrFilters *fs = nostr_filters_new();
    if (!fs || !nostr_filters_add(fs, f)) {
        if (fs) nostr_filters_free(fs);
        nostr_filter_free(f);
        nostr_simple_pool_stop(pool);
        free(s);
        return NH_SYNCD_ERR_OOM;
    }
    /* nostr_filters_add moved the filter contents; free the (zeroed) shell. */
    nostr_filter_free(f);
    /* No limit: we want backfill via EOSE and then live updates.
     * subscribe_async takes filters BY VALUE (a shallow copy). The
     * copy references the same internal array we allocated, so we
     * must keep fs alive for the pool's lifetime and free it in stop. */
    nostr_simple_pool_subscribe_async(pool, (const char **)relays, n_relays, *fs, true);
    s->filters = fs;

    *out = s;
    return NH_SYNCD_OK;
}

void nh_syncd_subscription_stop(nh_syncd_subscription *s) {
    if (!s) return;
    if (s->pool) nostr_simple_pool_stop(s->pool);
    /* Now that the pool no longer references our filters, tear them down. */
    if (s->filters) nostr_filters_free(s->filters);
    free(s);
}

/* Default recorder — used until I2 replaces it. Stores the max
 * created_at in a small JSON at ${state_dir}/remote.json (path is
 * fetched from the ud, which is a heap-owned char* set up by the
 * daemon at start-up). */
typedef struct {
    char    *state_dir;
    int64_t  max_seen;
    pthread_mutex_t mu;
} default_recorder_ctx;

static void write_remote_json(default_recorder_ctx *c) {
    char *tmp = NULL, *fin = NULL;
    if (asprintf(&tmp, "%s/remote.json.tmp", c->state_dir) < 0) return;
    if (asprintf(&fin, "%s/remote.json", c->state_dir) < 0) { free(tmp); return; }
    FILE *f = fopen(tmp, "w");
    if (f) {
        fprintf(f, "{\"remote_generation\":%lld}\n", (long long)c->max_seen);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
        rename(tmp, fin);
    }
    free(tmp);
    free(fin);
}

void nh_syncd_default_remote_pointer_recorder(void *ud,
                                              const char *event_id_hex,
                                              int64_t     created_at,
                                              const char *d_tag,
                                              const char *content_hex_or_b64)
{
    (void)event_id_hex; (void)d_tag; (void)content_hex_or_b64;
    default_recorder_ctx *c = (default_recorder_ctx *)ud;
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    if (created_at > c->max_seen) {
        c->max_seen = created_at;
        write_remote_json(c);
    }
    pthread_mutex_unlock(&c->mu);
}
