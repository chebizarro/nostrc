/*
 * hanami-nostr.c - Nostr integration layer for NIP-34 events
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-nostr.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-simple-pool.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"
#include "channel.h"
#include "select.h"
#include "error.h"
#include <nip34.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =========================================================================
 * Internal context structure
 * ========================================================================= */

struct hanami_nostr_ctx {
    char **relay_urls;      /* Owned, NULL-terminated */
    size_t relay_count;
    hanami_signer_t signer; /* Shallow copy — caller owns pointed-to data */
    bool has_signer;

    NostrSimplePool *pool;  /* Owns connections, reconnect and subscriptions */
    GoChannel *notice;
    pthread_mutex_t relay_mutex; /* Serialize synchronous operations */
};

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

hanami_error_t hanami_nostr_ctx_new(const char *const *relay_urls,
                                    const hanami_signer_t *signer,
                                    hanami_nostr_ctx_t **out)
{
    if (!relay_urls || !out)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;

    /* Count relay URLs */
    size_t count = 0;
    while (relay_urls[count]) count++;

    if (count == 0)
        return HANAMI_ERR_INVALID_ARG;

    hanami_nostr_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return HANAMI_ERR_NOMEM;

    ctx->relay_urls = calloc(count + 1, sizeof(char *));
    if (!ctx->relay_urls) {
        free(ctx);
        return HANAMI_ERR_NOMEM;
    }

    for (size_t i = 0; i < count; i++) {
        ctx->relay_urls[i] = strdup(relay_urls[i]);
        if (!ctx->relay_urls[i]) {
            /* Cleanup partial */
            for (size_t j = 0; j < i; j++)
                free(ctx->relay_urls[j]);
            free(ctx->relay_urls);
            free(ctx);
            return HANAMI_ERR_NOMEM;
        }
    }
    ctx->relay_count = count;

    if (signer && signer->sign) {
        ctx->signer = *signer;
        ctx->has_signer = true;
    }

    pthread_mutex_init(&ctx->relay_mutex, NULL);

    *out = ctx;
    return HANAMI_OK;
}

void hanami_nostr_ctx_free(hanami_nostr_ctx_t *ctx)
{
    if (!ctx) return;

    nostr_simple_pool_free(ctx->pool);
    if (ctx->notice)
        go_channel_free(ctx->notice);

    /* Free URLs */
    if (ctx->relay_urls) {
        for (size_t i = 0; i < ctx->relay_count; i++)
            free(ctx->relay_urls[i]);
        free(ctx->relay_urls);
    }

    pthread_mutex_destroy(&ctx->relay_mutex);

    free(ctx);
}

/* =========================================================================
 * Lazy relay connection
 * ========================================================================= */

enum { QUERY_DONE = 1, AUTH_REQUIRED = 2 };
#define RELAY_TIMEOUT_MS 10000

/* Hanami has no NIP-42 credential policy. Fail explicitly rather than signing
 * an authentication event without caller approval or ignoring the challenge. */
static void auth_required(NostrRelay *relay, const char *challenge, void *data)
{
    (void)relay;
    (void)challenge;
    hanami_nostr_ctx_t *ctx = data;
    go_channel_try_send(ctx->notice, (void *)(uintptr_t)AUTH_REQUIRED);
}

/* Called with relay_mutex held. Relay creation/dial/backoff belong to libnostr. */
static hanami_error_t ensure_relays(hanami_nostr_ctx_t *ctx)
{
    if (!ctx->pool) {
        ctx->notice = go_channel_create(1);
        if (!ctx->notice)
            return HANAMI_ERR_NOMEM;
        ctx->pool = nostr_simple_pool_new();
        if (!ctx->pool) {
            go_channel_free(ctx->notice);
            ctx->notice = NULL;
            return HANAMI_ERR_NOMEM;
        }
    }

    void *old_notice;
    while (go_channel_try_receive(ctx->notice, &old_notice) == 0) {}
    for (size_t i = 0; i < ctx->relay_count; i++)
        nostr_simple_pool_ensure_relay(ctx->pool, ctx->relay_urls[i]);

    bool connected = false;
    pthread_mutex_lock(&ctx->pool->pool_mutex);
    for (size_t i = 0; i < ctx->pool->relay_count; i++) {
        NostrRelay *relay = ctx->pool->relays[i];
        nostr_relay_set_auth_callback(relay, auth_required, ctx);
        connected |= nostr_relay_is_connected(relay);
    }
    pthread_mutex_unlock(&ctx->pool->pool_mutex);
    return connected ? HANAMI_OK : HANAMI_ERR_NETWORK;
}

/* =========================================================================
 * Internal: sign event via signer callback
 * ========================================================================= */

static hanami_error_t sign_event(hanami_nostr_ctx_t *ctx, NostrEvent *event)
{
    if (!ctx->has_signer)
        return HANAMI_ERR_AUTH;

    /* Set pubkey */
    nostr_event_set_pubkey(event, ctx->signer.pubkey);

    /* Serialize → sign callback → deserialize back */
    char *json = nostr_event_serialize_compact(event);
    if (!json)
        return HANAMI_ERR_NOSTR;

    char *signed_json = NULL;
    hanami_error_t err = ctx->signer.sign(json, &signed_json, ctx->signer.user_data);
    free(json);

    if (err != HANAMI_OK || !signed_json) {
        free(signed_json);
        return (err != HANAMI_OK) ? err : HANAMI_ERR_AUTH;
    }

    /* Deserialize signed event back into the event struct */
    NostrEvent *signed_ev = nostr_event_new();
    if (!signed_ev) {
        free(signed_json);
        return HANAMI_ERR_NOMEM;
    }

    if (nostr_event_deserialize_compact(signed_ev, signed_json, NULL) != 1) {
        nostr_event_free(signed_ev);
        free(signed_json);
        return HANAMI_ERR_NOSTR;
    }
    free(signed_json);

    /* Copy signed fields back to original event */
    if (signed_ev->id) {
        free(event->id);  /* free old id to prevent leak */
        event->id = strdup(signed_ev->id);
    }
    if (signed_ev->sig)
        nostr_event_set_sig(event, signed_ev->sig);
    if (signed_ev->pubkey)
        nostr_event_set_pubkey(event, signed_ev->pubkey);

    nostr_event_free(signed_ev);
    return HANAMI_OK;
}

/* =========================================================================
 * Publishing
 * ========================================================================= */

hanami_error_t hanami_nostr_publish_event(hanami_nostr_ctx_t *ctx,
                                          NostrEvent *event)
{
    if (!ctx || !event)
        return HANAMI_ERR_INVALID_ARG;

    pthread_mutex_lock(&ctx->relay_mutex);
    hanami_error_t err = ensure_relays(ctx);
    bool any_published = false;
    if (err == HANAMI_OK) {
        err = HANAMI_ERR_NETWORK;
        /* The pool owns relays; it has no publish API. Keep only the NIP-34
         * best-effort acceptance policy here, using libnostr's OK waiter. */
        for (size_t i = 0; i < ctx->pool->relay_count; i++) {
            NostrRelay *relay = ctx->pool->relays[i];
            if (!nostr_relay_is_connected(relay))
                continue;
            Error *publish_err = NULL;
            if (nostr_relay_publish_and_wait(relay, event, RELAY_TIMEOUT_MS,
                                             &publish_err)) {
                any_published = true;
            } else if (publish_err && publish_err->message) {
                if (strstr(publish_err->message, "auth-required:"))
                    err = HANAMI_ERR_AUTH;
                else if (strstr(publish_err->message, "relay rejected"))
                    err = HANAMI_ERR_NOSTR;
                else if (strstr(publish_err->message, "timed out"))
                    err = HANAMI_ERR_TIMEOUT;
                else
                    err = HANAMI_ERR_NOSTR;
            }
            free_error(publish_err);
        }
        void *notice = NULL;
        if (go_channel_try_receive(ctx->notice, &notice) == 0 &&
            (uintptr_t)notice == AUTH_REQUIRED)
            err = HANAMI_ERR_AUTH;
    }
    pthread_mutex_unlock(&ctx->relay_mutex);
    return any_published ? HANAMI_OK : err;
}

hanami_error_t hanami_nostr_publish_repo(hanami_nostr_ctx_t *ctx,
                                         const char *repo_id,
                                         const char *name,
                                         const char *desc,
                                         const char *const *clone_urls,
                                         const char *const *web_urls)
{
    if (!ctx || !repo_id || !name)
        return HANAMI_ERR_INVALID_ARG;
    if (!ctx->has_signer)
        return HANAMI_ERR_AUTH;

    /* Build relay URL array from context for the event */
    NostrEvent *ev = nip34_create_repo_announcement(
        repo_id, name, desc, clone_urls, web_urls,
        (const char *const *)ctx->relay_urls, NULL);
    if (!ev)
        return HANAMI_ERR_NOSTR;

    hanami_error_t err = sign_event(ctx, ev);
    if (err != HANAMI_OK) {
        nostr_event_free(ev);
        return err;
    }

    err = hanami_nostr_publish_event(ctx, ev);
    nostr_event_free(ev);
    return err;
}

hanami_error_t hanami_nostr_publish_state(hanami_nostr_ctx_t *ctx,
                                          const char *repo_id,
                                          const nip34_ref_t *refs,
                                          size_t ref_count,
                                          const char *head)
{
    if (!ctx || !repo_id)
        return HANAMI_ERR_INVALID_ARG;
    if (!ctx->has_signer)
        return HANAMI_ERR_AUTH;

    NostrEvent *ev = nip34_create_repo_state(repo_id, refs, ref_count, head);
    if (!ev)
        return HANAMI_ERR_NOSTR;

    hanami_error_t err = sign_event(ctx, ev);
    if (err != HANAMI_OK) {
        nostr_event_free(ev);
        return err;
    }

    err = hanami_nostr_publish_event(ctx, ev);
    nostr_event_free(ev);
    return err;
}

/* =========================================================================
 * Filter construction helpers
 * ========================================================================= */

NostrFilter *hanami_nostr_build_repo_filter(const char *repo_id,
                                            const char *owner_pubkey)
{
    if (!repo_id) return NULL;

    NostrFilter *f = nostr_filter_new();
    if (!f) return NULL;

    nostr_filter_add_kind(f, NIP34_KIND_REPOSITORY);
    nostr_filter_tags_append(f, "d", repo_id, NULL);
    nostr_filter_set_limit(f, 1);

    if (owner_pubkey)
        nostr_filter_add_author(f, owner_pubkey);

    return f;
}

NostrFilter *hanami_nostr_build_state_filter(const char *repo_id,
                                             const char *owner_pubkey)
{
    if (!repo_id) return NULL;

    NostrFilter *f = nostr_filter_new();
    if (!f) return NULL;

    nostr_filter_add_kind(f, NIP34_KIND_REPOSITORY_STATE);
    nostr_filter_tags_append(f, "d", repo_id, NULL);
    nostr_filter_set_limit(f, 1);

    if (owner_pubkey)
        nostr_filter_add_author(f, owner_pubkey);

    return f;
}

NostrFilter *hanami_nostr_build_patches_filter(const char *repo_addr)
{
    if (!repo_addr) return NULL;

    NostrFilter *f = nostr_filter_new();
    if (!f) return NULL;

    nostr_filter_add_kind(f, NIP34_KIND_PATCH);
    nostr_filter_tags_append(f, "a", repo_addr, NULL);

    return f;
}

/* =========================================================================
 * Querying (pool catch-up, completed only by EOSE)
 * ========================================================================= */

typedef struct {
    hanami_nostr_ctx_t *ctx;
    size_t remaining;
    NostrEvent *latest;
    hanami_error_t error;
} repo_query;

static void collect_event(NostrIncomingEvent *incoming, void *data)
{
    repo_query *query = data;
    NostrEvent *event = incoming->event;
    /* The pool assumes validation at ingestion. Unlike nostrdb consumers,
     * Hanami must validate here before admitting a repository state. */
    if (!nostr_event_check_signature(event)) {
        nostr_event_free(event);
        return;
    }
    /* NIP-01 replaceable-event ordering, independent of relay arrival order. */
    if (!query->latest || event->created_at > query->latest->created_at ||
        (event->created_at == query->latest->created_at &&
         strcmp(event->id, query->latest->id) < 0)) {
        nostr_event_free(query->latest);
        query->latest = event;
    } else {
        nostr_event_free(event);
    }
}

static void query_boundary(NostrRelay *relay, const char *state,
                           const char *reason, void *data)
{
    (void)relay;
    repo_query *query = data;
    if (strcmp(state, "CLOSED") == 0) {
        query->error = reason && strncmp(reason, "auth-required:", 14) == 0
                     ? HANAMI_ERR_AUTH : HANAMI_ERR_NOSTR;
    } else if (strcmp(state, "EOSE") != 0) {
        return;
    }
    if (query->error != HANAMI_OK || --query->remaining == 0)
        go_channel_try_send(query->ctx->notice, (void *)(uintptr_t)QUERY_DONE);
}

static hanami_error_t query_single_event(hanami_nostr_ctx_t *ctx,
                                         NostrFilter *filter, NostrEvent **out)
{
    *out = NULL;
    pthread_mutex_lock(&ctx->relay_mutex);
    hanami_error_t err = ensure_relays(ctx);
    if (err != HANAMI_OK)
        goto done;

    repo_query query = { .ctx = ctx, .error = HANAMI_OK };
    NostrFilters filters = { .filters = filter, .count = 1 };
    nostr_simple_pool_set_auto_unsub_on_eose(ctx->pool, true);
    nostr_simple_pool_set_event_middleware_ex(ctx->pool, collect_event, &query);
    nostr_simple_pool_set_subscription_callback(ctx->pool, query_boundary, &query);
    nostr_simple_pool_subscribe(ctx->pool, (const char **)ctx->relay_urls,
                               ctx->relay_count, filters, false);
    /* Each fetch is a fresh snapshot. Pool-wide seen IDs persist across
     * subscriptions, so replay reduction belongs to collect_event here. */
    query.remaining = ctx->pool->subs_count;
    if (query.remaining == 0) {
        err = HANAMI_ERR_NETWORK;
    } else {
        nostr_simple_pool_start(ctx->pool);
        void *notice = NULL;
        GoSelectCase wait = { .op = GO_SELECT_RECEIVE, .chan = ctx->notice,
                              .recv_buf = &notice };
        GoSelectResult result = go_select_timeout(&wait, 1, RELAY_TIMEOUT_MS);
        /* EOSE is notified before the pool delivers its final event batch.
         * Join before reading latest or releasing callback data. */
        nostr_simple_pool_stop(ctx->pool);
        err = result.selected_case != 0 ? HANAMI_ERR_TIMEOUT
            : (uintptr_t)notice == AUTH_REQUIRED ? HANAMI_ERR_AUTH : query.error;
    }
    nostr_simple_pool_set_subscription_callback(ctx->pool, NULL, NULL);
    nostr_simple_pool_set_event_middleware_ex(ctx->pool, NULL, NULL);
    if (err == HANAMI_OK) {
        *out = query.latest;
        if (!*out)
            err = HANAMI_ERR_NOT_FOUND;
    } else {
        nostr_event_free(query.latest);
    }
done:
    nostr_filter_free(filter);
    pthread_mutex_unlock(&ctx->relay_mutex);
    return err;
}

hanami_error_t hanami_nostr_fetch_repo(hanami_nostr_ctx_t *ctx,
                                       const char *repo_id,
                                       const char *owner_pubkey,
                                       nip34_repository_t **out)
{
    if (!ctx || !repo_id || !owner_pubkey || !out)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;

    NostrFilter *f = hanami_nostr_build_repo_filter(repo_id, owner_pubkey);
    if (!f)
        return HANAMI_ERR_NOMEM;

    NostrEvent *ev = NULL;
    hanami_error_t err = query_single_event(ctx, f, &ev);
    if (err != HANAMI_OK)
        return err;

    nip34_result_t res = nip34_parse_repository(ev, out);
    nostr_event_free(ev);

    return (res == NIP34_OK) ? HANAMI_OK : HANAMI_ERR_NOSTR;
}

hanami_error_t hanami_nostr_fetch_state(hanami_nostr_ctx_t *ctx,
                                        const char *repo_id,
                                        const char *owner_pubkey,
                                        nip34_repo_state_t **out)
{
    if (!ctx || !repo_id || !owner_pubkey || !out)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;

    NostrFilter *f = hanami_nostr_build_state_filter(repo_id, owner_pubkey);
    if (!f)
        return HANAMI_ERR_NOMEM;

    NostrEvent *ev = NULL;
    hanami_error_t err = query_single_event(ctx, f, &ev);
    if (err != HANAMI_OK)
        return err;

    nip34_result_t res = nip34_parse_repo_state(ev, out);
    nostr_event_free(ev);

    return (res == NIP34_OK) ? HANAMI_OK : HANAMI_ERR_NOSTR;
}
