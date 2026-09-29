#ifndef GH_MESSAGE_H
#define GH_MESSAGE_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Bounds shared with the NIP-17 inbox (gh-nip17-inbox.h): a rumor is at most
 * one NIP-44 plaintext and names at most 128 recipients. */
#define GH_MESSAGE_MAX_RUMOR_JSON 65535
#define GH_MESSAGE_MAX_RECIPIENTS 128

/* Honest send status of an own message (privacy/UX charter §3.6). There is
 * deliberately no "delivered" or "read" value: relay acceptance is all a
 * sender can know. Placeholder: the durable outbox (G06) derives it and owns
 * the derivation; until then only a local echo sets it. NONE is every
 * incoming message and every own message known only from its self-copy. */
typedef enum {
  GH_MESSAGE_STATUS_NONE,
  GH_MESSAGE_STATUS_WAITING_FOR_SIGNER,
  GH_MESSAGE_STATUS_QUEUED_OFFLINE,
  GH_MESSAGE_STATUS_SENDING,
  GH_MESSAGE_STATUS_SENT,
  GH_MESSAGE_STATUS_PARTIALLY_SENT,
  GH_MESSAGE_STATUS_RETRYING,
  GH_MESSAGE_STATUS_NOT_SENT,
  GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX
} GhMessageStatus;

GType gh_message_status_get_type(void);
#define GH_TYPE_MESSAGE_STATUS (gh_message_status_get_type())

#define GH_TYPE_MESSAGE (gh_message_get_type())
G_DECLARE_FINAL_TYPE(GhMessage, gh_message, GH, MESSAGE, GObject)

/* One NIP-17 kind-14 chat message as seen by one account. Built only from a
 * canonical unsigned rumor (the inbox's verified unwrap result, or the
 * sender's own rumor for a local echo), so every field is derived from checked
 * data: kind 14, no signature, lowercase hex author and p tags, a positive
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

/* NIP-17 rumor kind; 14 (kind-15 files are not accepted yet). */
gint gh_message_get_kind(GhMessage *self);
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

G_END_DECLS
#endif
