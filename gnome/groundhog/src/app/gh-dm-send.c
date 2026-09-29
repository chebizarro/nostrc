#include "gh-dm-send.h"
#include "gh-auth-policy.h"
#include "gh-identity.h"
#include "gh-nip17-envelope.h"
#include "gh-signer.h"
#include "nostr/nip59/nip59.h"

#include <nostr-event.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TARGETS 16 /* the relay publish's own bound */

typedef enum { MODE_SEND, MODE_SEAL, MODE_PUBLISH } Mode;

struct _GhDmSender {
  GObject parent_instance;
  GhAccountController *accounts; /* NULL once disposed */
  GhAccountRelays *account_relays;
  GhInboxResolver *inboxes;
  GhRelayPublishTransport transport;
  gpointer transport_data;
  gboolean custom_transport;
  guint publish_deadline; /* 0: the publish default */
  GHashTable *inflight;   /* GhDmSend (owned) until done */
};

struct _GhDmSend {
  GObject parent_instance;
  GhDmSender *sender; /* owned while in flight, NULL once done */
  GhDmSendStatus status;
  Mode mode;
  gchar *content;     /* wiped once sealed */
  gchar *rumor;       /* seal_rumor: the caller's stored rumor; wiped once sealed */
  gboolean has_outer; /* the rumor expires: outer holds its layers' expirations */
  GhNip17OuterExpiration outer;
  gboolean self_dm;
  GMainContext *context;
  GCancellable *cancel; /* revokes the inbox lookup and signer approvals */
  GSource *start;       /* pending low-priority continuation */
  GSource *caller_cancel;
  GSource *generation_cancel;
  gulong relays_handler;
  void (*after_relays)(GhDmSend *self);
  GHashTable *publishes; /* GhRelayPublish (owned) -> GhDmSendLeg (borrowed) */
};

enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhDmSend, gh_dm_send, G_TYPE_OBJECT)
G_DEFINE_FINAL_TYPE(GhDmSender, gh_dm_sender, G_TYPE_OBJECT)

/* ---- status -------------------------------------------------------------- */

static void
relay_outcome_free(gpointer data)
{
  GhDmRelayOutcome *outcome = data;
  g_free(outcome->url);
  g_free(outcome->message);
  g_free(outcome);
}

static GhDmSendLeg *
leg_new(const gchar *pubkey)
{
  GhDmSendLeg *leg = g_new0(GhDmSendLeg, 1);
  leg->pubkey = g_strdup(pubkey);
  leg->inbox = GH_INBOX_NOT_FOUND;
  leg->relays = g_ptr_array_new_with_free_func(relay_outcome_free);
  return leg;
}

static void
leg_free(gpointer data)
{
  GhDmSendLeg *leg = data;
  if (!leg)
    return;
  g_free(leg->pubkey);
  g_free(leg->inbox_event_id);
  g_free(leg->wrap_id);
  g_free(leg->wrap_json);
  g_ptr_array_unref(leg->relays);
  g_free(leg);
}

static GhDmSendLeg *
leg_copy(const GhDmSendLeg *src)
{
  if (!src)
    return NULL;
  GhDmSendLeg *leg = leg_new(src->pubkey);
  leg->inbox = src->inbox;
  leg->inbox_event_id = g_strdup(src->inbox_event_id);
  leg->wrap_id = g_strdup(src->wrap_id);
  leg->wrap_json = g_strdup(src->wrap_json);
  leg->accepted = src->accepted;
  leg->complete = src->complete;
  for (guint i = 0; src->relays && i < src->relays->len; i++) {
    const GhDmRelayOutcome *from = g_ptr_array_index(src->relays, i);
    GhDmRelayOutcome *to = g_new0(GhDmRelayOutcome, 1);
    *to = *from;
    to->url = g_strdup(from->url);
    to->message = g_strdup(from->message);
    g_ptr_array_add(leg->relays, to);
  }
  return leg;
}

static void
status_clear(GhDmSendStatus *status)
{
  g_clear_pointer(&status->error_message, g_free);
  g_clear_pointer(&status->sender, g_free);
  g_clear_pointer(&status->rumor_id, g_free);
  g_clear_pointer(&status->rumor_json, g_free);
  g_clear_pointer(&status->recipients, g_ptr_array_unref);
  g_clear_pointer(&status->self_copy, leg_free);
}

static void
status_copy_into(GhDmSendStatus *dest, const GhDmSendStatus *src)
{
  *dest = *src;
  dest->error_message = g_strdup(src->error_message);
  dest->sender = g_strdup(src->sender);
  dest->rumor_id = g_strdup(src->rumor_id);
  dest->rumor_json = g_strdup(src->rumor_json);
  dest->recipients = g_ptr_array_new_with_free_func(leg_free);
  for (guint i = 0; src->recipients && i < src->recipients->len; i++)
    g_ptr_array_add(dest->recipients, leg_copy(g_ptr_array_index(src->recipients, i)));
  dest->self_copy = leg_copy(src->self_copy);
}

GhDmSendStatus *
gh_dm_send_status_copy(const GhDmSendStatus *status)
{
  g_return_val_if_fail(status != NULL, NULL);
  GhDmSendStatus *copy = g_new0(GhDmSendStatus, 1);
  status_copy_into(copy, status);
  return copy;
}

void
gh_dm_send_status_free(GhDmSendStatus *status)
{
  if (!status)
    return;
  status_clear(status);
  g_free(status);
}

static guint
recipients_reached(const GhDmSendStatus *status)
{
  guint reached = 0;
  for (guint i = 0; status->recipients && i < status->recipients->len; i++)
    reached += ((GhDmSendLeg *)g_ptr_array_index(status->recipients, i))->accepted > 0;
  return reached;
}

GhMessageStatus
gh_dm_send_status_get_message_status(const GhDmSendStatus *status)
{
  g_return_val_if_fail(status != NULL, GH_MESSAGE_STATUS_NOT_SENT);
  switch (status->result) {
  case GH_DM_SEND_RESULT_CANCELLED:          return GH_MESSAGE_STATUS_CANCELLED;
  case GH_DM_SEND_RESULT_NO_RECIPIENT_INBOX: return GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX;
  case GH_DM_SEND_RESULT_SENT:               return GH_MESSAGE_STATUS_SENT;
  case GH_DM_SEND_RESULT_PARTIALLY_SENT:     return GH_MESSAGE_STATUS_PARTIALLY_SENT;
  case GH_DM_SEND_RESULT_FAILED:
  case GH_DM_SEND_RESULT_INBOX_UNKNOWN:      return GH_MESSAGE_STATUS_NOT_SENT;
  case GH_DM_SEND_RESULT_SEALED:             return GH_MESSAGE_STATUS_SENDING;
  case GH_DM_SEND_RESULT_PENDING:            break;
  }
  if (status->phase == GH_DM_SEND_PHASE_SEALING)
    return GH_MESSAGE_STATUS_WAITING_FOR_SIGNER;
  guint reached = recipients_reached(status);
  guint total = status->recipients ? status->recipients->len : 0;
  if (total && reached == total)
    return GH_MESSAGE_STATUS_SENT;
  return reached ? GH_MESSAGE_STATUS_PARTIALLY_SENT : GH_MESSAGE_STATUS_SENDING;
}

gboolean
gh_dm_send_status_self_copy_stored(const GhDmSendStatus *status)
{
  g_return_val_if_fail(status != NULL, FALSE);
  if (status->self_copy)
    return status->self_copy->accepted > 0;
  /* A note to self is its own copy. */
  return status->recipients && status->recipients->len &&
         ((GhDmSendLeg *)g_ptr_array_index(status->recipients, 0))->accepted > 0;
}

/* Validated, deduplicated targets from an inbox list; FALSE when none. */
static gboolean
leg_set_targets(GhDmSendLeg *leg, const gchar *const *urls)
{
  for (guint i = 0; urls && urls[i] && leg->relays->len < MAX_TARGETS; i++) {
    gboolean seen = FALSE;
    for (guint j = 0; j < leg->relays->len && !seen; j++)
      seen = g_strcmp0(((GhDmRelayOutcome *)g_ptr_array_index(leg->relays, j))->url,
                       urls[i]) == 0;
    if (seen || !gh_relay_url_validate(urls[i], NULL))
      continue;
    GhDmRelayOutcome *outcome = g_new0(GhDmRelayOutcome, 1);
    outcome->url = g_strdup(urls[i]);
    outcome->outcome = GH_RELAY_PUBLISH_PENDING;
    g_ptr_array_add(leg->relays, outcome);
  }
  return leg->relays->len > 0;
}

static GhDmRelayOutcome *
leg_outcome(GhDmSendLeg *leg, const gchar *url)
{
  for (guint i = 0; i < leg->relays->len; i++) {
    GhDmRelayOutcome *outcome = g_ptr_array_index(leg->relays, i);
    if (g_strcmp0(outcome->url, url) == 0)
      return outcome;
  }
  return NULL;
}

static void
leg_mark_pending(GhDmSendLeg *leg, GhRelayPublishOutcome outcome, const gchar *message)
{
  for (guint i = 0; leg && i < leg->relays->len; i++) {
    GhDmRelayOutcome *entry = g_ptr_array_index(leg->relays, i);
    if (entry->outcome != GH_RELAY_PUBLISH_PENDING)
      continue;
    entry->outcome = outcome;
    g_free(entry->message);
    entry->message = g_strdup(message);
  }
}

static GhDmSendLeg *
first_recipient(GhDmSend *self)
{
  return g_ptr_array_index(self->status.recipients, 0);
}

/* ---- operation lifecycle ------------------------------------------------- */

static void
emit_changed(GhDmSend *self)
{
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

static gboolean
is_done(GhDmSend *self)
{
  return self->status.phase == GH_DM_SEND_PHASE_DONE;
}

static gboolean
is_current(GhDmSend *self)
{
  return self->sender && self->sender->accounts &&
         gh_account_controller_is_current(self->sender->accounts,
                                          self->status.account_generation) &&
         !g_cancellable_is_cancelled(self->cancel);
}

static void
destroy_source(GSource **source)
{
  if (!*source)
    return;
  g_source_destroy(*source);
  g_clear_pointer(source, g_source_unref);
}

static void
wipe_string(gchar **text)
{
  if (!*text)
    return;
  memset(*text, 0, strlen(*text));
  g_clear_pointer(text, g_free);
}

static void
wipe_content(GhDmSend *self)
{
  wipe_string(&self->content);
  wipe_string(&self->rumor);
}

/* Terminal transition; runs once. Every hook is released, and the inbox
 * lookup and any signer approval still pending are revoked via self->cancel. */
static void
finish(GhDmSend *self, GhDmSendResult result, GhDmSendFailure failure,
       const gchar *message, gboolean emit)
{
  if (is_done(self))
    return;
  g_object_ref(self);
  GHashTableIter iter;
  gpointer publish;
  g_hash_table_iter_init(&iter, self->publishes);
  while (g_hash_table_iter_next(&iter, &publish, NULL))
    gh_relay_publish_cancel(publish);
  g_hash_table_remove_all(self->publishes);
  destroy_source(&self->start);
  destroy_source(&self->caller_cancel);
  destroy_source(&self->generation_cancel);
  if (self->relays_handler) {
    g_signal_handler_disconnect(self->sender->account_relays, self->relays_handler);
    self->relays_handler = 0;
  }
  g_cancellable_cancel(self->cancel);
  wipe_content(self);
  GhDmSendStatus *status = &self->status;
  if (result == GH_DM_SEND_RESULT_CANCELLED) {
    for (guint i = 0; status->recipients && i < status->recipients->len; i++)
      leg_mark_pending(g_ptr_array_index(status->recipients, i),
                       GH_RELAY_PUBLISH_CANCELLED, NULL);
    leg_mark_pending(status->self_copy, GH_RELAY_PUBLISH_CANCELLED, NULL);
  }
  status->result = result;
  status->failure = failure;
  if (message && !status->error_message)
    status->error_message = g_strdup(message);
  status->phase = GH_DM_SEND_PHASE_DONE;
  GhDmSender *sender = g_steal_pointer(&self->sender);
  if (emit)
    emit_changed(self);
  if (sender) {
    g_hash_table_remove(sender->inflight, self);
    g_object_unref(sender);
  }
  g_object_unref(self);
}

static void
finish_cancelled(GhDmSend *self)
{
  finish(self, GH_DM_SEND_RESULT_CANCELLED, GH_DM_SEND_FAILURE_NONE,
         "Cancelled, or the account changed", TRUE);
}

static void
finish_invalid(GhDmSend *self, const gchar *message)
{
  finish(self, GH_DM_SEND_RESULT_FAILED, GH_DM_SEND_FAILURE_INVALID, message, FALSE);
}

static gboolean
on_cancelled(GCancellable *cancellable, gpointer data)
{
  (void)cancellable;
  finish_cancelled(data);
  return G_SOURCE_REMOVE;
}

static GSource *
watch_cancellable(GhDmSend *self, GCancellable *cancellable)
{
  GSource *source = g_cancellable_source_new(cancellable);
  g_source_set_callback(source, G_SOURCE_FUNC(on_cancelled), self, NULL);
  g_source_attach(source, self->context);
  return source;
}

/* Registers an operation with its sender and binds it to the caller's
 * cancellable and the current account generation. */
static gboolean
track(GhDmSend *self, GhDmSender *sender, GCancellable *cancellable)
{
  self->status.account_generation = gh_account_controller_get_generation(sender->accounts);
  self->sender = g_object_ref(sender);
  g_hash_table_add(sender->inflight, g_object_ref(self));
  if (cancellable && g_cancellable_is_cancelled(cancellable)) {
    finish(self, GH_DM_SEND_RESULT_CANCELLED, GH_DM_SEND_FAILURE_NONE,
           "Cancelled before start", FALSE);
    return FALSE;
  }
  if (cancellable)
    self->caller_cancel = watch_cancellable(self, cancellable);
  self->generation_cancel =
    watch_cancellable(self, gh_account_controller_get_cancellable(sender->accounts));
  return TRUE;
}

static void
schedule(GhDmSend *self, gint priority, GSourceFunc func)
{
  destroy_source(&self->start);
  self->start = g_idle_source_new();
  g_source_set_priority(self->start, priority);
  g_source_set_callback(self->start, func, self, NULL);
  g_source_attach(self->start, self->context);
}

/* ---- own relay lists ----------------------------------------------------- */

/* The account's own relay lists (own inbox for the self-copy; read/write as
 * lookup sources) for this generation, once their discovery has settled. */
static gboolean
own_relays_settled(GhDmSend *self)
{
  GhAccountRelays *relays = self->sender->account_relays;
  GhAccountRelaysState state = gh_account_relays_get_state(relays);
  return gh_account_relays_get_generation(relays) == self->status.account_generation &&
         state != GH_ACCOUNT_RELAYS_DISCOVERING && state != GH_ACCOUNT_RELAYS_INACTIVE;
}

/* How the own inbox list stands once settled, as an inbox status. */
static GhInboxStatus
own_inbox_status(GhDmSend *self)
{
  GhAccountRelays *relays = self->sender->account_relays;
  const gchar *const *own = gh_account_relays_get_inbox_relays(relays);
  if (own)
    return own[0] ? GH_INBOX_FOUND : GH_INBOX_EMPTY;
  switch (gh_account_relays_get_state(relays)) {
  case GH_ACCOUNT_RELAYS_COMPLETE:   return GH_INBOX_NOT_FOUND;
  case GH_ACCOUNT_RELAYS_NO_SOURCES: return GH_INBOX_NO_SOURCES;
  default:                           return GH_INBOX_UNREACHABLE;
  }
}

static void on_account_relays_changed(GhAccountRelays *relays, gpointer data);

/* The own lists settle at their EOSE, which the transport delivers after the
 * stored events before it (nostrc-qp24.10.6), so the own 10050 list is in. */
static gboolean
own_relays_ready(gpointer data)
{
  GhDmSend *self = data;
  g_clear_pointer(&self->start, g_source_unref);
  if (is_done(self))
    return G_SOURCE_REMOVE;
  if (!is_current(self)) {
    finish_cancelled(self);
    return G_SOURCE_REMOVE;
  }
  if (!own_relays_settled(self)) {
    self->relays_handler = g_signal_connect(self->sender->account_relays, "changed",
                                            G_CALLBACK(on_account_relays_changed), self);
    return G_SOURCE_REMOVE;
  }
  self->after_relays(self);
  return G_SOURCE_REMOVE;
}

static void
on_account_relays_changed(GhAccountRelays *relays, gpointer data)
{
  GhDmSend *self = data;
  (void)relays;
  if (is_done(self) || !own_relays_settled(self))
    return;
  g_signal_handler_disconnect(self->sender->account_relays, self->relays_handler);
  self->relays_handler = 0;
  schedule(self, G_PRIORITY_DEFAULT, own_relays_ready);
}

/* Continues with next once the own relay lists of this generation have
 * settled; always from the main loop, never synchronously. */
static void
await_own_relays(GhDmSend *self, void (*next)(GhDmSend *self))
{
  self->after_relays = next;
  if (own_relays_settled(self)) {
    schedule(self, G_PRIORITY_DEFAULT, own_relays_ready);
    return;
  }
  self->relays_handler = g_signal_connect(self->sender->account_relays, "changed",
                                          G_CALLBACK(on_account_relays_changed), self);
}

/* ---- publishing ---------------------------------------------------------- */

static void
finalize_result(GhDmSend *self)
{
  GhDmSendStatus *status = &self->status;
  for (guint i = 0; i < status->recipients->len; i++) {
    GhDmSendLeg *leg = g_ptr_array_index(status->recipients, i);
    if (leg->accepted > 0 && leg->accepted < leg->relays->len)
      status->flags |= GH_DM_SEND_FLAG_RECIPIENT_PARTIAL;
  }
  GhDmSendLeg *copy = status->self_copy;
  if (copy && copy->relays->len > 0) {
    if (copy->accepted == 0)
      status->flags |= GH_DM_SEND_FLAG_SELF_COPY_FAILED;
    else if (copy->accepted < copy->relays->len)
      status->flags |= GH_DM_SEND_FLAG_SELF_COPY_PARTIAL;
  }
  guint reached = recipients_reached(status);
  if (reached == status->recipients->len)
    finish(self, GH_DM_SEND_RESULT_SENT, GH_DM_SEND_FAILURE_NONE, NULL, TRUE);
  else if (reached > 0)
    finish(self, GH_DM_SEND_RESULT_PARTIALLY_SENT, GH_DM_SEND_FAILURE_NONE,
           "Some recipients' inbox relays did not accept the message", TRUE);
  else
    finish(self, GH_DM_SEND_RESULT_FAILED, GH_DM_SEND_FAILURE_RELAYS,
           "No recipient inbox relay accepted the message", TRUE);
}

static gboolean
all_legs_complete(GhDmSend *self)
{
  for (guint i = 0; i < self->status.recipients->len; i++)
    if (!((GhDmSendLeg *)g_ptr_array_index(self->status.recipients, i))->complete)
      return FALSE;
  return !self->status.self_copy || self->status.self_copy->complete;
}

static void
check_published(GhDmSend *self)
{
  if (!is_done(self) && all_legs_complete(self))
    finalize_result(self);
}

static void
on_publish_update(GhRelayPublish *publish, const GhRelayPublishResult *result,
                  gpointer data)
{
  GhDmSend *self = data;
  if (is_done(self))
    return;
  if (!is_current(self)) {
    finish_cancelled(self); /* a late OK from a revoked generation */
    return;
  }
  GhDmSendLeg *leg = g_hash_table_lookup(self->publishes, publish);
  GhDmRelayOutcome *outcome = leg ? leg_outcome(leg, result->url) : NULL;
  if (!outcome || outcome->outcome != GH_RELAY_PUBLISH_PENDING)
    return;
  outcome->outcome = result->outcome;
  outcome->prefix = result->prefix;
  g_free(outcome->message);
  outcome->message = g_strdup(result->message);
  if (result->outcome == GH_RELAY_PUBLISH_ACCEPTED)
    leg->accepted++;
  if (result->outcome == GH_RELAY_PUBLISH_AUTH_REQUIRED)
    self->status.flags |= GH_DM_SEND_FLAG_AUTH_REQUIRED;
  emit_changed(self);
}

static void
on_publish_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary,
                gpointer data)
{
  GhDmSend *self = data;
  (void)summary;
  if (is_done(self))
    return;
  if (!is_current(self)) {
    finish_cancelled(self);
    return;
  }
  GhDmSendLeg *leg = g_hash_table_lookup(self->publishes, publish);
  if (!leg)
    return;
  leg->complete = TRUE;
  check_published(self);
}

/* One GhRelayPublish (own connections) per wrap, carrying the stored signed
 * JSON unchanged to exactly the leg's targets that are still pending. The
 * NIP-42 identity of each URL is GhAuthPolicy's: a recipient's inbox relay
 * is RECIPIENT_WRAP (ephemeral, never the account), the self-copy and a note
 * to self go to the own inbox as SELF_WRAP (the account, on challenge). */
static void
start_leg(GhDmSend *self, GhDmSendLeg *leg)
{
  guint pending = 0;
  for (guint i = 0; i < leg->relays->len; i++)
    pending += ((GhDmRelayOutcome *)g_ptr_array_index(leg->relays, i))->outcome ==
               GH_RELAY_PUBLISH_PENDING;
  if (pending == 0) {
    leg->complete = TRUE;
    return;
  }
  GhDmSender *sender = self->sender;
  g_autoptr(GError) error = NULL;
  GhRelayPublish *publish = sender->custom_transport
    ? gh_relay_publish_new_with_transport(self->status.account_generation, leg->wrap_json,
                                          &sender->transport, sender->transport_data,
                                          on_publish_update, on_publish_done, self, &error)
    : gh_relay_publish_new(self->status.account_generation, leg->wrap_json,
                           on_publish_update, on_publish_done, self, &error);
  if (publish) {
    g_hash_table_insert(self->publishes, publish, leg);
    if (sender->publish_deadline)
      gh_relay_publish_set_deadline(publish, sender->publish_deadline);
    for (guint i = 0; i < leg->relays->len && !error; i++) {
      GhDmRelayOutcome *target = g_ptr_array_index(leg->relays, i);
      if (target->outcome != GH_RELAY_PUBLISH_PENDING ||
          !gh_relay_publish_add_url(publish, target->url, &error))
        continue;
      g_autoptr(GError) auth_error = NULL;
      GhAuthPurpose purpose = leg == self->status.self_copy || self->self_dm
        ? GH_AUTH_PURPOSE_SELF_WRAP : GH_AUTH_PURPOSE_RECIPIENT_WRAP;
      if (sender->accounts &&
          !gh_auth_policy_apply_publish(gh_auth_policy_get_for_accounts(sender->accounts),
                                        publish, purpose, target->url, &auth_error))
        g_debug("Groundhog will not sign in to a message relay: %s", auth_error->message);
    }
    if (!error && gh_relay_publish_start(publish, &error))
      return; /* outcomes arrive through the callbacks */
    if (g_hash_table_contains(self->publishes, publish)) {
      gh_relay_publish_cancel(publish);
      g_hash_table_remove(self->publishes, publish);
    }
  }
  leg_mark_pending(leg, GH_RELAY_PUBLISH_CONNECTION_FAILED,
                   error ? error->message : "publish could not start");
  leg->complete = TRUE;
}

static void
begin_publish(GhDmSend *self)
{
  GhDmSendStatus *status = &self->status;
  status->phase = GH_DM_SEND_PHASE_PUBLISHING;
  g_object_ref(self); /* publish callbacks may finish (and release) the operation */
  for (guint i = 0; i < status->recipients->len && !is_done(self); i++)
    start_leg(self, g_ptr_array_index(status->recipients, i));
  if (status->self_copy && !is_done(self))
    start_leg(self, status->self_copy);
  if (!is_done(self)) {
    emit_changed(self);
    check_published(self);
  }
  g_object_unref(self);
}

/* ---- sealing ------------------------------------------------------------- */

/* Canonical NIP-01 id of a rumor or wrap, recomputed rather than trusted. */
static gchar *
event_id(const gchar *json)
{
  NostrEvent *event = json ? nostr_event_new() : NULL;
  gchar id[65] = { 0 };
  gboolean ok = event && nostr_event_deserialize_compact(event, json, NULL) == 1 &&
                nostr_event_compute_id(event, id) == NOSTR_EVENT_VALIDATION_OK;
  if (event)
    nostr_event_free(event);
  return ok ? g_strdup(id) : NULL;
}

static void
envelope_done(GObject *source, GAsyncResult *result, gpointer data)
{
  GhDmSend *self = data;
  (void)source;
  g_autoptr(GError) error = NULL;
  GhNip17Envelope *envelope = gh_nip17_envelope_build_finish(result, &error);
  if (is_done(self)) {
    gh_nip17_envelope_free(envelope);
    g_object_unref(self);
    return;
  }
  if (!envelope || !is_current(self)) {
    if (!is_current(self) || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
        g_error_matches(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED))
      finish_cancelled(self);
    else if (error && error->domain == GH_SIGNER_ERROR)
      finish(self, GH_DM_SEND_RESULT_FAILED, GH_DM_SEND_FAILURE_SIGNER, error->message, TRUE);
    else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT))
      finish(self, GH_DM_SEND_RESULT_FAILED, GH_DM_SEND_FAILURE_INVALID, error->message, TRUE);
    else
      finish(self, GH_DM_SEND_RESULT_FAILED, GH_DM_SEND_FAILURE_ENVELOPE,
             error ? error->message : "Could not build the NIP-17 envelope", TRUE);
    gh_nip17_envelope_free(envelope);
    g_object_unref(self);
    return;
  }
  GhDmSendStatus *status = &self->status;
  GhDmSendLeg *recipient = first_recipient(self);
  status->rumor_json = g_strdup(envelope->rumor_json);
  status->rumor_id = event_id(envelope->rumor_json);
  if (self->self_dm) {
    recipient->wrap_json = g_strdup(envelope->sender_wrap_json);
  } else {
    recipient->wrap_json = g_strdup(envelope->recipient_wrap_json);
    /* Kept even without an own inbox, so an outbox can publish it later. */
    status->self_copy->wrap_json = g_strdup(envelope->sender_wrap_json);
    status->self_copy->wrap_id = event_id(status->self_copy->wrap_json);
  }
  recipient->wrap_id = event_id(recipient->wrap_json);
  gh_nip17_envelope_free(envelope);
  wipe_content(self);
  if (self->mode == MODE_SEAL)
    finish(self, GH_DM_SEND_RESULT_SEALED, GH_DM_SEND_FAILURE_NONE, NULL, TRUE);
  else
    begin_publish(self);
  g_object_unref(self); /* the envelope callback's reference */
}

static void
begin_seal(GhDmSend *self)
{
  if (!is_current(self)) {
    finish_cancelled(self);
    return;
  }
  GhDmSendStatus *status = &self->status;
  if (status->self_copy) {
    status->self_copy->inbox = own_inbox_status(self);
    if (!leg_set_targets(status->self_copy,
                         gh_account_relays_get_inbox_relays(self->sender->account_relays)))
      status->flags |= GH_DM_SEND_FLAG_NO_OWN_INBOX;
    else
      status->self_copy->inbox = GH_INBOX_FOUND;
  }
  status->phase = GH_DM_SEND_PHASE_SEALING;
  emit_changed(self);
  if (is_done(self))
    return;
  if (self->rumor)
    gh_nip17_envelope_seal_expiring_async(self->sender->accounts, self->rumor,
                                          self->has_outer ? &self->outer : NULL, self->cancel,
                                          envelope_done, g_object_ref(self));
  else if (self->self_dm)
    gh_nip17_envelope_build_self_async(self->sender->accounts, self->content, self->cancel,
                                       envelope_done, g_object_ref(self));
  else
    gh_nip17_envelope_build_async(self->sender->accounts, first_recipient(self)->pubkey,
                                  self->content, self->cancel, envelope_done,
                                  g_object_ref(self));
}

/* ---- recipient inbox ----------------------------------------------------- */

static void
finish_without_inbox(GhDmSend *self, GhInboxStatus inbox)
{
  if (inbox == GH_INBOX_EMPTY || inbox == GH_INBOX_NOT_FOUND)
    finish(self, GH_DM_SEND_RESULT_NO_RECIPIENT_INBOX, GH_DM_SEND_FAILURE_NONE,
           "The recipient has not set up private messaging (no kind-10050 inbox relays)",
           TRUE);
  else
    finish(self, GH_DM_SEND_RESULT_INBOX_UNKNOWN, GH_DM_SEND_FAILURE_NONE,
           "No relay could be asked for the recipient's inbox relays", TRUE);
}

static void
inbox_resolved(GObject *source, GAsyncResult *result, gpointer data)
{
  GhDmSend *self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GhInboxResult) inbox =
    gh_inbox_resolver_resolve_finish(GH_INBOX_RESOLVER(source), result, &error);
  if (is_done(self)) {
    g_object_unref(self);
    return;
  }
  if (!inbox || !is_current(self)) {
    if (!is_current(self) || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      finish_cancelled(self);
    else
      finish(self, GH_DM_SEND_RESULT_FAILED, GH_DM_SEND_FAILURE_INVALID,
             error ? error->message : "Inbox lookup failed", TRUE);
    g_object_unref(self);
    return;
  }
  GhDmSendLeg *leg = first_recipient(self);
  leg->inbox = inbox->status;
  leg->inbox_event_id = g_strdup(inbox->event_id);
  if (inbox->status != GH_INBOX_FOUND ||
      !leg_set_targets(leg, (const gchar *const *)inbox->relays)) {
    if (inbox->status == GH_INBOX_FOUND)
      leg->inbox = GH_INBOX_EMPTY;
    finish_without_inbox(self, leg->inbox);
    g_object_unref(self);
    return;
  }
  emit_changed(self);
  if (!is_done(self))
    await_own_relays(self, begin_seal); /* they may be re-discovering */
  g_object_unref(self);
}

static void
resolve(GhDmSend *self)
{
  GhDmSendLeg *leg = first_recipient(self);
  if (!self->self_dm) {
    gh_inbox_resolver_resolve_async(self->sender->inboxes, leg->pubkey, self->cancel,
                                    inbox_resolved, g_object_ref(self));
    return;
  }
  /* Note to self: the recipient's inbox is the account's own 10050 list. */
  leg->inbox = own_inbox_status(self);
  if (!leg_set_targets(leg, gh_account_relays_get_inbox_relays(self->sender->account_relays))) {
    if (leg->inbox == GH_INBOX_FOUND)
      leg->inbox = GH_INBOX_EMPTY;
    finish_without_inbox(self, leg->inbox);
    return;
  }
  begin_seal(self);
}

/* ---- entry points -------------------------------------------------------- */

static gboolean
hex64(const gchar *value)
{
  if (!value || strlen(value) != 64)
    return FALSE;
  for (const gchar *p = value; *p; p++)
    if (!g_ascii_isxdigit(*p))
      return FALSE;
  return TRUE;
}

static gchar *
active_sender(GhDmSender *sender)
{
  const gchar *npub = sender->accounts &&
                      gh_account_controller_get_state(sender->accounts) == GH_ACCOUNT_STATE_ACTIVE
                        ? gh_account_controller_get_active_npub(sender->accounts) : NULL;
  return npub ? gh_identity_pubkey_hex(npub) : NULL;
}

/* One of content or rumor (seal_rumor) is set; outer only with a rumor. */
static GhDmSend *
compose(GhDmSender *sender, const gchar *recipient_pubkey_hex, const gchar *content,
        const gchar *rumor, const GhNip17OuterExpiration *outer, GCancellable *cancellable,
        Mode mode)
{
  GhDmSend *self = g_object_new(GH_TYPE_DM_SEND, NULL);
  GhDmSendStatus *status = &self->status;
  self->mode = mode;
  self->has_outer = outer != NULL;
  if (outer)
    self->outer = *outer;
  status->sender = active_sender(sender);
  gboolean text_ok = rumor || (content && *content && g_utf8_validate(content, -1, NULL));
  if (!status->sender || !hex64(recipient_pubkey_hex) || !text_ok) {
    finish_invalid(self, !status->sender ? "No active Groundhog account"
                         : rumor ? "Not a canonical NIP-17 rumor of the active account"
                                 : "A hex recipient pubkey and UTF-8 text are required");
    return self;
  }
  g_autofree gchar *recipient = g_ascii_strdown(recipient_pubkey_hex, -1);
  self->self_dm = g_strcmp0(status->sender, recipient) == 0;
  g_ptr_array_add(status->recipients, leg_new(recipient));
  if (self->self_dm)
    status->flags |= GH_DM_SEND_FLAG_SELF_DM;
  else
    status->self_copy = leg_new(status->sender);
  self->content = g_strdup(content);
  self->rumor = g_strdup(rumor);
  if (!track(self, sender, cancellable))
    return self;
  await_own_relays(self, resolve);
  return self;
}

GhDmSend *
gh_dm_sender_send(GhDmSender *self, const gchar *recipient_pubkey_hex,
                  const gchar *content, GCancellable *cancellable)
{
  g_return_val_if_fail(GH_IS_DM_SENDER(self), NULL);
  return compose(self, recipient_pubkey_hex, content, NULL, NULL, cancellable, MODE_SEND);
}

GhDmSend *
gh_dm_sender_seal(GhDmSender *self, const gchar *recipient_pubkey_hex,
                  const gchar *content, GCancellable *cancellable)
{
  g_return_val_if_fail(GH_IS_DM_SENDER(self), NULL);
  return compose(self, recipient_pubkey_hex, content, NULL, NULL, cancellable, MODE_SEAL);
}

GhDmSend *
gh_dm_sender_seal_rumor(GhDmSender *self, const gchar *rumor_json, GCancellable *cancellable)
{
  return gh_dm_sender_seal_rumor_expiring(self, rumor_json, NULL, cancellable);
}

GhDmSend *
gh_dm_sender_seal_rumor_expiring(GhDmSender *self, const gchar *rumor_json,
                                 const GhNip17OuterExpiration *outer,
                                 GCancellable *cancellable)
{
  g_return_val_if_fail(GH_IS_DM_SENDER(self), NULL);
  g_autofree gchar *sender = active_sender(self);
  g_autofree gchar *recipient = sender ? gh_nip17_rumor_get_recipient(rumor_json, sender) : NULL;
  /* An unusable rumor fails like an invalid recipient, before any lookup. */
  return compose(self, recipient, NULL, rumor_json, outer, cancellable, MODE_SEAL);
}

/* A stored wrap must be a signed gift wrap to exactly this leg's receiver,
 * with the id the status recorded. */
static gboolean
valid_wrap(const GhDmSendLeg *leg)
{
  if (!leg->wrap_json || !leg->wrap_id || !hex64(leg->pubkey))
    return FALSE;
  NostrEvent *wrap = nostr_event_new();
  gchar id[65] = { 0 };
  gboolean ok = wrap &&
    nostr_event_deserialize_signed(wrap, leg->wrap_json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_validate(wrap, id) == NOSTR_EVENT_VALIDATION_OK &&
    g_strcmp0(id, leg->wrap_id) == 0 && nostr_nip59_validate_gift_wrap(wrap);
  if (ok) {
    char *p = nostr_nip59_get_recipient(wrap);
    ok = g_strcmp0(p, leg->pubkey) == 0;
    free(p);
  }
  if (wrap)
    nostr_event_free(wrap);
  return ok;
}

/* Everything but ACCEPTED is pending again; ACCEPTED is never republished. */
static void
leg_reset_for_publish(GhDmSendLeg *leg)
{
  leg->accepted = 0;
  leg->complete = FALSE;
  for (guint i = 0; i < leg->relays->len; i++) {
    GhDmRelayOutcome *target = g_ptr_array_index(leg->relays, i);
    if (target->outcome == GH_RELAY_PUBLISH_ACCEPTED) {
      leg->accepted++;
      continue;
    }
    target->outcome = GH_RELAY_PUBLISH_PENDING;
    target->prefix = GH_RELAY_OK_PREFIX_NONE;
    g_clear_pointer(&target->message, g_free);
  }
}

static gboolean
publish_now(gpointer data)
{
  GhDmSend *self = data;
  g_clear_pointer(&self->start, g_source_unref);
  if (is_done(self))
    return G_SOURCE_REMOVE;
  if (!is_current(self))
    finish_cancelled(self);
  else
    begin_publish(self);
  return G_SOURCE_REMOVE;
}

GhDmSend *
gh_dm_sender_publish(GhDmSender *sender, const GhDmSendStatus *sealed,
                     GCancellable *cancellable)
{
  g_return_val_if_fail(GH_IS_DM_SENDER(sender), NULL);
  g_return_val_if_fail(sealed != NULL, NULL);
  GhDmSend *self = g_object_new(GH_TYPE_DM_SEND, NULL);
  self->mode = MODE_PUBLISH;
  GhDmSendStatus *status = &self->status;
  status_clear(status);
  status_copy_into(status, sealed);
  status->phase = GH_DM_SEND_PHASE_PUBLISHING;
  status->result = GH_DM_SEND_RESULT_PENDING;
  status->failure = GH_DM_SEND_FAILURE_NONE;
  g_clear_pointer(&status->error_message, g_free);
  status->flags &= GH_DM_SEND_FLAG_SELF_DM;
  g_autofree gchar *active = active_sender(sender);
  gboolean valid = active && g_strcmp0(active, status->sender) == 0 &&
                   status->recipients->len > 0;
  for (guint i = 0; valid && i < status->recipients->len; i++) {
    GhDmSendLeg *leg = g_ptr_array_index(status->recipients, i);
    valid = valid_wrap(leg) && leg->relays->len > 0;
    leg_reset_for_publish(leg);
  }
  if (valid && status->self_copy) {
    valid = valid_wrap(status->self_copy) &&
            g_strcmp0(status->self_copy->pubkey, status->sender) == 0;
    leg_reset_for_publish(status->self_copy);
    if (status->self_copy->relays->len == 0)
      status->flags |= GH_DM_SEND_FLAG_NO_OWN_INBOX;
  }
  if (!valid) {
    finish_invalid(self, active ? "Not a sealed message of the active account"
                                : "No active Groundhog account");
    return self;
  }
  if (!track(self, sender, cancellable))
    return self;
  schedule(self, G_PRIORITY_DEFAULT, publish_now);
  return self;
}

const GhDmSendStatus *
gh_dm_send_get_status(GhDmSend *self)
{
  g_return_val_if_fail(GH_IS_DM_SEND(self), NULL);
  return &self->status;
}

gboolean
gh_dm_send_is_done(GhDmSend *self)
{
  g_return_val_if_fail(GH_IS_DM_SEND(self), TRUE);
  return is_done(self);
}

void
gh_dm_send_cancel(GhDmSend *self)
{
  g_return_if_fail(GH_IS_DM_SEND(self));
  finish_cancelled(self);
}

static void
gh_dm_send_dispose(GObject *object)
{
  GhDmSend *self = GH_DM_SEND(object);
  if (!is_done(self))
    finish(self, GH_DM_SEND_RESULT_CANCELLED, GH_DM_SEND_FAILURE_NONE,
           "Send disposed", FALSE);
  G_OBJECT_CLASS(gh_dm_send_parent_class)->dispose(object);
}

static void
gh_dm_send_finalize(GObject *object)
{
  GhDmSend *self = GH_DM_SEND(object);
  status_clear(&self->status);
  g_hash_table_unref(self->publishes);
  g_clear_object(&self->cancel);
  g_main_context_unref(self->context);
  G_OBJECT_CLASS(gh_dm_send_parent_class)->finalize(object);
}

static void
gh_dm_send_class_init(GhDmSendClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_dm_send_dispose;
  object_class->finalize = gh_dm_send_finalize;
  signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
                                         G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);
}

static void
gh_dm_send_init(GhDmSend *self)
{
  self->context = g_main_context_ref_thread_default();
  self->cancel = g_cancellable_new();
  self->publishes = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                          (GDestroyNotify)gh_relay_publish_unref, NULL);
  self->status.phase = GH_DM_SEND_PHASE_RESOLVING;
  self->status.recipients = g_ptr_array_new_with_free_func(leg_free);
}

/* ---- sender -------------------------------------------------------------- */

GhDmSender *
gh_dm_sender_new(GhAccountController *accounts, GhAccountRelays *account_relays,
                 GhInboxResolver *inboxes, const GhRelayPublishTransport *transport,
                 gpointer transport_data)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts), NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_RELAYS(account_relays), NULL);
  g_return_val_if_fail(GH_IS_INBOX_RESOLVER(inboxes), NULL);
  g_return_val_if_fail(!transport || (transport->open && transport->close), NULL);
  GhDmSender *self = g_object_new(GH_TYPE_DM_SENDER, NULL);
  self->accounts = g_object_ref(accounts);
  self->account_relays = g_object_ref(account_relays);
  self->inboxes = g_object_ref(inboxes);
  if (transport) {
    self->transport = *transport;
    self->transport_data = transport_data;
    self->custom_transport = TRUE;
  }
  return self;
}

void
gh_dm_sender_set_publish_deadline(GhDmSender *self, guint seconds)
{
  g_return_if_fail(GH_IS_DM_SENDER(self));
  self->publish_deadline = seconds;
}

static void
gh_dm_sender_dispose(GObject *object)
{
  GhDmSender *self = GH_DM_SENDER(object);
  /* Each in-flight operation pins the sender, so this only runs via
   * run_dispose (application shutdown). */
  GList *sends = g_hash_table_get_keys(self->inflight);
  for (GList *item = sends; item; item = item->next)
    g_object_ref(item->data);
  for (GList *item = sends; item; item = item->next)
    finish_cancelled(item->data);
  g_list_free_full(sends, g_object_unref);
  g_clear_object(&self->inboxes);
  g_clear_object(&self->account_relays);
  g_clear_object(&self->accounts);
  G_OBJECT_CLASS(gh_dm_sender_parent_class)->dispose(object);
}

static void
gh_dm_sender_finalize(GObject *object)
{
  GhDmSender *self = GH_DM_SENDER(object);
  g_hash_table_unref(self->inflight);
  G_OBJECT_CLASS(gh_dm_sender_parent_class)->finalize(object);
}

static void
gh_dm_sender_class_init(GhDmSenderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_dm_sender_dispose;
  object_class->finalize = gh_dm_sender_finalize;
}

static void
gh_dm_sender_init(GhDmSender *self)
{
  self->inflight = g_hash_table_new_full(g_direct_hash, g_direct_equal, g_object_unref, NULL);
}
