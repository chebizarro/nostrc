#ifndef GH_CONVERSATION_H
#define GH_CONVERSATION_H

#include "gh-message.h"

G_BEGIN_DECLS

/* Values match the charter's store schema (§3.3 conversations.backend). */
typedef enum {
  GH_CONVERSATION_BACKEND_NIP17 = 1,
  GH_CONVERSATION_BACKEND_NIP29 = 2,
  GH_CONVERSATION_BACKEND_MLS = 3
} GhConversationBackend;

GType gh_conversation_backend_get_type(void);
#define GH_TYPE_CONVERSATION_BACKEND (gh_conversation_backend_get_type())

#define GH_TYPE_CONVERSATION (gh_conversation_get_type())
G_DECLARE_FINAL_TYPE(GhConversation, gh_conversation, GH, CONVERSATION, GObject)

/* One NIP-17 room of one account: a GListModel of GhMessage ordered by
 * rumor created_at, then rumor id, holding each rumor id once. The room is
 * identified by its participant set (see gh_message_get_room_id), never by
 * who wrote a message. Messages are added only through
 * gh_conversation_store_admit(). Readable properties for templates (charter
 * §7.4): "room-id", "backend", "title", "subject", "preview",
 * "last-activity", "unread-count" and "is-request"; all but the first two
 * notify on change. pinned, muted, privacy-label and has-failure belong to
 * the durable store, UI and outbox slices. */

const gchar *gh_conversation_get_account(GhConversation *self);
const gchar *gh_conversation_get_room_id(GhConversation *self);
/* Sorted, unique, including the account. NULL-terminated. */
const gchar *const *gh_conversation_get_participants(GhConversation *self);
/* Participants other than the account; empty for a note to self. */
const gchar *const *gh_conversation_get_peers(GhConversation *self);
/* The subject tag of the latest message (by the message order) that has one;
 * NULL when none has or the latest one is empty. */
const gchar *gh_conversation_get_subject(GhConversation *self);
/* created_at of the newest message. */
gint64 gh_conversation_get_last_activity(GhConversation *self);
/* Messages from others after the read marker, including any in the unloaded
 * older history of a durably stored room. The marker moves to the end on
 * mark_read, and to any own message added after it (replying implies having
 * read what came before). With a durable store, mark_read is persisted.
 *
 * Unread messages that are not listed are never marked read (W13b review
 * B1): while some remain in the unloaded older history, mark_read counts only
 * the listed messages as read, for this session, and neither moves nor
 * persists the marker, so the unloaded ones stay unread (after a restart
 * too) until a later mark_read once they are listed. */
guint gh_conversation_get_unread_count(GhConversation *self);
void gh_conversation_mark_read(GhConversation *self);
/* The unread messages that are listed, and in *out_first (nullable) the
 * position of the first of them (the number of listed messages when there
 * is none). The rest of the unread count is in the unloaded older history,
 * before every listed message. */
guint gh_conversation_get_listed_unread(GhConversation *self, guint *out_first);
/* A durable store holds older messages of this room than those listed; it
 * loads them on request (gh_store_conversations_load_older()). */
gboolean gh_conversation_get_has_older(GhConversation *self);
GhConversationBackend gh_conversation_get_backend(GhConversation *self);
/* The subject when there is one; otherwise the peers' abbreviated npubs
 * (the account's own for a note to self). A request is always titled by the
 * npubs (charter §7.9): its subject is text the sender chose, which the UI
 * shows only as secondary text (gh_conversation_get_subject()) until the
 * request is accepted. Profile names are not fetched here: a request must not
 * trigger a kind-0 lookup (charter PT-8). "title" is notified whenever this
 * value changes, accepting included. */
const gchar *gh_conversation_get_title(GhConversation *self);
/* First line of the newest message's body, at most 80 characters; the UI
 * decides whether previews are shown at all. NULL while empty. */
const gchar *gh_conversation_get_preview(GhConversation *self);
/* A message request: nobody on this account has written in the room and it
 * was not accepted. Requests are listed apart and never fetch profiles.
 * Accepting (or sending) makes it a conversation; it is local only (and
 * persisted with a durable store). */
gboolean gh_conversation_get_is_request(GhConversation *self);
void gh_conversation_accept(GhConversation *self);
/* Borrowed; NULL when the rumor is not in this room. */
GhMessage *gh_conversation_lookup_message(GhConversation *self, const gchar *rumor_id);

G_END_DECLS
#endif
