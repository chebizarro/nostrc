/**
 * GnostrDmService - NIP-17 Private Direct Message Processing
 *
 * Handles the gift wrap decryption flow:
 * Gift Wrap (1059) -> Seal (13) -> Rumor (14) -> Inbox
 */

#include "gnostr-dm-service.h"
#include "gnostr-dm-service-private.h"
#include "gnostr-dm-inbox-view.h"
#include "gnostr-dm-conversation-view.h"
#include "../util/dm_gift_wrap_validation.h"
#include "../util/utils.h"
#include <nostr-gobject-1.0/nostr_profile_provider.h>
#include "../ipc/gnostr-signer-service.h"
#include <nostr-gobject-1.0/gnostr-relays.h>
#include "../util/dm_files.h"
#include <nostr-gobject-1.0/nostr_pool.h>
#include <nostr-gobject-1.0/nostr_subscription.h>
#include <nostr-gobject-1.0/nostr_relay.h>
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-kinds.h"
#include "nostr-json.h"
#include "nostr-tag.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* DmConversation and struct _GnostrDmService are in gnostr-dm-service-private.h */

/* Max seconds to wait for EOSE before clearing the loading spinner */
#define DM_EOSE_TIMEOUT_SECONDS 15

static void dm_conversation_free(DmConversation *conv) {
    if (!conv) return;
    g_clear_pointer(&conv->peer_pubkey, g_free);
    g_clear_pointer(&conv->last_message, g_free);
    g_clear_pointer(&conv->display_name, g_free);
    g_clear_pointer(&conv->handle, g_free);
    g_clear_pointer(&conv->avatar_url, g_free);
    if (conv->messages) g_ptr_array_unref(conv->messages);
    if (conv->seen_event_ids) g_hash_table_destroy(conv->seen_event_ids);
    g_free(conv);
}

G_DEFINE_TYPE(GnostrDmService, gnostr_dm_service, G_TYPE_OBJECT)

/* Signals */
enum {
    SIGNAL_MESSAGE_RECEIVED,
    N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_pool_gift_wrap_event(GNostrSubscription *sub, const gchar *event_json, gpointer user_data);
static void on_pool_eose(GNostrSubscription *sub, gpointer user_data);
static gboolean on_loading_timeout(gpointer user_data);
static void dm_service_clear_loading(GnostrDmService *self);
static void decrypt_gift_wrap_async(GnostrDmService *self, NostrEvent *gift_wrap);
typedef struct _DecryptContext DecryptContext;
static void decrypt_ctx_free(DecryptContext *ctx);

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

    g_clear_pointer(&self->user_pubkey, g_free);
    g_clear_pointer(&self->conversations, g_hash_table_destroy);
    g_clear_pointer(&self->pending_decrypts, g_hash_table_destroy);

    G_OBJECT_CLASS(gnostr_dm_service_parent_class)->finalize(object);
}

static void
gnostr_dm_service_class_init(GnostrDmServiceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = gnostr_dm_service_dispose;
    object_class->finalize = gnostr_dm_service_finalize;

    /**
     * GnostrDmService::message-received:
     * @self: the DM service
     * @peer_pubkey: peer's public key (hex)
     * @content: message content (plaintext)
     * @created_at: unix timestamp
     * @is_outgoing: TRUE if sent by us
     *
     * Emitted when a new message is decrypted and stored.
     */
    signals[SIGNAL_MESSAGE_RECEIVED] = g_signal_new(
        "message-received",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2,
        G_TYPE_STRING,   /* peer_pubkey */
        G_TYPE_POINTER); /* GnostrDmMessage* (borrowed) */
}

static void
gnostr_dm_service_init(GnostrDmService *self)
{
    g_weak_ref_init(&self->inbox_ref, NULL);
    self->user_pubkey = NULL;
    self->conversations = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)dm_conversation_free);
    self->pending_decrypts = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)decrypt_ctx_free);
    self->pool = NULL;
    self->cancellable = NULL;
    self->events_handler = 0;
    self->eose_handler = 0;
    self->loading_timeout_id = 0;
    self->running = FALSE;
    self->eose_received = FALSE;
}

GnostrDmService *
gnostr_dm_service_new(void)
{
    return g_object_new(GNOSTR_TYPE_DM_SERVICE, NULL);
}

void
gnostr_dm_send_result_free(GnostrDmSendResult *result)
{
    if (!result)
        return;

    g_free(result->error_message);
    g_free(result);
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

    /* nostrc-akyz: defensively normalize npub/nprofile to hex */
    g_autofree gchar *hex = gnostr_ensure_hex_pubkey(pubkey_hex);
    if (!hex) return;

    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(hex);

    /* Update inbox if connected */
    GnostrDmInboxView *inbox = g_weak_ref_get(&self->inbox_ref);
    if (inbox) {
        gnostr_dm_inbox_view_set_user_pubkey(inbox, hex);
        g_object_unref(inbox);
    }
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
    self->pool = gnostr_pool_new();
    self->cancellable = g_cancellable_new();

    /* Add relays to pool */
    gnostr_pool_sync_relays(self->pool, (const gchar **)relay_urls, url_count);

    /* Build filter for gift wraps addressed to us (kind 1059 with p-tag) */
    NostrFilter *filter = nostr_filter_new();
    int kinds[] = { NOSTR_KIND_GIFT_WRAP };
    nostr_filter_set_kinds(filter, kinds, 1);

    /* Filter by p-tag for our pubkey */
    nostr_filter_tags_append(filter, "p", self->user_pubkey, NULL);

    NostrFilters *filters = nostr_filters_new();
    nostr_filters_add(filters, filter);

    /* Subscribe — gnostr_pool_subscribe takes ownership of filters on
     * success (stored as owned_filters in GNostrSubscription).  On failure,
     * filters are detached and ownership returns to the caller. */
    GError *sub_error = NULL;
    GNostrSubscription *sub = gnostr_pool_subscribe(self->pool, filters, &sub_error);
    nostr_filter_free(filter); /* safe: nostr_filters_add used move semantics */

    if (!sub) {
        nostr_filters_free(filters); /* caller retains ownership on failure */
        g_warning("[DM_SERVICE] Gift wrap subscription failed: %s",
                  sub_error ? sub_error->message : "(unknown)");
        g_clear_error(&sub_error);
        return;
    }
    /* filters now owned by subscription — do NOT free */

    self->sub = sub; /* takes ownership */
    self->events_handler = g_signal_connect(
        sub, "event",
        G_CALLBACK(on_pool_gift_wrap_event), self);
    self->eose_handler = g_signal_connect(
        sub, "eose",
        G_CALLBACK(on_pool_eose), self);

    g_message("[DM_SERVICE] Gift wrap subscription started successfully");
    self->running = TRUE;
    self->eose_received = FALSE;

    /* Show loading state on inbox */
    GnostrDmInboxView *inbox = g_weak_ref_get(&self->inbox_ref);
    if (inbox) {
        gnostr_dm_inbox_view_set_loading(inbox, TRUE);
        g_object_unref(inbox);
    }

    /* Safety-net timeout: clear loading if EOSE never arrives */
    if (self->loading_timeout_id > 0)
        g_source_remove(self->loading_timeout_id);
    self->loading_timeout_id = g_timeout_add_seconds_full(
        G_PRIORITY_DEFAULT,
        DM_EOSE_TIMEOUT_SECONDS,
        on_loading_timeout,
        g_object_ref(self),
        g_object_unref);
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

    /* Cancel loading timeout */
    if (self->loading_timeout_id > 0) {
        g_source_remove(self->loading_timeout_id);
        self->loading_timeout_id = 0;
    }

    if (self->sub) {
        if (self->events_handler > 0) {
            g_signal_handler_disconnect(self->sub, self->events_handler);
            self->events_handler = 0;
        }
        if (self->eose_handler > 0) {
            g_signal_handler_disconnect(self->sub, self->eose_handler);
            self->eose_handler = 0;
        }
        gnostr_subscription_close(self->sub);
        g_clear_object(&self->sub);
    }
    g_clear_object(&self->pool);

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

/* Compare messages by timestamp for sorting */
static gint
compare_dm_messages(gconstpointer a, gconstpointer b)
{
    const GnostrDmMessage *ma = *(const GnostrDmMessage **)a;
    const GnostrDmMessage *mb = *(const GnostrDmMessage **)b;
    if (ma->created_at < mb->created_at) return -1;
    if (ma->created_at > mb->created_at) return 1;
    return 0;
}

/* Store a message in the conversation's history.
 * Deduplicates by event_id, caps at DM_MAX_MESSAGES, keeps sorted by timestamp.
 * Returns TRUE if the message was actually stored (new, not a dup). */
static gboolean
store_message(DmConversation *conv,
              const char *event_id,
              const char *content,
              gint64 created_at,
              gboolean is_outgoing,
              GnostrDmFileMessage *file_msg)
{
    if (!conv || !content) return FALSE;

    /* Lazy-init storage */
    if (!conv->messages) {
        conv->messages = g_ptr_array_new_with_free_func(
            (GDestroyNotify)gnostr_dm_message_free);
    }
    if (!conv->seen_event_ids) {
        conv->seen_event_ids = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, NULL);
    }

    /* Dedup by event_id */
    if (event_id && g_hash_table_contains(conv->seen_event_ids, event_id)) {
        return FALSE;
    }

    GnostrDmMessage *msg = g_new0(GnostrDmMessage, 1);
    msg->event_id = event_id ? g_strdup(event_id) : NULL;
    msg->content = g_strdup(content);
    msg->created_at = created_at;
    msg->is_outgoing = is_outgoing;

    /* Copy file metadata if present */
    if (file_msg) {
        msg->file_url = g_strdup(file_msg->file_url);
        msg->file_type = g_strdup(file_msg->file_type);
        msg->decryption_key = g_strdup(file_msg->decryption_key_b64);
        msg->decryption_nonce = g_strdup(file_msg->decryption_nonce_b64);
        msg->original_hash = g_strdup(file_msg->original_hash);
        msg->file_size = file_msg->size;
    }

    g_ptr_array_add(conv->messages, msg);

    if (event_id) {
        g_hash_table_insert(conv->seen_event_ids, g_strdup(event_id),
                            GINT_TO_POINTER(TRUE));
    }

    /* Sort by timestamp */
    g_ptr_array_sort(conv->messages, compare_dm_messages);

    /* Cap at DM_MAX_MESSAGES (remove oldest) */
    while (conv->messages->len > DM_MAX_MESSAGES) {
        GnostrDmMessage *oldest = g_ptr_array_index(conv->messages, 0);
        if (oldest->event_id) {
            g_hash_table_remove(conv->seen_event_ids, oldest->event_id);
        }
        g_ptr_array_remove_index(conv->messages, 0);
    }

    return TRUE;
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
struct _DecryptContext {
    GnostrDmService *service;   /* weak ref */
    char *gift_wrap_id;
    char *outer_pubkey;         /* Gift wrap signer pubkey from the outer event */
    char *encrypted_seal;       /* Encrypted seal content */
    /* After first decrypt: */
    char *seal_pubkey;          /* Seal sender (real sender) */
    char *encrypted_rumor;      /* Encrypted rumor content */
};

static void decrypt_ctx_free(DecryptContext *ctx) {
    if (!ctx) return;
    g_clear_pointer(&ctx->gift_wrap_id, g_free);
    g_clear_pointer(&ctx->outer_pubkey, g_free);
    g_clear_pointer(&ctx->encrypted_seal, g_free);
    g_clear_pointer(&ctx->seal_pubkey, g_free);
    g_clear_pointer(&ctx->encrypted_rumor, g_free);
    g_free(ctx);
}

/* Step 3: Process decrypted rumor and update inbox */
static void
on_rumor_decrypted(GnostrSignerService *service, const char *rumor_json,
                   GError *error, gpointer user_data)
{
    (void)service;
    DecryptContext *ctx = (DecryptContext*)user_data;

    if (!ctx || !ctx->service) {
        decrypt_ctx_free(ctx);
        return;
    }

    GnostrDmService *self = ctx->service;

    /* Steal from pending (suppress destroy func) — we still need ctx fields below */
    {
        gpointer stolen_key = NULL;
        g_hash_table_steal_extended(self->pending_decrypts, ctx->gift_wrap_id,
                                     &stolen_key, NULL);
        g_free(stolen_key);
    }

    if (!rumor_json) {
        g_warning("[DM_SERVICE] Failed to decrypt rumor: %s",
                  error ? error->message : "unknown");
        decrypt_ctx_free(ctx);
        return;
    }

    g_debug("[DM_SERVICE] Decrypted rumor: %.100s...", rumor_json);

    /* Parse rumor event */
    NostrEvent *rumor = nostr_event_new();
    if (!nostr_event_deserialize_compact(rumor, rumor_json, NULL)) {
        g_warning("[DM_SERVICE] Failed to parse rumor JSON");
        nostr_event_free(rumor);
        decrypt_ctx_free(ctx);
        return;
    }

    /* Validate: rumor kind should be 14 (DIRECT_MESSAGE) or 15 (FILE_MESSAGE) */
    int rumor_kind = nostr_event_get_kind(rumor);
    if (rumor_kind != NOSTR_KIND_DIRECT_MESSAGE && rumor_kind != NOSTR_KIND_FILE_MESSAGE) {
        g_warning("[DM_SERVICE] Invalid rumor kind: %d", rumor_kind);
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

    if (peer_pubkey) {
        /* For file messages (kind 15), show a file attachment indicator */
        const char *display_content = content;
        char *file_preview = NULL;

        if (rumor_kind == NOSTR_KIND_FILE_MESSAGE) {
            /* Extract file-type tag if available for better preview */
            NostrTags *tags = nostr_event_get_tags(rumor);
            const char *file_type = NULL;
            if (tags) {
                NostrTag *prefix = nostr_tag_new("file-type", NULL);
                NostrTag *ft_tag = nostr_tags_get_first(tags, prefix);
                nostr_tag_free(prefix);
                if (ft_tag && nostr_tag_size(ft_tag) >= 2) {
                    file_type = nostr_tag_get(ft_tag, 1);
                }
            }

            if (file_type && g_str_has_prefix(file_type, "image/")) {
                file_preview = g_strdup("[Image attachment]");
            } else if (file_type && g_str_has_prefix(file_type, "video/")) {
                file_preview = g_strdup("[Video attachment]");
            } else if (file_type && g_str_has_prefix(file_type, "audio/")) {
                file_preview = g_strdup("[Audio attachment]");
            } else {
                file_preview = g_strdup("[File attachment]");
            }
            display_content = file_preview;

            g_message("[DM_SERVICE] Received file message from %s: %s (type=%s)",
                      is_outgoing ? "self" : peer_pubkey,
                      content ? content : "(no url)",
                      file_type ? file_type : "unknown");
        } else if (content) {
            g_message("[DM_SERVICE] Received DM from %s: %.50s%s",
                      is_outgoing ? "self" : peer_pubkey,
                      content,
                      strlen(content) > 50 ? "..." : "");
        }

        if (display_content) {
            update_conversation(self, peer_pubkey, display_content, created_at,
                                is_outgoing, TRUE);
        }

        /* Store message in conversation history */
        char *rumor_id = nostr_event_get_id(rumor);
        DmConversation *conv = g_hash_table_lookup(self->conversations, peer_pubkey);
        if (conv && display_content) {
            /* Parse kind 15 file metadata if applicable */
            GnostrDmFileMessage *file_msg = NULL;
            if (rumor_kind == NOSTR_KIND_FILE_MESSAGE) {
                char *rumor_json_str = nostr_event_serialize_compact(rumor);
                if (rumor_json_str) {
                    file_msg = gnostr_dm_file_parse_message(rumor_json_str);
                    g_free(rumor_json_str);
                }
            }

            gboolean stored = store_message(conv, rumor_id, display_content,
                                            created_at, is_outgoing, file_msg);
            if (stored) {
                /* Find the just-stored message to pass via signal */
                GnostrDmMessage *stored_msg = NULL;
                if (conv->messages && conv->messages->len > 0) {
                    for (guint i = conv->messages->len; i > 0; i--) {
                        GnostrDmMessage *m = g_ptr_array_index(conv->messages, i - 1);
                        if (m->created_at == created_at &&
                            m->is_outgoing == is_outgoing) {
                            stored_msg = m;
                            break;
                        }
                    }
                }
                if (stored_msg) {
                    g_signal_emit(self, signals[SIGNAL_MESSAGE_RECEIVED], 0,
                                  peer_pubkey, stored_msg);
                }
            }

            if (file_msg) gnostr_dm_file_message_free(file_msg);
        }

        free(rumor_id);
        g_free(file_preview);
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
on_seal_decrypted(GnostrSignerService *service, const char *seal_json,
                  GError *error, gpointer user_data)
{
    DecryptContext *ctx = (DecryptContext*)user_data;

    if (!ctx || !ctx->service) {
        decrypt_ctx_free(ctx);
        return;
    }

    GnostrDmService *self = ctx->service;

    if (!seal_json) {
        g_warning("[DM_SERVICE] Failed to decrypt seal: %s",
                  error ? error->message : "unknown");
        /* g_hash_table_remove frees ctx via destroy func */
        g_hash_table_remove(self->pending_decrypts, ctx->gift_wrap_id);
        return;
    }

    g_debug("[DM_SERVICE] Decrypted seal: %.100s...", seal_json);

    /* Parse seal event */
    NostrEvent *seal = nostr_event_new();
    if (!nostr_event_deserialize_compact(seal, seal_json, NULL)) {
        g_warning("[DM_SERVICE] Failed to parse seal JSON");
        nostr_event_free(seal);
        g_hash_table_remove(self->pending_decrypts, ctx->gift_wrap_id);
        return;
    }

    /* Validate seal kind */
    if (nostr_event_get_kind(seal) != NOSTR_KIND_SEAL) {
        g_warning("[DM_SERVICE] Invalid seal kind: %d", nostr_event_get_kind(seal));
        nostr_event_free(seal);
        g_hash_table_remove(self->pending_decrypts, ctx->gift_wrap_id);
        return;
    }

    /* Verify seal signature */
    if (!nostr_event_check_signature(seal)) {
        g_warning("[DM_SERVICE] Invalid seal signature");
        nostr_event_free(seal);
        g_hash_table_remove(self->pending_decrypts, ctx->gift_wrap_id);
        return;
    }

    /* Store seal pubkey (real sender) and encrypted rumor */
    ctx->seal_pubkey = g_strdup(nostr_event_get_pubkey(seal));
    ctx->encrypted_rumor = g_strdup(nostr_event_get_content(seal));

    nostr_event_free(seal);

    if (!ctx->seal_pubkey || !ctx->encrypted_rumor) {
        g_warning("[DM_SERVICE] Missing seal pubkey or content");
        g_hash_table_remove(self->pending_decrypts, ctx->gift_wrap_id);
        return;
    }

    /* Decrypt rumor using seal sender's pubkey.
     * nostrc-dbus1: Use signer service abstraction — works with both
     * NIP-46 (remote signer) and NIP-55L (local D-Bus signer). */
    gnostr_signer_service_nip44_decrypt_async(
        service,
        ctx->seal_pubkey,
        ctx->encrypted_rumor,
        NULL, /* GCancellable */
        on_rumor_decrypted,
        ctx);
}

/* Step 1: Start async decryption of gift wrap */
static void
decrypt_gift_wrap_async(GnostrDmService *self, NostrEvent *gift_wrap)
{
    char *id = nostr_event_get_id(gift_wrap);
    const char *outer_pk = nostr_event_get_pubkey(gift_wrap);
    const char *encrypted_content = nostr_event_get_content(gift_wrap);

    if (!id || !outer_pk || !encrypted_content) {
        g_warning("[DM_SERVICE] Invalid gift wrap event");
        free(id);
        return;
    }

    /* Check if already processing */
    if (g_hash_table_contains(self->pending_decrypts, id)) {
        g_debug("[DM_SERVICE] Already processing gift wrap %.8s", id);
        free(id);
        return;
    }

    g_debug("[DM_SERVICE] Processing gift wrap %.8s from outer signer %.8s",
            id, outer_pk);

    /* nostrc-dbus1: Check signer availability through the signer service
     * abstraction, which handles both NIP-46 and NIP-55L methods. */
    GnostrSignerService *signer = gnostr_signer_service_get_default();
    if (!gnostr_signer_service_is_available(signer)) {
        g_debug("[DM_SERVICE] No signer connected, skipping gift wrap %.8s", id);
        free(id);
        return;
    }

    /* Create decrypt context */
    DecryptContext *ctx = g_new0(DecryptContext, 1);
    ctx->service = self;
    ctx->gift_wrap_id = g_strdup(id);
    ctx->outer_pubkey = g_strdup(outer_pk);
    ctx->encrypted_seal = g_strdup(encrypted_content);

    /* Track pending decryption */
    g_hash_table_insert(self->pending_decrypts, g_strdup(id), ctx);

    /* Start async decrypt of gift wrap content using signer service.
     * This works with whichever signing method is active (NIP-46 or NIP-55L). */
    gnostr_signer_service_nip44_decrypt_async(
        signer,
        ctx->outer_pubkey,
        ctx->encrypted_seal,
        NULL, /* GCancellable */
        on_seal_decrypted,
        ctx);
    free(id);
}

/* Clear the loading spinner on the inbox and cancel the safety timeout. */
static void
dm_service_clear_loading(GnostrDmService *self)
{
    if (self->loading_timeout_id > 0) {
        g_source_remove(self->loading_timeout_id);
        self->loading_timeout_id = 0;
    }

    GnostrDmInboxView *inbox = g_weak_ref_get(&self->inbox_ref);
    if (inbox) {
        gnostr_dm_inbox_view_set_loading(inbox, FALSE);
        g_object_unref(inbox);
    }
}

/* EOSE callback: relay has delivered all stored events.
 * Clear loading state so user sees the conversation list or the empty state. */
static void
on_pool_eose(GNostrSubscription *sub, gpointer user_data)
{
    (void)sub;
    GnostrDmService *self = GNOSTR_DM_SERVICE(user_data);
    if (!GNOSTR_IS_DM_SERVICE(self)) return;

    if (self->eose_received) return;  /* already handled */
    self->eose_received = TRUE;

    g_message("[DM_SERVICE] EOSE received — clearing loading state (%u conversations)",
              g_hash_table_size(self->conversations));

    dm_service_clear_loading(self);
}

/* Safety-net timeout: if EOSE never arrives after 15 seconds, stop the spinner. */
static gboolean
on_loading_timeout(gpointer user_data)
{
    GnostrDmService *self = GNOSTR_DM_SERVICE(user_data);
    if (!GNOSTR_IS_DM_SERVICE(self))
        return G_SOURCE_REMOVE;

    self->loading_timeout_id = 0;  /* source is being removed */

    if (self->eose_received) return G_SOURCE_REMOVE;  /* already cleared */

    g_warning("[DM_SERVICE] Loading timeout — clearing spinner (EOSE not received)");

    self->eose_received = TRUE;
    dm_service_clear_loading(self);

    return G_SOURCE_REMOVE;
}

/* Subscription event callback for gift wraps */
static void
on_pool_gift_wrap_event(GNostrSubscription *sub, const gchar *event_json, gpointer user_data)
{
    (void)sub;
    GnostrDmService *self = GNOSTR_DM_SERVICE(user_data);

    if (!GNOSTR_IS_DM_SERVICE(self) || !event_json) return;

    NostrEvent *evt = nostr_event_new();
    if (!evt || nostr_event_deserialize(evt, event_json) != 0) {
        if (evt) nostr_event_free(evt);
        return;
    }

    int kind = nostr_event_get_kind(evt);
    if (kind != NOSTR_KIND_GIFT_WRAP) {
        nostr_event_free(evt);
        return;
    }

    /* Validate gift wrap signature */
    if (!nostr_event_check_signature(evt)) {
        g_warning("[DM_SERVICE] Invalid gift wrap signature");
        nostr_event_free(evt);
        return;
    }

    decrypt_gift_wrap_async(self, evt);
    nostr_event_free(evt);
}

void
gnostr_dm_service_process_gift_wrap(GnostrDmService *self,
                                     const char *gift_wrap_json)
{
    g_return_if_fail(GNOSTR_IS_DM_SERVICE(self));
    g_return_if_fail(gift_wrap_json != NULL);

    NostrEvent *gift_wrap = NULL;
    g_autofree gchar *reason = NULL;
    if (!gnostr_dm_gift_wrap_parse_for_processing(gift_wrap_json, &gift_wrap, &reason)) {
        g_warning("[DM_SERVICE] Failed to parse gift wrap JSON: %s",
                  reason ? reason : "unknown error");
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
        g_warning("[DM_SERVICE] No DM relays available — showing empty state");
        g_ptr_array_unref(dm_relays);

        /* Clear loading on inbox so user sees empty state instead of spinner */
        GnostrDmInboxView *inbox = g_weak_ref_get(&self->inbox_ref);
        if (inbox) {
            gnostr_dm_inbox_view_set_loading(inbox, FALSE);
            g_object_unref(inbox);
        }
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

/* ============== Public Send/Relay API → delegates to gnostr-dm-service-send.c ============== */

void
gnostr_dm_service_get_recipient_relays_async(const char *recipient_pubkey,
                                              GCancellable *cancellable,
                                              GnostrDmRelaysCallback callback,
                                              gpointer user_data)
{
    gnostr_dm_service_get_recipient_relays_async_internal(
        recipient_pubkey, cancellable, callback, user_data);
}

void
gnostr_dm_service_send_dm_async(GnostrDmService *self,
                                 const char *recipient_pubkey,
                                 const char *content,
                                 GCancellable *cancellable,
                                 GnostrDmSendCallback callback,
                                 gpointer user_data)
{
    g_return_if_fail(GNOSTR_IS_DM_SERVICE(self));
    g_return_if_fail(recipient_pubkey != NULL);
    g_return_if_fail(content != NULL);

    if (!self->user_pubkey) {
        GnostrDmSendResult *result = g_new0(GnostrDmSendResult, 1);
        result->success = FALSE;
        result->error_message = g_strdup("User not logged in");
        if (callback) callback(result, user_data);
        else gnostr_dm_send_result_free(result);
        return;
    }

    gnostr_dm_service_send_dm_async_internal(self, recipient_pubkey, content,
                                              cancellable, callback, user_data);
}

void
gnostr_dm_service_send_file_async(GnostrDmService *self,
                                    const char *recipient_pubkey,
                                    const char *file_path,
                                    GCancellable *cancellable,
                                    GnostrDmSendCallback callback,
                                    gpointer user_data)
{
    g_return_if_fail(GNOSTR_IS_DM_SERVICE(self));
    g_return_if_fail(recipient_pubkey != NULL);
    g_return_if_fail(file_path != NULL);

    if (!self->user_pubkey) {
        GnostrDmSendResult *result = g_new0(GnostrDmSendResult, 1);
        result->success = FALSE;
        result->error_message = g_strdup("User not logged in");
        if (callback) callback(result, user_data);
        else gnostr_dm_send_result_free(result);
        return;
    }

    gnostr_dm_service_send_file_async_internal(self, recipient_pubkey, file_path,
                                                cancellable, callback, user_data);
}

/* ============== Message History API ============== */

GPtrArray *
gnostr_dm_service_get_messages(GnostrDmService *self,
                                const char *peer_pubkey)
{
    g_return_val_if_fail(GNOSTR_IS_DM_SERVICE(self), NULL);
    g_return_val_if_fail(peer_pubkey != NULL, NULL);

    DmConversation *conv = g_hash_table_lookup(self->conversations, peer_pubkey);
    if (!conv) return NULL;

    return conv->messages;
}

void
gnostr_dm_service_load_history_async(GnostrDmService *self,
                                      const char *peer_pubkey,
                                      GnostrDmHistoryCallback callback,
                                      gpointer user_data)
{
    g_return_if_fail(GNOSTR_IS_DM_SERVICE(self));
    g_return_if_fail(peer_pubkey != NULL);

    DmConversation *conv = g_hash_table_lookup(self->conversations, peer_pubkey);

    /* If we have cached messages, return immediately */
    if (conv && conv->messages && conv->messages->len > 0) {
        g_debug("[DM_SERVICE] Returning %u cached messages for %.8s",
                conv->messages->len, peer_pubkey);
        if (callback) callback(conv->messages, user_data);
        return;
    }

    /* Load historical gift wraps from nostrdb (once per service lifetime).
     * NIP-17: can't filter by peer pre-decryption — sender is encrypted.
     * So we load ALL gift wraps for our user and let decryption sort them. */
    if (!self->history_loaded && self->user_pubkey) {
        self->history_loaded = TRUE;

        g_autofree char *filter_json = g_strdup_printf(
            "{\"kinds\":[1059],\"#p\":[\"%s\"],\"limit\":200}",
            self->user_pubkey);

        void *txn = NULL;
        if (storage_ndb_begin_query(&txn, NULL) == 0 && txn) {
            char **results = NULL;
            int count = 0;

            if (storage_ndb_query(txn, filter_json, &results, &count, NULL) == 0 && count > 0) {
                g_message("[DM_SERVICE] Loading %d historical gift wraps from nostrdb", count);

                for (int i = 0; i < count; i++) {
                    if (results[i]) {
                        gnostr_dm_service_process_gift_wrap(self, results[i]);
                    }
                }
                storage_ndb_free_results(results, count);
            } else {
                g_debug("[DM_SERVICE] No historical gift wraps in nostrdb");
            }

            storage_ndb_end_query(txn);
        }
    }

    /* Return whatever we have now — more messages will arrive via
     * the "message-received" signal as async decryptions complete */
    conv = g_hash_table_lookup(self->conversations, peer_pubkey);
    if (conv && conv->messages && conv->messages->len > 0) {
        if (callback) callback(conv->messages, user_data);
    } else {
        if (callback) callback(NULL, user_data);
    }
}
