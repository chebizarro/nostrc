#ifndef GH_MESSAGE_STATUS_H
#define GH_MESSAGE_STATUS_H

#include <glib.h>

#include "gh-relay-publish.h"

G_BEGIN_DECLS

/* The honest, user-facing state of one outgoing message (Groundhog privacy/UX
 * charter §3.6), GTK-free. A relay OK is acceptance by that relay, never
 * delivery: there is deliberately no DELIVERED or READ value (PD-1, UX-5). A
 * self-copy failure never changes this status (it is a secondary "Not saved
 * to your other devices" note). */
typedef enum {
  GH_MESSAGE_STATUS_WAITING_FOR_SIGNER,   /* "Waiting for approval" */
  GH_MESSAGE_STATUS_QUEUED_OFFLINE,       /* "Waiting for connection" */
  GH_MESSAGE_STATUS_SENDING,              /* "Sending…": no recipient reached yet */
  GH_MESSAGE_STATUS_SENT,                 /* every recipient: >= 1 inbox relay accepted */
  GH_MESSAGE_STATUS_PARTIALLY_SENT,       /* some recipients reached, others not */
  GH_MESSAGE_STATUS_RETRYING,             /* none reached, transient failures (outbox) */
  GH_MESSAGE_STATUS_NOT_SENT,             /* all terminal, or needs attention */
  GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX, /* a recipient has no kind-10050 */
  GH_MESSAGE_STATUS_CANCELLED
} GhMessageStatus;

/* No GType is registered here: the conversation model's interim
 * GhMessage:status (gh-message.h) still registers "GhMessageStatus"; G12/G13
 * move it onto this enum. GhOutboxItem:status is an int holding these values. */

/* UX-5: every status has a short label, a symbolic icon name and a full
 * accessible description. Labels and descriptions are translatable source
 * strings, looked up in the application's text domain; none claims delivery
 * or reading. */
const gchar *gh_message_status_get_label(GhMessageStatus status);
const gchar *gh_message_status_get_icon_name(GhMessageStatus status);
const gchar *gh_message_status_get_accessible_description(GhMessageStatus status);
/* The secondary note for a self-copy that will not be stored. */
const gchar *gh_message_status_get_self_copy_note(void);

/* ---- Derivation from per-relay outcomes (§3.6) -------------------------------- */

/* What one relay target of one stored event means for the outbox. */
typedef enum {
  GH_TARGET_CLASS_PENDING,   /* not reported yet: new, in flight, or never tried */
  GH_TARGET_CLASS_ACCEPTED,  /* relay-local OK true, or "duplicate:" (it has the id) */
  GH_TARGET_CLASS_TRANSIENT, /* retried with backoff */
  GH_TARGET_CLASS_RESUMABLE, /* cancelled by an account switch or quit; not an attempt */
  GH_TARGET_CLASS_TERMINAL   /* never retried automatically */
} GhTargetClass;

/* "error:" and prefix-less refusals are transient this many times, then terminal. */
#define GH_MESSAGE_STATUS_ERROR_ATTEMPTS 3

/* The §3.6 table: @attempts is the target's counted attempts so far,
 * including the one that produced @outcome. AUTH_REQUIRED is terminal: the
 * relay layer has already made its one AUTH per challenge (§4.4 R5). */
GhTargetClass gh_target_classify(GhRelayPublishOutcome outcome, GhRelayOkPrefix prefix,
                                 guint attempts);
/* Terminal no matter how often it is tried ("invalid:", "blocked:", sign-in
 * required, ...), as opposed to "error:" that became terminal by count. A
 * user's explicit Retry still skips these. */
gboolean gh_target_is_final_refusal(GhRelayPublishOutcome outcome, GhRelayOkPrefix prefix);
/* One receiver's copy: ACCEPTED if any target accepted; otherwise PENDING if
 * any is pending or resumable; otherwise TRANSIENT if any is; otherwise
 * TERMINAL (including no target at all). */
GhTargetClass gh_target_class_combine(const GhTargetClass *targets, gsize n_targets);

typedef enum {
  GH_MESSAGE_PHASE_UNSEALED,  /* QUEUED/SEALING: nothing signed yet */
  GH_MESSAGE_PHASE_SEALED,    /* signed events stored; per-target outcomes apply */
  GH_MESSAGE_PHASE_CANCELLED
} GhMessagePhase;

typedef struct {
  GhMessagePhase phase;
  gboolean online;           /* network available and the account active */
  gboolean gave_up;          /* NEEDS_ATTENTION: retry window over, or sealing failed */
  gboolean signer_pending;   /* UNSEALED: a signer approval is being asked */
  gboolean no_inbox;         /* UNSEALED: a recipient has no usable kind-10050 */
  gboolean retry_scheduled;  /* UNSEALED: the inbox lookup failed, retrying */
  const GhTargetClass *recipients; /* SEALED: one combined class per recipient */
  gsize n_recipients;
} GhMessageStatusInput;

/* The status of one outgoing message. The self-copy is not an input. */
GhMessageStatus gh_message_status_derive(const GhMessageStatusInput *input);
/* Whether to show the self-copy note: the self-copy can no longer be stored
 * (TERMINAL, e.g. no own inbox relay), or the message gave up without it. */
gboolean gh_message_status_self_copy_missing(GhTargetClass self_copy, gboolean gave_up);
/* One plain-language sentence for a relay's outcome (details on demand). */
const gchar *gh_message_status_describe_target(GhRelayPublishOutcome outcome,
                                               GhRelayOkPrefix prefix);

G_END_DECLS
#endif
