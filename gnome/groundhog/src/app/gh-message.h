#ifndef GH_MESSAGE_H
#define GH_MESSAGE_H

#include <gio/gio.h>

#include "gh-message-status.h"
#include "gh-nip17-file.h"

G_BEGIN_DECLS

/* Bounds shared with the NIP-17 inbox (gh-nip17-inbox.h): a rumor is at most
 * one NIP-44 plaintext and names at most 128 recipients. */
#define GH_MESSAGE_MAX_RUMOR_JSON 65535
#define GH_MESSAGE_MAX_RECIPIENTS 128

/* An own message's honest send status is a GhMessageStatus
 * (gh-message-status.h, privacy/UX charter §3.6): there is deliberately no
 * "delivered" or "read" value, since relay acceptance is all a sender can
 * know. The durable outbox (G06) derives it; GH_MESSAGE_STATUS_NONE is every
 * incoming message and every own message known only from its self-copy. */

#define GH_TYPE_MESSAGE (gh_message_get_type())
G_DECLARE_FINAL_TYPE(GhMessage, gh_message, GH, MESSAGE, GObject)

/* One NIP-17 kind-14 chat message (or kind-15 file message with valid file
 * tags, G21) as seen by one account. Built only from a
 * canonical unsigned rumor (the inbox's verified unwrap result, or the
 * sender's own rumor for a local echo), so every field is derived from checked
 * data: kind 14 or 15, no signature, lowercase hex author and p tags, a positive
 * created_at, and an id that is recomputed (a declared id must match).
 * account_pubkey (lowercase hex) must be the author or a p-tagged recipient;
 * otherwise G_IO_ERROR_INVALID_DATA. Everything but the relay provenance is
 * immutable. */
GhMessage *gh_message_new_from_rumor(const gchar *account_pubkey,
                                     const gchar *rumor_json,
                                     GError **error);

const gchar *gh_message_get_account(GhMessage *self);
const gchar *gh_message_get_rumor_id(GhMessage *self);
const gchar *gh_message_get_sender(GhMessage *self);
/* p-tag recipients: unique, in tag order, NULL-terminated. */
const gchar *const *gh_message_get_recipients(GhMessage *self);
/* NIP-17 room members: the sorted unique set of author and recipients. The
 * account is always one of them. */
const gchar *const *gh_message_get_participants(GhMessage *self);
/* The canonical room id: participants joined by ','. A→B, B→A and the
 * sender's self-copy share it; adding or removing anyone changes it. */
const gchar *gh_message_get_room_id(GhMessage *self);
gint64 gh_message_get_created_at(GhMessage *self);
const gchar *gh_message_get_content(GhMessage *self);
/* The sender is the account (an outgoing message or its self-copy). */
gboolean gh_message_is_self(GhMessage *self);
/* The first subject tag's value; NULL without one ("" is a real, empty
 * subject that clears the room's name). */
const gchar *gh_message_get_subject(GhMessage *self);

/* NIP-17 rumor kind: 14 (chat) or 15 (an encrypted file, G21); a NIP-29
 * message's own kind. */
gint gh_message_get_kind(GhMessage *self);
/* G21: the file a kind-15 message carries (gh-nip17-file.h; free with
 * gh_nip17_file_free()), or NULL for any other message. Its content is the
 * file's URL; nothing is fetched: downloading is the user's explicit action
 * (charter PD-2, AT-7; src/media/gh-attachment.h). */
GhNip17File *gh_message_dup_file(GhMessage *self);
/* What the message says, as text for a list, a preview or a notification:
 * its content, or for a kind-15 file message "Photo" or "File" (translated),
 * never its URL (W17 review #2): the one text every surface shows. */
gchar *gh_message_dup_display_text(GhMessage *self);
/* The verified rumor JSON the message was built from, which the durable store
 * keeps (and verifies again when it restores the message). */
const gchar *gh_message_get_rumor_json(GhMessage *self);
/* When the message disappears (NIP-40, charter §3.7), in unix seconds; 0 when
 * it does not. The rumor's own expiration tag, else the value given to
 * gh_message_set_expires_at(). */
gint64 gh_message_get_expires_at(GhMessage *self);
/* Sets the expiry from an outer layer when the rumor carries none: the
 * unwrap's GhNip17Message.expires_at (the seal's expiration, else the
 * wrap's), or the stored value on restore. Ignored when the message already
 * has one or expires_at <= 0. Notifies "expires-at". */
void gh_message_set_expires_at(GhMessage *self, gint64 expires_at);
/* Always NONE for incoming messages. set_status is for the sender (local
 * echo) and is ignored on an incoming message. Notifies "status". */
GhMessageStatus gh_message_get_status(GhMessage *self);
void gh_message_set_status(GhMessage *self, GhMessageStatus status);

/* Readable properties for templates (charter §7.4): "rumor-id", "sender",
 * "body" (the content), "is-outgoing", "created-at", "kind", "subject",
 * "expires-at", "status" and "relays". sender-name and attachment belong to
 * the contact directory and kind-15 slices. */

/* Inbox relays that delivered a wrap of this message, in arrival order.
 * NULL-terminated, never NULL; empty for a local echo. */
const gchar *const *gh_message_get_relays(GhMessage *self);
/* Returns TRUE and notifies "relays" when url was not yet recorded. */
gboolean gh_message_add_relay(GhMessage *self, const gchar *url);

/* Total order of a conversation: created_at, then rumor id. */
gint gh_message_compare(GhMessage *a, GhMessage *b);

/* ---- NIP-29 relay-group messages (charter §8.2 G20a) -------------------------
 * A message can also be one event of a NIP-29 group: kind 9 (chat), 10, 11 or
 * 12 carrying the group's h tag, seen on the group's relay. Its room is the
 * group's identity (charter §3.3 NIP-29 backend key):
 * "<normalized relay URL>\x1f<group id>", never who wrote it, so the same
 * group id on two relays is two rooms. It has no recipients, its only
 * participant is the account (a group's membership is the relay's to say,
 * never derived from who wrote, so no peer is ever looked up), no subject,
 * and "rumor" accessors return the event id and the event JSON.
 *
 * The event must be signed (valid id and Schnorr signature) unless it is the
 * account's own event not yet signed: the local echo of an outgoing message,
 * whose id is recomputed. The group id uses NIP-29's charset (a-z, 0-9, '-'
 * and '_'); relay_url must be a normalized ws(s) URL (gh-nip29-group.h). The
 * account need not be the author (anyone in a group may write), and the kind
 * is kept. Otherwise G_IO_ERROR_INVALID_DATA (G_IO_ERROR_INVALID_ARGUMENT for
 * a bad account or relay). */
#define GH_MESSAGE_NIP29_SEPARATOR "\x1f"
#define GH_MESSAGE_MAX_GROUP_ID 256
GhMessage *gh_message_new_from_nip29_event(const gchar *account_pubkey,
                                           const gchar *relay_url,
                                           const gchar *event_json,
                                           GError **error);
/* The room id of a group: relay_url "\x1f" group_id (no validation). */
gchar *gh_message_nip29_room_id(const gchar *relay_url, const gchar *group_id);
/* Splits a NIP-29 room id; FALSE when room_id is not one. */
gboolean gh_message_nip29_room_split(const gchar *room_id, gchar **relay_url,
                                     gchar **group_id);
/* TRUE for a NIP-29 group event (gh_message_new_from_nip29_event()). */
gboolean gh_message_is_nip29(GhMessage *self);
/* The group id and its relay; NULL for a NIP-17 message. */
const gchar *gh_message_get_group_id(GhMessage *self);
const gchar *gh_message_get_group_relay(GhMessage *self);
/* Whether the event JSON carries a signature (FALSE for a NIP-17 rumor and
 * for the unsigned local echo of an own group message). */
gboolean gh_message_is_signed(GhMessage *self);

G_END_DECLS
#endif
