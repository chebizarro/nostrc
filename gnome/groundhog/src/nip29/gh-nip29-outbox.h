#ifndef GH_NIP29_OUTBOX_H
#define GH_NIP29_OUTBOX_H

#include "gh-account-controller.h"
#include "gh-message-status.h"
#include "gh-relay-publish.h"
#include "gh-store.h"

G_BEGIN_DECLS

/*
 * GhNip29Outbox: the durable outbox of one account's NIP-29 group events
 * (privacy charter §3.5, §3.6, §4.3, §8.2 G20a), GTK-free, over that
 * account's GhStore. It is the NIP-29 engine of the durable outbox: the same
 * tables and transactions as the NIP-17 GhOutbox (gh-outbox.h), which leaves
 * NIP-29 entries to "their own engine" (gh_store_outbox_list_unfinished() is
 * per backend), and the same §3.6 outcome classes and backoff.
 *
 * Lifecycle of one operation (every step persisted before the next):
 *  1. T-enqueue: the unsigned event (chat: with its outgoing message row,
 *     gh_store_enqueue(); join/leave/admin: without, see gh-store-nip29.h),
 *     before any signer call.
 *  2. Sign: the account signer (gh_account_controller_sign_*) signs exactly
 *     that event; the result must verify and keep its id, or it is refused.
 *  3. T-seal: the signed event with the group's relay as its only target
 *     (role GH_STORE_OUTBOX_ROLE_NIP29_EVENT), before any publish.
 *  4. Publish: the stored event, byte-for-byte, to exactly that relay on its
 *     own connection, authenticating as the account on challenge
 *     (GhAuthPolicy purpose GROUP: §4.3 "NIP-29 group read/write"); one
 *     T-outcome per relay answer.
 *  5. Retry: transient outcomes (rate-limited:, error: up to three times,
 *     connection failures) with backoff 15 s, 1 min, 5 min, 30 min, 2 h, then
 *     every 6 h, each x U(0.8, 1.2) on the store's GhClock, while online and
 *     while the account is active, for 72 h; then it needs attention.
 * After a restart unsigned operations are signed again (the signer is asked
 * again) and signed ones republish their stored event, never re-signed.
 *
 * Outcomes are relay-local and honest (GhNip29OpResult): ACCEPTED is the
 * relay's OK true, never delivery. NIP-29 specifics: a "duplicate:" answer
 * means the relay already has the event, or for a join request that the
 * account is already a member (DUPLICATE); a join request refused with a
 * message that says it waits for approval or review is PENDING_APPROVAL (the
 * request is recorded; nothing is retried); other refusals are REJECTED with
 * the relay's reason, and the caller changes nothing locally (the relay's
 * next 39000-39003 snapshot is the truth).
 *
 * Generation binding: it runs only while the store's account is the active
 * account; a switch cancels every signature and publish in flight (no late OK
 * is recorded) and leaves the rows resumable. Main context only. The store is
 * borrowed: dispose the outbox before closing the store.
 */

typedef enum {
  GH_NIP29_OP_QUEUED,            /* stored, not signed yet (or offline, or inactive) */
  GH_NIP29_OP_WAITING_FOR_SIGNER,/* the signer is asking the user */
  GH_NIP29_OP_SENDING,           /* signed and stored; the relay has not answered */
  GH_NIP29_OP_RETRYING,          /* a transient failure; next_attempt_at is set */
  GH_NIP29_OP_ACCEPTED,          /* the group relay's OK true */
  GH_NIP29_OP_DUPLICATE,         /* already there / already a member ("duplicate:") */
  GH_NIP29_OP_PENDING_APPROVAL,  /* a join request the relay holds for review */
  GH_NIP29_OP_REJECTED,          /* refused by the relay; see the relay message */
  GH_NIP29_OP_NOT_SENT,          /* signer refused, sign-in refused, window over, storage */
  GH_NIP29_OP_CANCELLED
} GhNip29OpResult;

GType gh_nip29_op_result_get_type(void);
#define GH_TYPE_NIP29_OP_RESULT (gh_nip29_op_result_get_type())

/* Whether the result is final (nothing more happens without a retry). */
gboolean gh_nip29_op_result_is_final(GhNip29OpResult result);
/* The honest message status of a chat message with this result. */
GhMessageStatus gh_nip29_op_result_to_message_status(GhNip29OpResult result,
                                                      gboolean online);

/* Classifies a group relay's answer to an event of kind (the §3.6 classes
 * with the NIP-29 overlay above); exported for tests. attempts counts the
 * attempt that produced the answer. */
GhNip29OpResult gh_nip29_classify_answer(gint kind, GhRelayPublishOutcome outcome,
                                         GhRelayOkPrefix prefix, const gchar *message,
                                         guint attempts);

#define GH_TYPE_NIP29_OP (gh_nip29_op_get_type())
G_DECLARE_FINAL_TYPE(GhNip29Op, gh_nip29_op, GH, NIP29_OP, GObject)

/* One queued group event. Read-only properties, notified on change:
 * "outbox-id", "message-id" (0 for a join/leave/admin request),
 * "conversation-id", "kind", "result" (GhNip29OpResult), "relay-message" (the
 * relay's OK text, bounded, or a local reason), "next-attempt-at" (unix
 * seconds, 0 = none) and "can-retry". */
gint64 gh_nip29_op_get_outbox_id(GhNip29Op *self);
gint64 gh_nip29_op_get_message_id(GhNip29Op *self);
gint64 gh_nip29_op_get_conversation_id(GhNip29Op *self);
gint gh_nip29_op_get_kind(GhNip29Op *self);
/* The event's id (the unsigned template's, which signing must keep). */
const gchar *gh_nip29_op_get_event_id(GhNip29Op *self);
/* The group relay (normalized) and the group id. */
const gchar *gh_nip29_op_get_relay_url(GhNip29Op *self);
const gchar *gh_nip29_op_get_group_id(GhNip29Op *self);
GhNip29OpResult gh_nip29_op_get_result(GhNip29Op *self);
const gchar *gh_nip29_op_get_relay_message(GhNip29Op *self);
gint64 gh_nip29_op_get_next_attempt_at(GhNip29Op *self);
gboolean gh_nip29_op_get_can_retry(GhNip29Op *self);
/* The signed event as stored and published; NULL until signed. */
const gchar *gh_nip29_op_get_signed_json(GhNip29Op *self);

typedef struct {
  GhStore *store;                  /* the account's open store; borrowed */
  GhAccountController *accounts;   /* signer and generation */
  GNetworkMonitor *network;        /* NULL: g_network_monitor_get_default() */
  /* NULL: gnostr relays (with NIP-42). A custom transport authenticates only
   * with auth_transport (tests). */
  const GhRelayPublishTransport *transport;
  const GhRelayPublishAuthTransport *auth_transport;
  gpointer transport_data;
  guint publish_deadline;          /* per-relay seconds; 0: the publish default */
} GhNip29OutboxConfig;

#define GH_TYPE_NIP29_OUTBOX (gh_nip29_outbox_get_type())
G_DECLARE_FINAL_TYPE(GhNip29Outbox, gh_nip29_outbox, GH, NIP29_OUTBOX, GObject)

/* Loads every unfinished NIP-29 entry of the store and resumes them once
 * the store's account is active and the network is available. Signals:
 * "op-added" (GhNip29Op) and "op-changed" (GhNip29Op) after its result,
 * relay message or next attempt changed. */
GhNip29Outbox *gh_nip29_outbox_new(const GhNip29OutboxConfig *config, GError **error);
gboolean gh_nip29_outbox_is_active(GhNip29Outbox *self);

/* T-enqueue of unsigned_json (an unsigned NIP-29 event of the account with
 * the group's h tag; gh-nip29-template.h) for the group whose conversation is
 * conversation_id, then sending it. A chat message (kind 9-12) is stored
 * with its outgoing message row (body = its content); any other kind has
 * none. Errors: G_IO_ERROR_PERMISSION_DENIED when the store's account is not
 * the active one, G_IO_ERROR_INVALID_ARGUMENT for an event that is not the
 * account's unsigned group event of that conversation, or the store's error
 * (e.g. GH_STORE_ERROR_FULL). Once T-enqueue has committed this never
 * fails. Nothing is signed or published before this returns. */
GhNip29Op *gh_nip29_outbox_enqueue(GhNip29Outbox *self, gint64 conversation_id,
                                   const gchar *unsigned_json, GError **error);
/* The operation (loaded from the store if needed), or NULL. */
GhNip29Op *gh_nip29_outbox_lookup(GhNip29Outbox *self, gint64 outbox_id);
/* Every operation this outbox holds, oldest first. */
GPtrArray *gh_nip29_outbox_dup_ops(GhNip29Outbox *self);
/* The user's Retry of an operation that is not sent or waits for its next
 * attempt: tried again now (an unsigned one is signed again). Refusals
 * that are final by nature ("blocked:", "invalid:", sign-in required) stay
 * refused unless republishing could change them; G_IO_ERROR_INVALID_ARGUMENT
 * when there is nothing to retry. */
gboolean gh_nip29_outbox_retry(GhNip29Outbox *self, GhNip29Op *op, GError **error);
/* Stops an operation the relay has not accepted: nothing more is signed or
 * published (state CANCELLED). G_IO_ERROR_INVALID_ARGUMENT once the relay
 * accepted it. */
gboolean gh_nip29_outbox_cancel(GhNip29Outbox *self, GhNip29Op *op, GError **error);

G_END_DECLS
#endif
