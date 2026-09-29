#include "gh-message-status.h"

/* Marks translatable source strings for xgettext (--keyword=N_); tr() looks
 * them up in the application's text domain at run time. */
#define N_(text) (text)

static const gchar *
tr(const gchar *text)
{
  return g_dgettext(NULL, text);
}

/* ---- UX-5 copy ---------------------------------------------------------------- */

typedef struct {
  const gchar *label;
  const gchar *icon_name;
  const gchar *description;
} StatusCopy;

/* Charter §3.6 short labels. Descriptions are complete sentences for screen
 * readers; neither claims more than relay acceptance. */
static const StatusCopy status_copy[] = {
  [GH_MESSAGE_STATUS_WAITING_FOR_SIGNER] = {
    N_("Waiting for approval"), "dialog-password-symbolic",
    N_("Not sent yet. Waiting for you to approve sending in Nostr Signer.") },
  [GH_MESSAGE_STATUS_QUEUED_OFFLINE] = {
    N_("Waiting for connection"), "network-offline-symbolic",
    N_("Not sent yet. Groundhog will send it when you are back online.") },
  [GH_MESSAGE_STATUS_SENDING] = {
    N_("Sending…"), "content-loading-symbolic",
    N_("Sending to the recipient's message relays.") },
  [GH_MESSAGE_STATUS_SENT] = {
    N_("Sent"), "object-select-symbolic",
    N_("Sent. At least one of each recipient's message relays accepted it.") },
  [GH_MESSAGE_STATUS_PARTIALLY_SENT] = {
    N_("Sent to some people"), "dialog-warning-symbolic",
    N_("Sent to some recipients. Others' message relays have not accepted it.") },
  [GH_MESSAGE_STATUS_RETRYING] = {
    N_("Not sent yet · retrying"), "view-refresh-symbolic",
    N_("Not sent yet. Groundhog will try again automatically.") },
  [GH_MESSAGE_STATUS_NOT_SENT] = {
    N_("Not sent"), "dialog-error-symbolic",
    N_("Not sent. You can try again.") },
  [GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX] = {
    N_("Can't send"), "action-unavailable-symbolic",
    N_("Can't send. The recipient hasn't set up private messaging yet.") },
  [GH_MESSAGE_STATUS_CANCELLED] = {
    N_("Cancelled"), "edit-delete-symbolic",
    N_("Cancelled before it was sent.") },
};

/* NULL for NONE, which is shown as nothing. */
static const StatusCopy *
copy_for(GhMessageStatus status)
{
  if (status == GH_MESSAGE_STATUS_NONE)
    return NULL;
  g_return_val_if_fail((guint) status < G_N_ELEMENTS(status_copy), NULL);
  return &status_copy[status];
}

const gchar *
gh_message_status_get_label(GhMessageStatus status)
{
  const StatusCopy *copy = copy_for(status);
  return copy ? tr(copy->label) : NULL;
}

const gchar *
gh_message_status_get_icon_name(GhMessageStatus status)
{
  const StatusCopy *copy = copy_for(status);
  return copy ? copy->icon_name : NULL;
}

const gchar *
gh_message_status_get_accessible_description(GhMessageStatus status)
{
  const StatusCopy *copy = copy_for(status);
  return copy ? tr(copy->description) : NULL;
}

/* nostrc-lff5: what differs in a room of several people (W17). */
static const gchar *const room_descriptions[] = {
  [GH_MESSAGE_STATUS_SENDING] = N_("Sending to each person's message relays."),
  [GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX] =
    N_("Can't send. No one in this conversation has set up private messaging yet."),
};

const gchar *
gh_message_status_get_accessible_description_for(GhMessageStatus status, guint n_recipients)
{
  if (n_recipients > 1 && status >= 0 && (guint) status < G_N_ELEMENTS(room_descriptions) &&
      room_descriptions[status])
    return tr(room_descriptions[status]);
  return gh_message_status_get_accessible_description(status);
}

GType
gh_message_status_get_type(void)
{
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GH_MESSAGE_STATUS_NONE, "GH_MESSAGE_STATUS_NONE", "none" },
      { GH_MESSAGE_STATUS_WAITING_FOR_SIGNER, "GH_MESSAGE_STATUS_WAITING_FOR_SIGNER",
        "waiting-for-signer" },
      { GH_MESSAGE_STATUS_QUEUED_OFFLINE, "GH_MESSAGE_STATUS_QUEUED_OFFLINE", "queued-offline" },
      { GH_MESSAGE_STATUS_SENDING, "GH_MESSAGE_STATUS_SENDING", "sending" },
      { GH_MESSAGE_STATUS_SENT, "GH_MESSAGE_STATUS_SENT", "sent" },
      { GH_MESSAGE_STATUS_PARTIALLY_SENT, "GH_MESSAGE_STATUS_PARTIALLY_SENT", "partially-sent" },
      { GH_MESSAGE_STATUS_RETRYING, "GH_MESSAGE_STATUS_RETRYING", "retrying" },
      { GH_MESSAGE_STATUS_NOT_SENT, "GH_MESSAGE_STATUS_NOT_SENT", "not-sent" },
      { GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX, "GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX",
        "cannot-send-no-inbox" },
      { GH_MESSAGE_STATUS_CANCELLED, "GH_MESSAGE_STATUS_CANCELLED", "cancelled" },
      { 0, NULL, NULL }
    };
    g_once_init_leave(&type, g_enum_register_static(
      g_intern_static_string("GhMessageStatus"), values));
  }
  return type;
}

const gchar *
gh_message_status_get_self_copy_note(void)
{
  return tr(N_("Not saved to your other devices"));
}

/* ---- derivation --------------------------------------------------------------- */

gboolean
gh_target_is_final_refusal(GhRelayPublishOutcome outcome, GhRelayOkPrefix prefix)
{
  if (outcome == GH_RELAY_PUBLISH_AUTH_REQUIRED)
    return TRUE;
  if (outcome != GH_RELAY_PUBLISH_REJECTED)
    return FALSE;
  switch (prefix) {
  case GH_RELAY_OK_PREFIX_INVALID:
  case GH_RELAY_OK_PREFIX_POW:
  case GH_RELAY_OK_PREFIX_BLOCKED:
  case GH_RELAY_OK_PREFIX_RESTRICTED:
  case GH_RELAY_OK_PREFIX_MUTE:
  case GH_RELAY_OK_PREFIX_AUTH_REQUIRED:
    return TRUE;
  default:
    return FALSE;
  }
}

GhTargetClass
gh_target_classify(GhRelayPublishOutcome outcome, GhRelayOkPrefix prefix, guint attempts)
{
  switch (outcome) {
  case GH_RELAY_PUBLISH_PENDING:
    return GH_TARGET_CLASS_PENDING;
  case GH_RELAY_PUBLISH_ACCEPTED:
    return GH_TARGET_CLASS_ACCEPTED;
  case GH_RELAY_PUBLISH_CONNECTION_FAILED:
    /* Republishing the same id is idempotent; whether it was stored is unknown. */
    return GH_TARGET_CLASS_TRANSIENT;
  case GH_RELAY_PUBLISH_CANCELLED:
    return GH_TARGET_CLASS_RESUMABLE;
  case GH_RELAY_PUBLISH_AUTH_REQUIRED:
    return GH_TARGET_CLASS_TERMINAL;
  case GH_RELAY_PUBLISH_REJECTED:
    break;
  default:
    return GH_TARGET_CLASS_TERMINAL;
  }
  if (prefix == GH_RELAY_OK_PREFIX_DUPLICATE)
    return GH_TARGET_CLASS_ACCEPTED; /* the relay already has this id */
  if (prefix == GH_RELAY_OK_PREFIX_RATE_LIMITED)
    return GH_TARGET_CLASS_TRANSIENT;
  if (gh_target_is_final_refusal(outcome, prefix))
    return GH_TARGET_CLASS_TERMINAL;
  /* "error:" or no recognised prefix. */
  return attempts < GH_MESSAGE_STATUS_ERROR_ATTEMPTS ? GH_TARGET_CLASS_TRANSIENT
                                                     : GH_TARGET_CLASS_TERMINAL;
}

GhTargetClass
gh_target_class_combine(const GhTargetClass *targets, gsize n_targets)
{
  gboolean pending = FALSE;
  gboolean transient = FALSE;
  for (gsize i = 0; i < n_targets; i++) {
    switch (targets[i]) {
    case GH_TARGET_CLASS_ACCEPTED:
      return GH_TARGET_CLASS_ACCEPTED;
    case GH_TARGET_CLASS_PENDING:
    case GH_TARGET_CLASS_RESUMABLE:
      pending = TRUE;
      break;
    case GH_TARGET_CLASS_TRANSIENT:
      transient = TRUE;
      break;
    case GH_TARGET_CLASS_TERMINAL:
      break;
    }
  }
  if (pending)
    return GH_TARGET_CLASS_PENDING;
  return transient ? GH_TARGET_CLASS_TRANSIENT : GH_TARGET_CLASS_TERMINAL;
}

static GhMessageStatus
derive_unsealed(const GhMessageStatusInput *input)
{
  if (input->no_inbox)
    return GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX;
  if (input->gave_up)
    return GH_MESSAGE_STATUS_NOT_SENT;
  if (input->signer_pending)
    return GH_MESSAGE_STATUS_WAITING_FOR_SIGNER;
  if (!input->online)
    return GH_MESSAGE_STATUS_QUEUED_OFFLINE;
  /* Nothing leaves before the signer approves it. */
  return input->retry_scheduled ? GH_MESSAGE_STATUS_RETRYING
                                : GH_MESSAGE_STATUS_WAITING_FOR_SIGNER;
}

GhMessageStatus
gh_message_status_derive(const GhMessageStatusInput *input)
{
  g_return_val_if_fail(input != NULL, GH_MESSAGE_STATUS_NOT_SENT);
  if (input->phase == GH_MESSAGE_PHASE_CANCELLED)
    return GH_MESSAGE_STATUS_CANCELLED;
  if (input->phase == GH_MESSAGE_PHASE_UNSEALED)
    return derive_unsealed(input);

  gsize reached = 0;
  gboolean pending = FALSE;
  gboolean transient = FALSE;
  for (gsize i = 0; i < input->n_recipients; i++) {
    switch (input->recipients[i]) {
    case GH_TARGET_CLASS_ACCEPTED:
      reached++;
      break;
    case GH_TARGET_CLASS_PENDING:
    case GH_TARGET_CLASS_RESUMABLE:
      pending = TRUE;
      break;
    case GH_TARGET_CLASS_TRANSIENT:
      transient = TRUE;
      break;
    case GH_TARGET_CLASS_TERMINAL:
      break;
    }
  }
  if (input->n_recipients > 0 && reached == input->n_recipients)
    return GH_MESSAGE_STATUS_SENT;
  if (reached > 0)
    return GH_MESSAGE_STATUS_PARTIALLY_SENT;
  if (input->gave_up || (!pending && !transient))
    return GH_MESSAGE_STATUS_NOT_SENT;
  /* Nobody has it yet, and the signer is asking the user about it (§4.4
   * R6): say so rather than "Sending…". */
  if (input->approval_pending)
    return GH_MESSAGE_STATUS_WAITING_FOR_SIGNER;
  if (!input->online)
    return GH_MESSAGE_STATUS_QUEUED_OFFLINE;
  return pending ? GH_MESSAGE_STATUS_SENDING : GH_MESSAGE_STATUS_RETRYING;
}

gboolean
gh_message_status_self_copy_missing(GhTargetClass self_copy, gboolean gave_up)
{
  return self_copy == GH_TARGET_CLASS_TERMINAL ||
         (gave_up && self_copy != GH_TARGET_CLASS_ACCEPTED);
}

const gchar *
gh_message_status_describe_approval(void)
{
  return tr(N_("Waiting for your approval in Nostr Signer."));
}

const gchar *
gh_message_status_describe_target(GhRelayPublishOutcome outcome, GhRelayOkPrefix prefix)
{
  switch (outcome) {
  case GH_RELAY_PUBLISH_PENDING:
    return tr(N_("Not answered yet."));
  case GH_RELAY_PUBLISH_ACCEPTED:
    return prefix == GH_RELAY_OK_PREFIX_DUPLICATE
      ? tr(N_("Accepted: this relay already had it."))
      : tr(N_("Accepted by this relay."));
  case GH_RELAY_PUBLISH_AUTH_REQUIRED:
    return tr(N_("This relay requires sign-in."));
  case GH_RELAY_PUBLISH_CONNECTION_FAILED:
    return tr(N_("Couldn't reach this relay."));
  case GH_RELAY_PUBLISH_CANCELLED:
    return tr(N_("Paused before this relay answered."));
  case GH_RELAY_PUBLISH_REJECTED:
    break;
  }
  switch (prefix) {
  case GH_RELAY_OK_PREFIX_DUPLICATE:
    return tr(N_("Accepted: this relay already had it."));
  case GH_RELAY_OK_PREFIX_RATE_LIMITED:
    return tr(N_("This relay asked Groundhog to slow down."));
  case GH_RELAY_OK_PREFIX_INVALID:
    return tr(N_("This relay refused it as invalid."));
  case GH_RELAY_OK_PREFIX_POW:
    return tr(N_("This relay requires proof of work."));
  case GH_RELAY_OK_PREFIX_BLOCKED:
    return tr(N_("This relay blocked it."));
  case GH_RELAY_OK_PREFIX_RESTRICTED:
    /* §4.4 R7: typically after an ephemeral AUTH was not enough. */
    return tr(N_("This relay only accepts messages from signed-in users."));
  case GH_RELAY_OK_PREFIX_MUTE:
    return tr(N_("This relay muted it."));
  case GH_RELAY_OK_PREFIX_AUTH_REQUIRED:
    return tr(N_("This relay requires sign-in."));
  case GH_RELAY_OK_PREFIX_ERROR:
  case GH_RELAY_OK_PREFIX_NONE:
  default:
    return tr(N_("This relay reported an error."));
  }
}
