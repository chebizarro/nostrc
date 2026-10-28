/*
 * hanami-nostr.c - Nostr integration layer for NIP-34 events
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-nostr.h"
#include "hanami-sealed.h"
#include "hanami-discovery.h"
#include "hanami-nostr-internal.h"
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
#include <stdatomic.h>

/* =========================================================================
 * Internal context structure
 * ========================================================================= */

typedef struct {
    NostrRelay *relay; /* Borrowed from the context's pool. */
    uint64_t generation;
} authenticated_relay;

struct hanami_nostr_ctx {
    char **relay_urls;      /* Owned, NULL-terminated */
    size_t relay_count;
    hanami_signer_t signer; /* Shallow copy — caller owns pointed-to data */
    bool has_signer;

    NostrSimplePool *pool;  /* Owns connections, reconnect and subscriptions */
    GoChannel *notice;
    pthread_mutex_t relay_mutex; /* Serialize synchronous operations */
    pthread_mutex_t state_mutex; /* Serialize signed state timestamps + publish */
    int64_t last_state_time;
    authenticated_relay *authenticated; /* relay_count slots; under relay_mutex */
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
    pthread_mutex_init(&ctx->state_mutex, NULL);
    ctx->authenticated = calloc(count, sizeof(*ctx->authenticated));
    if (!ctx->authenticated) { hanami_nostr_ctx_free(ctx); return HANAMI_ERR_NOMEM; }

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
    pthread_mutex_destroy(&ctx->state_mutex);
    free(ctx->authenticated);

    free(ctx);
}

/* =========================================================================
 * Lazy relay connection
 * ========================================================================= */

enum { QUERY_DONE = 1, AUTH_REQUIRED = 2 };
#define RELAY_TIMEOUT_MS 10000

/* Reader callbacks only wake the caller. The retained connection challenge
 * is read and signed on the operation thread, never on the relay reader. */
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

static hanami_error_t relay_error(NostrRelay *relay, const Error *err)
{
    if (!nostr_relay_is_connected(relay)) return HANAMI_ERR_NETWORK;
    const char *message = err ? err->message : NULL;
    if (message && (strstr(message, "auth-required:") || strstr(message, "restricted:")))
        return HANAMI_ERR_AUTH;
    if (message && strstr(message, "timed out")) return HANAMI_ERR_TIMEOUT;
    return HANAMI_ERR_NOSTR;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

/* One authentication attempt, including its own OK. Caller holds relay_mutex.
 * A signer supplied to a context authorizes NIP-42 on its configured relays. */
static hanami_error_t authenticate_relay(hanami_nostr_ctx_t *ctx, NostrRelay *relay)
{
    if (!ctx->has_signer) return HANAMI_ERR_AUTH;
    uint64_t deadline = monotonic_ms() + RELAY_TIMEOUT_MS;
    char *challenge = NULL;
    uint64_t generation = 0, completed = 0;
    for (size_t i = 0; i < ctx->relay_count; i++)
        if (ctx->authenticated[i].relay == relay) completed = ctx->authenticated[i].generation;
    for (;;) {
        challenge = nostr_relay_get_auth_challenge(relay, &generation);
        if (challenge && generation != completed) break;
        free(challenge);
        challenge = NULL;
        /* A renewed refusal can precede its new AUTH frame. Do not sign the
         * consumed challenge while that newer frame is still being delivered. */
        uint64_t now = monotonic_ms();
        if (now >= deadline) return HANAMI_ERR_TIMEOUT;
        void *notice = NULL;
        GoSelectCase wait = { .op = GO_SELECT_RECEIVE, .chan = ctx->notice, .recv_buf = &notice };
        if (go_select_timeout(&wait, 1, deadline - now).selected_case != 0)
            return HANAMI_ERR_TIMEOUT;
        if (!nostr_relay_is_connected(relay)) return HANAMI_ERR_NETWORK;
    }
    NostrEvent *auth = nostr_event_new();
    if (!auth) { free(challenge); return HANAMI_ERR_NOMEM; }
    auth->kind = NOSTR_KIND_CLIENT_AUTHENTICATION;
    auth->created_at = time(NULL);
    nostr_event_set_content(auth, "");
    nostr_tags_free(auth->tags);
    auth->tags = nostr_tags_new(2, nostr_tag_new("relay", relay->url, NULL),
                               nostr_tag_new("challenge", challenge, NULL));
    free(challenge);
    hanami_error_t err = sign_event(ctx, auth);
    if (err == HANAMI_OK && (!nostr_event_check_signature(auth) ||
        strcmp(nostr_event_get_pubkey(auth), ctx->signer.pubkey)))
        err = HANAMI_ERR_AUTH;
    if (err == HANAMI_OK) {
        Error *failure = NULL;
        if (!nostr_relay_publish_and_wait(relay, auth, RELAY_TIMEOUT_MS, &failure)) {
            err = relay_error(relay, failure);
            if (err == HANAMI_ERR_NOSTR) err = HANAMI_ERR_AUTH;
        }
        free_error(failure);
    }
    if (err == HANAMI_OK) {
        for (size_t i = 0; i < ctx->relay_count; i++) {
            if (!ctx->authenticated[i].relay || ctx->authenticated[i].relay == relay) {
                ctx->authenticated[i] = (authenticated_relay){relay, generation};
                break;
            }
        }
    }
    nostr_event_free(auth);
    return err;
}

static bool has_pending_challenge(hanami_nostr_ctx_t *ctx, NostrRelay *relay)
{
    uint64_t generation = 0;
    char *challenge = nostr_relay_get_auth_challenge(relay, &generation);
    bool pending = challenge != NULL;
    free(challenge);
    for (size_t i = 0; pending && i < ctx->relay_count; i++)
        if (ctx->authenticated[i].relay == relay &&
            ctx->authenticated[i].generation == generation) pending = false;
    return pending;
}

hanami_error_t hanami_nostr_publish_event(hanami_nostr_ctx_t *ctx,
                                          NostrEvent *event)
{
    if (!ctx || !event) return HANAMI_ERR_INVALID_ARG;
    pthread_mutex_lock(&ctx->relay_mutex);
    hanami_error_t err = ensure_relays(ctx);
    bool any_published = false;
    if (err == HANAMI_OK) {
        err = HANAMI_ERR_NETWORK;
        /* Generic publication: at least one application-event OK true.
         * GRASP uses a root-only context to make that relay mandatory. */
        for (size_t i = 0; i < ctx->pool->relay_count; i++) {
            NostrRelay *relay = ctx->pool->relays[i];
            if (!nostr_relay_is_connected(relay)) continue;
            bool authenticated = false;
            if (has_pending_challenge(ctx, relay)) {
                err = authenticate_relay(ctx, relay);
                authenticated = true;
                if (err != HANAMI_OK) continue;
            }
            Error *failure = NULL;
            bool accepted = nostr_relay_publish_and_wait(relay, event, RELAY_TIMEOUT_MS, &failure);
            bool auth_refusal = failure && failure->message &&
                                strstr(failure->message, "auth-required:");
            if (!accepted && !authenticated &&
                (auth_refusal || has_pending_challenge(ctx, relay))) {
                free_error(failure); failure = NULL;
                err = authenticate_relay(ctx, relay);
                if (err != HANAMI_OK) continue;
                /* Retry exactly the same signed event, never count AUTH's OK. */
                accepted = nostr_relay_publish_and_wait(relay, event, RELAY_TIMEOUT_MS, &failure);
            }
            if (accepted) any_published = true;
            else err = relay_error(relay, failure);
            free_error(failure);
        }
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

    pthread_mutex_lock(&ctx->state_mutex);
    NostrEvent *ev = NULL;
    hanami_error_t err = ctx->last_state_time == INT64_MAX ? HANAMI_ERR_NOSTR
        : hanami_nostr_create_state(ctx, repo_id, refs,
            ref_count, head, ctx->last_state_time + 1, &ev);
    if (err == HANAMI_OK) {
        /* Reserve even on lost OK: that event might already be stored. */
        ctx->last_state_time = ev->created_at;
        err = hanami_nostr_publish_event(ctx, ev);
    }
    nostr_event_free(ev);
    pthread_mutex_unlock(&ctx->state_mutex);
    return err;
}

hanami_error_t hanami_nostr_create_state(hanami_nostr_ctx_t *ctx,
    const char *repo_id, const nip34_ref_t *refs, size_t count,
    const char *head, int64_t minimum_time, NostrEvent **out)
{
    *out = NULL;
    NostrEvent *ev = nip34_create_repo_state(repo_id, refs, count, head);
    if (!ev) return HANAMI_ERR_NOMEM;
    if (ev->created_at < minimum_time) ev->created_at = minimum_time;
    hanami_error_t err = sign_event(ctx, ev);
    if (err == HANAMI_OK && (!nostr_event_check_signature(ev) ||
        !ctx->signer.pubkey || !nostr_event_get_pubkey(ev) ||
        strcmp(nostr_event_get_pubkey(ev), ctx->signer.pubkey)))
        err = HANAMI_ERR_AUTH;
    if (err == HANAMI_OK) *out = ev;
    else nostr_event_free(ev);
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
    NostrRelay *auth_relay;
    atomic_bool done;
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
    repo_query *query = data;
    if (strcmp(state, "CLOSED") == 0) {
        bool needs_auth = reason && strncmp(reason, "auth-required:", 14) == 0;
        query->error = needs_auth ? HANAMI_ERR_AUTH : HANAMI_ERR_NOSTR;
        if (needs_auth) query->auth_relay = relay;
    } else if (strcmp(state, "EOSE") != 0) {
        return;
    }
    if (query->error != HANAMI_OK || --query->remaining == 0) {
        atomic_store(&query->done, true);
        go_channel_try_send(query->ctx->notice, (void *)(uintptr_t)QUERY_DONE);
    }
}

static hanami_error_t query_single_event(hanami_nostr_ctx_t *ctx,
                                         NostrFilter *filter, NostrEvent **out)
{
    *out = NULL;
    pthread_mutex_lock(&ctx->relay_mutex);
    hanami_error_t err = ensure_relays(ctx);
    if (err != HANAMI_OK)
        goto done;

    size_t relay_count = ctx->pool->relay_count;
    bool *authenticated = calloc(relay_count, sizeof(*authenticated));
    if (!authenticated) { err = HANAMI_ERR_NOMEM; goto done; }
    for (size_t i = 0; i < relay_count; i++) {
        NostrRelay *relay = ctx->pool->relays[i];
        if (!has_pending_challenge(ctx, relay)) continue;
        err = authenticate_relay(ctx, relay);
        authenticated[i] = true;
        if (err != HANAMI_OK) { free(authenticated); goto done; }
    }
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        void *notice = NULL;
        while (go_channel_try_receive(ctx->notice, &notice) == 0) {}
        repo_query query = { .ctx = ctx, .error = HANAMI_OK };
        NostrFilters filters = { .filters = filter, .count = 1 };
        nostr_simple_pool_set_auto_unsub_on_eose(ctx->pool, true);
        nostr_simple_pool_set_event_middleware_ex(ctx->pool, collect_event, &query);
        nostr_simple_pool_set_subscription_callback(ctx->pool, query_boundary, &query);
        nostr_simple_pool_subscribe(ctx->pool, (const char **)ctx->relay_urls,
                                   ctx->relay_count, filters, false);
        query.remaining = ctx->pool->subs_count;
        if (query.remaining == 0) {
            err = HANAMI_ERR_NETWORK;
        } else {
            nostr_simple_pool_start(ctx->pool);
            GoSelectCase wait = { .op = GO_SELECT_RECEIVE, .chan = ctx->notice,
                                  .recv_buf = &notice };
            uint64_t deadline = monotonic_ms() + RELAY_TIMEOUT_MS;
            GoSelectResult result;
            for (;;) {
                uint64_t now = monotonic_ms();
                result = go_select_timeout(&wait, 1, now < deadline ? deadline - now : 1);
                if (result.selected_case != 0 || (uintptr_t)notice != AUTH_REQUIRED) break;
                bool pending = false;
                for (size_t i = 0; i < relay_count; i++)
                    pending |= has_pending_challenge(ctx, ctx->pool->relays[i]);
                if (pending) break;
                if (atomic_load(&query.done)) { notice = (void *)(uintptr_t)QUERY_DONE; break; }
                /* A repeated, already accepted challenge is not a new boundary. */
                if (monotonic_ms() >= deadline) { result.selected_case = -1; break; }
            }
            /* Stop joins delivery and removes the attempt's subscriptions.
             * Partial events/counters never carry into an authenticated retry. */
            nostr_simple_pool_stop(ctx->pool);
            err = result.selected_case != 0 ? HANAMI_ERR_TIMEOUT
                : query.error != HANAMI_OK ? query.error
                : (uintptr_t)notice == AUTH_REQUIRED ? HANAMI_ERR_AUTH : HANAMI_OK;
        }
        nostr_simple_pool_set_subscription_callback(ctx->pool, NULL, NULL);
        nostr_simple_pool_set_event_middleware_ex(ctx->pool, NULL, NULL);
        if (err == HANAMI_OK) {
            *out = query.latest;
            if (!*out) err = HANAMI_ERR_NOT_FOUND;
            break;
        }
        nostr_event_free(query.latest);
        if (err != HANAMI_ERR_AUTH || attempt != 0) break;
        bool retried_auth = false;
        for (size_t i = 0; i < relay_count; i++) {
            NostrRelay *relay = ctx->pool->relays[i];
            if (authenticated[i] || (relay != query.auth_relay && !has_pending_challenge(ctx, relay))) continue;
            err = authenticate_relay(ctx, relay);
            authenticated[i] = true;
            if (err != HANAMI_OK) break;
            retried_auth = true;
        }
        if (!retried_auth || err != HANAMI_OK) break;
    }
    free(authenticated);
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

hanami_error_t hanami_nostr_fetch_state_event(hanami_nostr_ctx_t *ctx,
    const char *repo_id, const char *author, NostrEvent **out)
{
    *out = NULL;
    NostrFilter *f = hanami_nostr_build_state_filter(repo_id, author);
    if (!f) return HANAMI_ERR_NOMEM;
    hanami_error_t err = query_single_event(ctx, f, out);
    if (err != HANAMI_OK) return err;
    nip34_repo_state_t *state = NULL;
    if ((*out)->kind != NIP34_KIND_REPOSITORY_STATE ||
        strcmp((*out)->pubkey, author) != 0 ||
        nip34_parse_repo_state(*out, &state) != NIP34_OK ||
        !state->repo_id || strcmp(state->repo_id, repo_id) != 0) {
        nostr_event_free(*out);
        *out = NULL;
        err = HANAMI_ERR_NOSTR;
    }
    nip34_repo_state_free(state);
    return err;
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
    if (res == NIP34_OK) {
        pthread_mutex_lock(&ctx->state_mutex);
        if (ctx->last_state_time < ev->created_at) ctx->last_state_time = ev->created_at;
        pthread_mutex_unlock(&ctx->state_mutex);
    }
    nostr_event_free(ev);

    return (res == NIP34_OK) ? HANAMI_OK : HANAMI_ERR_NOSTR;
}

/* BUD-03: replaceable kind 10063, ordered `server` tags (most trusted first). */
hanami_error_t hanami_nostr_fetch_blossom_servers(hanami_nostr_ctx_t *ctx,
    const char *pubkey, char ***out, size_t *count)
{
    if (!ctx || !pubkey || !out || !count) return HANAMI_ERR_INVALID_ARG;
    *out = NULL; *count = 0;
    NostrFilter *filter = nostr_filter_new();
    if (!filter) return HANAMI_ERR_NOMEM;
    nostr_filter_add_kind(filter, NOSTR_KIND_BLOSSOM_SERVER_LIST);
    nostr_filter_add_author(filter, pubkey);
    nostr_filter_set_limit(filter, 1);
    NostrEvent *ev = NULL;
    hanami_error_t err = query_single_event(ctx, filter, &ev);
    if (err != HANAMI_OK) return err;
    if (nostr_event_get_kind(ev) != NOSTR_KIND_BLOSSOM_SERVER_LIST ||
        strcmp(nostr_event_get_pubkey(ev), pubkey) != 0) {
        nostr_event_free(ev);
        return HANAMI_ERR_AUTH;
    }
    size_t cap = ev->tags ? ev->tags->count : 0;
    char **urls = calloc(cap + 1, sizeof(*urls));
    if (!urls) { nostr_event_free(ev); return HANAMI_ERR_NOMEM; }
    size_t n = 0;
    for (size_t i = 0; i < cap; i++) {
        NostrTag *tag = ev->tags->data[i];
        const char *key = nostr_tag_get_key(tag);
        const char *url = nostr_tag_size(tag) >= 2 ? nostr_tag_get(tag, 1) : NULL;
        if (!key || strcmp(key, "server") != 0 || !url || !*url) continue;
        if (!(urls[n] = strdup(url))) {
            hanami_discovery_free_list(urls, n);
            nostr_event_free(ev);
            return HANAMI_ERR_NOMEM;
        }
        n++;
    }
    nostr_event_free(ev);
    *out = urls; *count = n;
    return HANAMI_OK;
}

hanami_error_t hanami_nostr_fetch_head(hanami_nostr_ctx_t *ctx,
    const uint8_t vault_id[32], const char *writer, NostrEvent **out)
{
    if (!ctx || !vault_id || !writer || !out) return HANAMI_ERR_INVALID_ARG;
    char d[PORTHOME_HEAD_D_LEN];
    if (porthome_head_d_tag(vault_id, d) != PORTHOME_OK) return HANAMI_ERR_INVALID_ARG;
    NostrFilter *filter = nostr_filter_new();
    if (!filter) return HANAMI_ERR_NOMEM;
    nostr_filter_add_kind(filter, PORTHOME_HEAD_KIND);
    nostr_filter_add_author(filter, writer);
    nostr_filter_tags_append(filter, "d", d, NULL);
    nostr_filter_set_limit(filter, 1);
    hanami_error_t err = query_single_event(ctx, filter, out);
    if (err == HANAMI_OK &&
        (nostr_event_get_kind(*out) != PORTHOME_HEAD_KIND ||
         strcmp(nostr_event_get_pubkey(*out), writer) != 0)) {
        nostr_event_free(*out);
        *out = NULL;
        return HANAMI_ERR_AUTH;
    }
    return err;
}

hanami_error_t hanami_nostr_sign_head(hanami_nostr_ctx_t *ctx, NostrEvent *event)
{
    hanami_error_t err = sign_event(ctx, event);
    if (err == HANAMI_OK &&
        (!nostr_event_check_signature(event) ||
         strcmp(nostr_event_get_pubkey(event), ctx->signer.pubkey) != 0))
        err = HANAMI_ERR_AUTH;
    return err;
}
