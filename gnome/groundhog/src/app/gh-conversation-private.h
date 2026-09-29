#ifndef GH_CONVERSATION_PRIVATE_H
#define GH_CONVERSATION_PRIVATE_H

#include "gh-conversation-store.h"

G_BEGIN_DECLS

/* Store-only: the conversation of message's room for its account. */
GhConversation *gh_conversation_new_for_message(GhMessage *message);
/* Store-only: an empty, accepted room the user opened (G18: New Message,
 * note to self). participants: sorted, unique, lowercase hex, including
 * account. opened_at (unix seconds) is its last activity until it has a
 * message. */
GhConversation *gh_conversation_new_for_room(const gchar *account,
                                             const gchar *const *participants,
                                             gint64 opened_at);
/* Store-only: an empty NIP-29 group room (a joined group with no message
 * yet); room_id as gh_message_nip29_room_id(). */
GhConversation *gh_conversation_new_nip29(const gchar *account, const gchar *room_id);
/* Store-only: an empty MLS group room (a joined encrypted group with no
 * message yet); room_id as gh_message_mls_room_id(). */
GhConversation *gh_conversation_new_mls(const gchar *account, const gchar *room_id);
/* Store-only: the room's stored name (the NIP-17 subject kept durably, or a
 * group's relay-signed name); NULL or "" clears. Notifies subject and title
 * when they change. */
void gh_conversation_set_name(GhConversation *self, const gchar *name);
/* Store-only: inserts message in order. FALSE (nothing changed) when its
 * rumor id is already present or it belongs to another room or account.
 * delivered: a relay delivered it (an own message then was written on
 * another device), else it is a local echo (see the read state in
 * gh-conversation.h). */
gboolean gh_conversation_insert(GhConversation *self, GhMessage *message, gboolean delivered);
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
  /* nostrc-qp24.75: how far the arrival order the marker covered, and the
   * reply boundary (the newest own message another device wrote). */
  guint64 read_seq;
  gboolean has_reply;
  gint64 reply_created_at;
  const gchar *reply_id;
  gint64 pinned_rank;        /* 0: not pinned (nostrc-qp24.86) */
  gint64 timer_seconds;      /* the timer after its last change (nostrc-qp24.83) */
  gint64 timer_changed_at;   /* 0: never changed */
} GhConversationState;

/* Store-only: inserts restored messages (committed earlier; no read-marker
 * movement), then applies state. Emits items-changed and property
 * notifications, never "message-added". */
void gh_conversation_restore(GhConversation *self, GPtrArray *messages,
                             const GhConversationState *state);
/* Whether message belongs to the unloaded older history (before the floor). */
gboolean gh_conversation_is_older_history(GhConversation *self, GhMessage *message);
/* A new message committed into the unloaded older history: moves the read
 * state for an own message (delivered as for gh_conversation_insert()); the
 * caller syncs the durable unread count. */
void gh_conversation_add_older_history(GhConversation *self, GhMessage *message,
                                       gboolean delivered);
/* The newest arrival (gh_message_get_seq()) the read marker covers, for a
 * durable delegate's mark_read(). */
guint64 gh_conversation_get_read_seq(GhConversation *self);
/* Store-only: the pin rank (0 unpins); notifies "pinned" when it turns on
 * or off. The store orders pinned rooms first (gh_conversation_store_pin()). */
void gh_conversation_set_pinned_rank(GhConversation *self, gint64 rank);
gint64 gh_conversation_get_pinned_rank(GhConversation *self);
/* Store-only: the disappearing timer's last change (charter §3.7, nostrc-
 * qp24.83); notifies "timer-seconds" and "timer-changed-at". */
void gh_conversation_set_timer_change(GhConversation *self, gint64 seconds,
                                      gint64 changed_at);
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
/* first_unread and what follows became unread (gh_conversation_mark_unread). */
void gh_conversation_store_persist_unread(GhConversationStore *self,
                                          GhConversation *conversation,
                                          GhMessage *first_unread);

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

/* ---- Group rooms: NIP-29 (G20a) and MLS (nostrc-qp24.13) ---------------------
 * The main delegate (gh_conversation_store_set_account()) persists NIP-17
 * rooms. Group messages (gh_message_is_nip29(), gh_message_is_mls()) never
 * reach it: they go to their backend's delegate set here (gh-store-nip29.h,
 * gh-store-mls.h), or stay in memory when there is none. For such a message,
 * gh_conversation_store_admit()'s wrap_id is non-NULL when a relay delivered
 * it (the event id, or for MLS the kind-445 envelope's: the event is its own
 * carrier) and NULL for the local echo of an own message. Read markers of
 * group rooms are persisted through their delegate too. Each is dropped
 * whenever the bound account changes (including to none). */
/* backend must be GH_CONVERSATION_BACKEND_NIP29 or _MLS; FALSE (nothing set)
 * when account_pubkey is not the bound account. */
gboolean gh_conversation_store_set_backend_delegate(GhConversationStore *self,
                                                    GhConversationBackend backend,
                                                    const gchar *account_pubkey,
                                                    const GhConversationDelegate *delegate,
                                                    gpointer delegate_data,
                                                    GDestroyNotify destroy);
/* Drops backend's delegate if delegate_data is the one set. */
void gh_conversation_store_clear_backend_delegate(GhConversationStore *self,
                                                  GhConversationBackend backend,
                                                  gpointer delegate_data);
/* Lists a group room of the bound account (empty when new) and sets its
 * name (NULL keeps the current one): a NIP-29 room id or an MLS one
 * (gh_message_mls_room_id()). Returns it (borrowed), or NULL when no account
 * is bound or room_id is neither. */
GhConversation *gh_conversation_store_ensure_group(GhConversationStore *self,
                                                   const gchar *room_id,
                                                   const gchar *name);
/* Store-layer only: takes a message that expired or was purged out of its
 * room (gh_conversation_remove()) and moves the room to its new place in
 * the store order; the room stays listed, even when empty. Calls no
 * delegate. TRUE when the model held the rumor. */
gboolean gh_conversation_store_remove_message(GhConversationStore *self,
                                              const gchar *rumor_id);

G_END_DECLS
#endif
