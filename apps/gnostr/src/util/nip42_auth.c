/**
 * NIP-42 Relay Authentication - Pool Integration (nostrc-kn38)
 *
 * Bridges the GNostrPool auth handler with the signer service to provide
 * automatic NIP-42 AUTH challenge responses. When a relay sends an AUTH
 * challenge, this module:
 * 1. Receives the unsigned kind 22242 event from the relay wrapper
 * 2. Serializes it to JSON
 * 3. Signs it via the signer service (blocking wait on async result)
 * 4. Parses the signed JSON back and updates the event fields
 */

#include "nip42_auth.h"
#include "../ipc/gnostr-signer-service.h"
#include "nostr_relay.h"
#include "nostr_json.h"
#include "json.h"
#include "nostr-event.h"
#include <string.h>

/* Context for synchronous wait on async signer result.
 * Heap-allocated because the async callback may fire AFTER the waiter
 * times out and returns (stack-use-after-return if on stack). */
typedef struct {
    GMutex mutex;
    GCond cond;
    gboolean done;
    gboolean timed_out;  /* set by waiter on timeout; callback owns cleanup */
    char *signed_json;   /* result: signed event JSON (owned) */
    GError *error;       /* result: error if signing failed (owned) */
} SyncSignCtx;

static void
sync_sign_ctx_free(SyncSignCtx *ctx)
{
    if (!ctx) return;
    g_free(ctx->signed_json);
    g_clear_error(&ctx->error);
    g_mutex_clear(&ctx->mutex);
    g_cond_clear(&ctx->cond);
    g_free(ctx);
}

/* Callback for async signer - signals the waiting thread */
static void
on_sign_complete(GnostrSignerService *service G_GNUC_UNUSED,
                 const char          *signed_event_json,
                 GError              *error,
                 gpointer             user_data)
{
    SyncSignCtx *ctx = user_data;

    g_mutex_lock(&ctx->mutex);
    if (ctx->timed_out) {
        /* Waiter already returned — we own the ctx, free it */
        g_mutex_unlock(&ctx->mutex);
        sync_sign_ctx_free(ctx);
        return;
    }
    ctx->done = TRUE;
    if (error) {
        ctx->error = g_error_copy(error);
    } else if (signed_event_json) {
        ctx->signed_json = g_strdup(signed_event_json);
    } else {
        ctx->error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                                  "signer returned no event and no error");
    }
    g_cond_signal(&ctx->cond);
    g_mutex_unlock(&ctx->mutex);
}

/**
 * NIP-42 auth signing function for GNostrRelayAuthSignFunc.
 *
 * Called synchronously on the main thread when a relay receives an AUTH
 * challenge. Signs the kind 22242 event using the default signer service.
 */
static void
nip42_auth_sign_func(NostrEvent *event,
                     GError    **error,
                     gpointer    user_data G_GNUC_UNUSED)
{
    GnostrSignerService *signer = gnostr_signer_service_get_default();
    if (!signer || !gnostr_signer_service_is_ready(signer)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                    "NIP-42: signer not available for AUTH");
        return;
    }

    /* Serialize the unsigned event to JSON */
    char *unsigned_json = nostr_event_serialize(event);
    if (!unsigned_json) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "NIP-42: failed to serialize auth event");
        return;
    }

    /* Sign synchronously by waiting on an async callback.
     * Heap-allocate ctx because the callback may fire AFTER timeout
     * (stack-use-after-return if on stack — nostrc-auth1). */
    SyncSignCtx *ctx = g_new0(SyncSignCtx, 1);
    g_mutex_init(&ctx->mutex);
    g_cond_init(&ctx->cond);

    gnostr_signer_service_sign_event_async(signer, unsigned_json, NULL,
                                            on_sign_complete, ctx);
    free(unsigned_json);

    /* Wait for the signer callback */
    g_mutex_lock(&ctx->mutex);
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (!ctx->done) {
        if (!g_cond_wait_until(&ctx->cond, &ctx->mutex, deadline)) {
            /* Timeout — transfer ownership to callback */
            ctx->timed_out = TRUE;
            g_mutex_unlock(&ctx->mutex);
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                        "NIP-42: signer timed out after 10s");
            /* Do NOT free ctx — on_sign_complete will free when it fires */
            return;
        }
    }
    g_mutex_unlock(&ctx->mutex);

    if (ctx->error) {
        if (error) *error = g_error_copy(ctx->error);
        sync_sign_ctx_free(ctx);
        return;
    }

    /* Parse signed JSON to extract id, pubkey, sig and update the event */
    if (ctx->signed_json) {
        gchar *id = gnostr_json_get_string(ctx->signed_json, "id", NULL);
        gchar *pubkey = gnostr_json_get_string(ctx->signed_json, "pubkey", NULL);
        gchar *sig = gnostr_json_get_string(ctx->signed_json, "sig", NULL);

        if (id && pubkey && sig) {
            if (event->id) free(event->id);
            event->id = id;
            if (event->pubkey) free(event->pubkey);
            event->pubkey = pubkey;
            if (event->sig) free(event->sig);
            event->sig = sig;

            GError *ts_err = NULL;
            gint64 created_at = gnostr_json_get_int64(ctx->signed_json, "created_at", &ts_err);
            if (!ts_err) event->created_at = created_at;
            g_clear_error(&ts_err);

            g_debug("NIP-42: AUTH event signed (id=%.8s pubkey=%.8s)", id, pubkey);
        } else {
            g_free(id);
            g_free(pubkey);
            g_free(sig);
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "NIP-42: signed event missing id/pubkey/sig fields");
        }
    }

    sync_sign_ctx_free(ctx);
}

void
gnostr_nip42_setup_pool_auth(GNostrPool *pool)
{
    g_return_if_fail(GNOSTR_IS_POOL(pool));

    gnostr_pool_set_auth_handler(pool, nip42_auth_sign_func, NULL, NULL);
    g_message("NIP-42: AUTH handler installed on pool");
}
