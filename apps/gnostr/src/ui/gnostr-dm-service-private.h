#ifndef GNOSTR_DM_SERVICE_PRIVATE_H
#define GNOSTR_DM_SERVICE_PRIVATE_H

#include "gnostr-dm-service.h"
#include "gnostr-dm-inbox-view.h"
#include <nostr-gobject-1.0/nostr_pool.h>
#include <nostr-gobject-1.0/nostr_subscription.h>

G_BEGIN_DECLS

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

/* --- Send module entry points (gnostr-dm-service-send.c) --- */

void gnostr_dm_service_send_dm_async_internal(GnostrDmService *self,
                                               const char *recipient_pubkey,
                                               const char *content,
                                               GCancellable *cancellable,
                                               GnostrDmSendCallback callback,
                                               gpointer user_data);

void gnostr_dm_service_send_file_async_internal(GnostrDmService *self,
                                                 const char *recipient_pubkey,
                                                 const char *file_path,
                                                 GCancellable *cancellable,
                                                 GnostrDmSendCallback callback,
                                                 gpointer user_data);

void gnostr_dm_service_get_recipient_relays_async_internal(const char *recipient_pubkey,
                                                            GCancellable *cancellable,
                                                            GnostrDmRelaysCallback callback,
                                                            gpointer user_data);

G_END_DECLS

#endif /* GNOSTR_DM_SERVICE_PRIVATE_H */
