/*
 * hanami-nostr.c - Nostr integration layer for NIP-34 events
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-nostr.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"
#include "channel.h"
#include "context.h"
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

    /* Lazily connected relays */
    NostrRelay **relays;    /* Owned array, NULL until connected */
    bool relays_connected;
    pthread_mutex_t relay_mutex;
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

    /* Free relays */
    if (ctx->relays) {
        for (size_t i = 0; i < ctx->relay_count; i++) {
            if (ctx->relays[i])
                nostr_relay_free(ctx->relays[i]);
        }
        free(ctx->relays);
    }

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

static hanami_error_t ensure_relays(hanami_nostr_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->relay_mutex);

    if (ctx->relays_connected) {
        pthread_mutex_unlock(&ctx->relay_mutex);
        return HANAMI_OK;
    }

    ctx->relays = calloc(ctx->relay_count, sizeof(NostrRelay *));
    if (!ctx->relays) {
        pthread_mutex_unlock(&ctx->relay_mutex);
        return HANAMI_ERR_NOMEM;
    }

    bool any_connected = false;
    for (size_t i = 0; i < ctx->relay_count; i++) {
        Error *err = NULL;
        ctx->relays[i] = nostr_relay_new(NULL, ctx->relay_urls[i], &err);
        if (ctx->relays[i]) {
            any_connected = true;
        } else if (err) {
            free_error(err);  /* prevent leak on connection failure */
        }
    }

    ctx->relays_connected = true;
    pthread_mutex_unlock(&ctx->relay_mutex);
    return any_connected ? HANAMI_OK : HANAMI_ERR_NETWORK;
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

    hanami_error_t err = ensure_relays(ctx);
    if (err != HANAMI_OK)
        return err;

    /* Publish to all connected relays (best-effort) */
    bool any_published = false;
    for (size_t i = 0; i < ctx->relay_count; i++) {
        if (ctx->relays[i]) {
            nostr_relay_publish(ctx->relays[i], event);
            any_published = true;
        }
    }

    return any_published ? HANAMI_OK : HANAMI_ERR_NETWORK;
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
 * Querying (using relay subscribe + collect first match)
 * ========================================================================= */

/* Query timeout for single-event queries (seconds) */
#define QUERY_TIMEOUT_SECONDS 10

/* Internal: query relays with a filter and collect the most recent event.
 * Uses the subscription events channel with a timeout context to receive
 * the first matching event. */
static NostrEvent *query_single_event(hanami_nostr_ctx_t *ctx,
                                      NostrFilter *filter)
{
    hanami_error_t err = ensure_relays(ctx);
    if (err != HANAMI_OK) {
        nostr_filter_free(filter);
        return NULL;
    }

    NostrFilters *filters = nostr_filters_new();
    if (!filters) {
        nostr_filter_free(filter);
        return NULL;
    }
    nostr_filters_add(filters, filter);

    /* Create a timeout context for the query */
    GoContext *timeout_ctx = go_context_background();
    if (timeout_ctx) {
        go_context_init(timeout_ctx, QUERY_TIMEOUT_SECONDS);
    }

    /* Try each relay until we get a result */
    NostrEvent *result = NULL;
    for (size_t i = 0; i < ctx->relay_count && !result; i++) {
        if (!ctx->relays[i]) continue;

        /* Prepare and fire subscription */
        struct NostrSubscription *sub = nostr_relay_prepare_subscription(
            ctx->relays[i], timeout_ctx, filters);
        if (!sub) continue;

        Error *sub_err = NULL;
        bool ok = nostr_subscription_fire(sub, &sub_err);
        if (!ok) {
            nostr_subscription_free(sub);
            continue;
        }

        /* Wait for EOSE or an event, whichever comes first */
        GoChannel *events_ch = nostr_subscription_get_events_channel(sub);
        if (events_ch) {
            void *data = NULL;
            int rc = go_channel_receive_with_context(events_ch, &data, timeout_ctx);
            if (rc == 0 && data) {
                /* We received an event — clone it since sub cleanup will free originals */
                NostrEvent *received = (NostrEvent *)data;
                char *json = nostr_event_serialize_compact(received);
                if (json) {
                    result = nostr_event_new();
                    if (result) {
                        if (nostr_event_deserialize_compact(result, json, NULL) != 1) {
                            nostr_event_free(result);
                            result = NULL;
                        }
                    }
                    free(json);
                }
            }
        }

        nostr_subscription_unsubscribe(sub);
        nostr_subscription_free(sub);
    }

    nostr_filters_free(filters);
    if (timeout_ctx)
        go_context_unref(timeout_ctx);
    return result;
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

    NostrEvent *ev = query_single_event(ctx, f);
    if (!ev)
        return HANAMI_ERR_NOT_FOUND;

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

    NostrEvent *ev = query_single_event(ctx, f);
    if (!ev)
        return HANAMI_ERR_NOT_FOUND;

    nip34_result_t res = nip34_parse_repo_state(ev, out);
    nostr_event_free(ev);

    return (res == NIP34_OK) ? HANAMI_OK : HANAMI_ERR_NOSTR;
}
