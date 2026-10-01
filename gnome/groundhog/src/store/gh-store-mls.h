#ifndef GH_STORE_MLS_H
#define GH_STORE_MLS_H

#include "gh-conversation-store.h"
#include "gh-store.h"

G_BEGIN_DECLS

/*
 * GhStoreMls: the encrypted store's MLS (Marmot) conversation layer
 * (nostrc-qp24.13; privacy charter §3.3 backend 3, §3.5 T-admit). GTK-free;
 * used on the store's thread (the main context). Nothing here is written
 * anywhere but the account's GhStore.
 *
 * Rooms. An encrypted group's conversation is the conversations row with
 * backend MLS and backend_key the lowercase hex MLS group id (never the
 * routing h), created accepted, titled with the group's name. In the
 * conversation model its room id is gh_message_mls_room_id() ("mls:" + the
 * hex id), so it can never equal a NIP-17 or NIP-29 room id.
 *
 * Messages. The decrypted kind-9 inner events (gh_message_new_from_mls())
 * are admitted with gh_store_admit() (T-admit: the inner event id, scoped
 * to its group, in `seen` ns GH_STORE_SEEN_MLS_MESSAGE and the message, in
 * one transaction; inside
 * the caller's transaction when there is one, so a received kind 445's
 * ratchet step and its message commit together). The same conversation also
 * holds the outbox rows of the account's own Commits (kind 445) and Welcome
 * wraps (kind 444) from gh-mls-commits.c and gh-mls-service.c: they are
 * never listed as messages. A message the group withdrew when it resolved a
 * conflict (gh_store_mls_mark_withdrawn(), nostrc-xrza) is listed marked
 * so (gh_message_get_withdrawn()).
 *
 * The GhStoreMls object is the MLS persistence delegate of a
 * GhConversationStore (gh-conversation-private.h): gh_store_mls_attach()
 * lists the stored group rooms with their newest page of messages (each
 * inner event checked again), read markers are kept in the conversations
 * row. NIP-17 and NIP-29 rooms are never touched.
 *
 * Lifetime: the store is borrowed; call gh_store_mls_close() before
 * gh_store_close(). Afterwards every call fails with GH_STORE_ERROR_STATE.
 */

#define GH_STORE_MLS_PAGE_SIZE 50

/* Creates (accepted) or updates the conversation of the group
 * group_id_hex; title NULL keeps the stored one. *out_conversation_id
 * (nullable) receives its id. Inside the caller's transaction if any. */
gboolean gh_store_mls_save_room(GhStore *store, const gchar *group_id_hex, const gchar *title,
                                gint64 *out_conversation_id, GError **error);

#define GH_TYPE_STORE_MLS (gh_store_mls_get_type())
G_DECLARE_FINAL_TYPE(GhStoreMls, gh_store_mls, GH, STORE_MLS, GObject)

GhStoreMls *gh_store_mls_new(GhStore *store);
/* Installs this as model's MLS delegate (model must be bound to the store's
 * account: GH_STORE_ERROR_STATE otherwise) and lists every stored group room
 * that has messages, each with its newest page_size (0: the default)
 * messages, read marker, unread count and name. */
gboolean gh_store_mls_attach(GhStoreMls *self, GhConversationStore *model, guint page_size,
                             GError **error);
/* Lists up to limit (1 to 1000) older messages of an attached group room;
 * *out_loaded (nullable) is how many. Nothing older is success with 0. */
gboolean gh_store_mls_load_older(GhStoreMls *self, GhConversation *conversation, guint limit,
                                 guint *out_loaded, GError **error);
/* Detaches from the model and the store. Idempotent. */
void gh_store_mls_close(GhStoreMls *self);

G_END_DECLS
#endif
