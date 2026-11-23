#ifndef GH_DM_SEND_H
#define GH_DM_SEND_H

#include "gh-account-relays.h"
#include "gh-inbox-resolver.h"
#include "gh-nip17-envelope.h"
#include "gh-relay-publish.h"

G_BEGIN_DECLS

/* Resolve kind-10050 inboxes and seal the caller's stored rumor. The outbox
 * owns publishing and retries. Operations are main-context only and cancel
 * on account switch, caller cancellation or sender shutdown. */
typedef enum {
  GH_DM_SEND_PHASE_RESOLVING,
  GH_DM_SEND_PHASE_SEALING,    /* signer approvals pending */
  GH_DM_SEND_PHASE_DONE
} GhDmSendPhase;

typedef enum {
  GH_DM_SEND_RESULT_PENDING = 0,
  GH_DM_SEND_RESULT_SEALED,             /* seal-only operation: wraps ready, unpublished */
  GH_DM_SEND_RESULT_FAILED,             /* see GhDmSendFailure */
  GH_DM_SEND_RESULT_NO_RECIPIENT_INBOX, /* a recipient has no usable kind-10050 */
  GH_DM_SEND_RESULT_INBOX_UNKNOWN,      /* no inbox source could be asked or answered */
  GH_DM_SEND_RESULT_CANCELLED           /* caller cancel or account switch */
} GhDmSendResult;

typedef enum {
  GH_DM_SEND_FAILURE_NONE = 0,
  GH_DM_SEND_FAILURE_INVALID,  /* no/other active account, bad recipient, content or status */
  GH_DM_SEND_FAILURE_SIGNER,   /* signer unavailable, denied, or invalid result */
  GH_DM_SEND_FAILURE_ENVELOPE  /* local rumor/seal/wrap construction failed */
} GhDmSendFailure;

typedef enum {
  GH_DM_SEND_FLAG_NONE              = 0,
  GH_DM_SEND_FLAG_SELF_DM           = 1 << 0, /* note to self: one wrap */
  GH_DM_SEND_FLAG_NO_OWN_INBOX      = 1 << 2, /* no own 10050: self-copy has no target */
  GH_DM_SEND_FLAG_RECIPIENT_NO_INBOX = 1 << 6 /* a room recipient has no usable 10050:
                                               * their wrap has no target */
} GhDmSendFlags;

typedef struct {
  gchar *url;
} GhDmRelayOutcome;

/* One receiver's gift wrap and where it went. */
typedef struct {
  gchar *pubkey;           /* receiver (a recipient, or the sender for the self-copy) */
  GhInboxStatus inbox;     /* how the targets were found */
  gchar *inbox_event_id;   /* the kind-10050 used, if known */
  gchar *wrap_id;          /* NULL until sealed */
  gchar *wrap_json;        /* signed kind-1059; republish this, never re-seal */
  GPtrArray *relays;       /* GhDmRelayOutcome, in target order; may be empty */
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

#define GH_TYPE_DM_SEND (gh_dm_send_get_type())
G_DECLARE_FINAL_TYPE(GhDmSend, gh_dm_send, GH, DM_SEND, GObject)

/* Borrowed; valid until the next "changed" emission or the last unref. */
const GhDmSendStatus *gh_dm_send_get_status(GhDmSend *self);
gboolean gh_dm_send_is_done(GhDmSend *self);
/* Cancels an unfinished operation synchronously ("changed" is emitted). */
void gh_dm_send_cancel(GhDmSend *self);

#define GH_TYPE_DM_SENDER (gh_dm_sender_get_type())
G_DECLARE_FINAL_TYPE(GhDmSender, gh_dm_sender, GH, DM_SENDER, GObject)

GhDmSender *gh_dm_sender_new(GhAccountController *accounts,
                             GhAccountRelays *account_relays,
                             GhInboxResolver *inboxes);

/* Returns a new operation reference. Invalid rumors finish synchronously;
 * subsequent transitions emit "changed". An unreachable room member still
 * gets a wrap without targets if another member has a usable inbox.
 * @outer is copied; see gh_nip17_envelope_seal_room_async(). */
GhDmSend *gh_dm_sender_seal_room_rumor(GhDmSender *self, const gchar *rumor_json,
                                       const GhNip17RoomExpiration *outer,
                                       GCancellable *cancellable);
G_END_DECLS
#endif
