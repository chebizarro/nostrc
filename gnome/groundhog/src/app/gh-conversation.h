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
 * who wrote a message. A NIP-29 group (backend NIP29) is a room too: its id
 * is the group's (relay URL, group id) identity, its only participant the
 * account (no peers), its title the relay-signed group name or else the
 * group id, and it is never a request (the account joined it). Messages are added only through
 * gh_conversation_store_admit(). Readable properties for templates (charter
 * §7.4): "room-id", "backend", "title", "subject", "preview",
 * "last-activity", "unread-count", "is-request", "pinned" (nostrc-qp24.86),
 * "timer-seconds" and "timer-changed-at" (nostrc-qp24.83); all but the first
 * two notify on change. muted, privacy-label and has-failure belong to the
 * durable store, UI and outbox slices. */

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
/* Unread messages from others, including any in the unloaded older history
 * of a durably stored room. Read state is local only (charter PD-1, P8) and
 * follows arrival, not only the sender-claimed order (nostrc-qp24.75):
 * mark_read reads exactly the messages listed at that moment, and an own
 * message written on this device reads what had arrived before it and sorts
 * before it (replying implies having read what came before). A message that
 * arrives later is unread wherever it sorts, e.g. one with the same second
 * as the last read one and a lower rumor id, or a delayed one. An own
 * message another device wrote (delivered by a relay) reads whatever sorts
 * before it, since it was written there after reading. With a durable
 * store, mark_read is persisted.
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
/* Whether @message (one of the room's) counts as unread now. */
gboolean gh_conversation_is_unread(GhConversation *self, GhMessage *message);
/* Mark as Unread (charter §7.4; nostrc-qp24.86): the newest listed message
 * from someone else becomes unread again, and everything before it stays
 * read. Local only, never published (P8); persisted with a durable store.
 * Offered when nothing is unread, the room has a message from someone else
 * and the place just before it is known (it is not the oldest listed one of
 * a room with unloaded older history). FALSE when it is not offered. */
gboolean gh_conversation_can_mark_unread(GhConversation *self);
gboolean gh_conversation_mark_unread(GhConversation *self);
/* Pinned (charter §7.4, §7.5; nostrc-qp24.86): pinned rooms are listed
 * first, in the order they were pinned. Local only; kept in the encrypted
 * store's conversations.pinned_rank, never in GSettings (PD-11). */
gboolean gh_conversation_get_pinned(GhConversation *self);
/* When the disappearing timer last changed (unix seconds; 0: never) and in
 * *out_seconds (nullable) the timer it was set to: the local timeline row
 * "You set messages to disappear after 1 day" (charter §3.7). */
gint64 gh_conversation_get_timer_change(GhConversation *self, gint64 *out_seconds);
/* A durable store holds older messages than those listed, or a NIP-29 relay
 * may have history beyond the stored edge. The backend loads it on request. */
gboolean gh_conversation_get_has_older(GhConversation *self);
/* The relay could not safely page through a same-second NIP-29 boundary. */
gboolean gh_conversation_get_history_partial(GhConversation *self);
GhConversationBackend gh_conversation_get_backend(GhConversation *self);
/* Whether this conversation is a direct message (two parties, one-on-one).
 * For NIP-17, true when there is exactly one peer. For MLS, set by the
 * service when the group has two members and no name (the shape White Noise
 * uses for DMs). The "is-direct" property notifies on change. */
gboolean gh_conversation_get_is_direct(GhConversation *self);
/* A NIP-17 room containing only the account is titled "Note to Self".
 * Otherwise the subject when there is one; then the peers' names set with
 * gh_conversation_set_contact_title(), else their abbreviated npubs (the
 * account's own only as a defensive fallback). A request is always titled by the
 * npubs (charter §7.9): its subject is text the sender chose, which the UI
 * shows only as secondary text (gh_conversation_get_subject()) until the
 * request is accepted. Profile names are not fetched here: a request must not
 * trigger a kind-0 lookup (charter PT-8). "title" is notified whenever this
 * value changes, accepting included. */
const gchar *gh_conversation_get_title(GhConversation *self);
/* The peers' display names from the contact directory's cache
 * (GhContactTitles, nostrc-qp24.66): it replaces the abbreviated npubs as the
 * title of an accepted conversation without a subject, and is never used
 * while the conversation is a message request (PT-8). NULL or "" clears.
 * Notifies "title" when the title changes. */
void gh_conversation_set_contact_title(GhConversation *self, const gchar *title);
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
