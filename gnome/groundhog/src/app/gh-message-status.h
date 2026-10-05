#ifndef GH_MESSAGE_STATUS_H
#define GH_MESSAGE_STATUS_H

#include <glib-object.h>

#include "gh-relay-publish.h"

G_BEGIN_DECLS

/* The honest, user-facing state of one outgoing message (Groundhog privacy/UX
 * charter §3.6), GTK-free. A relay OK is acceptance by that relay, never
 * delivery: there is deliberately no DELIVERED or READ value (PD-1, UX-5). A
 * self-copy failure never changes this status (it is a secondary "Not saved
 * to your other devices" note). */
typedef enum {
  /* Not a sending state: an incoming message, or an own message the outbox
   * does not track (known only from its self-copy). Nothing is shown for it
   * and the copy getters below return NULL. */
  GH_MESSAGE_STATUS_NONE = -1,
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

/* The enum GType, e.g. for GhMessage:status (gh-message.h). GhOutboxItem:status
 * is an int holding these values. */
GType gh_message_status_get_type(void);
#define GH_TYPE_MESSAGE_STATUS (gh_message_status_get_type())

/* UX-5: every status but NONE has a short label, a symbolic icon name and a
 * full accessible description. Labels and descriptions are translatable
 * source strings, looked up in the application's text domain; none claims
 * delivery or reading. */
const gchar *gh_message_status_get_label(GhMessageStatus status);
const gchar *gh_message_status_get_icon_name(GhMessageStatus status);
const gchar *gh_message_status_get_accessible_description(GhMessageStatus status);
/* The description for a message to @n_recipients people (nostrc-lff5): in a
 * room (more than one) the copy that would speak of "the recipient" speaks
 * of everyone ("No one in this conversation has set up private messaging
 * yet"); otherwise gh_message_status_get_accessible_description(). */
const gchar *gh_message_status_get_accessible_description_for(GhMessageStatus status,
                                                              guint n_recipients);
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
  /* SEALED: a relay of this message waits for the user to approve signing
   * in to it as the account in Grotto (charter §4.4 R6): the
   * self-copy's own inbox relay, or a note to self's. */
  gboolean approval_pending;
  const GhTargetClass *recipients; /* SEALED: one combined class per recipient */
  gsize n_recipients;
} GhMessageStatusInput;

/* The status of one outgoing message. The self-copy's outcome is not an
 * input. A pending approval (approval_pending) is WAITING_FOR_SIGNER while
 * no recipient has the message yet and it is still going out; once one
 * does, the status says so. */
GhMessageStatus gh_message_status_derive(const GhMessageStatusInput *input);
/* Whether to show the self-copy note: the self-copy can no longer be stored
 * (TERMINAL, e.g. no own inbox relay), or the message gave up without it. */
gboolean gh_message_status_self_copy_missing(GhTargetClass self_copy, gboolean gave_up);
/* One plain-language sentence for a relay's outcome (details on demand). */
const gchar *gh_message_status_describe_target(GhRelayPublishOutcome outcome,
                                               GhRelayOkPrefix prefix);
/* The sentence for a relay that has not answered because signing in to it
 * waits for the user's approval in Grotto (see approval_pending). */
const gchar *gh_message_status_describe_approval(void);

G_END_DECLS
#endif
