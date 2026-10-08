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
/* W33: a NIP-04 (kind 4) DM, shown read-only and marked less private. It is
 * stored as a local kind-14 rumor carrying the tag
 * [GH_MESSAGE_LEGACY_TAG, "nip04", <kind-4 event id>], never published. */
#define GH_MESSAGE_LEGACY_TAG "groundhog-legacy"
gboolean gh_message_get_legacy_nip04(GhMessage *self);
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
/* The message's place in its room's local arrival order (nostrc-qp24.75):
 * GhConversationStore numbers each admitted message (the durable store's
 * messages.seq, or its own counter in memory), so a message that arrives
 * after the room was read is unread wherever its sender-claimed time sorts
 * it. 0 while not admitted (or stored before arrival order was kept). Local
 * only: never published, never part of the rumor. Store-layer use. */
guint64 gh_message_get_seq(GhMessage *self);
void gh_message_set_seq(GhMessage *self, guint64 seq);
/* Always NONE for incoming messages. set_status is for the sender (local
 * echo) and is ignored on an incoming message. Notifies "status". */
GhMessageStatus gh_message_get_status(GhMessage *self);
void gh_message_set_status(GhMessage *self, GhMessageStatus status);

/* Readable properties for templates (charter §7.4): "rumor-id", "sender",
 * "body" (the content), "is-outgoing", "created-at", "kind", "subject",
 * "expires-at", "status", "relays" and "withdrawn". sender-name and attachment belong to
 * the contact directory and kind-15 slices. */

/* nostrc-xrza (W25 review M3): an encrypted-group message the group
 * withdrew when it resolved a conflict -- its epoch lost the group's branch
 * selection (Marmot convergence.md), so other members never saw it. Shown
 * marked, never as delivered text: gh_message_dup_display_text() is then
 * gh_message_withdrawn_text(). Set only on an MLS message (ignored on any
 * other); notifies "withdrawn". Local only. */
gboolean gh_message_get_withdrawn(GhMessage *self);
void gh_message_set_withdrawn(GhMessage *self, gboolean withdrawn);
/* "This message was withdrawn when the group resolved a conflict"
 * (translated). */
const gchar *gh_message_withdrawn_text(void);

/* Inbox relays that delivered a wrap of this message, in arrival order.
 * NULL-terminated, never NULL; empty for a local echo. */
const gchar *const *gh_message_get_relays(GhMessage *self);
/* Returns TRUE and notifies "relays" when url was not yet recorded. */
gboolean gh_message_add_relay(GhMessage *self, const gchar *url);

/* nostrc-zjkv: the event id this message replies to or quotes (from the
 * first e-reply or q tag); NULL when it is not a reply. Group messages
 * (NIP-29 and MLS) only. */
const gchar *gh_message_get_reply_to_id(GhMessage *self);

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

/* ---- Marmot (MLS) encrypted-group messages (nostrc-qp24.13) -----------------
 * A message can also be the decrypted inner event of a Marmot group's kind
 * 445 (MIP-03): an unsigned kind-9 chat event whose author libmarmot
 * authenticated as the MLS sender (0.9.0: the inner pubkey must be the
 * sender leaf's account). Its room is the group: "mls:" followed by the
 * lowercase hex MLS group id (never the routing h, which can change), so no
 * MLS room id can ever equal a NIP-17 or NIP-29 one. Like a NIP-29 message it
 * has no recipients and its only participant is the account (membership is
 * the MLS state's, never derived from who wrote). The "rumor" accessors
 * return the inner event's id and JSON.
 *
 * The inner event must be unsigned (a signed inner event would be a
 * publishable proof of authorship, MIP-03), kind 9, authored by a lowercase
 * hex pubkey, with a positive created_at, content, and an id that matches
 * (a missing one is computed). G_IO_ERROR_INVALID_DATA otherwise
 * (G_IO_ERROR_INVALID_ARGUMENT for a bad account or group id). */
#define GH_MESSAGE_MLS_ROOM_PREFIX "mls:"
#define GH_MESSAGE_MLS_KIND 9
/* NIP-88 polls carried in MLS app messages (MDK v0.11). */
#define GH_MESSAGE_MLS_POLL_KIND      1068
#define GH_MESSAGE_MLS_POLL_VOTE_KIND 1018
/* The longest MLS group id accepted, in bytes (GhStoreMarmot's bound). */
#define GH_MESSAGE_MAX_MLS_GROUP_ID 256
GhMessage *gh_message_new_from_mls(const gchar *account_pubkey,
                                   const gchar *group_id_hex,
                                   const gchar *inner_event_json,
                                   GError **error);
/* "mls:" + group_id_hex; NULL unless group_id_hex is 2 to 512 lowercase hex
 * characters of whole bytes. */
gchar *gh_message_mls_room_id(const gchar *group_id_hex);
/* Splits an MLS room id; FALSE when room_id is not one. */
gboolean gh_message_mls_room_split(const gchar *room_id, gchar **group_id_hex);
/* TRUE for an MLS group message (gh_message_new_from_mls()); then
 * gh_message_get_group_id() is its MLS group id (hex) and
 * gh_message_get_group_relay() NULL. */
gboolean gh_message_is_mls(GhMessage *self);
/* The source epoch libmarmot authenticated for an MLS inner event
 * (MarmotMessageResult.app_msg.epoch; for an own message, the epoch it was
 * sent in), never a sender-chosen tag: what opening its attachments needs.
 * Set by the MLS layer on receive and send, and restored from the store.
 * FALSE when unknown (a row stored before Groundhog 0.12.0: its files can't
 * be opened). */
void gh_message_set_mls_epoch(GhMessage *self, guint64 source_epoch);
gboolean gh_message_get_mls_epoch(GhMessage *self, guint64 *out_source_epoch);

/* An encrypted group message's attachments (W25, nostrc-q3a6), as the MLS
 * layer read them from the inner event's imeta tags with libmarmot's strict
 * MIP-04 v2 parser (gh-mls-imeta.h), in tag order. Display data only (the
 * declared type, never trusted to decode; the sender's file name, shown and
 * sanitized before any save; the size hint), plus file_id, the media cache
 * identity (gh_store_mls_media_file_id(); NULL without a source epoch).
 * Opening a file goes through the MLS layer. A tag the parser rejected is
 * counted, never shown, and never invalidates the message. */
typedef struct {
  gchar *media_type;
  gchar *filename;
  guint width, height;   /* the dim hint; 0 when absent */
  gchar *file_id;
} GhMessageAttachment;

void gh_message_attachment_free(GhMessageAttachment *attachment);
/* attachments: GhMessageAttachment (a reference is taken; free func
 * gh_message_attachment_free), NULL or empty for none. MLS messages only. */
void gh_message_set_attachments(GhMessage *self, GPtrArray *attachments, guint rejected);
guint gh_message_get_n_attachments(GhMessage *self);
/* Borrowed; NULL past the end. */
const GhMessageAttachment *gh_message_get_attachment(GhMessage *self, guint index);
/* imeta tags the parser rejected (attachment-local). */
guint gh_message_get_rejected_attachments(GhMessage *self);

/* Distinct lowercase pubkeys in valid nostr:npub mentions in content.
 * Transfer full; malformed lookalikes are ignored. */
GStrv gh_message_extract_mentions(const gchar *content);

G_END_DECLS
#endif
