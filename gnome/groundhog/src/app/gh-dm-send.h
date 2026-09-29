#ifndef GH_DM_SEND_H
#define GH_DM_SEND_H

#include "gh-account-relays.h"
#include "gh-inbox-resolver.h"
#include "gh-message-status.h"
#include "gh-nip17-envelope.h"
#include "gh-relay-publish.h"

G_BEGIN_DECLS

/*
 * NIP-17 text send pipeline (GTK-free): one-to-one, a note to self, or a
 * room of up to GH_NIP17_MAX_SEND_RECIPIENTS other people (W17).
 *
 * Sealing and publishing are separate steps, so that a durable outbox can
 * persist the sealed status between them and later republish the stored
 * signed wraps byte-for-byte (it never re-seals):
 *
 *   gh_dm_sender_seal()     RESOLVING -> SEALING -> DONE (result SEALED)
 *   gh_dm_sender_publish()  PUBLISHING -> DONE, from a sealed status, no signer
 *   gh_dm_sender_send()     both, in one operation
 *
 * RESOLVING waits for the account's own relay lists to settle, then takes the
 * recipient's kind-10050 inbox from a GhInboxResolver (in the app, the
 * cached contact directory, gh-contact-directory.h). Kind 10002 is never consulted and there
 * is no default relay: without a usable 10050 nothing is sealed or published
 * (NO_RECIPIENT_INBOX, or INBOX_UNKNOWN when no source could be asked).
 * SEALING runs the signer approvals (NIP-44 encryption, seal signatures) and
 * yields one rumor, one gift wrap per recipient and one self-copy wrap.
 *
 * Rooms (W17): the one rumor carries a "p" tag per recipient; every
 * recipient's 10050 is looked up (concurrently), and each gets its own seal
 * and wrap with its own ephemeral key and randomized created_at, so relays
 * cannot link one recipient's wrap to another's. A recipient without a
 * usable 10050 still gets a sealed wrap, but it has no targets: nothing is
 * published for them, anywhere (FLAG_RECIPIENT_NO_INBOX, PARTIALLY_SENT at
 * best). Only when no recipient has one does the send stop before sealing
 * (NO_RECIPIENT_INBOX, or INBOX_UNKNOWN when a lookup could not be made).
 *
 * PUBLISHING sends each recipient's wrap ONLY to that recipient's 10050 relays
 * and the self-copy ONLY to the account's own 10050 relays (GhAccountRelays),
 * one GhRelayPublish per wrap, so no two wraps share a connection (a relay
 * on several recipients' lists gets each wrap separately). A room's
 * recipient wraps go out in a random order, U(0, 3) s apart, on the
 * sender's clock (charter §4.5 S4, nostrc-yp69; each publish draws its own).
 * Only
 * targets not already ACCEPTED are published to, so republishing a partially
 * settled status resumes the fanout. GhAuthPolicy (gh-auth-policy.h) picks
 * each URL's NIP-42 identity: a recipient's inbox relay only ever gets an
 * ephemeral key (never the account, §4.4 R1/R7); the self-copy and a note to
 * self sign in as the account, on challenge, on the own inbox relays.
 * AUTH_REQUIRED is reported per relay.
 *
 * Results are reported per recipient and for the self-copy. SENT means every
 * recipient's wrap was accepted (NIP-01 OK true) by at least one of that
 * recipient's inbox relays: relay-local acceptance, never delivery or reading.
 * A self-copy failure never changes the result; it only sets flags.
 * Nothing is retried automatically. gh_dm_send_status_get_message_status()
 * maps a status onto the charter's GhMessageStatus.
 *
 * Note to self (recipient == account): the rumor is p-tagged to self and
 * sealed into ONE wrap, reported as the only recipient leg and published to
 * the own inbox; there is no separate self-copy leg (self_copy is NULL).
 *
 * Generation-bound: an account switch, gh_dm_send_cancel() or the caller's
 * cancellable revokes the lookup REQ, pending signer approvals and publishes;
 * the operation then finishes CANCELLED, and a stale lookup, envelope or
 * relay OK is never reported as success. Relays that had already accepted
 * keep their ACCEPTED outcome, because a written EVENT cannot be recalled.
 *
 * Threading: main-context only.
 */

typedef enum {
  GH_DM_SEND_PHASE_RESOLVING,
  GH_DM_SEND_PHASE_SEALING,    /* signer approvals pending */
  GH_DM_SEND_PHASE_PUBLISHING,
  GH_DM_SEND_PHASE_DONE
} GhDmSendPhase;

typedef enum {
  GH_DM_SEND_RESULT_PENDING = 0,
  GH_DM_SEND_RESULT_SEALED,             /* seal-only operation: wraps ready, unpublished */
  GH_DM_SEND_RESULT_SENT,               /* every recipient: >= 1 inbox relay OK true */
  GH_DM_SEND_RESULT_PARTIALLY_SENT,     /* some recipients reached, others not */
  GH_DM_SEND_RESULT_FAILED,             /* see GhDmSendFailure */
  GH_DM_SEND_RESULT_NO_RECIPIENT_INBOX, /* a recipient has no usable kind-10050 */
  GH_DM_SEND_RESULT_INBOX_UNKNOWN,      /* no inbox source could be asked or answered */
  GH_DM_SEND_RESULT_CANCELLED           /* caller cancel or account switch */
} GhDmSendResult;

typedef enum {
  GH_DM_SEND_FAILURE_NONE = 0,
  GH_DM_SEND_FAILURE_INVALID,  /* no/other active account, bad recipient, content or status */
  GH_DM_SEND_FAILURE_SIGNER,   /* signer unavailable, denied, or invalid result */
  GH_DM_SEND_FAILURE_ENVELOPE, /* local rumor/seal/wrap construction failed */
  GH_DM_SEND_FAILURE_RELAYS    /* no recipient inbox relay accepted */
} GhDmSendFailure;

typedef enum {
  GH_DM_SEND_FLAG_NONE              = 0,
  GH_DM_SEND_FLAG_SELF_DM           = 1 << 0, /* note to self: one wrap */
  GH_DM_SEND_FLAG_RECIPIENT_PARTIAL = 1 << 1, /* some recipient relays did not accept */
  GH_DM_SEND_FLAG_NO_OWN_INBOX      = 1 << 2, /* no own 10050: self-copy has no target */
  GH_DM_SEND_FLAG_SELF_COPY_PARTIAL = 1 << 3, /* some own inbox relays did not accept */
  GH_DM_SEND_FLAG_SELF_COPY_FAILED  = 1 << 4, /* no own inbox relay accepted */
  GH_DM_SEND_FLAG_AUTH_REQUIRED     = 1 << 5, /* a relay answered auth-required: */
  GH_DM_SEND_FLAG_RECIPIENT_NO_INBOX = 1 << 6 /* a room recipient has no usable 10050:
                                               * their wrap has no target */
} GhDmSendFlags;

typedef struct {
  gchar *url;
  GhRelayPublishOutcome outcome; /* PENDING until that relay is terminal */
  GhRelayOkPrefix prefix;
  gchar *message;                /* relay OK message or local failure detail */
} GhDmRelayOutcome;

/* One receiver's gift wrap and where it went. */
typedef struct {
  gchar *pubkey;           /* receiver (a recipient, or the sender for the self-copy) */
  GhInboxStatus inbox;     /* how the targets were found */
  gchar *inbox_event_id;   /* the kind-10050 used, if known */
  gchar *wrap_id;          /* NULL until sealed */
  gchar *wrap_json;        /* signed kind-1059; republish this, never re-seal */
  GPtrArray *relays;       /* GhDmRelayOutcome, in target order; may be empty */
  guint accepted;
  gboolean complete;       /* every target relay reported (or there is none) */
} GhDmSendLeg;

/* Plain data so that a durable outbox can persist and restore it. */
typedef struct {
  GhDmSendPhase phase;
  GhDmSendResult result;
  GhDmSendFailure failure;
  GhDmSendFlags flags;
  gchar *error_message;       /* nullable human-readable detail */
  guint64 account_generation; /* process-local; not meaningful once persisted */
  gchar *sender;              /* hex pubkey */
  gchar *rumor_id;            /* for local display and dedup; NULL until sealed */
  gchar *rumor_json;          /* unsigned kind-14; never published in clear */
  GPtrArray *recipients;      /* GhDmSendLeg*, one per recipient, in rumor "p" order */
  GhDmSendLeg *self_copy;     /* NULL for a note to self */
} GhDmSendStatus;

GhDmSendStatus *gh_dm_send_status_copy(const GhDmSendStatus *status);
void gh_dm_send_status_free(GhDmSendStatus *status);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhDmSendStatus, gh_dm_send_status_free)

/* Charter §3.6 mapping. With no outbox yet, transient failures are NOT_SENT
 * (this layer never retries); QUEUED_OFFLINE/RETRYING are the outbox's. */
GhMessageStatus gh_dm_send_status_get_message_status(const GhDmSendStatus *status);
/* TRUE when the message is known to be stored on >= 1 own inbox relay (or it
 * is a note to self that was sent); FALSE means "Not saved to your other
 * devices" once the operation is done. */
gboolean gh_dm_send_status_self_copy_stored(const GhDmSendStatus *status);

#define GH_TYPE_DM_SEND (gh_dm_send_get_type())
G_DECLARE_FINAL_TYPE(GhDmSend, gh_dm_send, GH, DM_SEND, GObject)

/* Borrowed; valid until the next "changed" emission or the last unref. */
const GhDmSendStatus *gh_dm_send_get_status(GhDmSend *self);
gboolean gh_dm_send_is_done(GhDmSend *self);
/* Cancels an unfinished operation synchronously ("changed" is emitted). */
void gh_dm_send_cancel(GhDmSend *self);

#define GH_TYPE_DM_SENDER (gh_dm_sender_get_type())
G_DECLARE_FINAL_TYPE(GhDmSender, gh_dm_sender, GH, DM_SENDER, GObject)

/* transport NULL publishes over gnostr relays. */
GhDmSender *gh_dm_sender_new(GhAccountController *accounts,
                             GhAccountRelays *account_relays,
                             GhInboxResolver *inboxes,
                             const GhRelayPublishTransport *transport,
                             gpointer transport_data);
/* Per-relay publish failure bound (see gh_relay_publish_set_deadline). */
void gh_dm_sender_set_publish_deadline(GhDmSender *self, guint seconds);
/* The GhClock (src/store/gh-clock.h) that spaces a room's wraps (S4) and
 * draws their order; NULL: the system clock. Operations started later use
 * it. */
struct _GhClock;
void gh_dm_sender_set_clock(GhDmSender *self, struct _GhClock *clock);

/* Each returns a new reference to an operation the sender keeps alive until
 * it is done (dropping the reference does not cancel it). Invalid input
 * yields an operation that is already DONE/FAILED on return; every later
 * transition emits "changed" on the main context. */
GhDmSend *gh_dm_sender_send(GhDmSender *self, const gchar *recipient_pubkey_hex,
                            const gchar *content, GCancellable *cancellable);
GhDmSend *gh_dm_sender_seal(GhDmSender *self, const gchar *recipient_pubkey_hex,
                            const gchar *content, GCancellable *cancellable);
/* gh_dm_sender_send() to a room: @recipients holds 1 to
 * GH_NIP17_MAX_SEND_RECIPIENTS distinct hex pubkeys, never the account
 * among others (the account alone is a note to self). */
GhDmSend *gh_dm_sender_send_room(GhDmSender *self, const gchar *const *recipients,
                                 const gchar *content, GCancellable *cancellable);
/* Like gh_dm_sender_seal(), for a rumor the caller already stored (see
 * gh_nip17_rumor_new(); a durable outbox persists it before any signer
 * call): the recipient is its single "p" tag (the account itself for a note
 * to self), and the wraps carry exactly this rumor and its id. A rumor that
 * is not a canonical kind 14 of the active account finishes FAILED/INVALID
 * at once. */
GhDmSend *gh_dm_sender_seal_rumor(GhDmSender *self, const gchar *rumor_json,
                                  GCancellable *cancellable);
/* gh_dm_sender_seal_rumor() for a rumor that disappears (charter §3.7):
 * @outer (copied) gives each seal and gift wrap its expiration, see
 * gh_nip17_envelope_seal_expiring_async(). A rumor with an expiration
 * needs it; one without must pass NULL (otherwise FAILED/INVALID). */
GhDmSend *gh_dm_sender_seal_rumor_expiring(GhDmSender *self, const gchar *rumor_json,
                                           const GhNip17OuterExpiration *outer,
                                           GCancellable *cancellable);
/* gh_dm_sender_seal_rumor_expiring() for a rumor to any number of
 * recipients (gh_nip17_rumor_new_room()): its "p" tags are the recipients.
 * @outer (copied, nullable) as for gh_nip17_envelope_seal_room_async(). */
GhDmSend *gh_dm_sender_seal_room_rumor(GhDmSender *self, const gchar *rumor_json,
                                       const GhNip17RoomExpiration *outer,
                                       GCancellable *cancellable);
/* Republishes the stored wraps of a sealed (or partially published) status to
 * every target that has not ACCEPTED, byte-for-byte and without any signer
 * call. The status must belong to the active account; it is copied. */
GhDmSend *gh_dm_sender_publish(GhDmSender *self, const GhDmSendStatus *sealed,
                               GCancellable *cancellable);

G_END_DECLS
#endif
