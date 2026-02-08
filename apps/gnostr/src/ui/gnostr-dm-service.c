/**
 * GnostrDmService - NIP-17 Private Direct Message Processing
 *
 * Handles the gift wrap decryption flow:
 * Gift Wrap (1059) -> Seal (13) -> Rumor (14) -> Inbox
 */

#include "gnostr-dm-service.h"
#include "gnostr-dm-inbox-view.h"
#include "gnostr-dm-conversation-view.h"
#include "gnostr-profile-provider.h"
#include "../ipc/signer_ipc.h"
#include "../util/relays.h"
#include "../util/dm_files.h"
#include "nostr_pool.h"
#include "nostr_subscription.h"
#include "nostr_relay.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-kinds.h"
#include "nostr-json.h"
#include "nostr-tag.h"
#include "../storage_ndb.h"
#include <string.h>
#include <time.h>

/* Maximum messages to keep per conversation */
#define DM_MAX_MESSAGES 100

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
    /* Message history */
    GPtrArray *messages;        /* GnostrDmMessage*, sorted by created_at */
    GHashTable *seen_event_ids; /* event_id -> TRUE, for dedup */
} DmConversation;

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

struct _GnostrDmService {
    GObject parent_instance;

    /* Target inbox view (weak ref to avoid cycles) */
    GWeakRef inbox_ref;

    /* Current user's pubkey */
    char *user_pubkey;

    /* Conversations: peer_pubkey -> DmConversation* */
    GHashTable *conversations;

    /* Relay subscription */
    GNostrPool       *pool;
    GNostrSubscription *sub;
    GCancellable *cancellable;
    gulong events_handler;
    gboolean running;

    /* Pending decryptions: gift_wrap_id -> DecryptContext* */
    GHashTable *pending_decrypts;

    /* Whether historical gift wraps have been loaded from nostrdb */
    gboolean history_loaded;
};

G_DEFINE_TYPE(GnostrDmService, gnostr_dm_service, G_TYPE_OBJECT)

/* Signals */
enum {
    SIGNAL_MESSAGE_RECEIVED,
    N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_pool_gift_wrap_event(GNostrSubscription *sub, const gchar *event_json, gpointer user_data);
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

    /* Subscribe */
    GError *sub_error = NULL;
    GNostrSubscription *sub = gnostr_pool_subscribe(self->pool, filters, &sub_error);
    nostr_filters_free(filters);
    nostr_filter_free(filter);

    if (!sub) {
        g_warning("[DM_SERVICE] Gift wrap subscription failed: %s",
                  sub_error ? sub_error->message : "(unknown)");
        g_clear_error(&sub_error);
        return;
    }

    self->sub = sub; /* takes ownership */
    self->events_handler = g_signal_connect(
        sub, "event",
        G_CALLBACK(on_pool_gift_wrap_event), self);

    g_message("[DM_SERVICE] Gift wrap subscription started successfully");
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

    if (self->sub) {
        if (self->events_handler > 0) {
            g_signal_handler_disconnect(self->sub, self->events_handler);
            self->events_handler = 0;
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
    char *ephemeral_pubkey;     /* Gift wrap sender (ephemeral key) */
    char *encrypted_seal;       /* Encrypted seal content */
    /* After first decrypt: */
    char *seal_pubkey;          /* Seal sender (real sender) */
    char *encrypted_rumor;      /* Encrypted rumor content */
};

static void decrypt_ctx_free(DecryptContext *ctx) {
    if (!ctx) return;
    g_clear_pointer(&ctx->gift_wrap_id, g_free);
    g_clear_pointer(&ctx->ephemeral_pubkey, g_free);
    g_clear_pointer(&ctx->encrypted_seal, g_free);
    g_clear_pointer(&ctx->seal_pubkey, g_free);
    g_clear_pointer(&ctx->encrypted_rumor, g_free);
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
    if (!nostr_event_deserialize_compact(rumor, rumor_json, NULL)) {
        g_warning("[DM_SERVICE] Failed to parse rumor JSON");
        nostr_event_free(rumor);
        g_free(rumor_json);
        decrypt_ctx_free(ctx);
        return;
    }
    g_free(rumor_json);

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
        const char *rumor_id = nostr_event_get_id(rumor);
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
    if (!nostr_event_deserialize_compact(seal, seal_json, NULL)) {
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

/* ============== Send DM using NIP-59 Gift Wrap ============== */

#include "../util/nip59_giftwrap.h"

void gnostr_dm_send_result_free(GnostrDmSendResult *result) {
    if (!result) return;
    g_free(result->error_message);
    g_free(result);
}

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

    /* Publish to each relay */
    ctx->relays_published = 0;
    ctx->relays_failed = 0;

    for (guint i = 0; i < relays->len; i++) {
        const char *url = (const char *)g_ptr_array_index(relays, i);
        GNostrRelay *relay = gnostr_relay_new(url);
        if (!relay) {
            ctx->relays_failed++;
            continue;
        }

        GError *conn_err = NULL;
        if (!gnostr_relay_connect(relay, &conn_err)) {
            g_debug("[DM_SERVICE] Failed to connect to %s: %s",
                    url, conn_err ? conn_err->message : "unknown");
            g_clear_error(&conn_err);
            g_object_unref(relay);
            ctx->relays_failed++;
            continue;
        }

        GError *pub_err = NULL;
        if (gnostr_relay_publish(relay, gift_wrap, &pub_err)) {
            g_message("[DM_SERVICE] Published DM to %s", url);
            ctx->relays_published++;
        } else {
            g_debug("[DM_SERVICE] Publish failed to %s: %s",
                    url, pub_err ? pub_err->message : "unknown");
            g_clear_error(&pub_err);
            ctx->relays_failed++;
        }
        g_object_unref(relay);
    }

    nostr_event_free(gift_wrap);
    g_ptr_array_unref(relays);

    /* Check result */
    GnostrDmSendResult *result = g_new0(GnostrDmSendResult, 1);
    if (ctx->relays_published > 0) {
        result->success = TRUE;
        result->relays_published = ctx->relays_published;
        g_message("[DM_SERVICE] DM sent successfully to %u relays (failed: %u)",
                  ctx->relays_published, ctx->relays_failed);
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
    gnostr_dm_service_get_recipient_relays_async(
        ctx->recipient_pubkey,
        ctx->cancellable,
        on_dm_relays_fetched,
        ctx);
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
    gnostr_dm_service_get_recipient_relays_async(
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
        if (storage_ndb_begin_query_retry(&txn, 3, 50) == 0 && txn) {
            char **results = NULL;
            int count = 0;

            if (storage_ndb_query(txn, filter_json, &results, &count) == 0 && count > 0) {
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
