#ifndef GH_CONVERSATION_PRIVATE_H
#define GH_CONVERSATION_PRIVATE_H

#include "gh-conversation-store.h"

G_BEGIN_DECLS

/* Store-only: the conversation of message's room for its account. */
GhConversation *gh_conversation_new_for_message(GhMessage *message);
/* Store-only: inserts message in order. FALSE (nothing changed) when its
 * rumor id is already present or it belongs to another room or account. */
gboolean gh_conversation_insert(GhConversation *self, GhMessage *message);
/* Store-only: the store that persists the room's read marker and acceptance
 * through its delegate; NULL detaches. Not a reference. */
void gh_conversation_set_store(GhConversation *self, GhConversationStore *store);
/* Store-only: takes rumor_id's message out of the room (it expired or was
 * purged, charter §3.7). Subject, title, preview, last activity and the
 * unread count follow; the read marker keeps its place in the order and
 * the room stays accepted. FALSE when the rumor is not loaded here. */
gboolean gh_conversation_remove(GhConversation *self, const gchar *rumor_id);

/* ---- Durable restore (store layer, e.g. gh-store-conversations.c) ----------
 * A durable delegate loads each room's newest messages, then older pages on
 * request. The loaded messages are all stored ones at or after the history
 * floor; messages before it are "older history", counted but not loaded. */

/* A room's durable state; every field is authoritative. */
typedef struct {
  gboolean accepted;         /* not (or no longer) a message request */
  gboolean has_marker;       /* a read marker is stored */
  gint64 marker_created_at;  /* the last read message's place in the order */
  const gchar *marker_id;
  const gchar *subject;      /* the stored room name (latest subject) or NULL */
  guint unread;              /* unread messages, loaded or not */
  gboolean has_older;        /* messages before the floor are not loaded */
  gint64 floor_created_at;   /* the oldest stored message fetched so far */
  const gchar *floor_id;
} GhConversationState;

/* Store-only: inserts restored messages (committed earlier; no read-marker
 * movement), then applies state. Emits items-changed and property
 * notifications, never "message-added". */
void gh_conversation_restore(GhConversation *self, GPtrArray *messages,
                             const GhConversationState *state);
/* Whether message belongs to the unloaded older history (before the floor). */
gboolean gh_conversation_is_older_history(GhConversation *self, GhMessage *message);
/* A new message committed into the unloaded older history: moves the read
 * marker for an own message; the caller syncs the durable unread count. */
void gh_conversation_add_older_history(GhConversation *self, GhMessage *message);
/* Makes the unread count equal the durable one (the unloaded part absorbs
 * the difference). */
void gh_conversation_sync_unread(GhConversation *self, guint unread);
/* The history floor; FALSE when the room has no unloaded older history. */
gboolean gh_conversation_get_floor(GhConversation *self, gint64 *created_at,
                                   const gchar **id);

/* ---- Store side of the conversation hooks (gh-conversation-store.c) ------- */

/* The read marker moved to last_read (gh_conversation_mark_read). */
void gh_conversation_store_persist_read(GhConversationStore *self,
                                        GhConversation *conversation,
                                        GhMessage *last_read);
/* The room stopped being a request (gh_conversation_accept). */
void gh_conversation_store_persist_accept(GhConversationStore *self,
                                          GhConversation *conversation);

/* Store-layer only: restores one room of the bound account from the durable
 * store (see GhConversationState). A room that is not listed yet is created
 * from messages and inserted in store order; a listed one merges the messages
 * it lacks and takes the state. Messages of another room or account are
 * skipped. Returns the room (borrowed), or NULL when it is not listed and
 * messages holds none of it. Calls no delegate and emits no "message-added". */
GhConversation *gh_conversation_store_restore(GhConversationStore *self,
                                              const gchar *room_id,
                                              GPtrArray *messages,
                                              const GhConversationState *state);
/* Store-layer only: unlists a room (forget conversation). TRUE when listed. */
gboolean gh_conversation_store_remove(GhConversationStore *self, const gchar *room_id);
/* Store-layer only: takes a message that expired or was purged out of its
 * room (gh_conversation_remove()) and moves the room to its new place in
 * the store order; the room stays listed, even when empty. Calls no
 * delegate. TRUE when the model held the rumor. */
gboolean gh_conversation_store_remove_message(GhConversationStore *self,
                                              const gchar *rumor_id);

G_END_DECLS
#endif
