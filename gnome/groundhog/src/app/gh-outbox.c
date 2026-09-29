#include "gh-outbox.h"
#include "gh-auth-policy.h"
#include "gh-expiry.h"
#include "gh-identity.h"
#include "gh-nip17-envelope.h"
#include "gh-nip17-file.h"
#include "nostr-event.h"

#include <stdlib.h>
#include <string.h>

#define RETRY_WINDOW_S        (72 * 3600)
#define SELF_COPY_DELAY_MIN_S 5
#define SELF_COPY_DELAY_MAX_S 90
#define ROOM_SPACING_MAX_S    3     /* §4.5 S4: room wraps U(0, 3) s apart */
#define MAX_TARGETS           16    /* one GhRelayPublish's URL bound */
#define MAX_RELAY_TEXT        1024  /* as the store keeps it */
#define STORE_RETRY_S         30    /* re-check after a failed store write */
/* The largest rumor that still fits a gift wrap. The wrap NIP-44-encrypts
 * the signed seal, whose content is the rumor's NIP-44 payload in base64,
 * and a NIP-44 plaintext is at most 65535 bytes. A rumor of up to 40960
 * bytes pads to 40960, so its sealed form is about 55 KB; the next padding
 * step (49152) no longer fits. */
#define MAX_RUMOR_JSON        40960

/* Charter §3.6: the delay after the nth publish round; every 6 h after. */
static const guint backoff_seconds[] = { 15, 60, 5 * 60, 30 * 60, 2 * 3600, 6 * 3600 };

/* Reason codes kept in outbox.last_error. */
#define REASON_NO_INBOX      "no-recipient-inbox"
#define REASON_INBOX_UNKNOWN "inbox-unreachable"
#define REASON_SIGNER        "signer"
#define REASON_INVALID       "invalid"
#define REASON_ENVELOPE      "envelope"
#define REASON_STORAGE       "storage"
#define REASON_TIMED_OUT     "retry-window-over"
#define REASON_REFUSED       "relays-refused"   /* no recipient reached, nothing left to try */
#define REASON_CANCELLED     "cancelled"

/* Marks translatable source strings for xgettext (--keyword=N_). */
#define N_(text) (text)

static const gchar *
tr(const gchar *text)
{
  return g_dgettext(NULL, text);
}

typedef struct _Msg Msg;

/* One stored event (wrap) of a message and its publish in flight. */
typedef struct {
  Msg *msg;
  GhStoreOutboxEvent *event; /* borrowed from msg->entry */
  GhRelayPublish *publish;   /* this round's, while in flight */
  gboolean waiting;          /* this round waits for its not_before (D8) or round_at */
  gint64 round_at;           /* S4 on a later round: this round's own spacing (in
                              * memory, never stored); 0 = none */
} Leg;

struct _Msg {
  gint refs;
  GhOutbox *outbox;          /* borrowed: the outbox stops every message first */
  GhStoreOutboxEntry *entry; /* the persisted state, kept in step with the store */
  GhOutboxItem *item;
  /* The rumor's recipients ("p" order; the account alone for a note to
   * self), NULL when the stored rumor is unusable. */
  GStrv recipients;
  /* What this session learned of each recipient's 10050 (pubkey ->
   * GhInboxStatus + 1): a wrap without targets is "no inbox" only when
   * known absent; otherwise its lookup is retried. */
  GHashTable *inboxes;
  guint resolving;           /* recipient lookups of this round still out */
  GPtrArray *legs;           /* Leg, one per stored event */
  GhDmSend *seal;
  gulong seal_handler;
  gboolean signer_pending;
  guint seal_tries;          /* this session; paces inbox lookups that failed */
  GCancellable *resolve;     /* re-reading the recipients' 10050 after sealing */
  guint timer;
  gint64 timer_at;
  GSource *idle;
  gboolean in_round;         /* a publish round is running */
  gboolean starting;         /* inside round_publish: do not end the round yet */
  gboolean manual;           /* this round is the user's Retry */
  gboolean targets_fresh;    /* the target lists were read in this session */
  /* The resolver reported a changed list for the recipient (S2 background
   * refresh) that this message has not re-read yet: one more round adds the
   * new relays as targets of the same stored wrap. */
  gboolean inbox_changed;
  gboolean dropped;
};

struct _GhOutbox {
  GObject parent_instance;
  GhStore *store;
  GhClock *clock;
  gchar *account;            /* the store's account (hex) */
  GhAccountController *accounts;
  GhAccountRelays *account_relays;
  GhInboxResolver *inboxes;
  gulong inboxes_handler;
  GhAuthPolicy *policy;      /* the accounts' NIP-42 identity policy */
  GhDmSender *sender;
  GNetworkMonitor *network;
  GhRelayPublishTransport transport;
  GhRelayPublishAuthTransport auth_transport;
  gboolean custom_transport;
  gboolean has_auth_transport;
  gpointer transport_data;
  guint publish_deadline;
  GMainContext *context;
  guint64 generation;        /* the account generation it runs in; 0: paused */
  gboolean online;
  gulong accounts_handler;
  gulong network_handler;
  GHashTable *messages;      /* gint64 outbox id -> Msg (owned ref) */
};

struct _GhOutboxItem {
  GObject parent_instance;
  Msg *msg;                  /* NULL once the message is gone */
  gint64 outbox_id;
  gint64 message_id;
  gint64 conversation_id;
  GhStoreOutboxState state;
  GhMessageStatus status;
  gchar *detail;
  gboolean self_copy_missing;
  gint64 next_attempt_at;
  gboolean can_retry;
  gchar *rumor_json;
  gchar *rumor_id;
  guint n_recipients;        /* people it goes to (1 for a note to self; nostrc-lff5) */
};

enum {
  ITEM_PROP_0,
  ITEM_PROP_OUTBOX_ID,
  ITEM_PROP_MESSAGE_ID,
  ITEM_PROP_CONVERSATION_ID,
  ITEM_PROP_STATE,
  ITEM_PROP_STATUS,
  ITEM_PROP_LABEL,
  ITEM_PROP_ICON_NAME,
  ITEM_PROP_ACCESSIBLE_DESCRIPTION,
  ITEM_PROP_DETAIL,
  ITEM_PROP_SELF_COPY_MISSING,
  ITEM_PROP_NEXT_ATTEMPT_AT,
  ITEM_PROP_CAN_RETRY,
  ITEM_N_PROPS
};
static GParamSpec *item_props[ITEM_N_PROPS];

enum { SIGNAL_ITEM_ADDED, SIGNAL_ITEM_REMOVED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhOutboxItem, gh_outbox_item, G_TYPE_OBJECT)
G_DEFINE_FINAL_TYPE(GhOutbox, gh_outbox, G_TYPE_OBJECT)

static void msg_eval(Msg *msg);
static void msg_queue_eval(Msg *msg);
static void round_check(Msg *msg);
static void round_publish(Msg *msg);
static gboolean own_relays_settled(GhOutbox *self);

/* ---- small helpers ------------------------------------------------------- */

static gint64
now_unix(GhOutbox *self)
{
  return gh_clock_get_unix(self->clock);
}

static gboolean
running(GhOutbox *self)
{
  return self->generation != 0 && self->online;
}

static gint64
deadline(const GhStoreOutboxEntry *entry)
{
  return entry->created_at + RETRY_WINDOW_S;
}

/* base ×U(0.8, 1.2), in whole seconds (at least one). */
static gint64
jittered(GhClock *clock, guint base_seconds)
{
  guint64 permille = 800 + gh_clock_random_uniform(clock, 401);
  return MAX((gint64) ((base_seconds * permille + 500) / 1000), 1);
}

static gint64
backoff_after(GhClock *clock, guint rounds)
{
  guint index = MIN(MAX(rounds, 1), G_N_ELEMENTS(backoff_seconds)) - 1;
  return jittered(clock, backoff_seconds[index]);
}

/* scheme://host:port/path?query, lowercase scheme and host, default ports
 * made explicit, trailing slashes dropped: two spellings of one relay
 * compare equal (D8 overlap). */
static gchar *
url_key(const gchar *url)
{
  g_autoptr(GUri) uri = url ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
  if (!uri || !g_uri_get_host(uri))
    return url ? g_ascii_strdown(url, -1) : NULL;
  g_autofree gchar *scheme = g_ascii_strdown(g_uri_get_scheme(uri), -1);
  g_autofree gchar *host = g_ascii_strdown(g_uri_get_host(uri), -1);
  gint port = g_uri_get_port(uri);
  if (port < 0)
    port = g_str_equal(scheme, "wss") ? 443 : 80;
  g_autofree gchar *path = g_strdup(g_uri_get_path(uri));
  gsize len = strlen(path);
  while (len > 0 && path[len - 1] == '/')
    path[--len] = '\0';
  const gchar *query = g_uri_get_query(uri);
  return g_strdup_printf("%s://%s:%d%s%s%s", scheme, host, port, path, query ? "?" : "",
                         query ? query : "");
}

static gchar *
url_host(const gchar *url)
{
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
  return g_strdup(uri && g_uri_get_host(uri) ? g_uri_get_host(uri) : url);
}

static GhRelayOkPrefix
target_prefix(const GhStoreOutboxTarget *target)
{
  return target->ok_prefix < 0 ? GH_RELAY_OK_PREFIX_NONE : (GhRelayOkPrefix) target->ok_prefix;
}

static GhTargetClass
target_class(const GhStoreOutboxTarget *target)
{
  return gh_target_classify((GhRelayPublishOutcome) target->outcome, target_prefix(target),
                            target->attempts);
}

/* Whether a round publishes to this target: everything not accepted and not
 * terminal; a user's Retry also takes "error:" given up by count. */
static gboolean
target_due(const GhStoreOutboxTarget *target, gboolean manual)
{
  switch (target_class(target)) {
  case GH_TARGET_CLASS_PENDING:
  case GH_TARGET_CLASS_RESUMABLE:
  case GH_TARGET_CLASS_TRANSIENT:
    return TRUE;
  case GH_TARGET_CLASS_TERMINAL:
    return manual && !gh_target_is_final_refusal((GhRelayPublishOutcome) target->outcome,
                                                 target_prefix(target));
  case GH_TARGET_CLASS_ACCEPTED:
  default:
    return FALSE;
  }
}

static GhTargetClass
event_class(const GhStoreOutboxEvent *event)
{
  g_autofree GhTargetClass *classes = g_new0(GhTargetClass, MAX(event->targets->len, 1));
  for (guint i = 0; i < event->targets->len; i++)
    classes[i] = target_class(g_ptr_array_index(event->targets, i));
  return gh_target_class_combine(classes, event->targets->len);
}

static GhStoreOutboxTarget *
event_target(GhStoreOutboxEvent *event, const gchar *url)
{
  for (guint i = 0; i < event->targets->len; i++) {
    GhStoreOutboxTarget *target = g_ptr_array_index(event->targets, i);
    if (g_strcmp0(target->relay_url, url) == 0)
      return target;
  }
  return NULL;
}

static gboolean
leg_has_due(const Leg *leg, gboolean manual)
{
  for (guint i = 0; leg->event->event_json && i < leg->event->targets->len; i++)
    if (target_due(g_ptr_array_index(leg->event->targets, i), manual))
      return TRUE;
  return FALSE;
}

static gboolean
msg_has_due(Msg *msg, gboolean manual)
{
  for (guint i = 0; i < msg->legs->len; i++)
    if (leg_has_due(g_ptr_array_index(msg->legs, i), manual))
      return TRUE;
  return FALSE;
}

static gboolean
msg_sealed(const Msg *msg)
{
  return msg->entry->events->len > 0;
}

static gboolean
msg_note_to_self(const Msg *msg)
{
  return msg->recipients && msg->recipients[0] && !msg->recipients[1] &&
         g_strcmp0(msg->recipients[0], msg->outbox->account) == 0;
}

static guint
msg_n_recipients(const Msg *msg)
{
  return msg->recipients ? g_strv_length(msg->recipients) : 0;
}

static void
msg_set_inbox(Msg *msg, const gchar *pubkey, GhInboxStatus status)
{
  g_hash_table_replace(msg->inboxes, g_ascii_strdown(pubkey, -1), GINT_TO_POINTER(status + 1));
}

/* Known this session to have no usable kind-10050 (NOT_FOUND or EMPTY). */
static gboolean
msg_inbox_absent(Msg *msg, const gchar *pubkey)
{
  gint known = GPOINTER_TO_INT(g_hash_table_lookup(msg->inboxes, pubkey));
  return known == GH_INBOX_NOT_FOUND + 1 || known == GH_INBOX_EMPTY + 1;
}

/* A recipient's stored wrap that has nowhere to go: nothing was ever
 * published for that recipient (no 10050 when it was sealed or since). */
static gboolean
event_targetless(const GhStoreOutboxEvent *event)
{
  return event->role == GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP && event->targets->len == 0;
}

/* nostrc-9cho: whether a recipient had no usable 10050 is kept with its
 * stored wrap (outbox_events.no_inbox) while the wrap has no target, so a
 * restart still says "hasn't set up private messaging" rather than "Not
 * sent". Recorded as this session learns it; a failed write only loses
 * that after a restart (the next lookup finds it again). */
static void
msg_keep_inbox(Msg *msg, GhStoreOutboxEvent *event)
{
  gboolean no_inbox = event_targetless(event) && msg_inbox_absent(msg, event->target_pubkey);
  if (event->no_inbox == no_inbox)
    return;
  g_autoptr(GError) error = NULL;
  if (!gh_store_outbox_set_no_inbox(msg->outbox->store, event->id, no_inbox, &error))
    g_message("Groundhog could not record a recipient without message relays: %s",
              error->message);
  else
    event->no_inbox = no_inbox;
}

/* What a restart knows from the store: a stored wrap marked without an inbox,
 * and, for a message that stopped before sealing because nobody had one
 * (REASON_NO_INBOX), every recipient. */
static void
msg_restore_inboxes(Msg *msg)
{
  GhStoreOutboxEntry *entry = msg->entry;
  if (entry->events->len == 0) {
    if (g_strcmp0(entry->last_error, REASON_NO_INBOX) == 0)
      for (guint i = 0; msg->recipients && msg->recipients[i]; i++)
        msg_set_inbox(msg, msg->recipients[i], GH_INBOX_NOT_FOUND);
    return;
  }
  for (guint i = 0; i < entry->events->len; i++) {
    GhStoreOutboxEvent *event = g_ptr_array_index(entry->events, i);
    if (event->role == GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP && event->no_inbox &&
        event_targetless(event) && event->target_pubkey)
      msg_set_inbox(msg, event->target_pubkey, GH_INBOX_NOT_FOUND);
  }
}

static gboolean
msg_has_targetless(Msg *msg)
{
  for (guint i = 0; i < msg->entry->events->len; i++)
    if (event_targetless(g_ptr_array_index(msg->entry->events, i)))
      return TRUE;
  return FALSE;
}

/* A targetless recipient whose inbox is not known to be absent (its lookup
 * failed, or was not made this session): the outbox looks again on retry. */
static gboolean
msg_lookup_pending(Msg *msg)
{
  for (guint i = 0; i < msg->entry->events->len; i++) {
    GhStoreOutboxEvent *event = g_ptr_array_index(msg->entry->events, i);
    if (event_targetless(event) && !msg_inbox_absent(msg, event->target_pubkey))
      return TRUE;
  }
  return FALSE;
}

/* One recipient's combined class. A wrap without targets is TERMINAL when
 * the recipient has no inbox, TRANSIENT while the lookup is to be retried. */
static GhTargetClass
recipient_class(Msg *msg, const GhStoreOutboxEvent *event)
{
  if (event_targetless(event))
    return msg_inbox_absent(msg, event->target_pubkey) ? GH_TARGET_CLASS_TERMINAL
                                                       : GH_TARGET_CLASS_TRANSIENT;
  return event_class(event);
}

/* Whether every recipient's wrap was accepted by at least one relay. */
static gboolean
all_recipients_reached(Msg *msg)
{
  guint recipients = 0;
  for (guint i = 0; i < msg->entry->events->len; i++) {
    GhStoreOutboxEvent *event = g_ptr_array_index(msg->entry->events, i);
    if (event->role == GH_STORE_OUTBOX_ROLE_SELF_WRAP)
      continue;
    recipients++;
    if (event_class(event) != GH_TARGET_CLASS_ACCEPTED)
      return FALSE;
  }
  return recipients > 0;
}

static gboolean
any_recipient_reached(Msg *msg)
{
  for (guint i = 0; i < msg->entry->events->len; i++) {
    GhStoreOutboxEvent *event = g_ptr_array_index(msg->entry->events, i);
    if (event->role != GH_STORE_OUTBOX_ROLE_SELF_WRAP &&
        event_class(event) == GH_TARGET_CLASS_ACCEPTED)
      return TRUE;
  }
  return FALSE;
}

/* ---- item ---------------------------------------------------------------- */

static gchar *
detail_not_sent(Msg *msg)
{
  GhStoreOutboxEntry *entry = msg->entry;
  if (!msg_sealed(msg)) {
    const gchar *reason = entry->last_error;
    if (g_strcmp0(reason, REASON_SIGNER) == 0)
      return g_strdup(tr(N_("Nostr Signer didn't approve sending, or couldn't be reached.")));
    if (g_strcmp0(reason, REASON_STORAGE) == 0)
      return g_strdup(tr(N_("There wasn't enough storage space to prepare this message.")));
    if (g_strcmp0(reason, REASON_INBOX_UNKNOWN) == 0 || g_strcmp0(reason, REASON_TIMED_OUT) == 0)
      return g_strdup(msg_n_recipients(msg) > 1
        ? tr(N_("Groundhog couldn't look up the message relays of the people in this "
                "conversation."))
        : tr(N_("Groundhog couldn't look up the recipient's message relays.")));
    return g_strdup(tr(N_("This message couldn't be prepared for sending.")));
  }
  GString *text = g_string_new(NULL);
  for (guint i = 0; i < entry->events->len; i++) {
    GhStoreOutboxEvent *event = g_ptr_array_index(entry->events, i);
    if (event->role == GH_STORE_OUTBOX_ROLE_SELF_WRAP)
      continue;
    for (guint j = 0; j < event->targets->len; j++) {
      GhStoreOutboxTarget *target = g_ptr_array_index(event->targets, j);
      g_autofree gchar *host = url_host(target->relay_url);
      if (text->len)
        g_string_append_c(text, '\n');
      g_string_append_printf(text, "%s: %s", host,
        gh_message_status_describe_target((GhRelayPublishOutcome) target->outcome,
                                          target_prefix(target)));
    }
  }
  return g_string_free(text, FALSE);
}

static gchar *
detail_for(Msg *msg, GhMessageStatus status)
{
  guint accepted = 0, total = 0, reached = 0, recipients = 0;
  for (guint i = 0; i < msg->entry->events->len; i++) {
    GhStoreOutboxEvent *event = g_ptr_array_index(msg->entry->events, i);
    if (event->role == GH_STORE_OUTBOX_ROLE_SELF_WRAP)
      continue;
    recipients++;
    reached += recipient_class(msg, event) == GH_TARGET_CLASS_ACCEPTED;
    for (guint j = 0; j < event->targets->len; j++) {
      total++;
      accepted += target_class(g_ptr_array_index(event->targets, j)) == GH_TARGET_CLASS_ACCEPTED;
    }
  }
  gboolean room = msg_n_recipients(msg) > 1;
  switch (status) {
  case GH_MESSAGE_STATUS_WAITING_FOR_SIGNER:
    return g_strdup(tr(N_("Approve sending in Nostr Signer.")));
  case GH_MESSAGE_STATUS_QUEUED_OFFLINE:
    return g_strdup(tr(N_("Groundhog will send it when you're back online.")));
  case GH_MESSAGE_STATUS_SENDING:
    return g_strdup(room ? tr(N_("Sending to each person's message relays."))
                         : tr(N_("Sending to the recipient's message relays.")));
  case GH_MESSAGE_STATUS_SENT:
    if (room)
      return g_strdup(tr(N_("Accepted by at least one message relay of each person. "
                            "Groundhog can't tell when they receive or read it.")));
    return g_strdup_printf(tr(N_("Accepted by %u of %u of the recipient's message relays. "
                                 "Groundhog can't tell when they receive or read it.")),
                           accepted, total);
  case GH_MESSAGE_STATUS_PARTIALLY_SENT:
    return g_strdup_printf(tr(N_("Accepted for %u of %u recipients.")), reached, recipients);
  case GH_MESSAGE_STATUS_RETRYING:
    return g_strdup(tr(N_("Not sent yet. Groundhog will try again automatically.")));
  case GH_MESSAGE_STATUS_NOT_SENT:
    return detail_not_sent(msg);
  case GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX:
    return g_strdup(room ? tr(N_("No one in this conversation has set up private messaging "
                                 "yet."))
                         : tr(N_("The recipient hasn't set up private messaging yet.")));
  case GH_MESSAGE_STATUS_CANCELLED:
  default:
    return g_strdup(tr(N_("Cancelled before it was sent.")));
  }
}

/* Recomputes the item from the message and notifies what changed. */
static void
item_refresh(Msg *msg)
{
  GhOutboxItem *item = msg->item;
  GhStoreOutboxEntry *entry = msg->entry;
  gboolean sealed = msg_sealed(msg);
  g_autoptr(GArray) recipients = g_array_new(FALSE, FALSE, sizeof(GhTargetClass));
  GhTargetClass self_copy = GH_TARGET_CLASS_TERMINAL;
  gboolean has_self_copy = FALSE;
  for (guint i = 0; i < entry->events->len; i++) {
    GhStoreOutboxEvent *event = g_ptr_array_index(entry->events, i);
    GhTargetClass klass = recipient_class(msg, event);
    if (event->role == GH_STORE_OUTBOX_ROLE_SELF_WRAP) {
      self_copy = klass;
      has_self_copy = TRUE;
    } else {
      g_array_append_val(recipients, klass);
    }
  }
  gboolean gave_up = entry->state == GH_STORE_OUTBOX_NEEDS_ATTENTION;
  GhMessageStatusInput input = {
    .phase = entry->state == GH_STORE_OUTBOX_CANCELLED ? GH_MESSAGE_PHASE_CANCELLED
           : sealed ? GH_MESSAGE_PHASE_SEALED : GH_MESSAGE_PHASE_UNSEALED,
    .online = running(msg->outbox),
    .gave_up = gave_up,
    .signer_pending = msg->signer_pending,
    .no_inbox = gave_up && g_strcmp0(entry->last_error, REASON_NO_INBOX) == 0,
    .retry_scheduled = !sealed && entry->next_attempt_at > 0,
    .recipients = (const GhTargetClass *) recipients->data,
    .n_recipients = recipients->len,
  };
  GhMessageStatus status = gh_message_status_derive(&input);
  gboolean missing = has_self_copy &&
    gh_message_status_self_copy_missing(self_copy,
                                        gave_up || entry->state == GH_STORE_OUTBOX_SETTLED);
  gboolean waiting = entry->state == GH_STORE_OUTBOX_WAITING_RETRY ||
                     (!sealed && entry->state != GH_STORE_OUTBOX_CANCELLED &&
                      entry->state != GH_STORE_OUTBOX_NEEDS_ATTENTION);
  gint64 next = waiting ? entry->next_attempt_at : 0;
  /* Retry now: what gave up, or waits for its next automatic attempt. */
  gboolean can_retry = gave_up || (next > 0 && !msg->seal && !msg->in_round);
  g_autofree gchar *detail = detail_for(msg, status);

  g_object_freeze_notify(G_OBJECT(item));
  if (item->message_id != entry->message_id) {
    item->message_id = entry->message_id;
    g_object_notify_by_pspec(G_OBJECT(item), item_props[ITEM_PROP_MESSAGE_ID]);
  }
  if (item->state != entry->state) {
    item->state = entry->state;
    g_object_notify_by_pspec(G_OBJECT(item), item_props[ITEM_PROP_STATE]);
  }
  if (item->status != status) {
    item->status = status;
    g_object_notify_by_pspec(G_OBJECT(item), item_props[ITEM_PROP_STATUS]);
    g_object_notify_by_pspec(G_OBJECT(item), item_props[ITEM_PROP_LABEL]);
    g_object_notify_by_pspec(G_OBJECT(item), item_props[ITEM_PROP_ICON_NAME]);
    g_object_notify_by_pspec(G_OBJECT(item), item_props[ITEM_PROP_ACCESSIBLE_DESCRIPTION]);
  }
  if (g_strcmp0(item->detail, detail) != 0) {
    g_free(item->detail);
    item->detail = g_steal_pointer(&detail);
    g_object_notify_by_pspec(G_OBJECT(item), item_props[ITEM_PROP_DETAIL]);
  }
  if (item->self_copy_missing != missing) {
    item->self_copy_missing = missing;
    g_object_notify_by_pspec(G_OBJECT(item), item_props[ITEM_PROP_SELF_COPY_MISSING]);
  }
  if (item->next_attempt_at != next) {
    item->next_attempt_at = next;
    g_object_notify_by_pspec(G_OBJECT(item), item_props[ITEM_PROP_NEXT_ATTEMPT_AT]);
  }
  if (item->can_retry != can_retry) {
    item->can_retry = can_retry;
    g_object_notify_by_pspec(G_OBJECT(item), item_props[ITEM_PROP_CAN_RETRY]);
  }
  g_object_thaw_notify(G_OBJECT(item));
}

/* ---- message lifecycle --------------------------------------------------- */

static Msg *
msg_ref(Msg *msg)
{
  msg->refs++;
  return msg;
}

static void
leg_free(gpointer data)
{
  Leg *leg = data;
  g_assert(leg->publish == NULL);
  g_free(leg);
}

static void
msg_unref(Msg *msg)
{
  if (--msg->refs > 0)
    return;
  g_assert(!msg->seal && !msg->timer && !msg->idle && !msg->resolve);
  if (msg->item)
    msg->item->msg = NULL;
  g_clear_object(&msg->item);
  g_ptr_array_unref(msg->legs);
  gh_store_outbox_entry_free(msg->entry);
  g_strfreev(msg->recipients);
  g_hash_table_unref(msg->inboxes);
  g_free(msg);
}

static void
msg_set_entry(Msg *msg, GhStoreOutboxEntry *entry)
{
  g_ptr_array_set_size(msg->legs, 0);
  gh_store_outbox_entry_free(msg->entry);
  msg->entry = entry;
  for (guint i = 0; i < entry->events->len; i++) {
    Leg *leg = g_new0(Leg, 1);
    leg->msg = msg;
    leg->event = g_ptr_array_index(entry->events, i);
    g_ptr_array_add(msg->legs, leg);
  }
}

/* The id of a stored rumor (recomputed, as GhMessage does), or NULL. */
static gchar *
rumor_id_of(const gchar *rumor_json)
{
  if (!rumor_json)
    return NULL;
  NostrEvent *event = nostr_event_new();
  gchar *id = NULL;
  if (nostr_event_deserialize_compact(event, rumor_json, NULL) == 1) {
    char *computed = nostr_event_get_id(event);
    id = g_strdup(computed);
    free(computed);
  }
  nostr_event_free(event);
  return id;
}

static Msg *
msg_new(GhOutbox *self, GhStoreOutboxEntry *entry)
{
  Msg *msg = g_new0(Msg, 1);
  msg->refs = 1;
  msg->outbox = self;
  msg->legs = g_ptr_array_new_with_free_func(leg_free);
  msg_set_entry(msg, entry);
  msg->recipients = gh_nip17_rumor_dup_recipients(entry->rumor_json, self->account);
  msg->inboxes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  msg_restore_inboxes(msg);
  msg->item = g_object_new(GH_TYPE_OUTBOX_ITEM, NULL);
  msg->item->msg = msg;
  msg->item->outbox_id = entry->id;
  msg->item->message_id = entry->message_id;
  msg->item->conversation_id = entry->conversation_id;
  msg->item->state = entry->state;
  msg->item->rumor_json = g_strdup(entry->rumor_json);
  msg->item->rumor_id = rumor_id_of(entry->rumor_json);
  msg->item->n_recipients = MAX(msg_n_recipients(msg), 1);
  item_refresh(msg);
  g_hash_table_insert(self->messages, g_memdup2(&entry->id, sizeof entry->id), msg);
  return msg;
}

static void
msg_cancel_timer(Msg *msg)
{
  if (!msg->timer)
    return;
  guint timer = msg->timer;
  msg->timer = 0;
  gh_clock_source_remove(msg->outbox->clock, timer);
}

/* Stops everything in flight; the persisted state is left as it is, so the
 * message resumes from it. No callback of this message runs afterwards. */
static void
msg_stop(Msg *msg)
{
  if (msg->seal) {
    GhDmSend *seal = g_steal_pointer(&msg->seal);
    g_signal_handler_disconnect(seal, msg->seal_handler);
    msg->seal_handler = 0;
    gh_dm_send_cancel(seal);
    g_object_unref(seal);
  }
  msg->signer_pending = FALSE;
  for (guint i = 0; i < msg->legs->len; i++) {
    Leg *leg = g_ptr_array_index(msg->legs, i);
    leg->waiting = FALSE;
    if (leg->publish) {
      GhRelayPublish *publish = g_steal_pointer(&leg->publish);
      gh_relay_publish_cancel(publish);
      gh_relay_publish_unref(publish);
    }
  }
  if (msg->resolve) {
    g_cancellable_cancel(msg->resolve);
    g_clear_object(&msg->resolve);
  }
  msg->resolving = 0;
  msg_cancel_timer(msg);
  if (msg->idle) {
    GSource *idle = g_steal_pointer(&msg->idle);
    g_source_destroy(idle);
    g_source_unref(idle);
  }
  msg->in_round = FALSE;
  msg->starting = FALSE;
}

/* The message is deleted or gone from the store. */
static void
msg_drop(Msg *msg)
{
  if (msg->dropped)
    return;
  GhOutbox *self = msg->outbox;
  msg_ref(msg);
  msg->dropped = TRUE;
  msg_stop(msg);
  msg->entry->state = GH_STORE_OUTBOX_CANCELLED;
  item_refresh(msg);
  g_autoptr(GhOutboxItem) item = g_object_ref(msg->item);
  gint64 id = msg->entry->id;
  g_hash_table_remove(self->messages, &id);
  g_signal_emit(self, signals[SIGNAL_ITEM_REMOVED], 0, item);
  msg_unref(msg);
}

static void msg_schedule(Msg *msg, gint64 at);

/* A store write failed: a message whose rows are gone (its conversation was
 * forgotten or purged) is dropped; anything else (disk full, busy) is
 * re-evaluated a little later from what the store holds. */
static void
msg_store_failed(Msg *msg, const GError *error)
{
  g_autoptr(GError) load_error = NULL;
  g_autoptr(GhStoreOutboxEntry) entry =
    gh_store_outbox_load(msg->outbox->store, msg->entry->id, &load_error);
  if (!entry && g_error_matches(load_error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND)) {
    g_debug("Outbox entry %" G_GINT64_FORMAT " is gone from the store", msg->entry->id);
    msg_drop(msg);
    return;
  }
  g_warning("Outbox entry %" G_GINT64_FORMAT ": %s", msg->entry->id,
            error ? error->message : "store write failed");
  gint64 again = gh_clock_get_unix(msg->outbox->clock) + STORE_RETRY_S;
  if (!msg->timer || msg->timer_at > again)
    msg_schedule(msg, again);
}

/* Records a transition in the store and, once durable, in memory. */
static gboolean
msg_persist(Msg *msg, GhStoreOutboxState state, gint64 next, gboolean count,
            const gchar *reason)
{
  GhStoreOutboxUpdate update = { state, next, count, reason };
  g_autoptr(GError) error = NULL;
  if (!gh_store_outbox_update(msg->outbox->store, msg->entry->id, &update, &error)) {
    msg_store_failed(msg, error);
    return FALSE;
  }
  GhStoreOutboxEntry *entry = msg->entry;
  entry->state = state;
  entry->next_attempt_at = next;
  entry->attempts += count ? 1 : 0;
  g_free(entry->last_error);
  entry->last_error = g_strdup(reason);
  return TRUE;
}

/* Re-reads the entry (after T-seal); nothing may be in flight. */
static gboolean
msg_reload(Msg *msg)
{
  g_autoptr(GError) error = NULL;
  GhStoreOutboxEntry *entry = gh_store_outbox_load(msg->outbox->store, msg->entry->id, &error);
  if (!entry) {
    msg_store_failed(msg, error);
    return FALSE;
  }
  msg_set_entry(msg, entry);
  return TRUE;
}

/* ---- scheduling ---------------------------------------------------------- */

static gboolean
msg_timer_fired(gpointer data)
{
  Msg *msg = data;
  msg->timer = 0;
  if (!msg->dropped)
    msg_eval(msg); /* re-checks the time: the wall clock may have moved */
  return G_SOURCE_REMOVE;
}

/* One wake-up per message, at unix time at. */
static void
msg_schedule(Msg *msg, gint64 at)
{
  GhOutbox *self = msg->outbox;
  if (msg->timer && msg->timer_at == at)
    return;
  msg_cancel_timer(msg);
  gint64 now_ms = gh_clock_get_real_time(self->clock) / 1000;
  gint64 delay_ms = MAX(at * 1000 - now_ms, 0);
  msg->timer_at = at;
  msg->timer = gh_clock_timeout_add(self->clock, (guint64) delay_ms, msg_timer_fired,
                                    msg_ref(msg), (GDestroyNotify) msg_unref);
}

static gboolean
msg_idle(gpointer data)
{
  Msg *msg = data;
  g_clear_pointer(&msg->idle, g_source_unref);
  if (!msg->dropped)
    msg_eval(msg);
  return G_SOURCE_REMOVE;
}

static void
msg_queue_eval(Msg *msg)
{
  if (msg->idle || msg->dropped)
    return;
  msg->idle = g_idle_source_new();
  g_source_set_callback(msg->idle, msg_idle, msg_ref(msg), (GDestroyNotify) msg_unref);
  g_source_attach(msg->idle, msg->outbox->context);
}

/* ---- giving up and settling ---------------------------------------------- */

static void
give_up(Msg *msg, const gchar *reason)
{
  msg->in_round = FALSE;
  msg->manual = FALSE;
  msg_cancel_timer(msg);
  if (!msg_persist(msg, GH_STORE_OUTBOX_NEEDS_ATTENTION, 0, FALSE, reason) && !msg->dropped) {
    /* Not durable, but this session must not keep trying (e.g. asking the
     * signer again): it waits for the user as if it were. */
    msg->entry->state = GH_STORE_OUTBOX_NEEDS_ATTENTION;
    msg->entry->next_attempt_at = 0;
    g_free(msg->entry->last_error);
    msg->entry->last_error = g_strdup(reason);
  }
  if (!msg->dropped)
    item_refresh(msg);
}

/* ---- sealing ------------------------------------------------------------- */

static gboolean
inbox_status_absent(GhInboxStatus status)
{
  return status == GH_INBOX_NOT_FOUND || status == GH_INBOX_EMPTY;
}

static void
add_sealed_event(GArray *events, GPtrArray *url_lists, GhStoreOutboxRole role,
                 const GhDmSendLeg *leg, gint64 not_before)
{
  GPtrArray *urls = g_ptr_array_new();
  for (guint i = 0; i < leg->relays->len; i++)
    g_ptr_array_add(urls, ((GhDmRelayOutcome *) g_ptr_array_index(leg->relays, i))->url);
  g_ptr_array_add(urls, NULL);
  g_ptr_array_add(url_lists, urls);
  GhStoreSealedEvent event = {
    .role = role,
    .target_pubkey = leg->pubkey,
    .event_id = leg->wrap_id,
    .event_json = leg->wrap_json,
    .not_before = not_before,
    .relay_urls = (const gchar *const *) urls->pdata,
    /* nostrc-9cho: sealed for someone without a 10050: kept as such. */
    .no_inbox = leg->relays->len == 0 && inbox_status_absent(leg->inbox),
  };
  g_array_append_val(events, event);
}

/* D8: whether the self-copy's relays share one with a recipient's. */
static gboolean
inbox_sets_overlap(const GhDmSendStatus *status)
{
  if (!status->self_copy)
    return FALSE;
  g_autoptr(GHashTable) own = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < status->self_copy->relays->len; i++) {
    GhDmRelayOutcome *target = g_ptr_array_index(status->self_copy->relays, i);
    g_hash_table_add(own, url_key(target->url));
  }
  for (guint i = 0; i < status->recipients->len; i++) {
    GhDmSendLeg *leg = g_ptr_array_index(status->recipients, i);
    for (guint j = 0; j < leg->relays->len; j++) {
      g_autofree gchar *key = url_key(((GhDmRelayOutcome *) g_ptr_array_index(leg->relays, j))->url);
      if (g_hash_table_contains(own, key))
        return TRUE;
    }
  }
  return FALSE;
}

/* What the seal's lookups found for each recipient. */
static void
record_inboxes(Msg *msg, const GhDmSendStatus *status)
{
  for (guint i = 0; status->recipients && i < status->recipients->len; i++) {
    GhDmSendLeg *leg = g_ptr_array_index(status->recipients, i);
    msg_set_inbox(msg, leg->pubkey, leg->relays->len ? GH_INBOX_FOUND : leg->inbox);
  }
}

/* §4.5 S4: a room's recipient wraps go out in a random order, U(0, 3) s
 * apart (not_before; the first at once), so that a relay on several
 * recipients' lists does not get them back to back in "p" order. A
 * one-to-one message has one wrap and no delay. */
static void
room_schedule(GhOutbox *self, guint n, gint64 *not_before)
{
  if (n < 2)
    return;
  g_autofree guint *order = g_new(guint, n);
  for (guint i = 0; i < n; i++)
    order[i] = i;
  for (guint i = n - 1; i > 0; i--) {
    guint j = gh_clock_random_uniform(self->clock, i + 1);
    guint swap = order[i];
    order[i] = order[j];
    order[j] = swap;
  }
  gint64 at = now_unix(self);
  for (guint k = 1; k < n; k++) {
    at += gh_clock_random_range(self->clock, 0, ROOM_SPACING_MAX_S);
    not_before[order[k]] = at;
  }
}

/* When a leg goes out this round: its stored not_before (D8, and S4 of the
 * first round) or this round's own spacing, whichever is later. */
static gint64
leg_start_at(const Leg *leg)
{
  return MAX(leg->event->not_before, leg->round_at);
}

/* S4 for every later round too (nostrc-yp69): a retry, the user's Retry and
 * a republish after a restart draw a fresh random order and U(0, 3) s
 * spacing for the recipient wraps due this round, in memory (the stored
 * not_before is never rewritten), so that a relay on several recipients'
 * lists does not get them back to back in "p" order after it was down. The
 * first round keeps the spacing drawn at T-seal (a stored not_before still
 * ahead). The self-copy keeps its D8 delay only. */
static void
round_schedule(Msg *msg)
{
  GhOutbox *self = msg->outbox;
  gint64 now = now_unix(self);
  g_autoptr(GPtrArray) due = g_ptr_array_new();
  gboolean stored_ahead = FALSE;
  for (guint i = 0; i < msg->legs->len; i++) {
    Leg *leg = g_ptr_array_index(msg->legs, i);
    leg->round_at = 0;
    if (leg->event->role != GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP ||
        !leg_has_due(leg, msg->manual))
      continue;
    g_ptr_array_add(due, leg);
    stored_ahead = stored_ahead || leg->event->not_before > now;
  }
  if (due->len < 2 || stored_ahead)
    return;
  g_autofree gint64 *spacing = g_new0(gint64, due->len);
  room_schedule(self, due->len, spacing);
  for (guint i = 0; i < due->len; i++)
    ((Leg *) g_ptr_array_index(due, i))->round_at = spacing[i];
}

/* T-seal: every signed wrap and its targets, before any publish. */
static void
store_sealed(Msg *msg, const GhDmSendStatus *status)
{
  GhOutbox *self = msg->outbox;
  record_inboxes(msg, status);
  g_autoptr(GArray) events = g_array_new(FALSE, TRUE, sizeof(GhStoreSealedEvent));
  g_autoptr(GPtrArray) url_lists = g_ptr_array_new_with_free_func((GDestroyNotify) g_ptr_array_unref);
  g_autofree gint64 *spacing = g_new0(gint64, MAX(status->recipients->len, 1));
  room_schedule(self, status->recipients->len, spacing);
  for (guint i = 0; i < status->recipients->len; i++)
    add_sealed_event(events, url_lists, GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP,
                     g_ptr_array_index(status->recipients, i), spacing[i]);
  if (status->self_copy) {
    gint64 not_before = inbox_sets_overlap(status)
      ? now_unix(self) + gh_clock_random_range(self->clock, SELF_COPY_DELAY_MIN_S,
                                               SELF_COPY_DELAY_MAX_S)
      : 0;
    add_sealed_event(events, url_lists, GH_STORE_OUTBOX_ROLE_SELF_WRAP, status->self_copy,
                     not_before);
  }
  g_autoptr(GError) error = NULL;
  if (!gh_store_seal(self->store, msg->entry->id, (const GhStoreSealedEvent *) events->data,
                     events->len, &error)) {
    if (g_error_matches(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE)) {
      /* Sealed already: its stored wraps are the ones to publish. */
      if (msg_reload(msg))
        msg_queue_eval(msg);
    } else if (g_error_matches(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND)) {
      msg_drop(msg);
    } else {
      g_warning("Outbox entry %" G_GINT64_FORMAT " could not be sealed: %s", msg->entry->id,
                error->message);
      give_up(msg, REASON_STORAGE);
    }
    return;
  }
  if (!msg_reload(msg))
    return;
  /* Sealed with the lists as resolved; a list that changed meanwhile is
   * read again before the first round. */
  msg->targets_fresh = !msg->inbox_changed;
  item_refresh(msg);
  msg_eval(msg);
}

static void
seal_retry_later(Msg *msg)
{
  GhOutbox *self = msg->outbox;
  gint64 now = now_unix(self);
  if (now >= deadline(msg->entry)) {
    give_up(msg, REASON_INBOX_UNKNOWN);
    return;
  }
  gint64 delay = backoff_after(self->clock, msg->seal_tries); /* one draw: MIN() repeats */
  gint64 next = MIN(now + delay, deadline(msg->entry));
  if (!msg_persist(msg, GH_STORE_OUTBOX_QUEUED, next, FALSE, REASON_INBOX_UNKNOWN))
    return;
  item_refresh(msg);
  msg_schedule(msg, next);
}

static void
seal_finished(Msg *msg)
{
  GhDmSend *seal = g_steal_pointer(&msg->seal);
  g_signal_handler_disconnect(seal, msg->seal_handler);
  msg->seal_handler = 0;
  msg->signer_pending = FALSE;
  const GhDmSendStatus *status = gh_dm_send_get_status(seal);
  switch (status->result) {
  case GH_DM_SEND_RESULT_SEALED:
    store_sealed(msg, status);
    break;
  case GH_DM_SEND_RESULT_NO_RECIPIENT_INBOX:
    record_inboxes(msg, status);
    give_up(msg, REASON_NO_INBOX);
    break;
  case GH_DM_SEND_RESULT_INBOX_UNKNOWN:
    seal_retry_later(msg);
    break;
  case GH_DM_SEND_RESULT_CANCELLED:
    /* An account switch: it resumes with the account. */
    item_refresh(msg);
    break;
  case GH_DM_SEND_RESULT_FAILED:
    give_up(msg, status->failure == GH_DM_SEND_FAILURE_SIGNER ? REASON_SIGNER
               : status->failure == GH_DM_SEND_FAILURE_ENVELOPE ? REASON_ENVELOPE
               : REASON_INVALID);
    break;
  default:
    give_up(msg, REASON_INVALID);
    break;
  }
  g_object_unref(seal);
}

static void
on_seal_changed(GhDmSend *seal, gpointer data)
{
  Msg *msg = msg_ref(data);
  if (gh_dm_send_is_done(seal)) {
    seal_finished(msg);
  } else if (gh_dm_send_get_status(seal)->phase == GH_DM_SEND_PHASE_SEALING &&
             !msg->signer_pending) {
    msg->signer_pending = TRUE;
    if (msg->entry->state == GH_STORE_OUTBOX_QUEUED)
      msg_persist(msg, GH_STORE_OUTBOX_SEALING, 0, FALSE, NULL);
    if (!msg->dropped)
      item_refresh(msg);
  }
  msg_unref(msg);
}

static void
start_seal(Msg *msg)
{
  msg->seal_tries++;
  /* A disappearing message's seals and wraps get their own later
   * expirations (charter §3.7, PT-7), drawn again at every seal: each
   * recipient's and the self-copy's independently. Without them the
   * envelope refuses an expiring rumor. */
  GhNip17RoomExpiration outer;
  gint64 created_at = 0, expires_at = 0;
  guint n = msg_note_to_self(msg) ? 0 : msg_n_recipients(msg);
  gboolean expiring =
    gh_nip17_rumor_get_expiration(msg->entry->rumor_json, &created_at, &expires_at) &&
    expires_at > 0 && gh_expiry_draw_room(msg->outbox->clock, created_at, expires_at, n, &outer);
  GhDmSend *seal = gh_dm_sender_seal_room_rumor(msg->outbox->sender, msg->entry->rumor_json,
                                                expiring ? &outer : NULL, NULL);
  msg->seal = seal;
  msg->seal_handler = g_signal_connect(seal, "changed", G_CALLBACK(on_seal_changed), msg);
  if (gh_dm_send_is_done(seal))
    seal_finished(msg); /* refused before any lookup or signer call */
}

/* ---- publish rounds ------------------------------------------------------ */

static void
record_outcome(Msg *msg, Leg *leg, GhStoreOutboxTarget *target, GhRelayPublishOutcome outcome,
               GhRelayOkPrefix prefix, const gchar *message, gboolean count)
{
  GhStoreTargetOutcome record = {
    .relay_url = target->relay_url,
    .outcome = outcome,
    .ok_prefix = prefix == GH_RELAY_OK_PREFIX_NONE ? -1 : (gint) prefix,
    .ok_message = message,
    .count_attempt = count,
  };
  g_autoptr(GError) error = NULL;
  gboolean stored = gh_store_record_outcome(msg->outbox->store, leg->event->id, &record, &error);
  target->outcome = outcome;
  target->ok_prefix = record.ok_prefix;
  g_free(target->ok_message);
  target->ok_message = message ? g_utf8_make_valid(message, MIN(strlen(message), MAX_RELAY_TEXT))
                               : NULL;
  if (count) {
    target->attempts++;
    target->last_attempt_at = now_unix(msg->outbox);
  }
  if (!stored)
    msg_store_failed(msg, error);
}

static void
on_publish_update(GhRelayPublish *publish, const GhRelayPublishResult *result, gpointer data)
{
  Leg *leg = data;
  Msg *msg = msg_ref(leg->msg);
  GhStoreOutboxTarget *target = event_target(leg->event, result->url);
  if (!msg->dropped && leg->publish == publish && target) {
    /* T-outcome. CANCELLED never arrives here: a cancelled publish is silent. */
    record_outcome(msg, leg, target, result->outcome, result->prefix, result->message, TRUE);
    if (!msg->dropped)
      item_refresh(msg);
  }
  msg_unref(msg);
}

static void
on_publish_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{
  Leg *leg = data;
  Msg *msg = msg_ref(leg->msg);
  (void) summary;
  if (leg->publish == publish) {
    leg->publish = NULL;
    gh_relay_publish_unref(publish); /* the publish holds itself during the callback */
  }
  if (!msg->dropped)
    round_check(msg);
  msg_unref(msg);
}

/* Records a local failure for every target this round would publish to. */
static void
leg_fail(Leg *leg, const gchar *why)
{
  for (guint i = 0; i < leg->event->targets->len && !leg->msg->dropped; i++) {
    GhStoreOutboxTarget *target = g_ptr_array_index(leg->event->targets, i);
    if (target_due(target, leg->msg->manual))
      record_outcome(leg->msg, leg, target, GH_RELAY_PUBLISH_REJECTED,
                     GH_RELAY_OK_PREFIX_INVALID, why, TRUE);
  }
}

/* The NIP-42 purpose of publishing leg's wrap to url (GhAuthPolicy decides
 * the identity): a recipient's inbox is RECIPIENT_WRAP (ephemeral, never the
 * account, §4.4 R1/R7); the self-copy and a note to self go to the own inbox,
 * SELF_WRAP (the account, on challenge; W13 review 7a). A stored self-copy
 * target that has since left the account's own 10050 list is somebody
 * else's relay now and is treated like a recipient's. Before the own lists
 * are known this generation, the stored targets (taken from them) count as
 * own. */
static GhAuthPurpose
leg_purpose(GhOutbox *self, Leg *leg, const gchar *url)
{
  if (g_strcmp0(leg->event->target_pubkey, self->account) != 0)
    return GH_AUTH_PURPOSE_RECIPIENT_WRAP;
  if (!own_relays_settled(self))
    return GH_AUTH_PURPOSE_SELF_WRAP;
  const gchar *const *own = gh_account_relays_get_inbox_relays(self->account_relays);
  g_autofree gchar *key = url_key(url);
  for (guint i = 0; own && own[i]; i++) {
    g_autofree gchar *mine = url_key(own[i]);
    if (g_strcmp0(mine, key) == 0)
      return GH_AUTH_PURPOSE_SELF_WRAP;
  }
  return GH_AUTH_PURPOSE_RECIPIENT_WRAP;
}

/* One GhRelayPublish (its own connections) for the stored wrap, byte-for-byte,
 * to its due targets, each with the NIP-42 identity of its purpose. */
static void
leg_start(Leg *leg)
{
  Msg *msg = leg->msg;
  GhOutbox *self = msg->outbox;
  g_autoptr(GError) error = NULL;
  GhRelayPublish *publish = self->custom_transport
    ? gh_relay_publish_new_with_transport(self->generation, leg->event->event_json,
                                          &self->transport, self->transport_data,
                                          on_publish_update, on_publish_done, leg, &error)
    : gh_relay_publish_new(self->generation, leg->event->event_json, on_publish_update,
                           on_publish_done, leg, &error);
  if (!publish) {
    leg_fail(leg, error ? error->message : "The stored event cannot be published");
    return;
  }
  if (self->custom_transport && self->has_auth_transport)
    gh_relay_publish_set_auth_transport(publish, &self->auth_transport);
  if (self->publish_deadline)
    gh_relay_publish_set_deadline(publish, self->publish_deadline);
  guint added = 0;
  for (guint i = 0; i < leg->event->targets->len; i++) {
    GhStoreOutboxTarget *target = g_ptr_array_index(leg->event->targets, i);
    if (!target_due(target, msg->manual))
      continue;
    g_autoptr(GError) url_error = NULL;
    if (!gh_relay_publish_add_url(publish, target->relay_url, &url_error)) {
      record_outcome(msg, leg, target, GH_RELAY_PUBLISH_REJECTED, GH_RELAY_OK_PREFIX_INVALID,
                     url_error ? url_error->message : "Not a usable relay URL", TRUE);
      continue;
    }
    /* A refused identity (no current account session) leaves the URL
     * unauthenticated: a relay that demands AUTH then answers AUTH_REQUIRED. */
    if (!gh_auth_policy_apply_publish(self->policy, publish,
                                      leg_purpose(self, leg, target->relay_url),
                                      target->relay_url, &url_error))
      g_debug("Groundhog will not sign in to a message relay: %s", url_error->message);
    added++;
  }
  if (added == 0 || msg->dropped) {
    gh_relay_publish_unref(publish);
    return;
  }
  leg->publish = publish;
  if (gh_relay_publish_start(publish, &error))
    return; /* outcomes arrive through the callbacks, possibly already */
  if (leg->publish == publish) {
    leg->publish = NULL;
    gh_relay_publish_cancel(publish);
    gh_relay_publish_unref(publish);
  }
  for (guint i = 0; i < leg->event->targets->len && !msg->dropped; i++) {
    GhStoreOutboxTarget *target = g_ptr_array_index(leg->event->targets, i);
    if (target_due(target, msg->manual))
      record_outcome(msg, leg, target, GH_RELAY_PUBLISH_CONNECTION_FAILED,
                     GH_RELAY_OK_PREFIX_NONE, error ? error->message : NULL, TRUE);
  }
}

static void
round_end(Msg *msg)
{
  GhOutbox *self = msg->outbox;
  msg->in_round = FALSE;
  msg->manual = FALSE;
  gint64 now = now_unix(self);
  if (msg->inbox_changed && now < deadline(msg->entry)) {
    /* The recipient's list changed during this round (S2): read it again
     * and publish the same wrap to any new relay now. */
    msg->targets_fresh = FALSE;
    if (msg_persist(msg, GH_STORE_OUTBOX_WAITING_RETRY, now, FALSE, NULL)) {
      item_refresh(msg);
      msg_schedule(msg, now);
    }
    return;
  }
  if (!msg_has_due(msg, FALSE) && !msg_lookup_pending(msg)) {
    /* Nothing left to try automatically. Settled only if every recipient
     * has it; otherwise the user decides (Retry reaches "error:" relays,
     * and looks up again a recipient without message relays). */
    if (!all_recipients_reached(msg))
      give_up(msg, REASON_REFUSED);
    else if (msg_persist(msg, GH_STORE_OUTBOX_SETTLED, 0, FALSE, NULL))
      item_refresh(msg);
    return;
  }
  if (now >= deadline(msg->entry)) {
    give_up(msg, REASON_TIMED_OUT);
    return;
  }
  gint64 delay = backoff_after(self->clock, msg->entry->attempts); /* one draw: MIN() repeats */
  gint64 next = MIN(now + delay, deadline(msg->entry));
  if (!msg_persist(msg, GH_STORE_OUTBOX_WAITING_RETRY, next, FALSE, NULL))
    return;
  /* A retry re-reads the target lists first: they may have changed (OB-8).
   * Retries are sparse, and the resolver caches. */
  msg->targets_fresh = FALSE;
  item_refresh(msg);
  msg_schedule(msg, next);
}

/* Ends the round once nothing is in flight and no leg waits (D8). */
static void
round_check(Msg *msg)
{
  if (msg->dropped || !msg->in_round || msg->starting)
    return;
  gint64 wake = 0;
  gboolean busy = FALSE;
  for (guint i = 0; i < msg->legs->len; i++) {
    Leg *leg = g_ptr_array_index(msg->legs, i);
    busy |= leg->publish != NULL;
    if (leg->waiting && (!wake || leg_start_at(leg) < wake))
      wake = leg_start_at(leg);
  }
  /* A waiting leg goes out on time, whatever the others are doing. */
  if (wake)
    msg_schedule(msg, wake);
  else if (!busy)
    round_end(msg);
}

/* Starts the legs that are due now; the others wait for their not_before. */
static void
round_start_legs(Msg *msg, gboolean first)
{
  gint64 now = now_unix(msg->outbox);
  msg->starting = TRUE;
  for (guint i = 0; i < msg->legs->len && !msg->dropped; i++) {
    Leg *leg = g_ptr_array_index(msg->legs, i);
    if (leg->publish || (!first && !leg->waiting))
      continue;
    leg->waiting = FALSE;
    if (!leg_has_due(leg, msg->manual))
      continue;
    if (leg_start_at(leg) > now)
      leg->waiting = TRUE;
    else
      leg_start(leg);
  }
  msg->starting = FALSE;
  round_check(msg);
}

static void
round_publish(Msg *msg)
{
  /* A round interrupted by a switch or crash (still PUBLISHING) resumes
   * without counting another attempt: nobody answered it. */
  gboolean count = msg->entry->state != GH_STORE_OUTBOX_PUBLISHING;
  if (!msg_persist(msg, GH_STORE_OUTBOX_PUBLISHING, 0, count, NULL)) {
    msg->in_round = FALSE;
    return;
  }
  item_refresh(msg);
  round_schedule(msg);
  round_start_legs(msg, TRUE);
}

/* New relays of a list that changed after sealing become targets of the same
 * stored wrap (charter §3.6 "never re-seal", OB-8). */
static gboolean
event_has_relay(GhStoreOutboxEvent *event, const gchar *url)
{
  g_autofree gchar *key = url_key(url);
  for (guint i = 0; i < event->targets->len; i++) {
    g_autofree gchar *known =
      url_key(((GhStoreOutboxTarget *) g_ptr_array_index(event->targets, i))->relay_url);
    if (g_strcmp0(known, key) == 0)
      return TRUE;
  }
  return FALSE;
}

static void
add_targets(Msg *msg, GhStoreOutboxEvent *event, const gchar *const *urls)
{
  for (guint i = 0; urls && urls[i] && !msg->dropped; i++) {
    if (event->targets->len >= MAX_TARGETS)
      return;
    if (event_has_relay(event, urls[i]) || !gh_relay_url_validate(urls[i], NULL))
      continue;
    GhStoreTargetOutcome record = { .relay_url = urls[i], .outcome = GH_RELAY_PUBLISH_PENDING,
                                    .ok_prefix = -1 };
    g_autoptr(GError) error = NULL;
    if (!gh_store_record_outcome(msg->outbox->store, event->id, &record, &error)) {
      msg_store_failed(msg, error);
      return;
    }
    GhStoreOutboxTarget *target = g_new0(GhStoreOutboxTarget, 1);
    target->relay_url = g_strdup(urls[i]);
    target->ok_prefix = -1;
    g_ptr_array_add(event->targets, target);
  }
}

static gboolean
own_relays_settled(GhOutbox *self)
{
  GhAccountRelaysState state = gh_account_relays_get_state(self->account_relays);
  return gh_account_relays_get_generation(self->account_relays) == self->generation &&
         state != GH_ACCOUNT_RELAYS_DISCOVERING && state != GH_ACCOUNT_RELAYS_INACTIVE;
}

/* The account's own 10050 as it is now, for its self-copy (and a note to
 * self), when known for this generation; never waited for. */
static void
add_own_inbox_targets(Msg *msg)
{
  GhOutbox *self = msg->outbox;
  if (!own_relays_settled(self))
    return;
  const gchar *const *own = gh_account_relays_get_inbox_relays(self->account_relays);
  for (guint i = 0; i < msg->entry->events->len && !msg->dropped; i++) {
    GhStoreOutboxEvent *event = g_ptr_array_index(msg->entry->events, i);
    if (g_strcmp0(event->target_pubkey, self->account) == 0)
      add_targets(msg, event, own);
  }
}

typedef struct {
  GhOutbox *outbox;
  gint64 id;
  GCancellable *cancel;
  gchar *pubkey;       /* the recipient looked up */
} ResolveCall;

static void
on_resolved(GObject *source, GAsyncResult *result, gpointer data)
{
  ResolveCall *call = data;
  g_autoptr(GhInboxResult) inbox =
    gh_inbox_resolver_resolve_finish(GH_INBOX_RESOLVER(source), result, NULL);
  Msg *msg = g_cancellable_is_cancelled(call->cancel)
               ? NULL : g_hash_table_lookup(call->outbox->messages, &call->id);
  if (msg && msg->resolve == call->cancel) {
    msg_ref(msg);
    for (guint i = 0; inbox && i < msg->entry->events->len && !msg->dropped; i++) {
      GhStoreOutboxEvent *event = g_ptr_array_index(msg->entry->events, i);
      if (event->role != GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP ||
          g_strcmp0(event->target_pubkey, call->pubkey) != 0)
        continue;
      if (inbox->status == GH_INBOX_FOUND)
        add_targets(msg, event, (const gchar *const *) inbox->relays);
      /* A list naming no usable relay is as good as none. */
      msg_set_inbox(msg, call->pubkey, inbox->status == GH_INBOX_FOUND && event_targetless(event)
                                         ? GH_INBOX_EMPTY : inbox->status);
      if (!msg->dropped)
        msg_keep_inbox(msg, event);
    }
    if (msg->resolving > 0 && --msg->resolving == 0) {
      g_clear_object(&msg->resolve);
      msg->targets_fresh = TRUE;
      if (!msg->dropped) {
        item_refresh(msg);
        if (msg->in_round)
          round_publish(msg);
      }
    }
    msg_unref(msg);
  }
  g_object_unref(call->cancel);
  g_object_unref(call->outbox);
  g_free(call->pubkey);
  g_free(call);
}

/* Once per session and message, before its first round: the target lists
 * may have changed since sealing, so every recipient's is read again (one
 * lookup each, concurrently). A failed lookup never blocks publishing. */
static void
round_begin(Msg *msg)
{
  GhOutbox *self = msg->outbox;
  msg->in_round = TRUE;
  if (msg->targets_fresh) {
    round_publish(msg);
    return;
  }
  add_own_inbox_targets(msg);
  if (msg->dropped)
    return;
  if (!msg->recipients || msg_note_to_self(msg)) {
    msg->targets_fresh = TRUE;
    round_publish(msg);
    return;
  }
  msg->inbox_changed = FALSE; /* this resolve reads the changed lists */
  GCancellable *cancel = g_cancellable_new();
  msg->resolve = cancel;
  msg->resolving = msg_n_recipients(msg);
  for (guint i = 0; msg->recipients[i]; i++) {
    ResolveCall *call = g_new0(ResolveCall, 1);
    call->outbox = g_object_ref(self);
    call->id = msg->entry->id;
    call->cancel = g_object_ref(cancel);
    call->pubkey = g_strdup(msg->recipients[i]);
    gh_inbox_resolver_resolve_async(self->inboxes, call->pubkey, call->cancel, on_resolved,
                                    call);
  }
}

/* ---- the state machine --------------------------------------------------- */

static void
eval_unsealed(Msg *msg)
{
  GhStoreOutboxEntry *entry = msg->entry;
  if (msg->seal)
    return;
  gint64 now = now_unix(msg->outbox);
  if (entry->next_attempt_at > now) {
    msg_schedule(msg, entry->next_attempt_at);
    return;
  }
  if (msg->seal_tries > 0 && now >= deadline(entry)) {
    give_up(msg, REASON_INBOX_UNKNOWN);
    return;
  }
  /* The retry is happening: it no longer waits for one. */
  if (entry->next_attempt_at > 0 &&
      !msg_persist(msg, GH_STORE_OUTBOX_QUEUED, 0, FALSE, NULL))
    return;
  start_seal(msg);
}

static void
msg_eval(Msg *msg)
{
  GhStoreOutboxEntry *entry = msg->entry;
  if (msg->dropped || !running(msg->outbox))
    return;
  switch (entry->state) {
  case GH_STORE_OUTBOX_SETTLED:
  case GH_STORE_OUTBOX_CANCELLED:
  case GH_STORE_OUTBOX_NEEDS_ATTENTION:
    return;
  default:
    break;
  }
  msg_ref(msg);
  if (!msg_sealed(msg)) {
    eval_unsealed(msg);
  } else if (msg->in_round) {
    if (!msg->resolve)
      round_start_legs(msg, FALSE); /* a leg's not_before came */
  } else {
    gint64 now = now_unix(msg->outbox);
    /* A recipient without message relays is looked up again on a retry
     * (automatic while its lookup failed, the user's when it had none). */
    gboolean lookup = msg_lookup_pending(msg) || (msg->manual && msg_has_targetless(msg));
    if (entry->state == GH_STORE_OUTBOX_WAITING_RETRY && entry->next_attempt_at > now)
      msg_schedule(msg, entry->next_attempt_at);
    else if (!msg_has_due(msg, msg->manual) && !msg->inbox_changed && !lookup)
      round_end(msg); /* everything answered already (e.g. before a crash) */
    else if (entry->attempts > 0 && now >= deadline(entry) && !msg->manual)
      give_up(msg, REASON_TIMED_OUT);
    else
      round_begin(msg);
  }
  msg_unref(msg);
}

/* ---- account generation and network ------------------------------------- */

static void
pause_all(GhOutbox *self)
{
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->messages);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Msg *msg = value;
    msg_stop(msg);
    msg->manual = FALSE;
    msg->targets_fresh = FALSE;
    msg->seal_tries = 0;
  }
}

static void
update_activity(GhOutbox *self)
{
  guint64 generation = 0;
  if (self->accounts &&
      gh_account_controller_get_state(self->accounts) == GH_ACCOUNT_STATE_ACTIVE) {
    const gchar *npub = gh_account_controller_get_active_npub(self->accounts);
    g_autofree gchar *active = npub ? gh_identity_pubkey_hex(npub) : NULL;
    if (g_strcmp0(active, self->account) == 0)
      generation = gh_account_controller_get_generation(self->accounts);
  }
  if (generation != self->generation) {
    /* Nothing of the old generation may complete in the new one. */
    if (self->generation)
      pause_all(self);
    self->generation = generation;
  }
  self->online = self->network && g_network_monitor_get_network_available(self->network);
  g_autoptr(GList) messages = g_hash_table_get_values(self->messages);
  for (GList *l = messages; l; l = l->next)
    msg_ref(l->data);
  for (GList *l = messages; l; l = l->next) {
    Msg *msg = l->data;
    if (!msg->dropped) {
      item_refresh(msg);
      if (running(self))
        msg_queue_eval(msg);
    }
  }
  for (GList *l = messages; l; l = l->next)
    msg_unref(l->data);
}

static void
on_accounts_changed(GhAccountController *accounts, gpointer data)
{
  (void) accounts;
  update_activity(data);
}

/* S2: the resolver's background refresh found a changed list for pubkey.
 * Every message to that recipient still inside its retry window reads it
 * again and publishes the same stored wrap to any new relay: at once when
 * idle (a settled message included), after the current round or seal
 * otherwise. One that needs attention keeps waiting for the user's Retry,
 * which then reads the list again. */
static void
on_inbox_changed(GhInboxResolver *resolver, const gchar *pubkey, gpointer data)
{
  GhOutbox *self = data;
  (void) resolver;
  if (!pubkey || g_ascii_strcasecmp(pubkey, self->account) == 0)
    return;
  gint64 now = now_unix(self);
  g_autofree gchar *lower = g_ascii_strdown(pubkey, -1);
  g_autoptr(GList) messages = g_hash_table_get_values(self->messages);
  for (GList *l = messages; l; l = l->next)
    msg_ref(l->data);
  for (GList *l = messages; l; l = l->next) {
    Msg *msg = l->data;
    GhStoreOutboxEntry *entry = msg->entry;
    if (msg->dropped || !msg->recipients ||
        !g_strv_contains((const gchar *const *) msg->recipients, lower) ||
        entry->state == GH_STORE_OUTBOX_CANCELLED)
      continue;
    msg->targets_fresh = FALSE;
    if (entry->state == GH_STORE_OUTBOX_NEEDS_ATTENTION || now >= deadline(entry))
      continue;
    if (!msg_sealed(msg)) {
      /* A seal in flight resolved the old list; one not started yet will
       * resolve the new one. */
      msg->inbox_changed = msg->seal != NULL;
      continue;
    }
    msg->inbox_changed = TRUE;
    if (msg->in_round || msg->resolve || msg->seal)
      continue; /* round_end() starts the extra round */
    if (entry->state == GH_STORE_OUTBOX_SETTLED || entry->state == GH_STORE_OUTBOX_WAITING_RETRY) {
      if (!msg_persist(msg, GH_STORE_OUTBOX_WAITING_RETRY, now, FALSE, NULL))
        continue;
      msg_cancel_timer(msg);
      item_refresh(msg);
    }
    if (running(self))
      msg_queue_eval(msg);
  }
  for (GList *l = messages; l; l = l->next)
    msg_unref(l->data);
}

static void
on_network_changed(GNetworkMonitor *monitor, gboolean available, gpointer data)
{
  (void) monitor;
  (void) available;
  update_activity(data);
}

/* ---- public API ---------------------------------------------------------- */

static Msg *
load_msg(GhOutbox *self, gint64 outbox_id, GError **error)
{
  Msg *msg = g_hash_table_lookup(self->messages, &outbox_id);
  if (msg)
    return msg;
  GhStoreOutboxEntry *entry = gh_store_outbox_load(self->store, outbox_id, error);
  if (!entry)
    return NULL;
  if (entry->backend != GH_STORE_BACKEND_NIP17) {
    /* NIP-29 and MLS entries belong to their own engines. */
    gh_store_outbox_entry_free(entry);
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND,
                        "Not a NIP-17 outbox entry");
    return NULL;
  }
  msg = msg_ref(msg_new(self, entry));
  g_signal_emit(self, signals[SIGNAL_ITEM_ADDED], 0, msg->item);
  gboolean dropped = msg->dropped; /* a handler may have deleted it */
  if (!dropped)
    msg_queue_eval(msg);
  msg_unref(msg);
  return dropped ? NULL : msg;
}

GhOutbox *
gh_outbox_new(const GhOutboxConfig *config, GError **error)
{
  g_return_val_if_fail(config != NULL && config->store != NULL, NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(config->accounts), NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_RELAYS(config->account_relays), NULL);
  g_return_val_if_fail(GH_IS_INBOX_RESOLVER(config->inboxes), NULL);
  g_return_val_if_fail(GH_IS_DM_SENDER(config->sender), NULL);
  g_return_val_if_fail(!config->network || G_IS_NETWORK_MONITOR(config->network), NULL);
  g_return_val_if_fail(!config->transport || (config->transport->open && config->transport->close),
                       NULL);
  g_autoptr(GArray) ids = gh_store_outbox_list_unfinished(config->store, GH_STORE_BACKEND_NIP17,
                                                          error);
  if (!ids)
    return NULL;
  g_autoptr(GhOutbox) self = g_object_new(GH_TYPE_OUTBOX, NULL);
  self->store = config->store;
  self->clock = gh_clock_ref(gh_store_get_clock(config->store));
  self->account = g_strdup(gh_store_get_account_pubkey(config->store));
  self->accounts = g_object_ref(config->accounts);
  self->account_relays = g_object_ref(config->account_relays);
  self->inboxes = g_object_ref(config->inboxes);
  self->policy = g_object_ref(gh_auth_policy_get_for_accounts(config->accounts));
  self->sender = g_object_ref(config->sender);
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
    GhStoreOutboxEntry *entry = gh_store_outbox_load(self->store, g_array_index(ids, gint64, i),
                                                     error);
    if (!entry)
      return NULL;
    msg_new(self, entry);
  }
  self->accounts_handler = g_signal_connect(self->accounts, "changed",
                                            G_CALLBACK(on_accounts_changed), self);
  self->network_handler = g_signal_connect(self->network, "network-changed",
                                           G_CALLBACK(on_network_changed), self);
  self->inboxes_handler = g_signal_connect(self->inboxes, "changed",
                                           G_CALLBACK(on_inbox_changed), self);
  update_activity(self);
  return g_steal_pointer(&self);
}

gboolean
gh_outbox_is_active(GhOutbox *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX(self), FALSE);
  return self->generation != 0;
}

static gint
compare_strings(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const gchar *const *) a, *(const gchar *const *) b);
}

/* NIP-17 conversation key (charter §3.3): the sorted participant set, the
 * account included, lowercase and ','-joined (the account alone for a note
 * to self). The recipients are valid (the rumor was built from them). */
static gchar *
nip17_backend_key(const gchar *account, const gchar *const *recipients)
{
  g_autoptr(GPtrArray) members = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(members, g_strdup(account));
  for (guint i = 0; recipients[i]; i++) {
    gchar *lower = g_ascii_strdown(recipients[i], -1);
    if (g_strcmp0(lower, account) == 0)
      g_free(lower);
    else
      g_ptr_array_add(members, lower);
  }
  g_ptr_array_sort(members, compare_strings);
  g_ptr_array_add(members, NULL);
  return g_strjoinv(",", (gchar **) members->pdata);
}

GhOutboxItem *
gh_outbox_send(GhOutbox *self, const gchar *recipient_pubkey_hex, const gchar *content,
               GError **error)
{
  const gchar *const recipients[] = { recipient_pubkey_hex, NULL };
  return gh_outbox_send_room(self, recipient_pubkey_hex ? recipients : NULL, content, error);
}

/* The rumor of a room message: a kind-14 text (content) or, for G21, a
 * kind-15 file (file). The same room rule, "p" tags and expiration either
 * way (gh-nip17-envelope.h). */
static gchar *
room_rumor_new(GhOutbox *self, const gchar *const *recipients, const gchar *content,
               const GhNip17File *file, gint64 created_at, gint64 expires_at,
               gchar **out_rumor_id, GError **error)
{
  return file ? gh_nip17_rumor_new_file_room(self->account, recipients, file, created_at,
                                             expires_at, out_rumor_id, error)
              : gh_nip17_rumor_new_room(self->account, recipients, content, created_at,
                                        expires_at, out_rumor_id, error);
}

/* T-enqueue of one room message, text or file (see room_rumor_new()); the
 * seal, publish, retry and per-recipient state that follow depend only on
 * the stored rumor, so they are the same for both. */
static GhOutboxItem *
send_room_message(GhOutbox *self, const gchar *const *recipients, const gchar *content,
                  const GhNip17File *file, GError **error)
{
  g_return_val_if_fail(GH_IS_OUTBOX(self), NULL);
  if (!self->generation) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "This outbox's account is not the active account");
    return NULL;
  }
  /* Validates the recipient keys (1 to 10, distinct) and the text
   * (non-empty UTF-8) or the file: one rumor, whatever the number of
   * recipients. */
  g_autofree gchar *rumor_id = NULL;
  const gint64 created_at = now_unix(self);
  g_autofree gchar *rumor = room_rumor_new(self, recipients, content, file, created_at, 0,
                                           &rumor_id, error);
  if (!rumor)
    return NULL;
  if (strlen(rumor) > MAX_RUMOR_JSON) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "The message is too long to send privately");
    return NULL;
  }
  g_autofree gchar *key = nip17_backend_key(self->account, recipients);
  g_autofree gchar *op_id = gh_store_new_op_id();
  gint64 conversation_id = 0, outbox_id = 0, message_id = 0;
  /* The conversation and T-enqueue commit together, with the conversation's
   * disappearing timer as it is at this moment (charter §3.7). */
  gint64 timer = 0, expires_at = 0;
  if (!gh_store_begin(self->store, error))
    return NULL;
  if (!gh_store_ensure_conversation(self->store, GH_STORE_BACKEND_NIP17, key,
                                    GH_STORE_REQUEST_ACCEPTED, &conversation_id, error) ||
      !gh_store_get_disappearing(self->store, conversation_id, &timer, error)) {
    gh_store_rollback(self->store);
    return NULL;
  }
  expires_at = gh_expiry_message_expiration(created_at, timer);
  if (expires_at) {
    g_clear_pointer(&rumor_id, g_free);
    g_free(rumor);
    rumor = room_rumor_new(self, recipients, content, file, created_at, expires_at, &rumor_id,
                           error);
    if (rumor && strlen(rumor) > MAX_RUMOR_JSON) {
      g_clear_pointer(&rumor, g_free);
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "The message is too long to send privately");
    }
    if (!rumor) {
      gh_store_rollback(self->store);
      return NULL;
    }
  }
  GhStoreOutgoing outgoing = {
    .conversation_id = conversation_id,
    .op_id = op_id,
    .backend_msg_id = rumor_id,
    .sender_pubkey = self->account,
    .kind = file ? GH_NIP17_FILE_KIND : 14,
    .created_at = created_at,
    .body = file ? file->url : content, /* a file message's content is its URL */
    .rumor_json = rumor,
    .expires_at = expires_at,
  };
  if (!gh_store_enqueue(self->store, &outgoing, &outbox_id, &message_id, error)) {
    gh_store_rollback(self->store);
    return NULL;
  }
  if (!gh_store_commit(self->store, error))
    return NULL;
  /* T-enqueue is durable: from here on the message is the outbox's, and
   * this call must not report a failure (a resend would duplicate it). */
  GhStoreOutboxEntry *entry = g_new0(GhStoreOutboxEntry, 1);
  entry->id = outbox_id;
  entry->conversation_id = conversation_id;
  entry->message_id = message_id;
  entry->op_id = g_strdup(op_id);
  entry->backend = GH_STORE_BACKEND_NIP17;
  entry->state = GH_STORE_OUTBOX_QUEUED;
  entry->rumor_json = g_strdup(rumor);
  entry->created_at = created_at;
  entry->events = g_ptr_array_new();
  Msg *msg = msg_ref(msg_new(self, entry));
  GhOutboxItem *item = g_object_ref(msg->item);
  g_signal_emit(self, signals[SIGNAL_ITEM_ADDED], 0, item);
  if (!msg->dropped)
    msg_queue_eval(msg);
  msg_unref(msg);
  return item;
}

GhOutboxItem *
gh_outbox_send_room(GhOutbox *self, const gchar *const *recipients, const gchar *content,
                    GError **error)
{
  return send_room_message(self, recipients, content, NULL, error);
}

GhOutboxItem *
gh_outbox_send_file(GhOutbox *self, const gchar *recipient_pubkey_hex, const GhNip17File *file,
                    GError **error)
{
  const gchar *const recipients[] = { recipient_pubkey_hex, NULL };
  return gh_outbox_send_file_room(self, recipient_pubkey_hex ? recipients : NULL, file, error);
}

GhOutboxItem *
gh_outbox_send_file_room(GhOutbox *self, const gchar *const *recipients,
                         const GhNip17File *file, GError **error)
{
  if (!file) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A file message needs a complete encrypted file");
    return NULL;
  }
  return send_room_message(self, recipients, NULL, file, error);
}

GhOutboxItem *
gh_outbox_lookup(GhOutbox *self, gint64 outbox_id)
{
  g_return_val_if_fail(GH_IS_OUTBOX(self), NULL);
  Msg *msg = load_msg(self, outbox_id, NULL);
  return msg ? g_object_ref(msg->item) : NULL;
}

GhOutboxItem *
gh_outbox_lookup_message(GhOutbox *self, gint64 message_id)
{
  g_return_val_if_fail(GH_IS_OUTBOX(self), NULL);
  gint64 outbox_id = 0;
  if (!gh_store_outbox_find_by_message(self->store, message_id, &outbox_id, NULL))
    return NULL;
  return gh_outbox_lookup(self, outbox_id);
}

GhOutboxItem *
gh_outbox_lookup_rumor(GhOutbox *self, const gchar *room_key, const gchar *rumor_id)
{
  g_return_val_if_fail(GH_IS_OUTBOX(self), NULL);
  g_return_val_if_fail(room_key != NULL && rumor_id != NULL, NULL);
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->messages);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    GhOutboxItem *item = ((Msg *) value)->item;
    if (g_strcmp0(item->rumor_id, rumor_id) == 0)
      return g_object_ref(item);
  }
  gint64 conversation_id = 0, outbox_id = 0;
  if (!gh_store_find_conversation(self->store, GH_STORE_BACKEND_NIP17, room_key,
                                  &conversation_id, NULL) ||
      !gh_store_outbox_find_by_rumor(self->store, conversation_id, rumor_id, &outbox_id, NULL))
    return NULL;
  return gh_outbox_lookup(self, outbox_id);
}

gboolean
gh_outbox_text_fits(GhOutbox *self, const gchar *recipient_pubkey_hex, const gchar *content)
{
  const gchar *const recipients[] = { recipient_pubkey_hex, NULL };
  return gh_outbox_text_fits_room(self, recipient_pubkey_hex ? recipients : NULL, content);
}

gboolean
gh_outbox_text_fits_room(GhOutbox *self, const gchar *const *recipients, const gchar *content)
{
  g_return_val_if_fail(GH_IS_OUTBOX(self), FALSE);
  if (!content)
    return TRUE;
  /* JSON escaping at most sextuples a byte (\u00XX) and the rest of a rumor
   * (ten "p" tags and an expiration included) is under 2 KB: short texts
   * need no measuring. */
  gsize length = strlen(content);
  if (length <= (MAX_RUMOR_JSON - 2048) / 6)
    return TRUE;
  if (length > MAX_RUMOR_JSON)
    return FALSE;
  g_autofree gchar *rumor = gh_nip17_rumor_new_room(self->account, recipients, content,
                                                    now_unix(self), 0, NULL, NULL);
  return !rumor || strlen(rumor) <= MAX_RUMOR_JSON;
}

static gint
compare_items(gconstpointer a, gconstpointer b)
{
  gint64 x = (*(GhOutboxItem *const *) a)->outbox_id;
  gint64 y = (*(GhOutboxItem *const *) b)->outbox_id;
  return x < y ? -1 : x > y;
}

GPtrArray *
gh_outbox_dup_items(GhOutbox *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX(self), NULL);
  GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->messages);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    g_ptr_array_add(items, g_object_ref(((Msg *) value)->item));
  g_ptr_array_sort(items, compare_items);
  return items;
}

gboolean
gh_outbox_retry(GhOutbox *self, gint64 outbox_id, GError **error)
{
  g_return_val_if_fail(GH_IS_OUTBOX(self), FALSE);
  Msg *msg = load_msg(self, outbox_id, error);
  if (!msg)
    return FALSE;
  GhStoreOutboxEntry *entry = msg->entry;
  gboolean sealed = msg_sealed(msg);
  if (entry->state != GH_STORE_OUTBOX_NEEDS_ATTENTION &&
      entry->state != GH_STORE_OUTBOX_WAITING_RETRY &&
      !(!sealed && entry->state != GH_STORE_OUTBOX_CANCELLED && entry->next_attempt_at > 0)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "This message is not waiting to be retried");
    return FALSE;
  }
  if (msg->seal || msg->in_round)
    return TRUE; /* it is being tried right now */
  msg_ref(msg);
  msg_cancel_timer(msg);
  gboolean ok;
  if (!sealed) {
    msg->seal_tries = 0;
    ok = msg_persist(msg, GH_STORE_OUTBOX_QUEUED, 0, FALSE, NULL);
  } else {
    msg->manual = TRUE;
    msg->targets_fresh = FALSE;
    ok = msg_persist(msg, GH_STORE_OUTBOX_WAITING_RETRY, now_unix(self), FALSE, NULL);
  }
  if (ok && !msg->dropped) {
    item_refresh(msg);
    msg_queue_eval(msg);
  } else if (!ok) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                        "The retry could not be recorded");
  }
  msg_unref(msg);
  return ok;
}

gboolean
gh_outbox_cancel(GhOutbox *self, gint64 outbox_id, GError **error)
{
  g_return_val_if_fail(GH_IS_OUTBOX(self), FALSE);
  Msg *msg = load_msg(self, outbox_id, error);
  if (!msg)
    return FALSE;
  if (msg->entry->state == GH_STORE_OUTBOX_CANCELLED)
    return TRUE;
  /* Once a recipient's relay accepted it, "cancelled" would be untrue. */
  if (msg->entry->state == GH_STORE_OUTBOX_SETTLED || any_recipient_reached(msg)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "This message has already been sent");
    return FALSE;
  }
  msg_ref(msg);
  msg_stop(msg);
  GhStoreOutboxUpdate update = { GH_STORE_OUTBOX_CANCELLED, 0, FALSE, REASON_CANCELLED };
  gboolean ok = gh_store_outbox_update(self->store, outbox_id, &update, error);
  if (ok) {
    msg->entry->state = GH_STORE_OUTBOX_CANCELLED;
    g_free(msg->entry->last_error);
    msg->entry->last_error = g_strdup(REASON_CANCELLED);
    msg->entry->next_attempt_at = 0;
  } else {
    msg_queue_eval(msg); /* not cancelled: it carries on */
  }
  if (!msg->dropped)
    item_refresh(msg);
  msg_unref(msg);
  return ok;
}

void
gh_outbox_prune(GhOutbox *self)
{
  g_return_if_fail(GH_IS_OUTBOX(self));
  g_autoptr(GList) messages = g_hash_table_get_values(self->messages);
  for (GList *l = messages; l; l = l->next)
    msg_ref(l->data);
  for (GList *l = messages; l; l = l->next) {
    Msg *msg = l->data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GhStoreOutboxEntry) entry = msg->dropped ? NULL
      : gh_store_outbox_load(self->store, msg->entry->id, &error);
    if (!msg->dropped && !entry && g_error_matches(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND))
      msg_drop(msg);
  }
  for (GList *l = messages; l; l = l->next)
    msg_unref(l->data);
}

gboolean
gh_outbox_delete(GhOutbox *self, gint64 outbox_id, GError **error)
{
  g_return_val_if_fail(GH_IS_OUTBOX(self), FALSE);
  Msg *msg = load_msg(self, outbox_id, error);
  if (!msg)
    return FALSE;
  msg_ref(msg);
  msg_stop(msg);
  gboolean ok = gh_store_outbox_delete(self->store, outbox_id, error);
  if (ok)
    msg_drop(msg);
  else
    msg_queue_eval(msg);
  msg_unref(msg);
  return ok;
}

/* ---- GObject: item ------------------------------------------------------- */

void
gh_outbox_target_free(GhOutboxTarget *target)
{
  if (!target)
    return;
  g_free(target->pubkey);
  g_free(target->event_id);
  g_free(target->url);
  g_free(target->message);
  g_free(target);
}

void
gh_outbox_recipient_free(GhOutboxRecipient *recipient)
{
  if (!recipient)
    return;
  g_free(recipient->pubkey);
  g_free(recipient);
}

/* One recipient's state from its stored wrap (NULL: not sealed yet). */
static GhOutboxRecipientState
recipient_state(Msg *msg, const gchar *pubkey, const GhStoreOutboxEvent *event)
{
  gboolean gave_up = msg->entry->state == GH_STORE_OUTBOX_NEEDS_ATTENTION;
  if (msg_inbox_absent(msg, pubkey) && (!event || event_targetless(event)))
    return GH_OUTBOX_RECIPIENT_NO_INBOX;
  if (!event)
    return gave_up ? GH_OUTBOX_RECIPIENT_NOT_SENT : GH_OUTBOX_RECIPIENT_WAITING;
  switch (recipient_class(msg, event)) {
  case GH_TARGET_CLASS_ACCEPTED:
    return GH_OUTBOX_RECIPIENT_SENT;
  case GH_TARGET_CLASS_PENDING:
  case GH_TARGET_CLASS_RESUMABLE:
    return gave_up ? GH_OUTBOX_RECIPIENT_NOT_SENT : GH_OUTBOX_RECIPIENT_WAITING;
  case GH_TARGET_CLASS_TRANSIENT:
    return gave_up ? GH_OUTBOX_RECIPIENT_NOT_SENT : GH_OUTBOX_RECIPIENT_RETRYING;
  case GH_TARGET_CLASS_TERMINAL:
  default:
    return GH_OUTBOX_RECIPIENT_NOT_SENT;
  }
}

GPtrArray *
gh_outbox_item_dup_recipients(GhOutboxItem *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX_ITEM(self), NULL);
  GPtrArray *recipients =
    g_ptr_array_new_with_free_func((GDestroyNotify) gh_outbox_recipient_free);
  Msg *msg = self->msg;
  if (!msg || !msg->recipients || msg->entry->state == GH_STORE_OUTBOX_CANCELLED)
    return recipients;
  for (guint i = 0; msg->recipients[i]; i++) {
    const gchar *pubkey = msg->recipients[i];
    const GhStoreOutboxEvent *event = NULL;
    for (guint j = 0; j < msg->entry->events->len && !event; j++) {
      const GhStoreOutboxEvent *candidate = g_ptr_array_index(msg->entry->events, j);
      if (candidate->role == GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP &&
          g_strcmp0(candidate->target_pubkey, pubkey) == 0)
        event = candidate;
    }
    GhOutboxRecipient *recipient = g_new0(GhOutboxRecipient, 1);
    recipient->pubkey = g_strdup(pubkey);
    recipient->state = recipient_state(msg, pubkey, event);
    for (guint j = 0; event && j < event->targets->len; j++)
      recipient->accepted +=
        target_class(g_ptr_array_index(event->targets, j)) == GH_TARGET_CLASS_ACCEPTED;
    recipient->relays = event ? event->targets->len : 0;
    g_ptr_array_add(recipients, recipient);
  }
  return recipients;
}

GPtrArray *
gh_outbox_item_dup_targets(GhOutboxItem *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX_ITEM(self), NULL);
  GPtrArray *targets = g_ptr_array_new_with_free_func((GDestroyNotify) gh_outbox_target_free);
  if (!self->msg)
    return targets;
  GPtrArray *events = self->msg->entry->events;
  for (guint pass = 0; pass < 2; pass++) {
    for (guint i = 0; i < events->len; i++) {
      GhStoreOutboxEvent *event = g_ptr_array_index(events, i);
      if ((event->role == GH_STORE_OUTBOX_ROLE_SELF_WRAP) != (pass == 1))
        continue;
      for (guint j = 0; j < event->targets->len; j++) {
        GhStoreOutboxTarget *stored = g_ptr_array_index(event->targets, j);
        GhOutboxTarget *target = g_new0(GhOutboxTarget, 1);
        target->role = event->role;
        target->pubkey = g_strdup(event->target_pubkey);
        target->event_id = g_strdup(event->event_id);
        target->url = g_strdup(stored->relay_url);
        target->outcome = (GhRelayPublishOutcome) stored->outcome;
        target->prefix = target_prefix(stored);
        target->message = g_strdup(stored->ok_message);
        target->attempts = stored->attempts;
        target->target_class = target_class(stored);
        target->description = gh_message_status_describe_target(target->outcome, target->prefix);
        g_ptr_array_add(targets, target);
      }
    }
  }
  return targets;
}

gint64
gh_outbox_item_get_outbox_id(GhOutboxItem *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX_ITEM(self), 0);
  return self->outbox_id;
}

gint64
gh_outbox_item_get_message_id(GhOutboxItem *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX_ITEM(self), 0);
  return self->message_id;
}

gint64
gh_outbox_item_get_conversation_id(GhOutboxItem *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX_ITEM(self), 0);
  return self->conversation_id;
}

GhStoreOutboxState
gh_outbox_item_get_state(GhOutboxItem *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX_ITEM(self), GH_STORE_OUTBOX_QUEUED);
  return self->state;
}

GhMessageStatus
gh_outbox_item_get_status(GhOutboxItem *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX_ITEM(self), GH_MESSAGE_STATUS_NOT_SENT);
  return self->status;
}

const gchar *
gh_outbox_item_get_label(GhOutboxItem *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX_ITEM(self), NULL);
  return gh_message_status_get_label(self->status);
}

const gchar *
gh_outbox_item_get_icon_name(GhOutboxItem *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX_ITEM(self), NULL);
  return gh_message_status_get_icon_name(self->status);
}

const gchar *
gh_outbox_item_get_accessible_description(GhOutboxItem *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX_ITEM(self), NULL);
  return gh_message_status_get_accessible_description_for(self->status, self->n_recipients);
}

const gchar *
gh_outbox_item_get_detail(GhOutboxItem *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX_ITEM(self), NULL);
  return self->detail;
}

gboolean
gh_outbox_item_get_self_copy_missing(GhOutboxItem *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX_ITEM(self), FALSE);
  return self->self_copy_missing;
}

gint64
gh_outbox_item_get_next_attempt_at(GhOutboxItem *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX_ITEM(self), 0);
  return self->next_attempt_at;
}

gboolean
gh_outbox_item_get_can_retry(GhOutboxItem *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX_ITEM(self), FALSE);
  return self->can_retry;
}

const gchar *
gh_outbox_item_get_rumor_json(GhOutboxItem *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX_ITEM(self), NULL);
  return self->rumor_json;
}

const gchar *
gh_outbox_item_get_rumor_id(GhOutboxItem *self)
{
  g_return_val_if_fail(GH_IS_OUTBOX_ITEM(self), NULL);
  return self->rumor_id;
}

static void
gh_outbox_item_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  GhOutboxItem *self = GH_OUTBOX_ITEM(object);
  switch (prop_id) {
  case ITEM_PROP_OUTBOX_ID:       g_value_set_int64(value, self->outbox_id); break;
  case ITEM_PROP_MESSAGE_ID:      g_value_set_int64(value, self->message_id); break;
  case ITEM_PROP_CONVERSATION_ID: g_value_set_int64(value, self->conversation_id); break;
  case ITEM_PROP_STATE:           g_value_set_int(value, self->state); break;
  case ITEM_PROP_STATUS:          g_value_set_enum(value, self->status); break;
  case ITEM_PROP_LABEL:
    g_value_set_string(value, gh_message_status_get_label(self->status));
    break;
  case ITEM_PROP_ICON_NAME:
    g_value_set_string(value, gh_message_status_get_icon_name(self->status));
    break;
  case ITEM_PROP_ACCESSIBLE_DESCRIPTION:
    g_value_set_string(value, gh_message_status_get_accessible_description_for(
      self->status, self->n_recipients));
    break;
  case ITEM_PROP_DETAIL:          g_value_set_string(value, self->detail); break;
  case ITEM_PROP_SELF_COPY_MISSING: g_value_set_boolean(value, self->self_copy_missing); break;
  case ITEM_PROP_NEXT_ATTEMPT_AT: g_value_set_int64(value, self->next_attempt_at); break;
  case ITEM_PROP_CAN_RETRY:       g_value_set_boolean(value, self->can_retry); break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_outbox_item_finalize(GObject *object)
{
  GhOutboxItem *self = GH_OUTBOX_ITEM(object);
  g_free(self->detail);
  g_free(self->rumor_json);
  g_free(self->rumor_id);
  G_OBJECT_CLASS(gh_outbox_item_parent_class)->finalize(object);
}

static void
gh_outbox_item_class_init(GhOutboxItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = gh_outbox_item_get_property;
  object_class->finalize = gh_outbox_item_finalize;
  const GParamFlags flags = G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;
  item_props[ITEM_PROP_OUTBOX_ID] =
    g_param_spec_int64("outbox-id", NULL, NULL, 0, G_MAXINT64, 0, flags);
  item_props[ITEM_PROP_MESSAGE_ID] =
    g_param_spec_int64("message-id", NULL, NULL, 0, G_MAXINT64, 0, flags);
  item_props[ITEM_PROP_CONVERSATION_ID] =
    g_param_spec_int64("conversation-id", NULL, NULL, 0, G_MAXINT64, 0, flags);
  item_props[ITEM_PROP_STATE] =
    g_param_spec_int("state", NULL, NULL, GH_STORE_OUTBOX_QUEUED, GH_STORE_OUTBOX_CANCELLED,
                     GH_STORE_OUTBOX_QUEUED, flags);
  /* The enum of GhMessage:status (gh-message-status.h). */
  item_props[ITEM_PROP_STATUS] =
    g_param_spec_enum("status", NULL, NULL, GH_TYPE_MESSAGE_STATUS,
                      GH_MESSAGE_STATUS_WAITING_FOR_SIGNER, flags);
  item_props[ITEM_PROP_LABEL] = g_param_spec_string("label", NULL, NULL, NULL, flags);
  item_props[ITEM_PROP_ICON_NAME] = g_param_spec_string("icon-name", NULL, NULL, NULL, flags);
  item_props[ITEM_PROP_ACCESSIBLE_DESCRIPTION] =
    g_param_spec_string("accessible-description", NULL, NULL, NULL, flags);
  item_props[ITEM_PROP_DETAIL] = g_param_spec_string("detail", NULL, NULL, NULL, flags);
  item_props[ITEM_PROP_SELF_COPY_MISSING] =
    g_param_spec_boolean("self-copy-missing", NULL, NULL, FALSE, flags);
  item_props[ITEM_PROP_NEXT_ATTEMPT_AT] =
    g_param_spec_int64("next-attempt-at", NULL, NULL, 0, G_MAXINT64, 0, flags);
  item_props[ITEM_PROP_CAN_RETRY] = g_param_spec_boolean("can-retry", NULL, NULL, FALSE, flags);
  g_object_class_install_properties(object_class, ITEM_N_PROPS, item_props);
}

static void
gh_outbox_item_init(GhOutboxItem *self)
{
  self->status = GH_MESSAGE_STATUS_WAITING_FOR_SIGNER;
}

/* ---- GObject: outbox ----------------------------------------------------- */

static void
gh_outbox_dispose(GObject *object)
{
  GhOutbox *self = GH_OUTBOX(object);
  if (self->messages) {
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, self->messages);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      Msg *msg = value;
      msg->dropped = TRUE;
      msg_stop(msg);
    }
    g_hash_table_remove_all(self->messages);
  }
  if (self->accounts_handler) {
    g_signal_handler_disconnect(self->accounts, self->accounts_handler);
    self->accounts_handler = 0;
  }
  if (self->network_handler) {
    g_signal_handler_disconnect(self->network, self->network_handler);
    self->network_handler = 0;
  }
  if (self->inboxes_handler) {
    g_signal_handler_disconnect(self->inboxes, self->inboxes_handler);
    self->inboxes_handler = 0;
  }
  g_clear_object(&self->policy);
  self->generation = 0;
  g_clear_object(&self->sender);
  g_clear_object(&self->inboxes);
  g_clear_object(&self->account_relays);
  g_clear_object(&self->accounts);
  g_clear_object(&self->network);
  G_OBJECT_CLASS(gh_outbox_parent_class)->dispose(object);
}

static void
gh_outbox_finalize(GObject *object)
{
  GhOutbox *self = GH_OUTBOX(object);
  g_hash_table_unref(self->messages);
  g_clear_pointer(&self->clock, gh_clock_unref);
  g_free(self->account);
  g_main_context_unref(self->context);
  G_OBJECT_CLASS(gh_outbox_parent_class)->finalize(object);
}

static void
gh_outbox_class_init(GhOutboxClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_outbox_dispose;
  object_class->finalize = gh_outbox_finalize;
  signals[SIGNAL_ITEM_ADDED] =
    g_signal_new("item-added", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                 NULL, G_TYPE_NONE, 1, GH_TYPE_OUTBOX_ITEM);
  signals[SIGNAL_ITEM_REMOVED] =
    g_signal_new("item-removed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                 NULL, G_TYPE_NONE, 1, GH_TYPE_OUTBOX_ITEM);
}

static void
gh_outbox_init(GhOutbox *self)
{
  self->context = g_main_context_ref_thread_default();
  self->messages = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
                                         (GDestroyNotify) msg_unref);
}
