/*
 * hanami-nostr.c - Nostr integration layer for NIP-34 events
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-nostr.h"
#include "hanami-sealed.h"
#include "hanami-discovery.h"
#include "hanami-nostr-internal.h"
#include "hanami-manifest.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-simple-pool.h"
#include "nostr-subscription.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"
#include "channel.h"
#include "select.h"
#include "error.h"
#include "error_codes.h"
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

typedef struct {
    GoChannel *notice; /* Owned by the context, outlives the pool's workers. */
    atomic_uint_fast64_t losses;
} relay_observation;

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
    relay_observation *observations; /* Stable slots in pool relay order. */
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
    ctx->observations = calloc(count, sizeof(*ctx->observations));
    if (!ctx->authenticated || !ctx->observations) {
        hanami_nostr_ctx_free(ctx);
        return HANAMI_ERR_NOMEM;
    }
    for (size_t i = 0; i < count; i++) atomic_init(&ctx->observations[i].losses, 0);

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
    free(ctx->observations);

    free(ctx);
}

/* =========================================================================
 * Lazy relay connection
 * ========================================================================= */

enum { RELAY_CHANGED = 1, AUTH_REQUIRED = 2 };
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

/* Published by the relay reader after draining frames from the lost connection.
 * The counter, not the coalescing wake hint, records the loss across reconnects.
 * This context owns the private pool and outlives its joined relay workers. */
static void relay_state_changed(NostrRelay *relay, NostrRelayConnectionState old_state,
                                NostrRelayConnectionState new_state, void *data)
{
    (void)relay;
    (void)old_state;
    relay_observation *observation = data;
    if (new_state != NOSTR_RELAY_STATE_DISCONNECTED) return;
    atomic_fetch_add(&observation->losses, 1);
    go_channel_try_send(observation->notice, (void *)(uintptr_t)RELAY_CHANGED);
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
        if (!ctx->observations[i].notice) {
            ctx->observations[i].notice = ctx->notice;
            nostr_relay_set_state_callback(relay, relay_state_changed, &ctx->observations[i]);
        }
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
    /* Classify this operation's loss, not a later reconnect's current state. */
    if (err && err->code == ERR_WEBSOCKET_CLOSED) return HANAMI_ERR_NETWORK;
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
    return hanami_nostr_publish_state_manifest(ctx, repo_id, refs, ref_count, head, NULL);
}

hanami_error_t hanami_nostr_publish_state_manifest(hanami_nostr_ctx_t *ctx,
    const char *repo_id, const nip34_ref_t *refs, size_t ref_count,
    const char *head, const char *manifest)
{
    if (!ctx || !repo_id)
        return HANAMI_ERR_INVALID_ARG;
    if (!ctx->has_signer)
        return HANAMI_ERR_AUTH;

    pthread_mutex_lock(&ctx->state_mutex);
    NostrEvent *ev = NULL;
    hanami_error_t err = ctx->last_state_time == INT64_MAX ? HANAMI_ERR_NOSTR
        : hanami_nostr_create_state_tagged(ctx, repo_id, refs,
            ref_count, head, manifest, ctx->last_state_time + 1, &ev);
    if (err == HANAMI_OK) {
        /* Reserve even on lost OK: that event might already be stored. */
        ctx->last_state_time = ev->created_at;
        err = hanami_nostr_publish_event(ctx, ev);
    }
    nostr_event_free(ev);
    pthread_mutex_unlock(&ctx->state_mutex);
    return err;
}

const char *hanami_nostr_ctx_signer_pubkey(const hanami_nostr_ctx_t *ctx)
{
    return ctx && ctx->has_signer ? ctx->signer.pubkey : NULL;
}

hanami_error_t hanami_nostr_create_state(hanami_nostr_ctx_t *ctx,
    const char *repo_id, const nip34_ref_t *refs, size_t count,
    const char *head, int64_t minimum_time, NostrEvent **out)
{
    return hanami_nostr_create_state_tagged(ctx, repo_id, refs, count, head,
                                            NULL, minimum_time, out);
}

hanami_error_t hanami_nostr_create_state_tagged(hanami_nostr_ctx_t *ctx,
    const char *repo_id, const nip34_ref_t *refs, size_t count,
    const char *head, const char *manifest, int64_t minimum_time, NostrEvent **out)
{
    *out = NULL;
    if (manifest && !hanami_hex_valid(manifest, 64)) return HANAMI_ERR_INVALID_ARG;
    NostrEvent *ev = nip34_create_repo_state(repo_id, refs, count, head);
    if (!ev) return HANAMI_ERR_NOMEM;
    /* hanami extension (docs/libhanami-design.md 4.4b), signed with the refs. */
    if (manifest) {
        NostrTag *tag = nostr_tag_new(HANAMI_MANIFEST_TAG, manifest, NULL);
        if (!tag || !ev->tags) { nostr_tag_free(tag); nostr_event_free(ev); return HANAMI_ERR_NOMEM; }
        nostr_tags_append(ev->tags, tag);
    }
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
 * Querying (finite catch-up on pool-owned relays; EOSE, CLOSED and transport
 * loss are boundaries. A deadline is never treated as completion.)
 * ========================================================================= */

typedef struct {
    NostrEvent *latest;
    hanami_error_t failure; /* strongest CLOSED classification seen */
    NostrRelay *auth_relay; /* a relay that CLOSED with auth-required: */
} repo_query;

typedef struct {
    NostrRelay *relay;
    NostrSubscription *sub;
    uint64_t loss_generation;
    bool terminal;
    bool answered;
} query_relay;

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

static void query_refused(repo_query *query, query_relay *entry, const char *reason)
{
    entry->terminal = entry->answered = true;
    bool needs_auth = reason && strncmp(reason, "auth-required:", 14) == 0;
    bool restricted = reason && strncmp(reason, "restricted:", 11) == 0;
    if (needs_auth) query->auth_relay = entry->relay;
    if (needs_auth || restricted) query->failure = HANAMI_ERR_AUTH;
    else if (query->failure == HANAMI_OK) query->failure = HANAMI_ERR_NOSTR;
}

static void query_close(query_relay *entry)
{
    if (!entry->sub) return;
    nostr_subscription_unsubscribe(entry->sub);
    nostr_subscription_close(entry->sub, NULL);
    nostr_subscription_wait(entry->sub);
}

/* All reduction is on the caller thread. Observe the boundary BEFORE draining
 * events: EOSE or a reader-reported loss means preceding frames are enqueued.
 * Consuming separate channels in the reverse order can prune a buffered EVENT. */
static size_t query_drain(hanami_nostr_ctx_t *ctx, repo_query *query,
                         query_relay *entries, size_t count, size_t *unanswered)
{
    size_t remaining = 0;
    for (size_t i = 0; i < count; i++) {
        query_relay *entry = &entries[i];
        if (!entry->sub) continue;
        bool lost = atomic_load(&ctx->observations[i].losses) != entry->loss_generation;
        void *message = NULL;
        if (go_channel_try_receive(nostr_subscription_get_closed_channel(entry->sub), &message) == 0) {
            query_refused(query, entry, message);
            free(message);
        } else if (go_channel_try_receive(nostr_subscription_get_eose_channel(entry->sub), NULL) == 0) {
            entry->terminal = entry->answered = true;
        }
        entry->terminal |= lost;
        if (entry->terminal) query_close(entry);
        GoChannel *events = nostr_subscription_get_events_channel(entry->sub);
        size_t budget = entry->terminal ? SIZE_MAX : 256;
        while (budget-- > 0 && go_channel_try_receive(events, &message) == 0) {
            if (message) {
                NostrIncomingEvent incoming = { .relay = entry->relay, .event = message };
                collect_event(&incoming, query);
            }
        }
        if (entry->terminal) {
            *unanswered += !entry->answered;
            nostr_subscription_free(entry->sub);
            entry->sub = NULL;
        } else {
            remaining++;
        }
    }
    return remaining;
}

/* Each subscription owns its filters through its last reader/refire reference. */
static NostrSubscription *query_subscription(NostrRelay *relay, NostrFilter *filter)
{
    NostrFilters *filters = nostr_filters_new();
    NostrFilter *copy = nostr_filter_copy(filter);
    if (!filters || !copy || !nostr_filters_add(filters, copy)) {
        nostr_filters_free(filters);
        nostr_filter_free(copy);
        return NULL;
    }
    nostr_filter_free(copy); /* add moved its contents */
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    if (sub) nostr_subscription_set_filters(sub, filters);
    else nostr_filters_free(filters);
    return sub;
}

/* Preserve snapshot admission: wait for each current dial's outcome, then
 * await only established relays. A dial that cannot be admitted is unanswered,
 * not an open subscription whose deadline could prove absence. */
static void await_handshakes(NostrSimplePool *pool, uint64_t deadline)
{
    for (size_t i = 0; i < pool->relay_count; i++) {
        NostrRelay *relay = pool->relays[i];
        if (!nostr_relay_is_connected(relay) || nostr_relay_is_established(relay)) continue;
        uint64_t now = monotonic_ms();
        if (now >= deadline) break;
        Error *failure = NULL;
        (void)nostr_relay_wait_established(relay, (uint32_t)(deadline - now), &failure);
        free_error(failure);
    }
}

/* Distinct configured relays (the pool keys relays by exact URL). */
static size_t distinct_relays(const hanami_nostr_ctx_t *ctx)
{
    size_t n = 0;
    for (size_t i = 0; i < ctx->relay_count; i++) {
        size_t j = 0;
        while (j < i && strcmp(ctx->relay_urls[j], ctx->relay_urls[i]) != 0) j++;
        n += j == i;
    }
    return n;
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
    query_relay *entries = calloc(relay_count, sizeof(*entries));
    GoSelectCase *cases = calloc(1 + 3 * relay_count, sizeof(*cases));
    query_relay **waiting = calloc(relay_count, sizeof(*waiting));
    if (!authenticated || !entries || !cases || !waiting) { err = HANAMI_ERR_NOMEM; goto release; }
    /* A relay whose AUTH fails is that relay's failure: it is still queried
     * and its CLOSED is classified like any other. */
    for (size_t i = 0; i < relay_count; i++) {
        NostrRelay *relay = ctx->pool->relays[i];
        if (!has_pending_challenge(ctx, relay)) continue;
        authenticated[i] = true;
        if (authenticate_relay(ctx, relay) == HANAMI_ERR_NOMEM) {
            err = HANAMI_ERR_NOMEM;
            goto release;
        }
    }
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        void *notice = NULL;
        while (go_channel_try_receive(ctx->notice, &notice) == 0) {}
        memset(entries, 0, relay_count * sizeof(*entries));
        repo_query query = { .failure = HANAMI_OK };
        bool cut_for_auth = false;
        size_t unanswered = distinct_relays(ctx);
        uint64_t deadline = monotonic_ms() + RELAY_TIMEOUT_MS;
        await_handshakes(ctx->pool, deadline);
        err = HANAMI_OK;
        for (size_t i = 0; i < relay_count; i++) {
            query_relay *entry = &entries[i];
            entry->relay = ctx->pool->relays[i];
            /* Capture before admission: even an immediate loss/refire after
             * this check belongs to this attempt and cannot be erased. */
            entry->loss_generation = atomic_load(&ctx->observations[i].losses);
            if (!nostr_relay_is_established(entry->relay)) continue;
            entry->sub = query_subscription(entry->relay, filter);
            if (!entry->sub) { err = HANAMI_ERR_NOMEM; break; }
            Error *failure = NULL;
            bool fired = nostr_subscription_fire(entry->sub, &failure);
            free_error(failure);
            if (!fired) {
                query_close(entry);
                nostr_subscription_free(entry->sub);
                entry->sub = NULL;
                continue;
            }
            unanswered--;
        }
        while (err == HANAMI_OK) {
            size_t remaining = query_drain(ctx, &query, entries, relay_count, &unanswered);
            if (!remaining) {
                err = query.latest ? HANAMI_OK
                    : query.failure != HANAMI_OK ? query.failure
                    : unanswered ? HANAMI_ERR_NETWORK : HANAMI_ERR_NOT_FOUND;
                break;
            }
            uint64_t now = monotonic_ms();
            if (now >= deadline) { err = HANAMI_ERR_TIMEOUT; break; }
            for (size_t i = 0; ctx->has_signer && i < relay_count; i++)
                cut_for_auth |= !authenticated[i] && has_pending_challenge(ctx, ctx->pool->relays[i]);
            if (cut_for_auth) { err = HANAMI_ERR_AUTH; break; }

            void *message = NULL;
            size_t count = 1, active = 0;
            cases[0] = (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = ctx->notice,
                                       .recv_buf = &message };
            for (size_t i = 0; i < relay_count; i++) {
                NostrSubscription *sub = entries[i].sub;
                if (!sub) continue;
                waiting[active++] = &entries[i];
                GoChannel *channels[] = { nostr_subscription_get_events_channel(sub),
                    nostr_subscription_get_eose_channel(sub), nostr_subscription_get_closed_channel(sub) };
                for (size_t j = 0; j < 3; j++)
                    cases[count++] = (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = channels[j],
                                                     .recv_buf = &message };
            }
            GoSelectResult result = go_select_timeout(cases, count, deadline - now);
            /* Re-reduce even on timeout: a boundary may already be queued.
             * Neither timeout nor a coalesced notice is itself completion. */
            if (result.selected_case <= 0) continue;
            size_t selected = (size_t)result.selected_case - 1;
            query_relay *entry = waiting[selected / 3];
            if (selected % 3 == 0 && message) {
                NostrIncomingEvent incoming = { .relay = entry->relay, .event = message };
                collect_event(&incoming, &query);
            } else if (selected % 3 == 1) {
                entry->terminal = entry->answered = true;
            } else if (selected % 3 == 2) {
                query_refused(&query, entry, message);
                free(message);
            }
        }
        for (size_t i = 0; i < relay_count; i++) {
            query_close(&entries[i]);
            nostr_subscription_free(entries[i].sub);
        }
        bool retry = attempt == 0 && ctx->has_signer && err != HANAMI_ERR_TIMEOUT &&
                     err != HANAMI_ERR_NOMEM && (cut_for_auth || query.auth_relay);
        /* Authenticate each challenged relay once, then re-run the whole
         * snapshot. Partial events/counters never carry into the retry; if
         * no relay authenticates, this attempt's verdict stands. */
        bool retried_auth = false;
        for (size_t i = 0; retry && i < relay_count; i++) {
            NostrRelay *relay = ctx->pool->relays[i];
            if (authenticated[i] || (relay != query.auth_relay && !has_pending_challenge(ctx, relay))) continue;
            authenticated[i] = true;
            hanami_error_t auth_err = authenticate_relay(ctx, relay);
            if (auth_err == HANAMI_ERR_NOMEM) { err = auth_err; retried_auth = false; break; }
            retried_auth |= auth_err == HANAMI_OK;
        }
        if (err == HANAMI_OK && !retried_auth) {
            *out = query.latest;
            break;
        }
        nostr_event_free(query.latest);
        if (!retried_auth) break;
    }
release:
    free(authenticated);
    free(entries);
    free(cases);
    free(waiting);
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

    if (ev->kind != NIP34_KIND_REPOSITORY || !ev->pubkey ||
        strcmp(ev->pubkey, owner_pubkey)) {
        nostr_event_free(ev);
        return HANAMI_ERR_NOSTR;
    }
    nip34_result_t res = nip34_parse_repository(ev, out);
    nostr_event_free(ev);
    if (res != NIP34_OK || !*out || !(*out)->id ||
        strcmp((*out)->id, repo_id)) {
        nip34_repository_free(*out);
        *out = NULL;
        return HANAMI_ERR_NOSTR;
    }
    return HANAMI_OK;
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

/* NIP-65: replaceable kind 10002; write relays are `r` tags with no marker or
 * the "write" marker, kept in listed order. Only ws:// and wss:// URLs. */
hanami_error_t hanami_nostr_fetch_write_relays(hanami_nostr_ctx_t *ctx,
    const char *pubkey, char ***out, size_t *count)
{
    if (!ctx || !pubkey || !out || !count) return HANAMI_ERR_INVALID_ARG;
    *out = NULL; *count = 0;
    NostrFilter *filter = nostr_filter_new();
    if (!filter) return HANAMI_ERR_NOMEM;
    nostr_filter_add_kind(filter, NOSTR_KIND_RELAY_LIST_METADATA);
    nostr_filter_add_author(filter, pubkey);
    nostr_filter_set_limit(filter, 1);
    NostrEvent *ev = NULL;
    hanami_error_t err = query_single_event(ctx, filter, &ev);
    if (err != HANAMI_OK) return err;
    if (nostr_event_get_kind(ev) != NOSTR_KIND_RELAY_LIST_METADATA ||
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
        const char *marker = nostr_tag_size(tag) >= 3 ? nostr_tag_get(tag, 2) : NULL;
        if (!key || strcmp(key, "r") != 0 || !url) continue;
        if (strncmp(url, "wss://", 6) != 0 && strncmp(url, "ws://", 5) != 0) continue;
        if (marker && *marker && strcmp(marker, "write") != 0) continue;
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

/* 30618 with hanami's manifest extension tag (docs/libhanami-design.md 4.4b). */
hanami_error_t hanami_nostr_fetch_state_manifest(hanami_nostr_ctx_t *ctx,
    const char *repo, const char *owner, nip34_repo_state_t **state, char head[65])
{
    if (!ctx || !repo || !owner || !state || !head) return HANAMI_ERR_INVALID_ARG;
    *state = NULL; head[0] = '\0';
    NostrFilter *filter = hanami_nostr_build_state_filter(repo, owner);
    if (!filter) return HANAMI_ERR_NOMEM;
    NostrEvent *ev = NULL;
    hanami_error_t err = query_single_event(ctx, filter, &ev);
    if (err != HANAMI_OK) return err;
    if (nostr_event_get_kind(ev) != NIP34_KIND_REPOSITORY_STATE ||
        strcmp(nostr_event_get_pubkey(ev), owner) != 0) {
        nostr_event_free(ev);
        return HANAMI_ERR_AUTH;
    }
    size_t n = ev->tags ? ev->tags->count : 0;
    for (size_t i = 0; i < n; i++) {
        NostrTag *tag = ev->tags->data[i];
        const char *key = nostr_tag_get_key(tag);
        if (!key || strcmp(key, HANAMI_MANIFEST_TAG) != 0) continue;
        const char *value = nostr_tag_size(tag) >= 2 ? nostr_tag_get(tag, 1) : NULL;
        /* One well-formed tag only: a duplicate or malformed one is refused. */
        if (head[0] || !hanami_hex_valid(value, 64)) {
            nostr_event_free(ev);
            return HANAMI_ERR_NOSTR;
        }
        memcpy(head, value, 65);
    }
    nip34_result_t res = nip34_parse_repo_state(ev, state);
    if (res == NIP34_OK) {
        /* A later publish on this context must supersede what was observed
         * (also across process restarts: no same-second replaceable ties). */
        pthread_mutex_lock(&ctx->state_mutex);
        if (ctx->last_state_time < ev->created_at) ctx->last_state_time = ev->created_at;
        pthread_mutex_unlock(&ctx->state_mutex);
    }
    nostr_event_free(ev);
    if (res != NIP34_OK || !*state || !(*state)->repo_id ||
        strcmp((*state)->repo_id, repo) != 0) {
        nip34_repo_state_free(*state);
        *state = NULL;
        head[0] = '\0';
        return HANAMI_ERR_NOSTR;
    }
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
