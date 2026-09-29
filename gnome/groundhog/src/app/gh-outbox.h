#ifndef GH_OUTBOX_H
#define GH_OUTBOX_H

#include "gh-account-relays.h"
#include "gh-dm-send.h"
#include "gh-inbox-resolver.h"
#include "gh-message-status.h"
#include "gh-relay-publish.h"
#include "gh-store.h"

G_BEGIN_DECLS

/*
 * GhOutbox: the durable NIP-17 outbox of one account (privacy charter §3.5,
 * §3.6, §4.4, §4.5), GTK-free, over that account's GhStore.
 *
 * Lifecycle of a message (every step is persisted before the next starts):
 *  1. T-enqueue (gh_outbox_send): the canonical rumor, the outgoing message
 *     row and the cleared draft, before any signer call.
 *  2. Seal (GhDmSender): recipient 10050 lookup and the signer approvals.
 *  3. T-seal: every signed wrap and its target URLs, before any publish.
 *  4. Publish: each stored wrap, byte-for-byte, on its own connections; one
 *     T-outcome per relay answer.
 *  5. Retry: only targets that did not accept and are not terminal, with
 *     backoff 15 s, 1 min, 5 min, 30 min, 2 h, then every 6 h, each
 *     ×U(0.8, 1.2) from the store's GhClock, while online and while the
 *     account is active. 72 h after T-enqueue, or once no recipient is
 *     reached and nothing is left to retry, the message needs attention
 *     (NOT_SENT with Retry); an explicit retry gets one more round. It
 *     settles once nothing is left to try and every recipient has it.
 * After a restart or crash the outbox resumes from the store: unsealed
 * messages are sealed (the signer is asked again), sealed ones republish
 * their stored wraps and are never re-signed.
 *
 * Relays and identity:
 *  - A recipient wrap goes only to that recipient's kind-10050 relays and
 *    the self-copy only to the account's own 10050 relays (no other list,
 *    no default). Before each retry (and on resume) the lists are read again
 *    and new relays become targets of the same stored wrap. So does a
 *    resolver "changed" for the recipient (a background directory refresh,
 *    §4.5 S2), within the retry window, even after the message settled.
 *  - NIP-42: GhAuthPolicy (gh-auth-policy.h) picks each URL's identity. A
 *    recipient's inbox relay gets EPHEMERAL: one throwaway key per
 *    connection, never the account (§4.4 R1, R7). The self-copy and a note
 *    to self get SELF_WRAP, the account on challenge, on the account's own
 *    inbox relays only (a stored target that left the own 10050 list is
 *    treated like a recipient's); W13 review 7a.
 *  - D8: if the own and recipient inbox sets overlap, the self-copy waits
 *    U(5, 90) s (stored as not_before); it always uses its own connection.
 *
 * Generation binding: the outbox runs only while its store's account is the
 * active account. A switch (or loss) cancels every seal and publish in
 * flight at once (no late OK is recorded) and leaves the rows resumable;
 * reselecting the account resumes them in the new generation.
 *
 * Status: each message has a GhOutboxItem whose properties (status, label,
 * icon, detail, ...) a UI can bind; they only ever claim relay acceptance.
 *
 * The store is borrowed: dispose the outbox before closing the store.
 * Threading: main context only.
 */

#define GH_TYPE_OUTBOX_ITEM (gh_outbox_item_get_type())
G_DECLARE_FINAL_TYPE(GhOutboxItem, gh_outbox_item, GH, OUTBOX_ITEM, GObject)

/* One outgoing message. Every property is read-only and notifies only when
 * it changes: "outbox-id", "message-id", "conversation-id" (gint64),
 * "state" (a GhStoreOutboxState value), "status" (GH_TYPE_MESSAGE_STATUS, the
 * same enum as GhMessage:status, so one can be bound to the other), "label",
 * "icon-name", "accessible-description", "detail" (strings),
 * "self-copy-missing" (show gh_message_status_get_self_copy_note()),
 * "next-attempt-at" (unix seconds of the next automatic retry, 0 = none)
 * and "can-retry". */
gint64 gh_outbox_item_get_outbox_id(GhOutboxItem *self);
gint64 gh_outbox_item_get_message_id(GhOutboxItem *self);
gint64 gh_outbox_item_get_conversation_id(GhOutboxItem *self);
GhStoreOutboxState gh_outbox_item_get_state(GhOutboxItem *self);
GhMessageStatus gh_outbox_item_get_status(GhOutboxItem *self);
const gchar *gh_outbox_item_get_label(GhOutboxItem *self);
const gchar *gh_outbox_item_get_icon_name(GhOutboxItem *self);
const gchar *gh_outbox_item_get_accessible_description(GhOutboxItem *self);
const gchar *gh_outbox_item_get_detail(GhOutboxItem *self);
gboolean gh_outbox_item_get_self_copy_missing(GhOutboxItem *self);
gint64 gh_outbox_item_get_next_attempt_at(GhOutboxItem *self);
gboolean gh_outbox_item_get_can_retry(GhOutboxItem *self);
/* The canonical rumor the message was queued with (T-enqueue) and its id,
 * which is the rumor id of the GhMessage that shows it (its local echo).
 * Constant for the item's lifetime. */
const gchar *gh_outbox_item_get_rumor_json(GhOutboxItem *self);
const gchar *gh_outbox_item_get_rumor_id(GhOutboxItem *self);

/* Per-relay detail, for "details on demand". */
typedef struct {
  GhStoreOutboxRole role;        /* RECIPIENT_WRAP or SELF_WRAP */
  gchar *pubkey;                 /* the wrap's receiver */
  gchar *event_id;               /* the stored wrap id */
  gchar *url;
  GhRelayPublishOutcome outcome; /* PENDING until the relay answered */
  GhRelayOkPrefix prefix;
  gchar *message;                /* relay text (bounded), or NULL */
  guint attempts;
  GhTargetClass target_class;
  const gchar *description;      /* gh_message_status_describe_target() */
} GhOutboxTarget;

void gh_outbox_target_free(GhOutboxTarget *target);
/* GhOutboxTarget, recipient wraps first; empty once the message is gone. */
GPtrArray *gh_outbox_item_dup_targets(GhOutboxItem *self);

typedef struct {
  GhStore *store;                      /* the account's open store; borrowed */
  GhAccountController *accounts;
  GhAccountRelays *account_relays;     /* own 10050: self-copy targets */
  GhInboxResolver *inboxes;            /* recipient 10050, for lists changed after sealing */
  GhDmSender *sender;                  /* seals (lookup and signer approvals) */
  GNetworkMonitor *network;            /* NULL: g_network_monitor_get_default() */
  /* NULL: gnostr relays (with NIP-42). A custom transport authenticates only
   * with auth_transport (tests). */
  const GhRelayPublishTransport *transport;
  const GhRelayPublishAuthTransport *auth_transport;
  gpointer transport_data;
  guint publish_deadline;              /* per-relay seconds; 0: the publish default */
} GhOutboxConfig;

#define GH_TYPE_OUTBOX (gh_outbox_get_type())
G_DECLARE_FINAL_TYPE(GhOutbox, gh_outbox, GH, OUTBOX, GObject)

/* Loads every unfinished message of the store and resumes them once the
 * store's account is active and the network is available. Signals:
 * "item-added" (GhOutboxItem) for a message sent or first looked up,
 * "item-removed" (GhOutboxItem) once a message is deleted or vanished from
 * the store (e.g. its conversation was forgotten). */
GhOutbox *gh_outbox_new(const GhOutboxConfig *config, GError **error);
/* Whether the store's account is active (the outbox may seal and publish). */
gboolean gh_outbox_is_active(GhOutbox *self);

/* T-enqueue a one-to-one text (recipient == the account: a note to self)
 * and start sending it. In a conversation with a disappearing timer (charter
 * §3.7, gh-expiry.h) the message expires at send + timer: the stored rumor
 * and message carry that exact time, and each seal and gift wrap gets its
 * own later expiration, drawn when it is sealed. Errors:
 * G_IO_ERROR_PERMISSION_DENIED when the
 * store's account is not the active one, G_IO_ERROR_INVALID_ARGUMENT for a
 * bad recipient or text (empty, not UTF-8, or too long for a gift wrap:
 * about 40 KB), or the store's error (e.g. GH_STORE_ERROR_FULL; the draft is
 * then kept). Once T-enqueue has committed this never fails. Nothing is
 * signed or published before this returns. */
GhOutboxItem *gh_outbox_send(GhOutbox *self, const gchar *recipient_pubkey_hex,
                             const gchar *content, GError **error);
/* Whether @content is short enough for one gift wrap to @recipient_pubkey_hex:
 * gh_outbox_send() refuses a text whose rumor is too long (about 40 KB,
 * less for text that JSON must escape). Measures exactly the rumor it would
 * queue now; TRUE when no rumor can be built at all (e.g. empty text), since
 * length is then not what is wrong. */
gboolean gh_outbox_text_fits(GhOutbox *self, const gchar *recipient_pubkey_hex,
                             const gchar *content);
/* The message's item (loaded from the store if needed), or NULL. */
GhOutboxItem *gh_outbox_lookup(GhOutbox *self, gint64 outbox_id);
GhOutboxItem *gh_outbox_lookup_message(GhOutbox *self, gint64 message_id);
/* The item of the own message with rumor id @rumor_id in the NIP-17 room
 * @room_key (gh_message_get_room_id()), whatever its state, including a
 * settled one, which is loaded from the store; NULL when the outbox never
 * held it (e.g. a self-copy of a message sent from another device). */
GhOutboxItem *gh_outbox_lookup_rumor(GhOutbox *self, const gchar *room_key,
                                     const gchar *rumor_id);
/* The items of every message this outbox holds, oldest first. */
GPtrArray *gh_outbox_dup_items(GhOutbox *self);

/* The user's Retry: a message that needs attention (or waits for its next
 * retry) is tried again now, including relays that failed too often with
 * "error:"; refusals such as "invalid:" or sign-in are still skipped. An
 * unsealed one is sealed again (the signer is asked). */
gboolean gh_outbox_retry(GhOutbox *self, gint64 outbox_id, GError **error);
/* Stops sending: nothing more is sealed or published, the message stays
 * (state CANCELLED). Refused (G_IO_ERROR_INVALID_ARGUMENT) once any
 * recipient's relay accepted it: that cannot be recalled. */
gboolean gh_outbox_cancel(GhOutbox *self, gint64 outbox_id, GError **error);
/* Stops sending and deletes the message and its outbox rows (seen keys
 * stay, so a self-copy that comes back is not shown again). */
gboolean gh_outbox_delete(GhOutbox *self, gint64 outbox_id, GError **error);
/* Drops every message whose outbox entry is no longer in the store, as if
 * deleted: nothing more of it is sealed or published, and "item-removed" is
 * emitted. The expiry purge deletes a disappearing message's entry (its
 * rumor and wraps) when it expires (gh-expiry.h "purged"). */
void gh_outbox_prune(GhOutbox *self);

G_END_DECLS
#endif
