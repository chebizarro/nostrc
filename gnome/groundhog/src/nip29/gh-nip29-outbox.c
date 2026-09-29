#include "gh-nip29-outbox.h"

#include "gh-auth-policy.h"
#include "gh-identity.h"
#include "gh-message.h"
#include "gh-store-nip29.h"

#include <nostr-event.h>
#include <nostr-tag.h>
#include <string.h>

#define RETRY_WINDOW_S  (72 * 3600)
#define STORE_RETRY_S   30    /* re-check after a failed store write */
#define MAX_RELAY_TEXT  1024  /* as the store keeps it */
#define KIND_JOIN       9021

/* Charter §3.6: the delay after the nth publish round; every 6 h after. */
static const guint backoff_seconds[] = { 15, 60, 5 * 60, 30 * 60, 2 * 3600, 6 * 3600 };

/* Reason codes kept in outbox.last_error. */
#define REASON_SIGNER    "signer"
#define REASON_INVALID   "invalid"
#define REASON_STORAGE   "storage"
#define REASON_TIMED_OUT "retry-window-over"
#define REASON_REFUSED   "relay-refused"
#define REASON_CANCELLED "cancelled"

struct _GhNip29Op {
  GObject parent_instance;
  GhNip29Outbox *outbox;      /* borrowed; the outbox stops every op first */
  GhStoreOutboxEntry *entry;  /* the persisted state, kept in step with the store */
  gint kind;
  gchar event_id[65];
  gchar *relay_url;
  gchar *group_id;
  /* Derived, notified. */
  GhNip29OpResult result;
  gchar *relay_message;
  gint64 next_attempt_at;
  gboolean can_retry;
  /* Engine. */
  GCancellable *signing;      /* a signer request in flight */
  gboolean signer_pending;
  GhRelayPublish *publish;    /* this round's, while in flight */
  guint timer;
  gint64 timer_at;
  GSource *idle;
  gboolean manual;            /* this round is the user's Retry */
  gboolean dropped;
};

struct _GhNip29Outbox {
  GObject parent_instance;
  GhStore *store;
  GhClock *clock;
  gchar *account;
  GhAccountController *accounts;
  GhAuthPolicy *policy;
  GNetworkMonitor *network;
  GhRelayPublishTransport transport;
  GhRelayPublishAuthTransport auth_transport;
  gboolean custom_transport;
  gboolean has_auth_transport;
  gpointer transport_data;
  guint publish_deadline;
  GMainContext *context;
  guint64 generation;         /* the account generation it runs in; 0: paused */
  gboolean online;
  gulong accounts_handler;
  gulong network_handler;
  GHashTable *ops;            /* gint64 outbox id -> GhNip29Op (owned ref) */
};

enum {
  OP_PROP_0,
  OP_PROP_OUTBOX_ID,
  OP_PROP_MESSAGE_ID,
  OP_PROP_CONVERSATION_ID,
  OP_PROP_KIND,
  OP_PROP_RESULT,
  OP_PROP_RELAY_MESSAGE,
  OP_PROP_NEXT_ATTEMPT_AT,
  OP_PROP_CAN_RETRY,
  OP_N_PROPS
};
static GParamSpec *op_props[OP_N_PROPS];

enum { SIGNAL_OP_ADDED, SIGNAL_OP_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhNip29Op, gh_nip29_op, G_TYPE_OBJECT)
G_DEFINE_FINAL_TYPE(GhNip29Outbox, gh_nip29_outbox, G_TYPE_OBJECT)

static void op_eval(GhNip29Op *op);

/* ---- Results ------------------------------------------------------------- */

GType
gh_nip29_op_result_get_type(void)
{
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GH_NIP29_OP_QUEUED, "GH_NIP29_OP_QUEUED", "queued" },
      { GH_NIP29_OP_WAITING_FOR_SIGNER, "GH_NIP29_OP_WAITING_FOR_SIGNER", "waiting-for-signer" },
      { GH_NIP29_OP_SENDING, "GH_NIP29_OP_SENDING", "sending" },
      { GH_NIP29_OP_RETRYING, "GH_NIP29_OP_RETRYING", "retrying" },
      { GH_NIP29_OP_ACCEPTED, "GH_NIP29_OP_ACCEPTED", "accepted" },
      { GH_NIP29_OP_DUPLICATE, "GH_NIP29_OP_DUPLICATE", "duplicate" },
      { GH_NIP29_OP_PENDING_APPROVAL, "GH_NIP29_OP_PENDING_APPROVAL", "pending-approval" },
      { GH_NIP29_OP_REJECTED, "GH_NIP29_OP_REJECTED", "rejected" },
      { GH_NIP29_OP_NOT_SENT, "GH_NIP29_OP_NOT_SENT", "not-sent" },
      { GH_NIP29_OP_CANCELLED, "GH_NIP29_OP_CANCELLED", "cancelled" },
      { 0, NULL, NULL }
    };
    g_once_init_leave(&type, g_enum_register_static(g_intern_static_string("GhNip29OpResult"),
                                                    values));
  }
  return type;
}

gboolean
gh_nip29_op_result_is_final(GhNip29OpResult result)
{
  switch (result) {
  case GH_NIP29_OP_ACCEPTED:
  case GH_NIP29_OP_DUPLICATE:
  case GH_NIP29_OP_PENDING_APPROVAL:
  case GH_NIP29_OP_REJECTED:
  case GH_NIP29_OP_NOT_SENT:
  case GH_NIP29_OP_CANCELLED:
    return TRUE;
  default:
    return FALSE;
  }
}

GhMessageStatus
gh_nip29_op_result_to_message_status(GhNip29OpResult result, gboolean online)
{
  switch (result) {
  case GH_NIP29_OP_QUEUED:
    return online ? GH_MESSAGE_STATUS_SENDING : GH_MESSAGE_STATUS_QUEUED_OFFLINE;
  case GH_NIP29_OP_WAITING_FOR_SIGNER:
    return GH_MESSAGE_STATUS_WAITING_FOR_SIGNER;
  case GH_NIP29_OP_SENDING:
    return GH_MESSAGE_STATUS_SENDING;
  case GH_NIP29_OP_RETRYING:
    return GH_MESSAGE_STATUS_RETRYING;
  case GH_NIP29_OP_ACCEPTED:
  case GH_NIP29_OP_DUPLICATE:
  case GH_NIP29_OP_PENDING_APPROVAL:
    return GH_MESSAGE_STATUS_SENT; /* the group relay accepted it; nothing more is known */
  case GH_NIP29_OP_CANCELLED:
    return GH_MESSAGE_STATUS_CANCELLED;
  case GH_NIP29_OP_REJECTED:
  case GH_NIP29_OP_NOT_SENT:
  default:
    return GH_MESSAGE_STATUS_NOT_SENT;
  }
}

/* NIP-29 lets a relay explain in plain words that a join request waits for
 * an admin; there is no machine-readable prefix for it. */
static gboolean
says_pending(const gchar *message)
{
  if (!message)
    return FALSE;
  g_autofree gchar *lower = g_utf8_strdown(message, -1);
  return strstr(lower, "pending") || strstr(lower, "approval") || strstr(lower, "review");
}

GhNip29OpResult
gh_nip29_classify_answer(gint kind, GhRelayPublishOutcome outcome, GhRelayOkPrefix prefix,
                         const gchar *message, guint attempts)
{
  switch (outcome) {
  case GH_RELAY_PUBLISH_PENDING:
    return GH_NIP29_OP_SENDING;
  case GH_RELAY_PUBLISH_CANCELLED:
    return GH_NIP29_OP_QUEUED; /* an account switch or quit: resumes */
  case GH_RELAY_PUBLISH_ACCEPTED:
    if (prefix == GH_RELAY_OK_PREFIX_DUPLICATE)
      return GH_NIP29_OP_DUPLICATE;
    /* A relay must refuse a join it did not grant; one that accepts it with
     * a "pending" note still only recorded the request. */
    return kind == KIND_JOIN && says_pending(message) ? GH_NIP29_OP_PENDING_APPROVAL
                                                      : GH_NIP29_OP_ACCEPTED;
  case GH_RELAY_PUBLISH_AUTH_REQUIRED:
    return GH_NIP29_OP_NOT_SENT; /* the relay wants a sign-in that did not happen */
  case GH_RELAY_PUBLISH_REJECTED:
    if (prefix == GH_RELAY_OK_PREFIX_DUPLICATE)
      return GH_NIP29_OP_DUPLICATE;
    if (kind == KIND_JOIN && says_pending(message))
      return GH_NIP29_OP_PENDING_APPROVAL;
    break;
  case GH_RELAY_PUBLISH_CONNECTION_FAILED:
  default:
    break;
  }
  switch (gh_target_classify(outcome, prefix, attempts)) {
  case GH_TARGET_CLASS_ACCEPTED:
    return GH_NIP29_OP_ACCEPTED;
  case GH_TARGET_CLASS_PENDING:
  case GH_TARGET_CLASS_RESUMABLE:
    return GH_NIP29_OP_QUEUED;
  case GH_TARGET_CLASS_TRANSIENT:
    return GH_NIP29_OP_RETRYING;
  case GH_TARGET_CLASS_TERMINAL:
  default:
    return outcome == GH_RELAY_PUBLISH_REJECTED ? GH_NIP29_OP_REJECTED : GH_NIP29_OP_NOT_SENT;
  }
}

/* ---- Small helpers --------------------------------------------------------- */

static gint64
now_unix(GhNip29Outbox *self)
{
  return gh_clock_get_unix(self->clock);
}

static gboolean
running(GhNip29Outbox *self)
{
  return self->generation != 0 && self->online;
}

static gint64
deadline(const GhStoreOutboxEntry *entry)
{
  return entry->created_at + RETRY_WINDOW_S;
}

static gint64
backoff_after(GhClock *clock, guint rounds)
{
  guint index = MIN(MAX(rounds, 1), G_N_ELEMENTS(backoff_seconds)) - 1;
  guint64 permille = 800 + gh_clock_random_uniform(clock, 401);
  return MAX((gint64)((backoff_seconds[index] * permille + 500) / 1000), 1);
}

static gboolean
op_sealed(const GhNip29Op *op)
{
  return op->entry->events->len > 0;
}

/* The one target (the group relay) of the sealed event. */
static GhStoreOutboxTarget *
op_target(GhNip29Op *op, GhStoreOutboxEvent **out_event)
{
  if (!op_sealed(op))
    return NULL;
  GhStoreOutboxEvent *event = g_ptr_array_index(op->entry->events, 0);
  if (out_event)
    *out_event = event;
  return event->targets->len ? g_ptr_array_index(event->targets, 0) : NULL;
}

static GhRelayOkPrefix
target_prefix(const GhStoreOutboxTarget *target)
{
  return target->ok_prefix < 0 ? GH_RELAY_OK_PREFIX_NONE : (GhRelayOkPrefix)target->ok_prefix;
}

static GhNip29OpResult
target_result(GhNip29Op *op, const GhStoreOutboxTarget *target)
{
  return gh_nip29_classify_answer(op->kind, (GhRelayPublishOutcome)target->outcome,
                                  target_prefix(target), target->ok_message, target->attempts);
}

/* Parses an unsigned NIP-29 event of account: kind, id and group id. */
static gboolean
parse_unsigned(const gchar *json, const gchar *account, gint *kind, gchar id[65],
               gchar **group_id)
{
  NostrEvent *event = nostr_event_new();
  gboolean ok = json && nostr_event_deserialize_unsigned(event, json, NULL) ==
                          NOSTR_EVENT_VALIDATION_OK &&
                !event->sig && g_strcmp0(nostr_event_get_pubkey(event), account) == 0 &&
                nostr_event_get_created_at(event) > 0 &&
                nostr_event_compute_id(event, id) == NOSTR_EVENT_VALIDATION_OK;
  const gchar *group = NULL;
  NostrTags *tags = ok ? nostr_event_get_tags(event) : NULL;
  for (size_t i = 0; tags && i < nostr_tags_size(tags) && !group; i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (nostr_tag_size(tag) >= 2 && g_strcmp0(nostr_tag_get(tag, 0), "h") == 0)
      group = nostr_tag_get(tag, 1);
  }
  ok = ok && group && *group;
  if (ok) {
    *kind = nostr_event_get_kind(event);
    *group_id = g_strdup(group);
  }
  nostr_event_free(event);
  return ok;
}

/* ---- The op's derived state -------------------------------------------------- */

static const gchar *
local_reason(const gchar *code)
{
  if (g_strcmp0(code, REASON_SIGNER) == 0)
    return "Nostr Signer didn't approve it, or couldn't be reached.";
  if (g_strcmp0(code, REASON_STORAGE) == 0)
    return "There wasn't enough storage space to prepare it.";
  if (g_strcmp0(code, REASON_INVALID) == 0)
    return "The signed event didn't match what was asked for.";
  if (g_strcmp0(code, REASON_TIMED_OUT) == 0)
    return "The group's relay couldn't be reached for 72 hours.";
  return NULL;
}

static GhNip29OpResult
op_compute(GhNip29Op *op, const gchar **message)
{
  GhStoreOutboxEntry *entry = op->entry;
  *message = NULL;
  if (entry->state == GH_STORE_OUTBOX_CANCELLED)
    return GH_NIP29_OP_CANCELLED;
  if (!op_sealed(op)) {
    *message = local_reason(entry->last_error);
    if (entry->state == GH_STORE_OUTBOX_NEEDS_ATTENTION)
      return GH_NIP29_OP_NOT_SENT;
    if (op->signer_pending)
      return GH_NIP29_OP_WAITING_FOR_SIGNER;
    return entry->next_attempt_at > 0 ? GH_NIP29_OP_RETRYING : GH_NIP29_OP_QUEUED;
  }
  GhStoreOutboxTarget *target = op_target(op, NULL);
  if (!target)
    return GH_NIP29_OP_NOT_SENT;
  *message = target->ok_message;
  if (op->publish)
    return GH_NIP29_OP_SENDING;
  GhNip29OpResult result = target_result(op, target);
  switch (result) {
  case GH_NIP29_OP_QUEUED:
  case GH_NIP29_OP_SENDING:
  case GH_NIP29_OP_RETRYING:
    if (entry->state == GH_STORE_OUTBOX_NEEDS_ATTENTION) {
      if (!*message)
        *message = local_reason(entry->last_error);
      return GH_NIP29_OP_NOT_SENT;
    }
    if ((entry->state == GH_STORE_OUTBOX_WAITING_RETRY && entry->next_attempt_at > 0) ||
        result == GH_NIP29_OP_RETRYING)
      return GH_NIP29_OP_RETRYING;
    return running(op->outbox) ? GH_NIP29_OP_SENDING : GH_NIP29_OP_QUEUED;
  default:
    return result;
  }
}

/* Recomputes the derived properties and announces a change. */
static void
op_refresh(GhNip29Op *op)
{
  if (!op->outbox)
    return;
  const gchar *message = NULL;
  GhNip29OpResult result = op_compute(op, &message);
  GhStoreOutboxEntry *entry = op->entry;
  gboolean waiting = entry->state == GH_STORE_OUTBOX_WAITING_RETRY ||
                     (!op_sealed(op) && entry->next_attempt_at > 0 &&
                      entry->state != GH_STORE_OUTBOX_NEEDS_ATTENTION);
  gint64 next = waiting ? entry->next_attempt_at : 0;
  gboolean can_retry = !op->signing && !op->publish &&
                       (entry->state == GH_STORE_OUTBOX_NEEDS_ATTENTION || next > 0);
  gboolean changed = FALSE;
  g_object_freeze_notify(G_OBJECT(op));
  if (op->result != result) {
    op->result = result;
    g_object_notify_by_pspec(G_OBJECT(op), op_props[OP_PROP_RESULT]);
    changed = TRUE;
  }
  if (g_strcmp0(op->relay_message, message) != 0) {
    g_free(op->relay_message);
    op->relay_message = g_strdup(message);
    g_object_notify_by_pspec(G_OBJECT(op), op_props[OP_PROP_RELAY_MESSAGE]);
    changed = TRUE;
  }
  if (op->next_attempt_at != next) {
    op->next_attempt_at = next;
    g_object_notify_by_pspec(G_OBJECT(op), op_props[OP_PROP_NEXT_ATTEMPT_AT]);
    changed = TRUE;
  }
  if (op->can_retry != can_retry) {
    op->can_retry = can_retry;
    g_object_notify_by_pspec(G_OBJECT(op), op_props[OP_PROP_CAN_RETRY]);
    changed = TRUE;
  }
  g_object_thaw_notify(G_OBJECT(op));
  if (changed && op->outbox && !op->dropped)
    g_signal_emit(op->outbox, signals[SIGNAL_OP_CHANGED], 0, op);
}

/* ---- Lifecycle ---------------------------------------------------------------- */

static void
op_cancel_timer(GhNip29Op *op)
{
  if (!op->timer)
    return;
  guint timer = op->timer;
  op->timer = 0;
  gh_clock_source_remove(op->outbox->clock, timer);
}

/* Stops everything in flight; the persisted state stays for resuming. */
static void
op_stop(GhNip29Op *op)
{
  if (op->signing) {
    g_cancellable_cancel(op->signing);
    g_clear_object(&op->signing);
  }
  op->signer_pending = FALSE;
  if (op->publish) {
    GhRelayPublish *publish = g_steal_pointer(&op->publish);
    gh_relay_publish_cancel(publish);
    gh_relay_publish_unref(publish);
  }
  op_cancel_timer(op);
  if (op->idle) {
    GSource *idle = g_steal_pointer(&op->idle);
    g_source_destroy(idle);
    g_source_unref(idle);
  }
}

/* The op's rows are gone from the store. */
static void
op_drop(GhNip29Op *op)
{
  if (op->dropped)
    return;
  GhNip29Outbox *self = op->outbox;
  g_object_ref(op);
  op_stop(op);
  op->entry->state = GH_STORE_OUTBOX_CANCELLED;
  op_refresh(op);
  op->dropped = TRUE;
  gint64 id = op->entry->id;
  g_hash_table_remove(self->ops, &id);
  op->outbox = NULL;
  g_object_unref(op);
}

static void op_schedule(GhNip29Op *op, gint64 at);

static void
op_store_failed(GhNip29Op *op, const GError *error)
{
  g_autoptr(GError) load_error = NULL;
  g_autoptr(GhStoreOutboxEntry) entry =
    gh_store_outbox_load(op->outbox->store, op->entry->id, &load_error);
  if (!entry && g_error_matches(load_error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND)) {
    op_drop(op);
    return;
  }
  g_warning("Group outbox entry %" G_GINT64_FORMAT ": %s", op->entry->id,
            error ? error->message : "store write failed");
  gint64 again = now_unix(op->outbox) + STORE_RETRY_S;
  if (!op->timer || op->timer_at > again)
    op_schedule(op, again);
}

static gboolean
op_persist(GhNip29Op *op, GhStoreOutboxState state, gint64 next, gboolean count,
           const gchar *reason)
{
  GhStoreOutboxUpdate update = { state, next, count, reason };
  g_autoptr(GError) error = NULL;
  if (!gh_store_outbox_update(op->outbox->store, op->entry->id, &update, &error)) {
    op_store_failed(op, error);
    return FALSE;
  }
  GhStoreOutboxEntry *entry = op->entry;
  entry->state = state;
  entry->next_attempt_at = next;
  entry->attempts += count ? 1 : 0;
  g_free(entry->last_error);
  entry->last_error = g_strdup(reason);
  return TRUE;
}

static gboolean
op_reload(GhNip29Op *op)
{
  g_autoptr(GError) error = NULL;
  GhStoreOutboxEntry *entry = gh_store_outbox_load(op->outbox->store, op->entry->id, &error);
  if (!entry) {
    op_store_failed(op, error);
    return FALSE;
  }
  gh_store_outbox_entry_free(op->entry);
  op->entry = entry;
  return TRUE;
}

static gboolean
op_timer_fired(gpointer data)
{
  GhNip29Op *op = data;
  op->timer = 0;
  if (!op->dropped)
    op_eval(op);
  return G_SOURCE_REMOVE;
}

static void
op_schedule(GhNip29Op *op, gint64 at)
{
  GhNip29Outbox *self = op->outbox;
  if (op->timer && op->timer_at == at)
    return;
  op_cancel_timer(op);
  gint64 now_ms = gh_clock_get_real_time(self->clock) / 1000;
  gint64 delay_ms = MAX(at * 1000 - now_ms, 0);
  op->timer_at = at;
  op->timer = gh_clock_timeout_add(self->clock, (guint64)delay_ms, op_timer_fired,
                                   g_object_ref(op), g_object_unref);
}

static gboolean
op_idle(gpointer data)
{
  GhNip29Op *op = data;
  g_clear_pointer(&op->idle, g_source_unref);
  if (!op->dropped)
    op_eval(op);
  return G_SOURCE_REMOVE;
}

static void
op_queue_eval(GhNip29Op *op)
{
  if (op->idle || op->dropped)
    return;
  op->idle = g_idle_source_new();
  g_source_set_callback(op->idle, op_idle, g_object_ref(op), g_object_unref);
  g_source_attach(op->idle, op->outbox->context);
}

static void
give_up(GhNip29Op *op, const gchar *reason)
{
  op->manual = FALSE;
  op_cancel_timer(op);
  if (!op_persist(op, GH_STORE_OUTBOX_NEEDS_ATTENTION, 0, FALSE, reason) && !op->dropped) {
    /* Not durable, but this session must not keep trying. */
    op->entry->state = GH_STORE_OUTBOX_NEEDS_ATTENTION;
    op->entry->next_attempt_at = 0;
    g_free(op->entry->last_error);
    op->entry->last_error = g_strdup(reason);
  }
  if (!op->dropped)
    op_refresh(op);
}

/* ---- Signing and T-seal ---------------------------------------------------------- */

static void
store_signed(GhNip29Op *op, const gchar *signed_json)
{
  GhNip29Outbox *self = op->outbox;
  const gchar *urls[] = { op->relay_url, NULL };
  GhStoreSealedEvent event = {
    .role = GH_STORE_OUTBOX_ROLE_NIP29_EVENT,
    .event_id = op->event_id,
    .event_json = signed_json,
    .relay_urls = urls,
  };
  g_autoptr(GError) error = NULL;
  if (!gh_store_seal(self->store, op->entry->id, &event, 1, &error)) {
    if (g_error_matches(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE)) {
      if (op_reload(op)) /* sealed already: its stored event is published */
        op_queue_eval(op);
    } else if (g_error_matches(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND)) {
      op_drop(op);
    } else {
      g_warning("Group outbox entry %" G_GINT64_FORMAT " could not be stored: %s",
                op->entry->id, error->message);
      give_up(op, REASON_STORAGE);
    }
    return;
  }
  if (!op_reload(op))
    return;
  op_refresh(op);
  op_eval(op);
}

/* The signer's result must be exactly the event that was queued. */
static gboolean
signed_matches(GhNip29Op *op, const gchar *signed_json)
{
  NostrEvent *event = nostr_event_new();
  gchar id[65] = { 0 };
  gboolean ok = signed_json &&
    nostr_event_deserialize_signed(event, signed_json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_validate(event, id) == NOSTR_EVENT_VALIDATION_OK &&
    strcmp(id, op->event_id) == 0 &&
    g_strcmp0(nostr_event_get_pubkey(event), op->outbox->account) == 0;
  nostr_event_free(event);
  return ok;
}

typedef struct {
  GhNip29Op *op;
  GCancellable *signing; /* the attempt this answer belongs to */
} SignCall;

static void
on_signed(GObject *source, GAsyncResult *result, gpointer data)
{
  SignCall *call = data;
  GhNip29Op *op = call->op;
  (void)source;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_json = gh_account_controller_sign_finish(result, &error);
  gboolean current = op->signing == call->signing &&
                     !g_cancellable_is_cancelled(call->signing) && !op->dropped && op->outbox;
  g_object_unref(call->signing);
  g_free(call);
  if (!current) {
    g_object_unref(op);
    return; /* stopped by a switch or a cancel: resumes (or not) from the store */
  }
  g_clear_object(&op->signing);
  op->signer_pending = FALSE;
  if (!signed_json) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      op_refresh(op); /* the generation ended; it resumes with the account */
    else
      give_up(op, REASON_SIGNER);
  } else if (!signed_matches(op, signed_json)) {
    give_up(op, REASON_INVALID);
  } else {
    store_signed(op, signed_json);
  }
  g_object_unref(op);
}

static void
start_sign(GhNip29Op *op)
{
  GhNip29Outbox *self = op->outbox;
  if (op->entry->state == GH_STORE_OUTBOX_QUEUED &&
      !op_persist(op, GH_STORE_OUTBOX_SEALING, 0, FALSE, NULL))
    return;
  op->signing = g_cancellable_new();
  op->signer_pending = TRUE;
  op_refresh(op);
  SignCall *call = g_new0(SignCall, 1);
  call->op = g_object_ref(op);
  call->signing = g_object_ref(op->signing);
  gh_account_controller_sign_with_cancellable_async(self->accounts, op->entry->rumor_json,
                                                    op->signing, on_signed, call);
}

/* ---- Publishing ----------------------------------------------------------------- */

static void
record_outcome(GhNip29Op *op, GhStoreOutboxEvent *event, GhStoreOutboxTarget *target,
               GhRelayPublishOutcome outcome, GhRelayOkPrefix prefix, const gchar *message,
               gboolean count)
{
  GhStoreTargetOutcome record = {
    .relay_url = target->relay_url,
    .outcome = outcome,
    .ok_prefix = prefix == GH_RELAY_OK_PREFIX_NONE ? -1 : (gint)prefix,
    .ok_message = message,
    .count_attempt = count,
  };
  g_autoptr(GError) error = NULL;
  gboolean stored = gh_store_record_outcome(op->outbox->store, event->id, &record, &error);
  target->outcome = outcome;
  target->ok_prefix = record.ok_prefix;
  g_free(target->ok_message);
  target->ok_message = message ? g_utf8_make_valid(message, MIN(strlen(message), MAX_RELAY_TEXT))
                               : NULL;
  if (count) {
    target->attempts++;
    target->last_attempt_at = now_unix(op->outbox);
  }
  if (!stored)
    op_store_failed(op, error);
}

static void
round_end(GhNip29Op *op)
{
  GhNip29Outbox *self = op->outbox;
  GhStoreOutboxTarget *target = op_target(op, NULL);
  gint64 now = now_unix(self);
  op->manual = FALSE;
  switch (target ? target_result(op, target) : GH_NIP29_OP_NOT_SENT) {
  case GH_NIP29_OP_ACCEPTED:
  case GH_NIP29_OP_DUPLICATE:
  case GH_NIP29_OP_PENDING_APPROVAL:
    if (op_persist(op, GH_STORE_OUTBOX_SETTLED, 0, FALSE, NULL))
      op_refresh(op);
    return;
  case GH_NIP29_OP_REJECTED:
  case GH_NIP29_OP_NOT_SENT:
  case GH_NIP29_OP_CANCELLED:
    give_up(op, REASON_REFUSED);
    return;
  default:
    break;
  }
  if (now >= deadline(op->entry)) {
    give_up(op, REASON_TIMED_OUT);
    return;
  }
  gint64 next = MIN(now + backoff_after(self->clock, op->entry->attempts), deadline(op->entry));
  if (!op_persist(op, GH_STORE_OUTBOX_WAITING_RETRY, next, FALSE, NULL))
    return;
  op_refresh(op);
  op_schedule(op, next);
}

static void
on_publish_update(GhRelayPublish *publish, const GhRelayPublishResult *result, gpointer data)
{
  GhNip29Op *op = g_object_ref(data);
  GhStoreOutboxEvent *event = NULL;
  GhStoreOutboxTarget *target = op_target(op, &event);
  if (!op->dropped && op->publish == publish && target &&
      g_strcmp0(target->relay_url, result->url) == 0) {
    record_outcome(op, event, target, result->outcome, result->prefix, result->message, TRUE);
    if (!op->dropped)
      op_refresh(op);
  }
  g_object_unref(op);
}

static void
on_publish_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{
  GhNip29Op *op = g_object_ref(data);
  (void)summary;
  if (op->publish == publish) {
    op->publish = NULL;
    gh_relay_publish_unref(publish);
    if (!op->dropped)
      round_end(op);
  }
  g_object_unref(op);
}

/* One GhRelayPublish (its own connection) of the stored event to the group
 * relay, signing in as the account on challenge (§4.3 NIP-29 group). */
static void
publish_start(GhNip29Op *op)
{
  GhNip29Outbox *self = op->outbox;
  GhStoreOutboxEvent *event = NULL;
  GhStoreOutboxTarget *target = op_target(op, &event);
  if (!target || !event->event_json) {
    give_up(op, REASON_STORAGE);
    return;
  }
  /* A round interrupted by a switch or crash (still PUBLISHING) resumes
   * without counting another attempt: nobody answered it. */
  gboolean count = op->entry->state != GH_STORE_OUTBOX_PUBLISHING;
  if (!op_persist(op, GH_STORE_OUTBOX_PUBLISHING, 0, count, NULL))
    return;
  g_autoptr(GError) error = NULL;
  GhRelayPublish *publish = self->custom_transport
    ? gh_relay_publish_new_with_transport(self->generation, event->event_json, &self->transport,
                                          self->transport_data, on_publish_update,
                                          on_publish_done, op, &error)
    : gh_relay_publish_new(self->generation, event->event_json, on_publish_update,
                           on_publish_done, op, &error);
  if (!publish) {
    record_outcome(op, event, target, GH_RELAY_PUBLISH_REJECTED, GH_RELAY_OK_PREFIX_INVALID,
                   error ? error->message : "The stored event cannot be published", TRUE);
    round_end(op);
    return;
  }
  if (self->custom_transport && self->has_auth_transport)
    gh_relay_publish_set_auth_transport(publish, &self->auth_transport);
  if (self->publish_deadline)
    gh_relay_publish_set_deadline(publish, self->publish_deadline);
  if (!gh_relay_publish_add_url(publish, target->relay_url, &error)) {
    gh_relay_publish_unref(publish);
    record_outcome(op, event, target, GH_RELAY_PUBLISH_REJECTED, GH_RELAY_OK_PREFIX_INVALID,
                   error->message, TRUE);
    round_end(op);
    return;
  }
  g_autoptr(GError) auth_error = NULL;
  if (!gh_auth_policy_apply_publish(self->policy, publish, GH_AUTH_PURPOSE_GROUP,
                                    target->relay_url, &auth_error))
    g_debug("Groundhog will not sign in to a group relay: %s", auth_error->message);
  op->publish = publish;
  op_refresh(op);
  if (gh_relay_publish_start(publish, &error))
    return; /* outcomes arrive through the callbacks, possibly already */
  if (op->publish == publish) {
    op->publish = NULL;
    gh_relay_publish_cancel(publish);
    gh_relay_publish_unref(publish);
    record_outcome(op, event, target, GH_RELAY_PUBLISH_CONNECTION_FAILED,
                   GH_RELAY_OK_PREFIX_NONE, error ? error->message : NULL, TRUE);
    round_end(op);
  }
}

/* Whether a round publishes to the target: not yet answered for good. A
 * user's Retry also republishes a refusal that was not final by nature. */
static gboolean
target_due(GhNip29Op *op, GhStoreOutboxTarget *target)
{
  GhNip29OpResult result = target_result(op, target);
  switch (result) {
  case GH_NIP29_OP_QUEUED:
  case GH_NIP29_OP_SENDING:
  case GH_NIP29_OP_RETRYING:
    return TRUE;
  case GH_NIP29_OP_REJECTED:
  case GH_NIP29_OP_NOT_SENT:
    return op->manual &&
           !gh_target_is_final_refusal((GhRelayPublishOutcome)target->outcome,
                                       target_prefix(target));
  default:
    return FALSE;
  }
}

/* ---- The state machine ---------------------------------------------------------- */

static void
op_eval(GhNip29Op *op)
{
  if (op->dropped || !op->outbox || !running(op->outbox))
    return;
  GhStoreOutboxEntry *entry = op->entry;
  switch (entry->state) {
  case GH_STORE_OUTBOX_SETTLED:
  case GH_STORE_OUTBOX_CANCELLED:
  case GH_STORE_OUTBOX_NEEDS_ATTENTION:
    return;
  default:
    break;
  }
  if (op->signing || op->publish)
    return;
  g_object_ref(op);
  gint64 now = now_unix(op->outbox);
  if (!op_sealed(op)) {
    if (entry->next_attempt_at > now)
      op_schedule(op, entry->next_attempt_at);
    else
      start_sign(op);
  } else {
    GhStoreOutboxTarget *target = op_target(op, NULL);
    if (entry->state == GH_STORE_OUTBOX_WAITING_RETRY && entry->next_attempt_at > now)
      op_schedule(op, entry->next_attempt_at);
    else if (!target || !target_due(op, target))
      round_end(op); /* answered already (e.g. before a crash) */
    else if (entry->attempts > 0 && now >= deadline(entry) && !op->manual)
      give_up(op, REASON_TIMED_OUT);
    else
      publish_start(op);
  }
  g_object_unref(op);
}

/* ---- Generation and network ------------------------------------------------------ */

static void
update_activity(GhNip29Outbox *self)
{
  guint64 generation = 0;
  if (self->accounts &&
      gh_account_controller_get_state(self->accounts) == GH_ACCOUNT_STATE_ACTIVE) {
    const gchar *npub = gh_account_controller_get_active_npub(self->accounts);
    g_autofree gchar *active = npub ? gh_identity_pubkey_hex(npub) : NULL;
    if (g_strcmp0(active, self->account) == 0)
      generation = gh_account_controller_get_generation(self->accounts);
  }
  g_autoptr(GList) ops = g_hash_table_get_values(self->ops);
  g_list_foreach(ops, (GFunc)(void (*)(void))g_object_ref, NULL);
  if (generation != self->generation) {
    /* Nothing of the old generation may complete in the new one. */
    for (GList *l = ops; l; l = l->next) {
      op_stop(l->data);
      ((GhNip29Op *)l->data)->manual = FALSE;
    }
    self->generation = generation;
  }
  self->online = self->network && g_network_monitor_get_network_available(self->network);
  for (GList *l = ops; l; l = l->next) {
    GhNip29Op *op = l->data;
    if (!op->dropped) {
      op_refresh(op);
      if (running(self))
        op_queue_eval(op);
    }
  }
  g_list_free_full(g_steal_pointer(&ops), g_object_unref);
}

static void
on_accounts_changed(GhAccountController *accounts, gpointer data)
{
  (void)accounts;
  update_activity(data);
}

static void
on_network_changed(GNetworkMonitor *monitor, gboolean available, gpointer data)
{
  (void)monitor;
  (void)available;
  update_activity(data);
}

/* ---- Public ------------------------------------------------------------------------ */

static GhNip29Op *
op_new(GhNip29Outbox *self, GhStoreOutboxEntry *entry, GError **error)
{
  gint kind = 0;
  gchar id[65] = { 0 };
  g_autofree gchar *group_id = NULL;
  g_autofree gchar *relay_url = NULL;
  g_autofree gchar *room_group = NULL;
  if (!parse_unsigned(entry->rumor_json, self->account, &kind, id, &group_id) ||
      !gh_store_nip29_get_room(self->store, entry->conversation_id, &relay_url, &room_group,
                               error) ||
      g_strcmp0(group_id, room_group) != 0) {
    if (error && !*error)
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                          "Not an unsigned group event of this account for this group");
    return NULL;
  }
  GhNip29Op *op = g_object_new(GH_TYPE_NIP29_OP, NULL);
  op->outbox = self;
  op->entry = entry;
  op->kind = kind;
  memcpy(op->event_id, id, sizeof op->event_id);
  op->relay_url = g_steal_pointer(&relay_url);
  op->group_id = g_steal_pointer(&group_id);
  op->result = GH_NIP29_OP_QUEUED;
  g_hash_table_insert(self->ops, g_memdup2(&entry->id, sizeof entry->id), op);
  op_refresh(op);
  return op;
}

GhNip29Outbox *
gh_nip29_outbox_new(const GhNip29OutboxConfig *config, GError **error)
{
  g_return_val_if_fail(config != NULL && config->store != NULL, NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(config->accounts), NULL);
  g_return_val_if_fail(!config->network || G_IS_NETWORK_MONITOR(config->network), NULL);
  g_return_val_if_fail(!config->transport || (config->transport->open && config->transport->close),
                       NULL);
  g_autoptr(GArray) ids = gh_store_outbox_list_unfinished(config->store, GH_STORE_BACKEND_NIP29,
                                                          error);
  if (!ids)
    return NULL;
  g_autoptr(GhNip29Outbox) self = g_object_new(GH_TYPE_NIP29_OUTBOX, NULL);
  self->store = config->store;
  self->clock = gh_clock_ref(gh_store_get_clock(config->store));
  self->account = g_strdup(gh_store_get_account_pubkey(config->store));
  self->accounts = g_object_ref(config->accounts);
  self->policy = g_object_ref(gh_auth_policy_get_for_accounts(config->accounts));
  self->network = g_object_ref(config->network ? config->network
                                               : g_network_monitor_get_default());
  if (config->transport) {
    self->transport = *config->transport;
    self->custom_transport = TRUE;
    if (config->auth_transport) {
      self->auth_transport = *config->auth_transport;
      self->has_auth_transport = TRUE;
    }
  }
  self->transport_data = config->transport_data;
  self->publish_deadline = config->publish_deadline;
  for (guint i = 0; i < ids->len; i++) {
    g_autoptr(GError) load_error = NULL;
    GhStoreOutboxEntry *entry = gh_store_outbox_load(self->store, g_array_index(ids, gint64, i),
                                                     error);
    if (!entry)
      return NULL;
    if (!op_new(self, entry, &load_error)) {
      g_warning("Groundhog skipped a stored group operation: %s", load_error->message);
      gh_store_outbox_entry_free(entry);
    }
  }
  self->accounts_handler = g_signal_connect(self->accounts, "changed",
                                            G_CALLBACK(on_accounts_changed), self);
  self->network_handler = g_signal_connect(self->network, "network-changed",
                                           G_CALLBACK(on_network_changed), self);
  update_activity(self);
  return g_steal_pointer(&self);
}

gboolean
gh_nip29_outbox_is_active(GhNip29Outbox *self)
{
  g_return_val_if_fail(GH_IS_NIP29_OUTBOX(self), FALSE);
  return self->generation != 0;
}

GhNip29Op *
gh_nip29_outbox_enqueue(GhNip29Outbox *self, gint64 conversation_id,
                        const gchar *unsigned_json, GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_OUTBOX(self), NULL);
  if (!self->generation) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "This outbox's account is not the active account");
    return NULL;
  }
  gint kind = 0;
  gchar id[65] = { 0 };
  g_autofree gchar *group_id = NULL;
  g_autofree gchar *room_group = NULL;
  g_autoptr(GError) room_error = NULL;
  NostrEvent *event = NULL;
  if (!parse_unsigned(unsigned_json, self->account, &kind, id, &group_id) ||
      !gh_store_nip29_get_room(self->store, conversation_id, NULL, &room_group, &room_error) ||
      g_strcmp0(group_id, room_group) != 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Not an unsigned group event of this account for this group");
    return NULL;
  }
  g_autofree gchar *op_id = gh_store_new_op_id();
  gint64 outbox_id = 0, message_id = 0;
  if (kind >= 9 && kind <= 12) {
    event = nostr_event_new();
    nostr_event_deserialize_unsigned(event, unsigned_json, NULL);
    GhStoreOutgoing outgoing = {
      .conversation_id = conversation_id,
      .op_id = op_id,
      .backend_msg_id = id,
      .sender_pubkey = self->account,
      .kind = kind,
      .created_at = nostr_event_get_created_at(event),
      .body = nostr_event_get_content(event),
      .rumor_json = unsigned_json,
    };
    gboolean ok = gh_store_enqueue(self->store, &outgoing, &outbox_id, &message_id, error);
    nostr_event_free(event);
    if (!ok)
      return NULL;
  } else if (!gh_store_nip29_enqueue_operation(self->store, conversation_id, op_id,
                                               unsigned_json, &outbox_id, error)) {
    return NULL;
  }
  /* T-enqueue is durable: from here on the operation is the outbox's. */
  GhStoreOutboxEntry *entry = g_new0(GhStoreOutboxEntry, 1);
  entry->id = outbox_id;
  entry->conversation_id = conversation_id;
  entry->message_id = message_id;
  entry->op_id = g_strdup(op_id);
  entry->backend = GH_STORE_BACKEND_NIP29;
  entry->state = GH_STORE_OUTBOX_QUEUED;
  entry->rumor_json = g_strdup(unsigned_json);
  entry->created_at = now_unix(self);
  entry->events = g_ptr_array_new();
  GhNip29Op *op = op_new(self, entry, NULL);
  g_assert(op != NULL); /* validated above */
  g_object_ref(op);
  g_signal_emit(self, signals[SIGNAL_OP_ADDED], 0, op);
  if (!op->dropped)
    op_queue_eval(op);
  return op;
}

GhNip29Op *
gh_nip29_outbox_lookup(GhNip29Outbox *self, gint64 outbox_id)
{
  g_return_val_if_fail(GH_IS_NIP29_OUTBOX(self), NULL);
  GhNip29Op *op = g_hash_table_lookup(self->ops, &outbox_id);
  if (op)
    return g_object_ref(op);
  GhStoreOutboxEntry *entry = gh_store_outbox_load(self->store, outbox_id, NULL);
  if (!entry)
    return NULL;
  if (entry->backend != GH_STORE_BACKEND_NIP29) {
    gh_store_outbox_entry_free(entry);
    return NULL;
  }
  op = op_new(self, entry, NULL);
  if (!op) {
    gh_store_outbox_entry_free(entry);
    return NULL;
  }
  g_signal_emit(self, signals[SIGNAL_OP_ADDED], 0, op);
  if (!op->dropped)
    op_queue_eval(op);
  return op->dropped ? NULL : g_object_ref(op);
}

static gint
compare_ops(gconstpointer a, gconstpointer b)
{
  gint64 x = (*(GhNip29Op *const *)a)->entry->id;
  gint64 y = (*(GhNip29Op *const *)b)->entry->id;
  return x < y ? -1 : x > y;
}

GPtrArray *
gh_nip29_outbox_dup_ops(GhNip29Outbox *self)
{
  g_return_val_if_fail(GH_IS_NIP29_OUTBOX(self), NULL);
  GPtrArray *ops = g_ptr_array_new_with_free_func(g_object_unref);
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->ops);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    g_ptr_array_add(ops, g_object_ref(value));
  g_ptr_array_sort(ops, compare_ops);
  return ops;
}

gboolean
gh_nip29_outbox_retry(GhNip29Outbox *self, GhNip29Op *op, GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_OUTBOX(self), FALSE);
  g_return_val_if_fail(GH_IS_NIP29_OP(op), FALSE);
  if (op->outbox != self || op->dropped || !op->can_retry) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "This operation is not waiting to be retried");
    return FALSE;
  }
  g_object_ref(op);
  op_cancel_timer(op);
  gboolean ok;
  if (!op_sealed(op)) {
    ok = op_persist(op, GH_STORE_OUTBOX_QUEUED, 0, FALSE, NULL);
  } else {
    op->manual = TRUE;
    ok = op_persist(op, GH_STORE_OUTBOX_WAITING_RETRY, now_unix(self), FALSE, NULL);
  }
  if (ok && !op->dropped) {
    op_refresh(op);
    op_queue_eval(op);
  } else if (!ok) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                        "The retry could not be recorded");
  }
  g_object_unref(op);
  return ok;
}

gboolean
gh_nip29_outbox_cancel(GhNip29Outbox *self, GhNip29Op *op, GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_OUTBOX(self), FALSE);
  g_return_val_if_fail(GH_IS_NIP29_OP(op), FALSE);
  if (op->outbox != self || op->dropped)
    return FALSE;
  if (op->entry->state == GH_STORE_OUTBOX_CANCELLED)
    return TRUE;
  GhStoreOutboxTarget *target = op_target(op, NULL);
  if (op->entry->state == GH_STORE_OUTBOX_SETTLED ||
      (target && target->outcome == GH_RELAY_PUBLISH_ACCEPTED)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "The group relay already accepted it");
    return FALSE;
  }
  g_object_ref(op);
  op_stop(op);
  GhStoreOutboxUpdate update = { GH_STORE_OUTBOX_CANCELLED, 0, FALSE, REASON_CANCELLED };
  gboolean ok = gh_store_outbox_update(self->store, op->entry->id, &update, error);
  if (ok) {
    op->entry->state = GH_STORE_OUTBOX_CANCELLED;
    op->entry->next_attempt_at = 0;
    g_free(op->entry->last_error);
    op->entry->last_error = g_strdup(REASON_CANCELLED);
  } else {
    op_queue_eval(op);
  }
  if (!op->dropped)
    op_refresh(op);
  g_object_unref(op);
  return ok;
}

/* ---- GObject: op --------------------------------------------------------------- */

gint64
gh_nip29_op_get_outbox_id(GhNip29Op *self)
{
  g_return_val_if_fail(GH_IS_NIP29_OP(self), 0);
  return self->entry->id;
}

gint64
gh_nip29_op_get_message_id(GhNip29Op *self)
{
  g_return_val_if_fail(GH_IS_NIP29_OP(self), 0);
  return self->entry->message_id;
}

gint64
gh_nip29_op_get_conversation_id(GhNip29Op *self)
{
  g_return_val_if_fail(GH_IS_NIP29_OP(self), 0);
  return self->entry->conversation_id;
}

gint
gh_nip29_op_get_kind(GhNip29Op *self)
{
  g_return_val_if_fail(GH_IS_NIP29_OP(self), 0);
  return self->kind;
}

const gchar *
gh_nip29_op_get_event_id(GhNip29Op *self)
{
  g_return_val_if_fail(GH_IS_NIP29_OP(self), NULL);
  return self->event_id;
}

const gchar *
gh_nip29_op_get_relay_url(GhNip29Op *self)
{
  g_return_val_if_fail(GH_IS_NIP29_OP(self), NULL);
  return self->relay_url;
}

const gchar *
gh_nip29_op_get_group_id(GhNip29Op *self)
{
  g_return_val_if_fail(GH_IS_NIP29_OP(self), NULL);
  return self->group_id;
}

GhNip29OpResult
gh_nip29_op_get_result(GhNip29Op *self)
{
  g_return_val_if_fail(GH_IS_NIP29_OP(self), GH_NIP29_OP_NOT_SENT);
  return self->result;
}

const gchar *
gh_nip29_op_get_relay_message(GhNip29Op *self)
{
  g_return_val_if_fail(GH_IS_NIP29_OP(self), NULL);
  return self->relay_message;
}

gint64
gh_nip29_op_get_next_attempt_at(GhNip29Op *self)
{
  g_return_val_if_fail(GH_IS_NIP29_OP(self), 0);
  return self->next_attempt_at;
}

gboolean
gh_nip29_op_get_can_retry(GhNip29Op *self)
{
  g_return_val_if_fail(GH_IS_NIP29_OP(self), FALSE);
  return self->can_retry;
}

const gchar *
gh_nip29_op_get_signed_json(GhNip29Op *self)
{
  g_return_val_if_fail(GH_IS_NIP29_OP(self), NULL);
  if (!self->entry->events->len)
    return NULL;
  return ((GhStoreOutboxEvent *)g_ptr_array_index(self->entry->events, 0))->event_json;
}

static void
gh_nip29_op_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  GhNip29Op *self = GH_NIP29_OP(object);
  switch (prop_id) {
  case OP_PROP_OUTBOX_ID:
    g_value_set_int64(value, self->entry ? self->entry->id : 0);
    break;
  case OP_PROP_MESSAGE_ID:
    g_value_set_int64(value, self->entry ? self->entry->message_id : 0);
    break;
  case OP_PROP_CONVERSATION_ID:
    g_value_set_int64(value, self->entry ? self->entry->conversation_id : 0);
    break;
  case OP_PROP_KIND:
    g_value_set_int(value, self->kind);
    break;
  case OP_PROP_RESULT:
    g_value_set_enum(value, self->result);
    break;
  case OP_PROP_RELAY_MESSAGE:
    g_value_set_string(value, self->relay_message);
    break;
  case OP_PROP_NEXT_ATTEMPT_AT:
    g_value_set_int64(value, self->next_attempt_at);
    break;
  case OP_PROP_CAN_RETRY:
    g_value_set_boolean(value, self->can_retry);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_nip29_op_finalize(GObject *object)
{
  GhNip29Op *self = GH_NIP29_OP(object);
  g_assert(!self->signing && !self->publish && !self->timer && !self->idle);
  gh_store_outbox_entry_free(self->entry);
  g_free(self->relay_url);
  g_free(self->group_id);
  g_free(self->relay_message);
  G_OBJECT_CLASS(gh_nip29_op_parent_class)->finalize(object);
}

static void
gh_nip29_op_class_init(GhNip29OpClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = gh_nip29_op_get_property;
  object_class->finalize = gh_nip29_op_finalize;
  const GParamFlags ro = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;
  const GParamFlags notified = ro | G_PARAM_EXPLICIT_NOTIFY;
  op_props[OP_PROP_OUTBOX_ID] = g_param_spec_int64("outbox-id", NULL, NULL, 0, G_MAXINT64, 0, ro);
  op_props[OP_PROP_MESSAGE_ID] = g_param_spec_int64("message-id", NULL, NULL, 0, G_MAXINT64, 0,
                                                    ro);
  op_props[OP_PROP_CONVERSATION_ID] = g_param_spec_int64("conversation-id", NULL, NULL, 0,
                                                         G_MAXINT64, 0, ro);
  op_props[OP_PROP_KIND] = g_param_spec_int("kind", NULL, NULL, 0, G_MAXINT, 0, ro);
  op_props[OP_PROP_RESULT] = g_param_spec_enum("result", NULL, NULL, GH_TYPE_NIP29_OP_RESULT,
                                               GH_NIP29_OP_QUEUED, notified);
  op_props[OP_PROP_RELAY_MESSAGE] = g_param_spec_string("relay-message", NULL, NULL, NULL,
                                                        notified);
  op_props[OP_PROP_NEXT_ATTEMPT_AT] = g_param_spec_int64("next-attempt-at", NULL, NULL, 0,
                                                         G_MAXINT64, 0, notified);
  op_props[OP_PROP_CAN_RETRY] = g_param_spec_boolean("can-retry", NULL, NULL, FALSE, notified);
  g_object_class_install_properties(object_class, OP_N_PROPS, op_props);
}

static void
gh_nip29_op_init(GhNip29Op *self)
{
  (void)self;
}

/* ---- GObject: outbox --------------------------------------------------------- */

static void
gh_nip29_outbox_dispose(GObject *object)
{
  GhNip29Outbox *self = GH_NIP29_OUTBOX(object);
  if (self->ops) {
    g_autoptr(GList) ops = g_hash_table_get_values(self->ops);
    for (GList *l = ops; l; l = l->next) {
      GhNip29Op *op = l->data;
      op_stop(op);
      op->outbox = NULL;
    }
    g_clear_pointer(&self->ops, g_hash_table_unref);
  }
  if (self->accounts_handler)
    g_clear_signal_handler(&self->accounts_handler, self->accounts);
  if (self->network_handler)
    g_clear_signal_handler(&self->network_handler, self->network);
  g_clear_object(&self->accounts);
  g_clear_object(&self->policy);
  g_clear_object(&self->network);
  G_OBJECT_CLASS(gh_nip29_outbox_parent_class)->dispose(object);
}

static void
gh_nip29_outbox_finalize(GObject *object)
{
  GhNip29Outbox *self = GH_NIP29_OUTBOX(object);
  g_clear_pointer(&self->clock, gh_clock_unref);
  g_free(self->account);
  g_main_context_unref(self->context);
  G_OBJECT_CLASS(gh_nip29_outbox_parent_class)->finalize(object);
}

static void
gh_nip29_outbox_class_init(GhNip29OutboxClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_nip29_outbox_dispose;
  object_class->finalize = gh_nip29_outbox_finalize;
  signals[SIGNAL_OP_ADDED] = g_signal_new("op-added", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, GH_TYPE_NIP29_OP);
  signals[SIGNAL_OP_CHANGED] = g_signal_new("op-changed", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, GH_TYPE_NIP29_OP);
}

static void
gh_nip29_outbox_init(GhNip29Outbox *self)
{
  self->ops = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_object_unref);
  self->context = g_main_context_ref_thread_default();
}
