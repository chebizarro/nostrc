#include "gh-dm-send.h"
#include "gh-identity.h"
#include "gh-nip17-envelope.h"

#include <nostr-event.h>
#include <string.h>

#define MAX_TARGETS 16 /* the relay publish's own bound */
struct _GhDmSender {
  GObject parent_instance;
  GhAccountController *accounts; /* NULL once disposed */
  GhAccountRelays *account_relays;
  GhInboxResolver *inboxes;
  GHashTable *inflight;   /* GhDmSend (owned) until done */
};

struct _GhDmSend {
  GObject parent_instance;
  GhDmSender *sender; /* owned while in flight, NULL once done */
  GhDmSendStatus status;
  gchar *rumor;       /* seal_rumor: the caller's stored rumor; wiped once sealed */
  gboolean has_outer; /* the rumor expires: outer holds its layers' expirations */
  GhNip17RoomExpiration outer;
  gboolean self_dm;
  guint resolving;    /* recipient inbox lookups still out */
  GMainContext *context;
  GCancellable *cancel; /* revokes the inbox lookup and signer approvals */
  GSource *start;       /* pending low-priority continuation */
  GSource *caller_cancel;
  GSource *generation_cancel;
  gulong relays_handler;
  void (*after_relays)(GhDmSend *self);
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
    g_ptr_array_add(leg->relays, outcome);
  }
  return leg->relays->len > 0;
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
finish(GhDmSend *self, GhDmSendResult result, GhDmSendFailure failure,
       const gchar *message, gboolean emit)
{
  if (is_done(self))
    return;
  g_object_ref(self);
  destroy_source(&self->start);
  destroy_source(&self->caller_cancel);
  destroy_source(&self->generation_cancel);
  if (self->relays_handler) {
    g_signal_handler_disconnect(self->sender->account_relays, self->relays_handler);
    self->relays_handler = 0;
  }
  g_cancellable_cancel(self->cancel);
  wipe_string(&self->rumor);
  GhDmSendStatus *status = &self->status;
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
  status->rumor_json = g_strdup(envelope->rumor_json);
  status->rumor_id = event_id(envelope->rumor_json);
  if (self->self_dm) {
    first_recipient(self)->wrap_json = g_strdup(envelope->sender_wrap_json);
  } else {
    /* Each recipient's own wrap, matched by receiver. */
    for (guint i = 0; i < status->recipients->len; i++) {
      GhDmSendLeg *leg = g_ptr_array_index(status->recipients, i);
      for (guint j = 0; envelope->recipients && envelope->recipients[j] && !leg->wrap_json; j++)
        if (g_strcmp0(envelope->recipients[j], leg->pubkey) == 0)
          leg->wrap_json = g_strdup(envelope->recipient_wraps[j]);
    }
    /* Kept even without an own inbox, so an outbox can publish it later. */
    status->self_copy->wrap_json = g_strdup(envelope->sender_wrap_json);
    status->self_copy->wrap_id = event_id(status->self_copy->wrap_json);
  }
  gboolean complete = TRUE;
  for (guint i = 0; i < status->recipients->len; i++) {
    GhDmSendLeg *leg = g_ptr_array_index(status->recipients, i);
    leg->wrap_id = event_id(leg->wrap_json);
    complete &= leg->wrap_id != NULL;
  }
  gh_nip17_envelope_free(envelope);
  if (!complete) {
    finish(self, GH_DM_SEND_RESULT_FAILED, GH_DM_SEND_FAILURE_ENVELOPE,
           "The envelope has no gift wrap for a recipient", TRUE);
    g_object_unref(self);
    return;
  }
  finish(self, GH_DM_SEND_RESULT_SEALED, GH_DM_SEND_FAILURE_NONE, NULL, TRUE);
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
  gh_nip17_envelope_seal_room_async(self->sender->accounts, self->rumor,
                                    self->has_outer ? &self->outer : NULL, self->cancel,
                                    envelope_done, g_object_ref(self));
}

/* ---- recipient inbox ----------------------------------------------------- */

static gboolean
inbox_absent(GhInboxStatus inbox)
{
  return inbox == GH_INBOX_EMPTY || inbox == GH_INBOX_NOT_FOUND;
}

static void
finish_without_inbox(GhDmSend *self, GhInboxStatus inbox)
{
  gboolean room = self->status.recipients->len > 1;
  if (inbox_absent(inbox))
    finish(self, GH_DM_SEND_RESULT_NO_RECIPIENT_INBOX, GH_DM_SEND_FAILURE_NONE,
           room ? "No recipient has set up private messaging (no kind-10050 inbox relays)"
                : "The recipient has not set up private messaging (no kind-10050 inbox relays)",
           TRUE);
  else
    finish(self, GH_DM_SEND_RESULT_INBOX_UNKNOWN, GH_DM_SEND_FAILURE_NONE,
           room ? "No relay could be asked for the recipients' inbox relays"
                : "No relay could be asked for the recipient's inbox relays", TRUE);
}

/* Every recipient's lookup answered: seal if anyone can be reached. A
 * recipient without targets keeps its (absent or unknown) inbox status and
 * gets a wrap that is never published. */
static void
inboxes_resolved(GhDmSend *self)
{
  GhDmSendStatus *status = &self->status;
  guint reachable = 0;
  gboolean unknown = FALSE;
  for (guint i = 0; i < status->recipients->len; i++) {
    GhDmSendLeg *leg = g_ptr_array_index(status->recipients, i);
    if (leg->relays->len > 0)
      reachable++;
    else if (!inbox_absent(leg->inbox))
      unknown = TRUE;
  }
  if (reachable == 0) {
    finish_without_inbox(self, unknown ? GH_INBOX_UNREACHABLE : GH_INBOX_NOT_FOUND);
    return;
  }
  if (reachable < status->recipients->len)
    status->flags |= GH_DM_SEND_FLAG_RECIPIENT_NO_INBOX;
  emit_changed(self);
  if (!is_done(self))
    await_own_relays(self, begin_seal); /* they may be re-discovering */
}

typedef struct {
  GhDmSend *self; /* a reference */
  guint index;    /* its recipient leg */
} Lookup;

static void
inbox_resolved(GObject *source, GAsyncResult *result, gpointer data)
{
  Lookup *lookup = data;
  GhDmSend *self = lookup->self;
  guint index = lookup->index;
  g_free(lookup);
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
  GhDmSendLeg *leg = g_ptr_array_index(self->status.recipients, index);
  leg->inbox = inbox->status;
  leg->inbox_event_id = g_strdup(inbox->event_id);
  if (inbox->status == GH_INBOX_FOUND &&
      !leg_set_targets(leg, (const gchar *const *)inbox->relays))
    leg->inbox = GH_INBOX_EMPTY;
  if (--self->resolving == 0)
    inboxes_resolved(self);
  g_object_unref(self);
}

static void
resolve(GhDmSend *self)
{
  GhDmSendLeg *leg = first_recipient(self);
  if (!self->self_dm) {
    GPtrArray *recipients = self->status.recipients;
    self->resolving = recipients->len;
    for (guint i = 0; i < recipients->len && !is_done(self); i++) {
      Lookup *lookup = g_new0(Lookup, 1);
      lookup->self = g_object_ref(self);
      lookup->index = i;
      gh_inbox_resolver_resolve_async(self->sender->inboxes,
                                      ((GhDmSendLeg *)g_ptr_array_index(recipients, i))->pubkey,
                                      self->cancel, inbox_resolved, lookup);
    }
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

/* The recipients as legs (lowercase, distinct), or a reason they are not
 * usable: 1 to GH_NIP17_MAX_SEND_RECIPIENTS hex keys, the account only
 * alone (a note to self). */
static const gchar *
add_recipients(GhDmSend *self, const gchar *const *recipients)
{
  GhDmSendStatus *status = &self->status;
  guint n = recipients ? g_strv_length((gchar **)recipients) : 0;
  if (n == 0 || n > GH_NIP17_MAX_SEND_RECIPIENTS)
    return n ? "A NIP-17 message goes to at most 10 other people"
             : "A hex recipient pubkey and UTF-8 text are required";
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gboolean has_self = FALSE;
  for (guint i = 0; i < n; i++) {
    if (!hex64(recipients[i]))
      return "A hex recipient pubkey and UTF-8 text are required";
    gchar *recipient = g_ascii_strdown(recipients[i], -1);
    has_self |= g_strcmp0(status->sender, recipient) == 0;
    g_ptr_array_add(status->recipients, leg_new(recipient));
    if (!g_hash_table_add(seen, recipient))
      return "A NIP-17 message names each recipient once";
  }
  if (has_self && n > 1)
    return "A message to others cannot also be a note to self";
  self->self_dm = has_self;
  return NULL;
}

GhDmSend *
gh_dm_sender_seal_room_rumor(GhDmSender *self, const gchar *rumor_json,
                             const GhNip17RoomExpiration *outer, GCancellable *cancellable)
{
  g_return_val_if_fail(GH_IS_DM_SENDER(self), NULL);
  g_autofree gchar *sender = active_sender(self);
  g_auto(GStrv) recipients = sender ? gh_nip17_rumor_dup_recipients(rumor_json, sender) : NULL;
  GhDmSend *send = g_object_new(GH_TYPE_DM_SEND, NULL);
  send->status.sender = g_steal_pointer(&sender);
  const gchar *invalid = !send->status.sender ? "No active Groundhog account"
                       : !recipients ? "Not a canonical NIP-17 rumor of the active account"
                                     : add_recipients(send, (const gchar *const *)recipients);
  if (invalid) {
    finish_invalid(send, invalid);
    return send;
  }
  if (send->self_dm)
    send->status.flags |= GH_DM_SEND_FLAG_SELF_DM;
  else
    send->status.self_copy = leg_new(send->status.sender);
  send->rumor = g_strdup(rumor_json);
  send->has_outer = outer != NULL;
  if (outer)
    send->outer = *outer;
  if (track(send, self, cancellable))
    await_own_relays(send, resolve);
  return send;
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
  self->status.phase = GH_DM_SEND_PHASE_RESOLVING;
  self->status.recipients = g_ptr_array_new_with_free_func(leg_free);
}

/* ---- sender -------------------------------------------------------------- */

GhDmSender *
gh_dm_sender_new(GhAccountController *accounts, GhAccountRelays *account_relays,
                 GhInboxResolver *inboxes)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts), NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_RELAYS(account_relays), NULL);
  g_return_val_if_fail(GH_IS_INBOX_RESOLVER(inboxes), NULL);
  GhDmSender *self = g_object_new(GH_TYPE_DM_SENDER, NULL);
  self->accounts = g_object_ref(accounts);
  self->account_relays = g_object_ref(account_relays);
  self->inboxes = g_object_ref(inboxes);
  return self;
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
