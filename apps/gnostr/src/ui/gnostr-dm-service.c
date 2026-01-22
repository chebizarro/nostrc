/**
 * GnostrDmService - NIP-17 Private Direct Message Processing
 *
 * Handles the gift wrap decryption flow:
 * Gift Wrap (1059) -> Seal (13) -> Rumor (14) -> Inbox
 */

#include "gnostr-dm-service.h"
#include "gnostr-dm-inbox-view.h"
#include "gnostr-profile-provider.h"
#include "../ipc/signer_ipc.h"
#include "../util/relays.h"
#include "nostr_simple_pool.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-kinds.h"
#include "nostr-json.h"
#include "nostr-tag.h"
#include <jansson.h>
#include <string.h>
#include <time.h>

/* Conversation state for a peer */
typedef struct {
    char *peer_pubkey;          /* Peer's public key */
    char *last_message;         /* Preview of last message */
    gint64 last_timestamp;      /* Timestamp of last message */
    guint unread_count;         /* Number of unread messages */
    gboolean last_is_outgoing;  /* TRUE if last message was sent by us */
    /* Profile info (cached from profile provider) */
    char *display_name;
    char *handle;
    char *avatar_url;
} DmConversation;

static void dm_conversation_free(DmConversation *conv) {
    if (!conv) return;
    g_free(conv->peer_pubkey);
    g_free(conv->last_message);
    g_free(conv->display_name);
    g_free(conv->handle);
    g_free(conv->avatar_url);
    g_free(conv);
}

struct _GnostrDmService {
    GObject parent_instance;

    /* Target inbox view (weak ref to avoid cycles) */
    GWeakRef inbox_ref;

    /* Current user's pubkey */
    char *user_pubkey;

    /* Conversations: peer_pubkey -> DmConversation* */
    GHashTable *conversations;

    /* Relay subscription */
    GnostrSimplePool *pool;
    GCancellable *cancellable;
    gulong events_handler;
    gboolean running;

    /* Pending decryptions: gift_wrap_id -> DecryptContext* */
    GHashTable *pending_decrypts;
};

G_DEFINE_TYPE(GnostrDmService, gnostr_dm_service, G_TYPE_OBJECT)

/* Forward declarations */
static void on_pool_gift_wrap_events(GnostrSimplePool *pool, GPtrArray *batch, gpointer user_data);
static void decrypt_gift_wrap_async(GnostrDmService *self, NostrEvent *gift_wrap);

static void
gnostr_dm_service_dispose(GObject *object)
{
    GnostrDmService *self = GNOSTR_DM_SERVICE(object);

    gnostr_dm_service_stop(self);

    g_weak_ref_clear(&self->inbox_ref);

    G_OBJECT_CLASS(gnostr_dm_service_parent_class)->dispose(object);
}

static void
gnostr_dm_service_finalize(GObject *object)
{
    GnostrDmService *self = GNOSTR_DM_SERVICE(object);

    g_free(self->user_pubkey);
    g_hash_table_destroy(self->conversations);
    g_hash_table_destroy(self->pending_decrypts);

    G_OBJECT_CLASS(gnostr_dm_service_parent_class)->finalize(object);
}

static void
gnostr_dm_service_class_init(GnostrDmServiceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = gnostr_dm_service_dispose;
    object_class->finalize = gnostr_dm_service_finalize;
}

static void
gnostr_dm_service_init(GnostrDmService *self)
{
    g_weak_ref_init(&self->inbox_ref, NULL);
    self->user_pubkey = NULL;
    self->conversations = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)dm_conversation_free);
    self->pending_decrypts = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL);
    self->pool = NULL;
    self->cancellable = NULL;
    self->events_handler = 0;
    self->running = FALSE;
}

GnostrDmService *
gnostr_dm_service_new(void)
{
    return g_object_new(GNOSTR_TYPE_DM_SERVICE, NULL);
}

void
gnostr_dm_service_set_inbox_view(GnostrDmService *self,
                                  GnostrDmInboxView *inbox)
{
    g_return_if_fail(GNOSTR_IS_DM_SERVICE(self));

    g_weak_ref_set(&self->inbox_ref, inbox);

    /* Set user pubkey on inbox if we have one */
    if (inbox && self->user_pubkey) {
        gnostr_dm_inbox_view_set_user_pubkey(inbox, self->user_pubkey);
    }
}

void
gnostr_dm_service_set_user_pubkey(GnostrDmService *self,
                                   const char *pubkey_hex)
{
    g_return_if_fail(GNOSTR_IS_DM_SERVICE(self));

    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);

    /* Update inbox if connected */
    GnostrDmInboxView *inbox = g_weak_ref_get(&self->inbox_ref);
    if (inbox) {
        gnostr_dm_inbox_view_set_user_pubkey(inbox, pubkey_hex);
        g_object_unref(inbox);
    }
}

static void
on_pool_subscribe_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GnostrDmService *self = GNOSTR_DM_SERVICE(user_data);
    GError *error = NULL;

    gnostr_simple_pool_subscribe_many_finish(GNOSTR_SIMPLE_POOL(source), res, &error);

    if (error) {
        g_warning("[DM_SERVICE] Gift wrap subscription failed: %s", error->message);
        g_clear_error(&error);
    } else {
        g_message("[DM_SERVICE] Gift wrap subscription started successfully");
    }

    if (self) g_object_unref(self);
}

void
gnostr_dm_service_start(GnostrDmService *self,
                         const char **relay_urls)
{
    g_return_if_fail(GNOSTR_IS_DM_SERVICE(self));
    g_return_if_fail(relay_urls != NULL);

    if (self->running) {
        g_warning("[DM_SERVICE] Already running, stopping first");
        gnostr_dm_service_stop(self);
    }

    if (!self->user_pubkey) {
        g_warning("[DM_SERVICE] Cannot start without user pubkey set");
        return;
    }

    /* Count relay URLs */
    size_t url_count = 0;
    while (relay_urls[url_count]) url_count++;

    if (url_count == 0) {
        g_warning("[DM_SERVICE] No relay URLs provided");
        return;
    }

    g_message("[DM_SERVICE] Starting gift wrap subscription to %zu relays", url_count);

    /* Create pool and cancellable */
    self->pool = gnostr_simple_pool_new();
    self->cancellable = g_cancellable_new();

    /* Build filter for gift wraps addressed to us (kind 1059 with p-tag) */
    NostrFilter *filter = nostr_filter_new();
    int kinds[] = { NOSTR_KIND_GIFT_WRAP };
    nostr_filter_set_kinds(filter, kinds, 1);

    /* Filter by p-tag for our pubkey */
    const char *p_tags[] = { self->user_pubkey };
    nostr_filter_tags_append(filter, "p", p_tags[0], NULL);

    NostrFilters *filters = nostr_filters_new();
    nostr_filters_add(filters, filter);

    /* Connect events signal */
    self->events_handler = g_signal_connect(
        self->pool, "events",
        G_CALLBACK(on_pool_gift_wrap_events), self);

    /* Start subscription */
    gnostr_simple_pool_subscribe_many_async(
        self->pool,
        relay_urls,
        url_count,
        filters,
        self->cancellable,
        on_pool_subscribe_done,
        g_object_ref(self));

    nostr_filters_free(filters);

    self->running = TRUE;

    /* Show loading state on inbox */
    GnostrDmInboxView *inbox = g_weak_ref_get(&self->inbox_ref);
    if (inbox) {
        gnostr_dm_inbox_view_set_loading(inbox, TRUE);
        g_object_unref(inbox);
    }
}

void
gnostr_dm_service_stop(GnostrDmService *self)
{
    g_return_if_fail(GNOSTR_IS_DM_SERVICE(self));

    if (!self->running) return;

    g_message("[DM_SERVICE] Stopping gift wrap subscription");

    if (self->cancellable) {
        g_cancellable_cancel(self->cancellable);
        g_clear_object(&self->cancellable);
    }

    if (self->pool) {
        if (self->events_handler > 0) {
            g_signal_handler_disconnect(self->pool, self->events_handler);
            self->events_handler = 0;
        }
        g_clear_object(&self->pool);
    }

    self->running = FALSE;
}

/* Truncate message for preview */
static char *
truncate_preview(const char *content, size_t max_len)
{
    if (!content || !*content) return g_strdup("");

    size_t len = strlen(content);
    if (len <= max_len) return g_strdup(content);

    /* Truncate and add ellipsis */
    char *preview = g_malloc(max_len + 4);
    memcpy(preview, content, max_len);
    preview[max_len] = '\0';

    /* Replace newlines with spaces */
    for (char *p = preview; *p; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    }

    strcat(preview, "...");
    return preview;
}

/* Get peer pubkey from rumor (the other party in the conversation) */
static char *
get_peer_pubkey_from_rumor(NostrEvent *rumor, const char *user_pubkey)
{
    if (!rumor || !user_pubkey) return NULL;

    const char *sender = nostr_event_get_pubkey(rumor);

    /* If we sent this message, peer is the p-tag recipient */
    if (sender && strcmp(sender, user_pubkey) == 0) {
        NostrTags *tags = nostr_event_get_tags(rumor);
        if (tags) {
            NostrTag *prefix = nostr_tag_new("p", NULL);
            NostrTag *ptag = nostr_tags_get_first(tags, prefix);
            nostr_tag_free(prefix);
            if (ptag && nostr_tag_size(ptag) >= 2) {
                return g_strdup(nostr_tag_get(ptag, 1));
            }
        }
        return NULL;
    }

    /* Otherwise, peer is the sender */
    return g_strdup(sender);
}

/* Update conversation state and inbox view */
static void
update_conversation(GnostrDmService *self,
                    const char *peer_pubkey,
                    const char *content,
                    gint64 timestamp,
                    gboolean is_outgoing,
                    gboolean increment_unread)
{
    if (!peer_pubkey || !content) return;

    DmConversation *conv = g_hash_table_lookup(self->conversations, peer_pubkey);

    /* Only update if newer or new conversation */
    if (conv && timestamp <= conv->last_timestamp) {
        /* Older message, might need to increment unread but don't change preview */
        if (increment_unread && !is_outgoing) {
            conv->unread_count++;
        }
    } else {
        if (!conv) {
            conv = g_new0(DmConversation, 1);
            conv->peer_pubkey = g_strdup(peer_pubkey);
            g_hash_table_insert(self->conversations, g_strdup(peer_pubkey), conv);
        }

        g_free(conv->last_message);
        conv->last_message = truncate_preview(content, 100);
        conv->last_timestamp = timestamp;
        conv->last_is_outgoing = is_outgoing;

        if (increment_unread && !is_outgoing) {
            conv->unread_count++;
        }

        /* Fetch profile info if not cached */
        if (!conv->display_name) {
            GnostrProfileMeta *meta = gnostr_profile_provider_get(peer_pubkey);
            if (meta) {
                g_free(conv->display_name);
                g_free(conv->handle);
                g_free(conv->avatar_url);
                conv->display_name = g_strdup(meta->display_name);
                conv->handle = g_strdup(meta->name);
                conv->avatar_url = g_strdup(meta->picture);
                gnostr_profile_meta_free(meta);
            }
        }
    }

    /* Update inbox view */
    GnostrDmInboxView *inbox = g_weak_ref_get(&self->inbox_ref);
    if (inbox) {
        GnostrDmConversation inbox_conv = {
            .peer_pubkey = conv->peer_pubkey,
            .display_name = conv->display_name,
            .handle = conv->handle,
            .avatar_url = conv->avatar_url,
            .last_message = conv->last_message,
            .last_timestamp = conv->last_timestamp,
            .unread_count = conv->unread_count,
            .is_outgoing = conv->last_is_outgoing,
        };
        gnostr_dm_inbox_view_upsert_conversation(inbox, &inbox_conv);
        g_object_unref(inbox);
    }
}

/* Context for async decryption */
typedef struct {
    GnostrDmService *service;   /* weak ref */
    char *gift_wrap_id;
    char *ephemeral_pubkey;     /* Gift wrap sender (ephemeral key) */
    char *encrypted_seal;       /* Encrypted seal content */
    /* After first decrypt: */
    char *seal_pubkey;          /* Seal sender (real sender) */
    char *encrypted_rumor;      /* Encrypted rumor content */
} DecryptContext;

static void decrypt_ctx_free(DecryptContext *ctx) {
    if (!ctx) return;
    g_free(ctx->gift_wrap_id);
    g_free(ctx->ephemeral_pubkey);
    g_free(ctx->encrypted_seal);
    g_free(ctx->seal_pubkey);
    g_free(ctx->encrypted_rumor);
    g_free(ctx);
}

/* Step 3: Process decrypted rumor and update inbox */
static void
on_rumor_decrypted(GObject *source, GAsyncResult *res, gpointer user_data)
{
    DecryptContext *ctx = (DecryptContext*)user_data;
    GError *error = NULL;

    if (!ctx || !ctx->service) {
        decrypt_ctx_free(ctx);
        return;
    }

    GnostrDmService *self = ctx->service;
    NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);

    char *rumor_json = NULL;
    gboolean ok = nostr_org_nostr_signer_call_nip44_decrypt_finish(
        proxy, &rumor_json, res, &error);

    /* Remove from pending */
    g_hash_table_remove(self->pending_decrypts, ctx->gift_wrap_id);

    if (!ok || !rumor_json) {
        g_warning("[DM_SERVICE] Failed to decrypt rumor: %s",
                  error ? error->message : "unknown");
        g_clear_error(&error);
        decrypt_ctx_free(ctx);
        return;
    }

    g_debug("[DM_SERVICE] Decrypted rumor: %.100s...", rumor_json);

    /* Parse rumor event */
    NostrEvent *rumor = nostr_event_new();
    if (!nostr_event_deserialize_compact(rumor, rumor_json)) {
        g_warning("[DM_SERVICE] Failed to parse rumor JSON");
        nostr_event_free(rumor);
        g_free(rumor_json);
        decrypt_ctx_free(ctx);
        return;
    }
    g_free(rumor_json);

    /* Validate: rumor kind should be 14 (DIRECT_MESSAGE) */
    if (nostr_event_get_kind(rumor) != NOSTR_KIND_DIRECT_MESSAGE) {
        g_warning("[DM_SERVICE] Invalid rumor kind: %d", nostr_event_get_kind(rumor));
        nostr_event_free(rumor);
        decrypt_ctx_free(ctx);
        return;
    }

    /* Validate: seal pubkey must match rumor pubkey (anti-spoofing) */
    const char *rumor_pubkey = nostr_event_get_pubkey(rumor);
    if (!rumor_pubkey || !ctx->seal_pubkey ||
        strcmp(rumor_pubkey, ctx->seal_pubkey) != 0) {
        g_warning("[DM_SERVICE] Pubkey mismatch: seal=%s rumor=%s",
                  ctx->seal_pubkey ? ctx->seal_pubkey : "(null)",
                  rumor_pubkey ? rumor_pubkey : "(null)");
        nostr_event_free(rumor);
        decrypt_ctx_free(ctx);
        return;
    }

    /* Extract message details */
    const char *content = nostr_event_get_content(rumor);
    gint64 created_at = (gint64)nostr_event_get_created_at(rumor);

    /* Determine peer and direction */
    char *peer_pubkey = get_peer_pubkey_from_rumor(rumor, self->user_pubkey);
    gboolean is_outgoing = (rumor_pubkey && self->user_pubkey &&
                            strcmp(rumor_pubkey, self->user_pubkey) == 0);

    if (peer_pubkey && content) {
        g_message("[DM_SERVICE] Received DM from %s: %.50s%s",
                  is_outgoing ? "self" : peer_pubkey,
                  content,
                  strlen(content) > 50 ? "..." : "");

        update_conversation(self, peer_pubkey, content, created_at,
                            is_outgoing, TRUE);
    }

    g_free(peer_pubkey);
    nostr_event_free(rumor);
    decrypt_ctx_free(ctx);

    /* Update inbox loading state */
    GnostrDmInboxView *inbox = g_weak_ref_get(&self->inbox_ref);
    if (inbox) {
        gnostr_dm_inbox_view_set_loading(inbox, FALSE);
        g_object_unref(inbox);
    }
}

/* Step 2: Decrypt seal content to get rumor */
static void
on_seal_decrypted(GObject *source, GAsyncResult *res, gpointer user_data)
{
    DecryptContext *ctx = (DecryptContext*)user_data;
    GError *error = NULL;

    if (!ctx || !ctx->service) {
        decrypt_ctx_free(ctx);
        return;
    }

    GnostrDmService *self = ctx->service;
    NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);

    char *seal_json = NULL;
    gboolean ok = nostr_org_nostr_signer_call_nip44_decrypt_finish(
        proxy, &seal_json, res, &error);

    if (!ok || !seal_json) {
        g_warning("[DM_SERVICE] Failed to decrypt seal: %s",
                  error ? error->message : "unknown");
        g_clear_error(&error);
        g_hash_table_remove(self->pending_decrypts, ctx->gift_wrap_id);
        decrypt_ctx_free(ctx);
        return;
    }

    g_debug("[DM_SERVICE] Decrypted seal: %.100s...", seal_json);

    /* Parse seal event */
    NostrEvent *seal = nostr_event_new();
    if (!nostr_event_deserialize_compact(seal, seal_json)) {
        g_warning("[DM_SERVICE] Failed to parse seal JSON");
        nostr_event_free(seal);
        g_free(seal_json);
        g_hash_table_remove(self->pending_decrypts, ctx->gift_wrap_id);
        decrypt_ctx_free(ctx);
        return;
    }
    g_free(seal_json);

    /* Validate seal kind */
    if (nostr_event_get_kind(seal) != NOSTR_KIND_SEAL) {
        g_warning("[DM_SERVICE] Invalid seal kind: %d", nostr_event_get_kind(seal));
        nostr_event_free(seal);
        g_hash_table_remove(self->pending_decrypts, ctx->gift_wrap_id);
        decrypt_ctx_free(ctx);
        return;
    }

    /* Verify seal signature */
    if (!nostr_event_check_signature(seal)) {
        g_warning("[DM_SERVICE] Invalid seal signature");
        nostr_event_free(seal);
        g_hash_table_remove(self->pending_decrypts, ctx->gift_wrap_id);
        decrypt_ctx_free(ctx);
        return;
    }

    /* Store seal pubkey (real sender) and encrypted rumor */
    ctx->seal_pubkey = g_strdup(nostr_event_get_pubkey(seal));
    ctx->encrypted_rumor = g_strdup(nostr_event_get_content(seal));

    nostr_event_free(seal);

    if (!ctx->seal_pubkey || !ctx->encrypted_rumor) {
        g_warning("[DM_SERVICE] Missing seal pubkey or content");
        g_hash_table_remove(self->pending_decrypts, ctx->gift_wrap_id);
        decrypt_ctx_free(ctx);
        return;
    }

    /* Decrypt rumor using seal sender's pubkey */
    nostr_org_nostr_signer_call_nip44_decrypt(
        proxy,
        ctx->encrypted_rumor,
        ctx->seal_pubkey,
        self->user_pubkey,
        NULL, /* GCancellable */
        on_rumor_decrypted,
        ctx);
}

/* Step 1: Start async decryption of gift wrap */
static void
decrypt_gift_wrap_async(GnostrDmService *self, NostrEvent *gift_wrap)
{
    const char *id = nostr_event_get_id(gift_wrap);
    const char *ephemeral_pk = nostr_event_get_pubkey(gift_wrap);
    const char *encrypted_content = nostr_event_get_content(gift_wrap);

    if (!id || !ephemeral_pk || !encrypted_content) {
        g_warning("[DM_SERVICE] Invalid gift wrap event");
        return;
    }

    /* Check if already processing */
    if (g_hash_table_contains(self->pending_decrypts, id)) {
        g_debug("[DM_SERVICE] Already processing gift wrap %.8s", id);
        return;
    }

    g_debug("[DM_SERVICE] Processing gift wrap %.8s from ephemeral key %.8s",
            id, ephemeral_pk);

    /* Get signer proxy */
    GError *error = NULL;
    NostrSignerProxy *proxy = gnostr_signer_proxy_get(&error);
    if (!proxy) {
        g_warning("[DM_SERVICE] Failed to get signer proxy: %s",
                  error ? error->message : "unknown");
        g_clear_error(&error);
        return;
    }

    /* Create decrypt context */
    DecryptContext *ctx = g_new0(DecryptContext, 1);
    ctx->service = self;
    ctx->gift_wrap_id = g_strdup(id);
    ctx->ephemeral_pubkey = g_strdup(ephemeral_pk);
    ctx->encrypted_seal = g_strdup(encrypted_content);

    /* Track pending decryption */
    g_hash_table_insert(self->pending_decrypts, g_strdup(id), ctx);

    /* Start async decrypt of gift wrap content */
    nostr_org_nostr_signer_call_nip44_decrypt(
        proxy,
        ctx->encrypted_seal,
        ctx->ephemeral_pubkey,
        self->user_pubkey,
        NULL, /* GCancellable */
        on_seal_decrypted,
        ctx);
}

/* Pool events callback for gift wraps */
static void
on_pool_gift_wrap_events(GnostrSimplePool *pool, GPtrArray *batch, gpointer user_data)
{
    (void)pool;
    GnostrDmService *self = GNOSTR_DM_SERVICE(user_data);

    if (!GNOSTR_IS_DM_SERVICE(self) || !batch) return;

    g_debug("[DM_SERVICE] Received %u gift wrap events", batch->len);

    for (guint i = 0; i < batch->len; i++) {
        NostrEvent *evt = (NostrEvent*)batch->pdata[i];
        if (!evt) continue;

        int kind = nostr_event_get_kind(evt);
        if (kind != NOSTR_KIND_GIFT_WRAP) continue;

        /* Validate gift wrap signature */
        if (!nostr_event_check_signature(evt)) {
            g_warning("[DM_SERVICE] Invalid gift wrap signature");
            continue;
        }

        decrypt_gift_wrap_async(self, evt);
    }
}

void
gnostr_dm_service_process_gift_wrap(GnostrDmService *self,
                                     const char *gift_wrap_json)
{
    g_return_if_fail(GNOSTR_IS_DM_SERVICE(self));
    g_return_if_fail(gift_wrap_json != NULL);

    NostrEvent *gift_wrap = nostr_event_new();
    if (!nostr_event_deserialize(gift_wrap, gift_wrap_json)) {
        g_warning("[DM_SERVICE] Failed to parse gift wrap JSON");
        nostr_event_free(gift_wrap);
        return;
    }

    if (nostr_event_get_kind(gift_wrap) != NOSTR_KIND_GIFT_WRAP) {
        g_warning("[DM_SERVICE] Event is not a gift wrap (kind %d)",
                  nostr_event_get_kind(gift_wrap));
        nostr_event_free(gift_wrap);
        return;
    }

    decrypt_gift_wrap_async(self, gift_wrap);
    nostr_event_free(gift_wrap);
}

guint
gnostr_dm_service_get_conversation_count(GnostrDmService *self)
{
    g_return_val_if_fail(GNOSTR_IS_DM_SERVICE(self), 0);
    return g_hash_table_size(self->conversations);
}

void
gnostr_dm_service_mark_read(GnostrDmService *self,
                             const char *peer_pubkey)
{
    g_return_if_fail(GNOSTR_IS_DM_SERVICE(self));
    g_return_if_fail(peer_pubkey != NULL);

    DmConversation *conv = g_hash_table_lookup(self->conversations, peer_pubkey);
    if (conv) {
        conv->unread_count = 0;

        /* Update inbox view */
        GnostrDmInboxView *inbox = g_weak_ref_get(&self->inbox_ref);
        if (inbox) {
            gnostr_dm_inbox_view_mark_read(inbox, peer_pubkey);
            g_object_unref(inbox);
        }
    }
}

void
gnostr_dm_service_start_with_dm_relays(GnostrDmService *self)
{
    g_return_if_fail(GNOSTR_IS_DM_SERVICE(self));

    /* Get DM-specific relays (falls back to general if none configured) */
    GPtrArray *dm_relays = gnostr_get_dm_relays();

    if (dm_relays->len == 0) {
        g_warning("[DM_SERVICE] No DM relays available");
        g_ptr_array_unref(dm_relays);
        return;
    }

    g_message("[DM_SERVICE] Starting with %u DM relays", dm_relays->len);

    /* Build NULL-terminated array for gnostr_dm_service_start */
    const char **urls = g_new0(const char*, dm_relays->len + 1);
    for (guint i = 0; i < dm_relays->len; i++) {
        urls[i] = (const char*)g_ptr_array_index(dm_relays, i);
    }

    gnostr_dm_service_start(self, urls);

    g_free(urls);
    g_ptr_array_unref(dm_relays);
}

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
    g_free(ctx->recipient_pubkey);
    if (ctx->cancellable) g_object_unref(ctx->cancellable);
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
gnostr_dm_service_get_recipient_relays_async(const char *recipient_pubkey,
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
