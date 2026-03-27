/**
 * gnostr-dm-service-send.c - DM Send & Relay Lookup
 *
 * Extracted from gnostr-dm-service.c for manageability.
 * Handles:
 * - Recipient relay lookup (NIP-17 kind 10050, NIP-65 fallback)
 * - Text DM send via NIP-59 gift wrap (kind 14 rumor)
 * - File DM send via NIP-59 gift wrap (kind 15 rumor)
 */

#include "gnostr-dm-service-private.h"
#include "../util/nip59_giftwrap.h"
#include "../util/dm_files.h"
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/nostr_relay.h>
#include "nostr-event.h"
#include "nostr-kinds.h"

#include <string.h>

/* ============== Recipient Relay Lookup for Sending DMs ============== */

typedef struct {
    char *recipient_pubkey;
    GCancellable *cancellable;
    GnostrDmRelaysCallback callback;
    gpointer user_data;
    gboolean tried_10050;  /* TRUE if we already tried kind 10050 */
} RecipientRelayCtx;

static void recipient_relay_ctx_free(RecipientRelayCtx *ctx) {
    if (!ctx) return;
    g_clear_pointer(&ctx->recipient_pubkey, g_free);
    g_clear_object(&ctx->cancellable);
    g_free(ctx);
}

/* Forward declarations */
static void on_recipient_nip65_done(GPtrArray *relays, gpointer user_data);

static void
on_recipient_dm_relays_done(GPtrArray *dm_relays, gpointer user_data)
{
    RecipientRelayCtx *ctx = (RecipientRelayCtx*)user_data;
    if (!ctx) return;

    if (dm_relays && dm_relays->len > 0) {
        /* Found kind 10050 relays, use them */
        g_message("[DM_SERVICE] Found %u inbox relays (kind 10050) for recipient %.8s",
                  dm_relays->len, ctx->recipient_pubkey);
        if (ctx->callback) ctx->callback(dm_relays, ctx->user_data);
        recipient_relay_ctx_free(ctx);
        return;
    }

    /* No kind 10050, fall back to NIP-65 read relays */
    g_debug("[DM_SERVICE] No kind 10050 for %.8s, trying NIP-65",
            ctx->recipient_pubkey);

    ctx->tried_10050 = TRUE;

    /* Try fetching NIP-65 (kind 10002) for read relays */
    gnostr_nip65_fetch_relays_async(
        ctx->recipient_pubkey,
        ctx->cancellable,
        on_recipient_nip65_done,
        ctx);
}

static void
on_recipient_nip65_done(GPtrArray *relays, gpointer user_data)
{
    RecipientRelayCtx *ctx = (RecipientRelayCtx*)user_data;
    if (!ctx) return;

    if (relays && relays->len > 0) {
        /* Get read relays from NIP-65 (where recipient reads from) */
        GPtrArray *read_relays = gnostr_nip65_get_read_relays(relays);
        g_ptr_array_unref(relays);

        if (read_relays->len > 0) {
            g_message("[DM_SERVICE] Found %u NIP-65 read relays for recipient %.8s",
                      read_relays->len, ctx->recipient_pubkey);
            if (ctx->callback) ctx->callback(read_relays, ctx->user_data);
            recipient_relay_ctx_free(ctx);
            return;
        }

        g_ptr_array_unref(read_relays);
    }

    /* No recipient relays found, fall back to local DM relays */
    g_message("[DM_SERVICE] No remote relays for %.8s, using local DM relays",
              ctx->recipient_pubkey);

    GPtrArray *local_dm_relays = gnostr_get_dm_relays();
    if (ctx->callback) ctx->callback(local_dm_relays, ctx->user_data);
    recipient_relay_ctx_free(ctx);
}

void
gnostr_dm_service_get_recipient_relays_async_internal(const char *recipient_pubkey,
                                                       GCancellable *cancellable,
                                                       GnostrDmRelaysCallback callback,
                                                       gpointer user_data)
{
    if (!recipient_pubkey || !*recipient_pubkey) {
        g_warning("[DM_SERVICE] Invalid recipient pubkey");
        if (callback) callback(NULL, user_data);
        return;
    }

    RecipientRelayCtx *ctx = g_new0(RecipientRelayCtx, 1);
    ctx->recipient_pubkey = g_strdup(recipient_pubkey);
    ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->tried_10050 = FALSE;

    /* First try kind 10050 (NIP-17 DM relays) */
    g_debug("[DM_SERVICE] Fetching inbox relays for %.8s", recipient_pubkey);

    gnostr_nip17_fetch_dm_relays_async(
        recipient_pubkey,
        cancellable,
        on_recipient_dm_relays_done,
        ctx);
}

/* ============== Send DM using NIP-59 Gift Wrap ============== */

typedef struct {
    GnostrDmService *service;   /* weak ref */
    char *recipient_pubkey;
    char *content;
    char *gift_wrap_json;       /* Signed gift wrap to publish */
    GCancellable *cancellable;
    GnostrDmSendCallback callback;
    gpointer user_data;
    guint relays_published;
    guint relays_failed;
} DmSendCtx;

static void dm_send_ctx_free(DmSendCtx *ctx) {
    if (!ctx) return;
    g_clear_pointer(&ctx->recipient_pubkey, g_free);
    g_clear_pointer(&ctx->content, g_free);
    g_clear_pointer(&ctx->gift_wrap_json, g_free);
    g_clear_object(&ctx->cancellable);
    g_free(ctx);
}

static void finish_dm_send_with_error(DmSendCtx *ctx, const char *msg) {
    GnostrDmSendResult *result = g_new0(GnostrDmSendResult, 1);
    result->success = FALSE;
    result->error_message = g_strdup(msg);

    if (ctx->callback) {
        ctx->callback(result, ctx->user_data);
    } else {
        gnostr_dm_send_result_free(result);
    }

    dm_send_ctx_free(ctx);
}

/* Worker thread data for async relay publishing */
typedef struct {
    NostrEvent *event;
    GPtrArray  *relay_urls;
    guint       success_count;
    guint       fail_count;
} DmRelayPublishData;

static void dm_relay_publish_data_free(DmRelayPublishData *d) {
    if (!d) return;
    if (d->event) nostr_event_free(d->event);
    if (d->relay_urls) g_ptr_array_unref(d->relay_urls);
    g_free(d);
}

/* Worker thread — connect+publish loop runs off main thread */
static void
dm_publish_thread(GTask *task, gpointer source_object,
                  gpointer task_data, GCancellable *cancellable)
{
    (void)source_object; (void)cancellable;
    DmRelayPublishData *d = (DmRelayPublishData *)task_data;

    for (guint i = 0; i < d->relay_urls->len; i++) {
        const char *url = (const char *)g_ptr_array_index(d->relay_urls, i);
        g_autoptr(GNostrRelay) relay = gnostr_relay_new(url);
        if (!relay) { d->fail_count++; continue; }

        GError *conn_err = NULL;
        if (!gnostr_relay_connect(relay, &conn_err)) {
            g_debug("[DM_SERVICE] Failed to connect to %s: %s",
                    url, conn_err ? conn_err->message : "unknown");
            g_clear_error(&conn_err);
            d->fail_count++;
            continue;
        }

        GError *pub_err = NULL;
        if (gnostr_relay_publish(relay, d->event, &pub_err)) {
            g_message("[DM_SERVICE] Published DM to %s", url);
            d->success_count++;
        } else {
            g_debug("[DM_SERVICE] Publish failed to %s: %s",
                    url, pub_err ? pub_err->message : "unknown");
            g_clear_error(&pub_err);
            d->fail_count++;
        }
    }

    g_task_return_boolean(task, d->success_count > 0);
}

/* Completion callback — runs on main thread */
static void
dm_publish_task_done(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    (void)source_object;
    DmSendCtx *ctx = (DmSendCtx *)user_data;

    GTask *task = G_TASK(res);
    DmRelayPublishData *d = g_task_get_task_data(task);

    GnostrDmSendResult *result = g_new0(GnostrDmSendResult, 1);
    if (d->success_count > 0) {
        result->success = TRUE;
        result->relays_published = d->success_count;
        g_message("[DM_SERVICE] DM sent successfully to %u relays (failed: %u)",
                  d->success_count, d->fail_count);
    } else {
        result->success = FALSE;
        result->error_message = g_strdup("Failed to publish to any relay");
        g_warning("[DM_SERVICE] DM send failed - no successful publishes");
    }

    if (ctx->callback) {
        ctx->callback(result, ctx->user_data);
    } else {
        gnostr_dm_send_result_free(result);
    }

    dm_send_ctx_free(ctx);
}

/* Step 3: Publish gift wrap to relays */
static void
on_dm_relays_fetched(GPtrArray *relays, gpointer user_data)
{
    DmSendCtx *ctx = (DmSendCtx *)user_data;

    if (!relays || relays->len == 0) {
        g_warning("[DM_SERVICE] No relays available for recipient");
        finish_dm_send_with_error(ctx, "No relays available for recipient");
        if (relays) g_ptr_array_unref(relays);
        return;
    }

    g_message("[DM_SERVICE] Publishing DM to %u relays", relays->len);

    /* Parse gift wrap event for publishing */
    NostrEvent *gift_wrap = nostr_event_new();
    if (!nostr_event_deserialize_compact(gift_wrap, ctx->gift_wrap_json, NULL)) {
        g_warning("[DM_SERVICE] Failed to parse gift wrap for publishing");
        nostr_event_free(gift_wrap);
        g_ptr_array_unref(relays);
        finish_dm_send_with_error(ctx, "Failed to parse gift wrap");
        return;
    }

    /* Move connect+publish loop to worker thread to avoid blocking UI */
    DmRelayPublishData *wd = g_new0(DmRelayPublishData, 1);
    wd->event = gift_wrap;    /* transfer ownership */
    wd->relay_urls = relays;  /* transfer ownership */

    GTask *task = g_task_new(NULL, NULL, dm_publish_task_done, ctx);
    g_task_set_task_data(task, wd, (GDestroyNotify)dm_relay_publish_data_free);
    g_task_run_in_thread(task, dm_publish_thread);
    g_object_unref(task);
}

/* Step 2: Gift wrap created - fetch recipient relays and publish */
static void
on_gift_wrap_created(GnostrGiftWrapResult *wrap_result, gpointer user_data)
{
    DmSendCtx *ctx = (DmSendCtx *)user_data;

    if (!wrap_result->success || !wrap_result->gift_wrap_json) {
        g_warning("[DM_SERVICE] Failed to create gift wrap: %s",
                  wrap_result->error_message ? wrap_result->error_message : "unknown");
        finish_dm_send_with_error(ctx, wrap_result->error_message ?
                                  wrap_result->error_message : "Failed to create gift wrap");
        gnostr_gift_wrap_result_free(wrap_result);
        return;
    }

    ctx->gift_wrap_json = g_strdup(wrap_result->gift_wrap_json);
    gnostr_gift_wrap_result_free(wrap_result);

    g_message("[DM_SERVICE] Gift wrap created, fetching recipient relays");

    /* Fetch recipient's DM relays */
    gnostr_dm_service_get_recipient_relays_async_internal(
        ctx->recipient_pubkey,
        ctx->cancellable,
        on_dm_relays_fetched,
        ctx);
}

void
gnostr_dm_service_send_dm_async_internal(GnostrDmService *self,
                                          const char *recipient_pubkey,
                                          const char *content,
                                          GCancellable *cancellable,
                                          GnostrDmSendCallback callback,
                                          gpointer user_data)
{
    g_message("[DM_SERVICE] Sending DM to %.8s", recipient_pubkey);

    /* Create rumor (unsigned kind 14 event) */
    NostrEvent *rumor = gnostr_nip59_create_dm_rumor(
        self->user_pubkey,
        recipient_pubkey,
        content);

    if (!rumor) {
        GnostrDmSendResult *result = g_new0(GnostrDmSendResult, 1);
        result->success = FALSE;
        result->error_message = g_strdup("Failed to create DM rumor");
        if (callback) callback(result, user_data);
        else gnostr_dm_send_result_free(result);
        return;
    }

    /* Create context */
    DmSendCtx *ctx = g_new0(DmSendCtx, 1);
    ctx->service = self;
    ctx->recipient_pubkey = g_strdup(recipient_pubkey);
    ctx->content = g_strdup(content);
    ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
    ctx->callback = callback;
    ctx->user_data = user_data;

    /* Create gift wrap asynchronously */
    gnostr_nip59_create_gift_wrap_async(
        rumor,
        recipient_pubkey,
        self->user_pubkey,
        cancellable,
        on_gift_wrap_created,
        ctx);

    nostr_event_free(rumor);
}

/* ============== Send File as Kind 15 ============== */

typedef struct {
    GnostrDmService *service;
    char *recipient_pubkey;
    char *file_path;
    GCancellable *cancellable;
    GnostrDmSendCallback callback;
    gpointer user_data;
} DmFileSendCtx;

static void dm_file_send_ctx_free(DmFileSendCtx *ctx) {
    if (!ctx) return;
    g_clear_pointer(&ctx->recipient_pubkey, g_free);
    g_clear_pointer(&ctx->file_path, g_free);
    g_clear_object(&ctx->cancellable);
    g_free(ctx);
}

/* Step 3: gift wrap created → fetch relays → publish (reuses on_dm_relays_fetched) */
static void
on_file_gift_wrap_created(GnostrGiftWrapResult *wrap_result, gpointer user_data)
{
    DmFileSendCtx *fctx = (DmFileSendCtx *)user_data;

    if (!wrap_result->success || !wrap_result->gift_wrap_json) {
        g_warning("[DM_SERVICE] File gift wrap failed: %s",
                  wrap_result->error_message ? wrap_result->error_message : "unknown");
        GnostrDmSendResult *result = g_new0(GnostrDmSendResult, 1);
        result->success = FALSE;
        result->error_message = g_strdup(wrap_result->error_message ?
                                          wrap_result->error_message : "Failed to create gift wrap");
        if (fctx->callback) fctx->callback(result, fctx->user_data);
        else gnostr_dm_send_result_free(result);
        gnostr_gift_wrap_result_free(wrap_result);
        dm_file_send_ctx_free(fctx);
        return;
    }

    /* Reuse DmSendCtx + on_dm_relays_fetched for relay publishing */
    DmSendCtx *ctx = g_new0(DmSendCtx, 1);
    ctx->service = fctx->service;
    ctx->recipient_pubkey = g_strdup(fctx->recipient_pubkey);
    ctx->content = g_strdup("[File attachment]");
    ctx->gift_wrap_json = g_strdup(wrap_result->gift_wrap_json);
    ctx->cancellable = fctx->cancellable ? g_object_ref(fctx->cancellable) : NULL;
    ctx->callback = fctx->callback;
    ctx->user_data = fctx->user_data;

    gnostr_gift_wrap_result_free(wrap_result);
    dm_file_send_ctx_free(fctx);

    g_message("[DM_SERVICE] File gift wrap created, fetching recipient relays");
    gnostr_dm_service_get_recipient_relays_async_internal(
        ctx->recipient_pubkey,
        ctx->cancellable,
        on_dm_relays_fetched,
        ctx);
}

/* Step 2: file uploaded → build kind 15 rumor → gift wrap */
static void
on_file_uploaded(GnostrDmFileAttachment *attachment, GError *error, gpointer user_data)
{
    DmFileSendCtx *fctx = (DmFileSendCtx *)user_data;

    if (error || !attachment) {
        g_warning("[DM_SERVICE] File upload failed: %s",
                  error ? error->message : "unknown");
        GnostrDmSendResult *result = g_new0(GnostrDmSendResult, 1);
        result->success = FALSE;
        result->error_message = g_strdup(error ? error->message : "File upload failed");
        if (fctx->callback) fctx->callback(result, fctx->user_data);
        else gnostr_dm_send_result_free(result);
        dm_file_send_ctx_free(fctx);
        return;
    }

    GnostrDmService *self = fctx->service;

    /* Build kind 15 rumor JSON */
    char *rumor_json = gnostr_dm_file_build_rumor_json(
        self->user_pubkey,
        fctx->recipient_pubkey,
        attachment,
        0); /* 0 = current time */

    gnostr_dm_file_attachment_free(attachment);

    if (!rumor_json) {
        g_warning("[DM_SERVICE] Failed to build file rumor JSON");
        GnostrDmSendResult *result = g_new0(GnostrDmSendResult, 1);
        result->success = FALSE;
        result->error_message = g_strdup("Failed to build file message");
        if (fctx->callback) fctx->callback(result, fctx->user_data);
        else gnostr_dm_send_result_free(result);
        dm_file_send_ctx_free(fctx);
        return;
    }

    /* Parse rumor JSON into NostrEvent */
    NostrEvent *rumor = nostr_event_new();
    if (!nostr_event_deserialize_compact(rumor, rumor_json, NULL)) {
        g_warning("[DM_SERVICE] Failed to parse file rumor JSON");
        nostr_event_free(rumor);
        g_free(rumor_json);
        GnostrDmSendResult *result = g_new0(GnostrDmSendResult, 1);
        result->success = FALSE;
        result->error_message = g_strdup("Failed to parse file rumor");
        if (fctx->callback) fctx->callback(result, fctx->user_data);
        else gnostr_dm_send_result_free(result);
        dm_file_send_ctx_free(fctx);
        return;
    }
    g_free(rumor_json);

    g_message("[DM_SERVICE] File uploaded, creating gift wrap for %.8s",
              fctx->recipient_pubkey);

    /* Gift wrap the kind 15 rumor */
    gnostr_nip59_create_gift_wrap_async(
        rumor,
        fctx->recipient_pubkey,
        self->user_pubkey,
        fctx->cancellable,
        on_file_gift_wrap_created,
        fctx);

    nostr_event_free(rumor);
}

void
gnostr_dm_service_send_file_async_internal(GnostrDmService *self,
                                            const char *recipient_pubkey,
                                            const char *file_path,
                                            GCancellable *cancellable,
                                            GnostrDmSendCallback callback,
                                            gpointer user_data)
{
    g_message("[DM_SERVICE] Sending file '%s' to %.8s", file_path, recipient_pubkey);

    DmFileSendCtx *fctx = g_new0(DmFileSendCtx, 1);
    fctx->service = self;
    fctx->recipient_pubkey = g_strdup(recipient_pubkey);
    fctx->file_path = g_strdup(file_path);
    fctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
    fctx->callback = callback;
    fctx->user_data = user_data;

    /* Step 1: Encrypt and upload file */
    gnostr_dm_file_encrypt_and_upload_async(file_path,
                                             NULL, /* auto-detect MIME */
                                             on_file_uploaded,
                                             fctx,
                                             cancellable);
}
