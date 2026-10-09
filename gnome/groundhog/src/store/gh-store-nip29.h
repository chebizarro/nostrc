#ifndef GH_STORE_NIP29_H
#define GH_STORE_NIP29_H

#include "gh-conversation-store.h"
#include "gh-conversation-window.h"
#include "gh-store.h"

G_BEGIN_DECLS

/*
 * GhStoreNip29: the encrypted store's NIP-29 layer (privacy charter §3.3
 * `nip29_groups`, §3.5, §8.2 G20a). GTK-free; used on the store's thread (the
 * main context). Nothing here is written anywhere but the account's GhStore.
 *
 * Groups. Every group the account joined (or asked to join) has a
 * conversations row (backend NIP-29, backend_key "<relay URL>\x1f<group id>",
 * accepted, titled with the relay-signed name) and a nip29_groups row with
 * the relay's pinned NIP-11 key ("" until known) and snapshot_json: the
 * service's opaque record of the group (its membership, last relay-signed
 * 39000-39003 events, timeline ring and sync cursor; gh-nip29-service.c).
 * Saving both is one transaction.
 *
 * Operations. A join, leave or admin request is an outbox entry (backend
 * NIP-29) with no message row: gh_store_nip29_enqueue_operation() is its
 * T-enqueue. A chat message goes through gh_store_enqueue() like any
 * outgoing message. Either is then sealed (gh_store_seal(), role
 * GH_STORE_OUTBOX_ROLE_NIP29_EVENT, the group relay as the only target) and
 * published by the NIP-29 outbox engine (gh-nip29-outbox.h).
 *
 * Rooms. The GhStoreNip29 object is the NIP-29 persistence delegate of a
 * GhConversationStore (gh-conversation-private.h): group messages are
 * admitted with gh_store_admit() (T-admit: the event id in `seen` ns
 * GH_STORE_SEEN_NIP29_EVENT and the message, in one transaction), their read
 * markers are kept in the conversations row, and gh_store_nip29_attach()
 * lists the stored group rooms with their newest page of messages (every
 * event verified again from its stored JSON). NIP-17 rooms are never touched.
 *
 * Lifetime: the store is borrowed; call gh_store_nip29_close() before
 * gh_store_close(). Afterwards every call fails with GH_STORE_ERROR_STATE.
 */

typedef struct {
  gint64 conversation_id;
  gchar *relay_url;     /* normalized (gh_nip29_normalize_relay_url()) */
  gchar *group_id;
  gchar *relay_pubkey;  /* 64 lowercase hex, or "" while unknown */
  gchar *state_json;    /* the service's record, or NULL */
  gchar *title;         /* the conversation's name, or NULL */
} GhStoreNip29Group;

void gh_store_nip29_group_free(GhStoreNip29Group *group);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhStoreNip29Group, gh_store_nip29_group_free)

/* Creates or updates a group: its conversation (created accepted) and its
 * nip29_groups row, in one transaction. title NULL keeps the stored one.
 * relay_pubkey is 64 lowercase hex or "" (unknown). */
gboolean gh_store_nip29_save_group(GhStore *store, const gchar *relay_url,
                                   const gchar *group_id, const gchar *relay_pubkey,
                                   const gchar *state_json, const gchar *title,
                                   gint64 *out_conversation_id, GError **error);
/* Every stored group, oldest first; an empty array when none. */
GPtrArray *gh_store_nip29_list_groups(GhStore *store, GError **error);
/* Deletes a group's nip29_groups row; its conversation and messages stay.
 * NOT_FOUND when absent. */
gboolean gh_store_nip29_delete_group(GhStore *store, gint64 conversation_id,
                                     GError **error);

/* The group of a NIP-29 conversation (its backend key split in two).
 * NOT_FOUND when conversation_id is not a stored NIP-29 conversation. */
gboolean gh_store_nip29_get_room(GhStore *store, gint64 conversation_id,
                                 gchar **out_relay_url, gchar **out_group_id,
                                 GError **error);

/* T-enqueue of a group operation that shows no message (kinds 9000-9022):
 * a QUEUED NIP-29 outbox entry with the unsigned event, before any signer
 * call. Idempotent on op_id (32 lowercase hex): a repeat returns the
 * existing entry. */
gboolean gh_store_nip29_enqueue_operation(GhStore *store, gint64 conversation_id,
                                          const gchar *op_id, const gchar *unsigned_json,
                                          gint64 *out_outbox_id, GError **error);

/* A group event an admin deleted (kind 9005): its message leaves the room's
 * stored history and the unread count follows. Its id stays in `seen`, so
 * backfill does not bring it back. Nothing stored is success. */
gboolean gh_store_nip29_delete_message(GhStore *store, const gchar *room_id,
                                       const gchar *event_id, GError **error);

#define GH_TYPE_STORE_NIP29 (gh_store_nip29_get_type())
G_DECLARE_FINAL_TYPE(GhStoreNip29, gh_store_nip29, GH, STORE_NIP29, GObject)

#define GH_STORE_NIP29_PAGE_SIZE GH_CONVERSATION_WINDOW_OPEN

GhStoreNip29 *gh_store_nip29_new(GhStore *store);
/* Installs this as model's NIP-29 delegate (model must be bound to the
 * store's account: GH_STORE_ERROR_STATE otherwise) and lists every stored
 * group room that has messages, each with its newest page_size (0: the
 * default) messages, read marker, unread count and name. */
gboolean gh_store_nip29_attach(GhStoreNip29 *self, GhConversationStore *model,
                               guint page_size, GError **error);
/* Lists up to limit (1 to 1000) older messages of an attached group room;
 * *out_loaded (nullable) is how many. Nothing older is success with 0. */
gboolean gh_store_nip29_load_older(GhStoreNip29 *self, GhConversation *conversation, guint limit,
                                   guint *out_loaded, GError **error);
gboolean gh_store_nip29_load_newer(GhStoreNip29 *self, GhConversation *conversation, guint limit,
                                   guint *out_loaded, GError **error);
gboolean gh_store_nip29_reset_latest(GhStoreNip29 *self, GhConversation *conversation,
                                     GError **error);
/* Detaches from the model and the store. Idempotent. */
void gh_store_nip29_close(GhStoreNip29 *self);

G_END_DECLS
#endif
