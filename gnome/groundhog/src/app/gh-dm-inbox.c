#include "gh-dm-inbox.h"
#include "gh-account-auth.h"
#include "gh-identity.h"
#include "gh-nip17-inbox.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <nostr-event.h>
#include <nostr-filter.h>
#include <string.h>

#define KIND_GIFT_WRAP 1059
#define CHECKPOINT_MAGIC "groundhog-dm-inbox-checkpoint 1"
/* A settled checkpoint is written at most this often; the in-memory value
 * (used by a same-process reopen) advances every time. */
#define CHECKPOINT_WRITE_INTERVAL 60
#define MAX_SCOPE_URLS 16

enum { ENDPOINT_PENDING, ENDPOINT_EOSE, ENDPOINT_FAILED };
/* Whether everything the relay holds in [since, now] has been fetched. */
enum { BACKFILL_UNKNOWN, BACKFILL_PAGING, BACKFILL_COMPLETE, BACKFILL_INCOMPLETE };

typedef struct {
  GhDmInbox *inbox;       /* owner; an endpoint dies with its session */
  gchar *url;
  /* The live REQ {since, limit} on its own scope, so that what it delivers
   * is this relay's alone (a shared scope dedups across relays). */
  GhRelayScope *live;
  guint status;           /* ENDPOINT_* of the live REQ */
  gboolean answered;      /* sent EOSE or a backfill event */
  gchar *detail;          /* the relay's reason once FAILED */
  gboolean auth_required; /* FAILED with "auth-required:" */
  guint live_events;      /* distinct events the live REQ delivered */
  guint round_events;     /* its backfill events on the current connection */
  gint64 round_oldest;    /* their oldest created_at */
  gboolean check_pending; /* EOSE seen, completeness not judged yet */
  guint backfill;         /* BACKFILL_* */
  gboolean rerun;         /* judged again while paging: page again after */
  gint64 rerun_until;
  /* The older-page REQ {since, until, limit} in flight, on a fresh scope. */
  GhRelayScope *page;
  gint64 page_until;
  guint page_events;
  gint64 page_oldest;
  gboolean page_ended;    /* EOSE or failure seen; settled by the idle */
  gboolean page_failed;
  guint run_pages;        /* pages of the current paging run */
  gboolean run_skipped;   /* the run stepped over an unpageable second */
  GSource *settle;        /* low-priority idle that judges EOSEs */
} Endpoint;

typedef struct {
  gchar *wrap_json;
  gchar *wrap_id;
  gchar *relay_url;
} Job;

typedef struct {
  GWeakRef inbox;
  guint64 session;
  Job *job;
} UnwrapCall;

struct _GhDmInbox {
  GObject parent_instance;
  GhAccountController *accounts; /* NULL once disposed */
  GhAccountRelays *relays;
  GhConversationStore *store;
  gchar *state_dir;
  GhRelayTransport transport;
  GhRelayAuthTransport auth_transport;
  gpointer transport_data;
  gboolean custom_transport;
  gboolean custom_auth;
  guint max_in_flight;
  guint req_limit;
  guint max_pages;
  /* Storage mode (gh_dm_inbox_new_with_storage): no files of its own; runs
   * only with a grant for the current generation. */
  gboolean storage_mode;
  gboolean storage_granted;
  GhDmInboxStorage storage;
  gpointer storage_data;

  /* Account binding: one per account generation. */
  guint64 generation;
  gchar *pubkey_hex;
  gchar *checkpoint_path;
  gint64 checkpoint;         /* 0: none */
  gint64 checkpoint_written; /* last value written this generation */
  gchar *error;
  GhDmInboxCounters counters;
  GhAccountAuth *auth;       /* NULL: no account AUTH this generation */
  gulong auth_handler;

  /* Session: one live REQ per relay of one relay set. Bumped on teardown. */
  guint64 session;
  GStrv urls;
  gint64 since;
  guint limit;               /* the REQ limit of this session */
  GHashTable *endpoints;     /* url -> Endpoint */
  GCancellable *cancellable;
  GQueue queue;              /* Job, oldest first */
  GHashTable *pending_ids;   /* wrap ids queued or in flight */
  GHashTable *deferred_ids;  /* wrap ids a signer error deferred this session */
  gboolean hold_checkpoint;

  GhDmInboxState state;
};

enum { PROP_0, PROP_STATE, N_PROPS };
static GParamSpec *props[N_PROPS];
enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhDmInbox, gh_dm_inbox, G_TYPE_OBJECT)

GType
gh_dm_inbox_state_get_type(void)
{
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GH_DM_INBOX_INACTIVE, "GH_DM_INBOX_INACTIVE", "inactive" },
      { GH_DM_INBOX_NO_INBOX_RELAYS, "GH_DM_INBOX_NO_INBOX_RELAYS", "no-inbox-relays" },
      { GH_DM_INBOX_CONNECTING, "GH_DM_INBOX_CONNECTING", "connecting" },
      { GH_DM_INBOX_BACKFILLING, "GH_DM_INBOX_BACKFILLING", "backfilling" },
      { GH_DM_INBOX_LIVE, "GH_DM_INBOX_LIVE", "live" },
      { GH_DM_INBOX_ERROR, "GH_DM_INBOX_ERROR", "error" },
      { GH_DM_INBOX_NO_STORAGE, "GH_DM_INBOX_NO_STORAGE", "no-storage" },
      { 0, NULL, NULL }
    };
    g_once_init_leave(&type, g_enum_register_static(
      g_intern_static_string("GhDmInboxState"), values));
  }
  return type;
}

static void
job_free(gpointer data)
{
  Job *job = data;
  g_free(job->wrap_json);
  g_free(job->wrap_id);
  g_free(job->relay_url);
  g_free(job);
}

static void
unwrap_call_free(UnwrapCall *call)
{
  g_weak_ref_clear(&call->inbox);
  job_free(call->job);
  g_free(call);
}

static gint64
now_seconds(void)
{
  return g_get_real_time() / G_USEC_PER_SEC;
}

static void
emit_changed(GhDmInbox *self)
{
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

static gboolean
current(GhDmInbox *self)
{
  return self->accounts && gh_account_controller_is_current(self->accounts, self->generation);
}

/* ---- state ----------------------------------------------------------------- */

static GhDmInboxState
compute_state(GhDmInbox *self)
{
  if (!self->generation)
    return GH_DM_INBOX_INACTIVE;
  if (self->error)
    return GH_DM_INBOX_ERROR;
  if (self->storage_mode && !self->storage_granted)
    return GH_DM_INBOX_NO_STORAGE;
  if (!self->urls)
    return GH_DM_INBOX_NO_INBOX_RELAYS;
  gboolean pending = FALSE, answered = FALSE, eose = FALSE;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->endpoints);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Endpoint *endpoint = value;
    pending |= endpoint->status == ENDPOINT_PENDING || endpoint->check_pending ||
               endpoint->backfill == BACKFILL_PAGING;
    eose |= endpoint->status == ENDPOINT_EOSE;
    answered |= endpoint->answered;
  }
  if (pending)
    return answered ? GH_DM_INBOX_BACKFILLING : GH_DM_INBOX_CONNECTING;
  return eose ? GH_DM_INBOX_LIVE : GH_DM_INBOX_ERROR;
}

static void
update_state(GhDmInbox *self)
{
  GhDmInboxState state = compute_state(self);
  if (state == self->state)
    return;
  self->state = state;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATE]);
}

/* ---- checkpoint ------------------------------------------------------------ */

static gint64
checkpoint_load(const gchar *path, const gchar *pubkey_hex)
{
  g_autofree gchar *contents = NULL;
  gsize length = 0;
  if (!g_file_get_contents(path, &contents, &length, NULL) || length > 256)
    return 0;
  g_autofree gchar *prefix = g_strdup_printf(CHECKPOINT_MAGIC " %s ", pubkey_hex);
  if (!g_str_has_prefix(contents, prefix) || !g_str_has_suffix(contents, "\n"))
    return 0;
  g_autofree gchar *number = g_strndup(contents + strlen(prefix),
                                       length - strlen(prefix) - 1);
  gint64 value = 0;
  /* A foreign or malformed file only widens the window: start without one. */
  if (!g_ascii_string_to_signed(number, 10, 1, G_MAXINT64, &value, NULL))
    return 0;
  return value;
}

static void
checkpoint_write(GhDmInbox *self)
{
  if (self->storage_mode) {
    g_autoptr(GError) error = NULL;
    if (!self->storage_granted)
      return;
    if (!self->storage.save_checkpoint(self->storage_data, self->checkpoint, &error)) {
      g_message("Groundhog could not record the DM inbox checkpoint: %s", error->message);
      return;
    }
    self->checkpoint_written = self->checkpoint;
    return;
  }
  g_autofree gchar *contents = g_strdup_printf(CHECKPOINT_MAGIC " %s %" G_GINT64_FORMAT "\n",
                                               self->pubkey_hex, self->checkpoint);
  g_autoptr(GError) error = NULL;
  if (!g_file_set_contents_full(self->checkpoint_path, contents, -1,
                                G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error)) {
    g_message("Groundhog could not record the DM inbox checkpoint: %s", error->message);
    return;
  }
  self->checkpoint_written = self->checkpoint;
}

/* Advances the checkpoint only once everything any inbox relay holds has
 * been fetched and settled: every endpoint at EOSE and fully paged, nothing
 * queued or in flight, and no wrap deferred during this session. */
static void
maybe_advance_checkpoint(GhDmInbox *self)
{
  if (self->state != GH_DM_INBOX_LIVE || self->hold_checkpoint ||
      self->counters.pending != 0)
    return;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->endpoints);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Endpoint *endpoint = value;
    if (endpoint->status != ENDPOINT_EOSE || endpoint->check_pending ||
        endpoint->backfill != BACKFILL_COMPLETE)
      return;
  }
  gint64 now = now_seconds();
  if (now <= self->checkpoint)
    return;
  self->checkpoint = now;
  if (self->checkpoint - self->checkpoint_written >= CHECKPOINT_WRITE_INTERVAL)
    checkpoint_write(self);
}

static void
refresh(GhDmInbox *self)
{
  update_state(self);
  maybe_advance_checkpoint(self);
  emit_changed(self);
}

/* ---- unwrap queue ---------------------------------------------------------- */

static void unwrap_done(GObject *source, GAsyncResult *result, gpointer data);

static void
pump(GhDmInbox *self)
{
  while (self->counters.in_flight < self->max_in_flight && !g_queue_is_empty(&self->queue)) {
    Job *job = g_queue_pop_head(&self->queue);
    UnwrapCall *call = g_new0(UnwrapCall, 1);
    g_weak_ref_init(&call->inbox, self);
    call->session = self->session;
    call->job = job;
    self->counters.in_flight++;
    gh_nip17_unwrap_async(self->accounts, job->wrap_json, self->cancellable,
                          unwrap_done, call);
  }
}

/* The default persistence delegate: messages live in the store's memory only
 * and the per-account GhNip17Seen file (the delegate data, owned by the
 * store) holds the seen keys and the rejected namespace. The encrypted
 * store's delegate (gh-store-conversations.h) replaces this table with its
 * T-admit transaction and imports the file. */
static gboolean
seen_has_wrap(gpointer data, const gchar *wrap_id)
{
  return gh_nip17_seen_has_wrap(data, wrap_id);
}

static gboolean
seen_has_rumor(gpointer data, const gchar *rumor_id)
{
  return gh_nip17_seen_has_rumor(data, rumor_id);
}

static gboolean
seen_has_rejected(gpointer data, const gchar *wrap_id)
{
  return gh_nip17_seen_has_rejected(data, wrap_id);
}

static gboolean
seen_add_rejected(gpointer data, const gchar *wrap_id, GError **error)
{
  return gh_nip17_seen_record_rejected(data, wrap_id, error);
}

static gboolean
seen_admit(gpointer data, GhMessage *message, const gchar *wrap_id,
           GhConversationCommit *commit, GError **error)
{
  (void)commit;
  (void)error;
  if (!wrap_id)
    return TRUE; /* a local echo; its self-copy wrap is recorded on arrival */
  GhNip17Message record = {
    .account_pubkey = (gchar *)gh_message_get_account(message),
    .wrap_id = (gchar *)wrap_id,
    .rumor_id = (gchar *)gh_message_get_rumor_id(message),
  };
  g_autoptr(GError) local = NULL;
  /* The message itself is only in memory, so a failed append must not hide
   * it: GhNip17Seen keeps the keys for this process, and a restart (which
   * loses the in-memory message anyway) fetches the wrap again. */
  if (!gh_nip17_seen_record(data, &record, &local))
    g_warning("Groundhog could not record NIP-17 wrap %s as seen: %s", wrap_id,
              local->message);
  return TRUE;
}

static const GhConversationDelegate seen_delegate = {
  .has_wrap = seen_has_wrap,
  .has_rumor = seen_has_rumor,
  .admit = seen_admit,
  .has_rejected = seen_has_rejected,
  .add_rejected = seen_add_rejected,
};

/* A final verdict reached after a signer call: no later session asks the
 * signer about this wrap again (the delegate's rejected namespace). */
static void
record_rejected(GhDmInbox *self, const gchar *wrap_id)
{
  g_autoptr(GError) error = NULL;
  if (!gh_conversation_store_record_rejected(self->store, wrap_id, &error))
    g_warning("Groundhog could not record rejected NIP-17 wrap %s: %s", wrap_id,
              error ? error->message : "unknown error");
}

/* One admission call: the store commits through its delegate (message and
 * seen keys together) before its model changes. */
static void
admit(GhDmInbox *self, const GhNip17Message *message, const gchar *relay_url)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) parsed =
    gh_message_new_from_rumor(self->pubkey_hex, message->rumor_json, &error);
  GhConversationAddResult result = GH_CONVERSATION_ADD_REJECTED;
  if (parsed) {
    gh_message_add_relay(parsed, relay_url);
    /* The seal's or the wrap's expiration when the rumor has none (§3.7). */
    gh_message_set_expires_at(parsed, message->expires_at);
    result = gh_conversation_store_admit(self->store, parsed, message->wrap_id, &error);
  }
  switch (result) {
  case GH_CONVERSATION_ADD_NEW:
    self->counters.admitted++;
    break;
  case GH_CONVERSATION_ADD_DUPLICATE:
  case GH_CONVERSATION_ADD_HIDDEN:
    /* Another relay copy or a re-wrap; or recorded as seen only (expired on
     * arrival, forgotten room) and never shown. */
    self->counters.duplicates++;
    break;
  case GH_CONVERSATION_ADD_FAILED:
    /* Not committed, so not seen: a later session retries it. */
    g_message("Groundhog could not store NIP-17 message %s: %s", message->rumor_id,
              error->message);
    self->counters.deferred++;
    self->hold_checkpoint = TRUE;
    break;
  case GH_CONVERSATION_ADD_REJECTED:
  default:
    /* The unwrap already validated it; this only guards the invariant. The
     * verdict still followed signer calls, so it is recorded like one. */
    g_warning("Groundhog refused NIP-17 message %s: %s", message->rumor_id,
              error ? error->message : "unknown error");
    self->counters.rejected++;
    record_rejected(self, message->wrap_id);
    break;
  }
}

static void
unwrap_done(GObject *source, GAsyncResult *result, gpointer data)
{
  UnwrapCall *call = data;
  (void)source;
  g_autoptr(GError) error = NULL;
  GhNip17Message *message = gh_nip17_unwrap_finish(result, &error);
  GhDmInbox *self = g_weak_ref_get(&call->inbox);
  /* The session this unwrap belonged to is gone (switch, relay change or
   * dispose): its in-flight slot and verdict died with it. */
  if (!self || !self->accounts || call->session != self->session) {
    gh_nip17_message_free(message);
    unwrap_call_free(call);
    g_clear_object(&self);
    return;
  }
  self->counters.in_flight--;
  self->counters.pending--;
  g_hash_table_remove(self->pending_ids, call->job->wrap_id);
  if (message) {
    admit(self, message, call->job->relay_url);
  } else if (error->domain == GH_NIP17_INBOX_ERROR) {
    self->counters.rejected++;
    /* Rejected before any signer call, it costs nothing to reject again; a
     * verdict that cost an approval is recorded (the id is the verified
     * hash of the wrap the unwrap checked). */
    if (gh_nip17_unwrap_get_signer_calls(result) > 0)
      record_rejected(self, call->job->wrap_id);
  } else {
    /* Signer denial or failure: not recorded, so a later session offers it
     * again from a checkpoint held before it. Not asked again this session,
     * whichever relay or page delivers it next. */
    g_debug("Groundhog deferred NIP-17 wrap %s: %s", call->job->wrap_id, error->message);
    self->counters.deferred++;
    self->hold_checkpoint = TRUE;
    g_hash_table_add(self->deferred_ids, g_strdup(call->job->wrap_id));
  }
  gh_nip17_message_free(message);
  unwrap_call_free(call);
  pump(self);
  maybe_advance_checkpoint(self);
  emit_changed(self);
  g_object_unref(self);
}

static void
handle_wrap(GhDmInbox *self, const GhRelayUpdate *update)
{
  self->counters.received++;
  /* The id is the scope's verified one. Skip before any signer call. */
  if (gh_conversation_store_has_wrap(self->store, update->event_id) ||
      gh_conversation_store_has_rejected(self->store, update->event_id) ||
      g_hash_table_contains(self->pending_ids, update->event_id) ||
      g_hash_table_contains(self->deferred_ids, update->event_id)) {
    self->counters.skipped++;
    return;
  }
  if (self->counters.pending >= GH_DM_INBOX_QUEUE_LIMIT) {
    self->counters.deferred++;
    self->hold_checkpoint = TRUE;
    return;
  }
  Job *job = g_new0(Job, 1);
  job->wrap_json = g_strdup(update->event_json);
  job->wrap_id = g_strdup(update->event_id);
  job->relay_url = g_strdup(update->url);
  g_hash_table_add(self->pending_ids, g_strdup(job->wrap_id));
  g_queue_push_tail(&self->queue, job);
  self->counters.pending++;
  pump(self);
}

/* ---- relay scopes ---------------------------------------------------------- */

static NostrFilters *
inbox_filters(const gchar *pubkey_hex, gint64 since, gint64 until, guint limit)
{
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  if (!filters || !filter) {
    if (filters)
      nostr_filters_free(filters);
    if (filter)
      nostr_filter_free(filter);
    return NULL;
  }
  const int kinds[] = { KIND_GIFT_WRAP };
  nostr_filter_set_kinds(filter, kinds, G_N_ELEMENTS(kinds));
  nostr_filter_tags_append(filter, "p", pubkey_hex, NULL);
  nostr_filter_set_since_i64(filter, since);
  if (until)
    nostr_filter_set_until_i64(filter, until);
  nostr_filter_set_limit(filter, (int)limit);
  gboolean added = nostr_filters_add(filters, filter);
  nostr_filter_free(filter); /* contents moved into the vector */
  if (!added) {
    nostr_filters_free(filters);
    return NULL;
  }
  return filters;
}

/* A scope for exactly endpoint's URL, not started. It authenticates, on
 * challenge, as the account and only on that URL: an own inbox relay
 * (charter §4.3). */
static GhRelayScope *
new_scope(GhDmInbox *self, Endpoint *endpoint, gint64 until, GhRelayScopeFunc callback)
{
  NostrFilters *filters = inbox_filters(self->pubkey_hex, self->since, until, self->limit);
  if (!filters)
    return NULL;
  GhRelayScope *scope = self->custom_transport
    ? gh_relay_scope_new_with_transport(self->generation, filters, &self->transport,
                                        self->transport_data, callback, endpoint)
    : gh_relay_scope_new(self->generation, filters, callback, endpoint);
  if (self->custom_transport && self->custom_auth)
    gh_relay_scope_set_auth_transport(scope, &self->auth_transport);
  g_autoptr(GError) error = NULL;
  if (!gh_relay_scope_add_url(scope, endpoint->url, &error)) {
    g_message("Groundhog ignores DM inbox relay \"%s\": %s", endpoint->url, error->message);
    gh_relay_scope_unref(scope);
    return NULL;
  }
  if (self->auth &&
      (!gh_relay_scope_set_account_signer(scope, gh_account_auth_get_signer(self->auth), &error) ||
       !gh_relay_scope_set_url_auth(scope, endpoint->url, GH_RELAY_AUTH_ACCOUNT, &error)))
    g_message("Groundhog will not sign in to DM inbox relay \"%s\": %s", endpoint->url,
              error->message);
  return scope;
}

static void
scope_close(GhRelayScope **scope)
{
  if (!*scope)
    return;
  gh_relay_scope_cancel(*scope);
  g_clear_pointer(scope, gh_relay_scope_unref);
}

static gint64
event_created_at(const gchar *event_json)
{
  NostrEvent *event = nostr_event_new();
  gint64 created_at = G_MAXINT64;
  if (event && nostr_event_deserialize_compact(event, event_json, NULL) == 1)
    created_at = nostr_event_get_created_at(event);
  if (event)
    nostr_event_free(event);
  return created_at;
}

static gboolean settle_now(gpointer data);

/* EOSEs are judged from a low-priority idle: the gnostr transport dispatches
 * EOSE at default priority but stored EVENTs from a default-idle queue, so an
 * EOSE can overtake events received before it. Those are counted first; a
 * cross-thread window remains until the transport orders them
 * (nostrc-qp24.10.6). */
static void
schedule_settle(Endpoint *endpoint)
{
  if (endpoint->settle)
    return;
  endpoint->settle = g_idle_source_new();
  g_source_set_priority(endpoint->settle, G_PRIORITY_LOW);
  g_source_set_callback(endpoint->settle, settle_now, endpoint, NULL);
  g_source_attach(endpoint->settle, g_main_context_get_thread_default());
}

static void
set_failed(Endpoint *endpoint, const gchar *detail, gboolean auth_required)
{
  endpoint->status = ENDPOINT_FAILED;
  g_free(endpoint->detail);
  endpoint->detail = g_strdup(detail);
  endpoint->auth_required = auth_required;
}

static void start_page(GhDmInbox *self, Endpoint *endpoint, gint64 until);

static void
start_run(GhDmInbox *self, Endpoint *endpoint, gint64 until)
{
  endpoint->backfill = BACKFILL_PAGING;
  endpoint->rerun = FALSE;
  endpoint->run_pages = 0;
  endpoint->run_skipped = FALSE;
  start_page(self, endpoint, until);
}

static void
end_run(GhDmInbox *self, Endpoint *endpoint, gboolean complete)
{
  if (endpoint->rerun) {
    start_run(self, endpoint, endpoint->rerun_until);
    return;
  }
  complete = complete && !endpoint->run_skipped;
  endpoint->backfill = complete ? BACKFILL_COMPLETE : BACKFILL_INCOMPLETE;
  if (!complete) {
    self->counters.backfill_incomplete++;
    g_message("Groundhog could not fetch every older DM from %s; the inbox checkpoint "
              "stays where it was", endpoint->url);
  }
}

static void on_page_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data);

static void
start_page(GhDmInbox *self, Endpoint *endpoint, gint64 until)
{
  if (endpoint->run_pages >= self->max_pages) {
    end_run(self, endpoint, FALSE);
    return;
  }
  GhRelayScope *scope = new_scope(self, endpoint, until, on_page_update);
  if (!scope) {
    end_run(self, endpoint, FALSE);
    return;
  }
  endpoint->run_pages++;
  self->counters.pages++;
  endpoint->page_until = until;
  endpoint->page_events = 0;
  endpoint->page_oldest = G_MAXINT64;
  endpoint->page_ended = FALSE;
  endpoint->page_failed = FALSE;
  endpoint->page = scope;
  gh_relay_scope_start(scope);
}

static void
finish_page(GhDmInbox *self, Endpoint *endpoint)
{
  gboolean failed = endpoint->page_failed;
  guint events = endpoint->page_events;
  gint64 oldest = endpoint->page_oldest;
  gint64 until = endpoint->page_until;
  scope_close(&endpoint->page);
  if (failed) {
    end_run(self, endpoint, FALSE);
    return;
  }
  if (events < self->limit) {
    end_run(self, endpoint, TRUE); /* nothing older remains */
    return;
  }
  /* A full page: older wraps may remain. until is inclusive, so the next page
   * repeats the boundary second and its repeats are skipped by id. A full
   * page that got no older holds more than a page in that one second: step
   * over it, and the relay stays incomplete. */
  gint64 next = oldest;
  if (oldest >= until) {
    endpoint->run_skipped = TRUE;
    next = until - 1;
  }
  if (next < self->since) {
    end_run(self, endpoint, TRUE);
    return;
  }
  start_page(self, endpoint, next);
}

/* The live REQ reached EOSE: the relay answered with its newest limit wraps
 * in [since, now]. If it has delivered fewer distinct wraps than that during
 * this whole session, it holds fewer, and its window is complete. Otherwise
 * older ones may be cut off; every backfill event of this connection is among
 * the newest, so paging from the oldest of them misses nothing. */
static void
judge_live(GhDmInbox *self, Endpoint *endpoint)
{
  if (endpoint->live_events < self->limit) {
    endpoint->backfill = BACKFILL_COMPLETE;
    return;
  }
  gint64 until = endpoint->round_events ? endpoint->round_oldest : now_seconds();
  if (endpoint->backfill == BACKFILL_PAGING) {
    endpoint->rerun = TRUE;
    endpoint->rerun_until = until;
    return;
  }
  start_run(self, endpoint, until);
}

static gboolean
settle_now(gpointer data)
{
  Endpoint *endpoint = data;
  GhDmInbox *self = endpoint->inbox;
  g_clear_pointer(&endpoint->settle, g_source_unref);
  if (!current(self))
    return G_SOURCE_REMOVE;
  if (endpoint->page && endpoint->page_ended)
    finish_page(self, endpoint);
  if (endpoint->check_pending && endpoint->status == ENDPOINT_EOSE) {
    endpoint->check_pending = FALSE;
    judge_live(self, endpoint);
  }
  refresh(self);
  return G_SOURCE_REMOVE;
}

static void
on_live_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  Endpoint *endpoint = data;
  GhDmInbox *self = endpoint->inbox;
  /* The controller revokes a generation before its "changed" reaches us;
   * anything delivered in that window belongs to the previous account. */
  if (scope != endpoint->live || !current(self))
    return;
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT:
    endpoint->live_events++;
    if (update->backfill) {
      endpoint->answered = TRUE;
      endpoint->round_events++;
      endpoint->round_oldest = MIN(endpoint->round_oldest, event_created_at(update->event_json));
    }
    handle_wrap(self, update);
    break;
  case GH_RELAY_NOTICE_EOSE:
    endpoint->status = ENDPOINT_EOSE;
    endpoint->answered = TRUE;
    g_clear_pointer(&endpoint->detail, g_free);
    endpoint->auth_required = FALSE;
    endpoint->check_pending = TRUE;
    schedule_settle(endpoint);
    break;
  case GH_RELAY_NOTICE_ERROR:
    /* A relay that already answered stays answered while it redials. */
    if (endpoint->status == ENDPOINT_PENDING)
      set_failed(endpoint, update->detail, FALSE);
    break;
  case GH_RELAY_NOTICE_CLOSED:
    /* With AUTH, the scope reports an "auth-required:" CLOSED only once
     * signing in was refused, failed, or the relay refused the retry. */
    set_failed(endpoint, update->detail, gh_relay_auth_is_required(update->detail));
    break;
  case GH_RELAY_NOTICE_DISCONNECTED:
    /* The REQ is re-issued on reconnect and must reach EOSE again. */
    endpoint->status = ENDPOINT_PENDING;
    endpoint->answered = FALSE;
    g_clear_pointer(&endpoint->detail, g_free);
    endpoint->auth_required = FALSE;
    endpoint->round_events = 0;
    endpoint->round_oldest = G_MAXINT64;
    endpoint->check_pending = FALSE;
    break;
  default:
    return;
  }
  refresh(self);
}

static void
on_page_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  Endpoint *endpoint = data;
  GhDmInbox *self = endpoint->inbox;
  if (scope != endpoint->page || endpoint->page_failed || !current(self))
    return;
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT:
    /* Counted even after EOSE: the idle has not settled the page yet. */
    endpoint->page_events++;
    endpoint->page_oldest = MIN(endpoint->page_oldest, event_created_at(update->event_json));
    handle_wrap(self, update);
    break;
  case GH_RELAY_NOTICE_EOSE:
    endpoint->page_ended = TRUE;
    schedule_settle(endpoint);
    break;
  case GH_RELAY_NOTICE_ERROR:
  case GH_RELAY_NOTICE_CLOSED:
  case GH_RELAY_NOTICE_DISCONNECTED:
    if (endpoint->page_ended)
      return;
    g_message("Groundhog could not fetch older DMs from %s: %s", endpoint->url,
              update->detail ? update->detail : "connection lost");
    endpoint->page_ended = TRUE;
    endpoint->page_failed = TRUE;
    schedule_settle(endpoint);
    break;
  default:
    return;
  }
  refresh(self);
}

static Endpoint *
endpoint_new(GhDmInbox *self, const gchar *url)
{
  Endpoint *endpoint = g_new0(Endpoint, 1);
  endpoint->inbox = self;
  endpoint->url = g_strdup(url);
  endpoint->round_oldest = G_MAXINT64;
  return endpoint;
}

static void
endpoint_free(gpointer data)
{
  Endpoint *endpoint = data;
  if (endpoint->settle) {
    g_source_destroy(endpoint->settle);
    g_clear_pointer(&endpoint->settle, g_source_unref);
  }
  scope_close(&endpoint->page);
  scope_close(&endpoint->live);
  g_free(endpoint->detail);
  g_free(endpoint->url);
  g_free(endpoint);
}

static gint64
session_since(GhDmInbox *self)
{
  gint64 now = now_seconds();
  if (self->checkpoint > 0)
    return MIN(self->checkpoint, now) - GH_DM_INBOX_WRAP_SKEW;
  return now - GH_DM_INBOX_INITIAL_BACKFILL;
}

/* Cancel first: the scopes revoke their generation and close every
 * transport, and the cancellable revokes pending signer approvals, before
 * anything for the next session exists. Late unwrap callbacks see a newer
 * session. */
static void
teardown_session(GhDmInbox *self)
{
  self->session++;
  g_hash_table_remove_all(self->endpoints);
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  g_queue_clear_full(&self->queue, job_free);
  g_hash_table_remove_all(self->pending_ids);
  g_hash_table_remove_all(self->deferred_ids);
  self->counters.pending = 0;
  self->counters.in_flight = 0;
  self->hold_checkpoint = FALSE;
  self->since = 0;
  g_clear_pointer(&self->urls, g_strfreev);
}

static void
start_session(GhDmInbox *self, GStrv urls)
{
  self->urls = urls;
  self->since = session_since(self);
  self->limit = self->req_limit;
  self->cancellable = g_cancellable_new();
  /* Every endpoint and its scope exist before any scope starts, so a
   * synchronous open failure lands on its own endpoint. */
  for (guint i = 0; urls[i]; i++) {
    Endpoint *endpoint = endpoint_new(self, urls[i]);
    endpoint->live = new_scope(self, endpoint, 0, on_live_update);
    if (endpoint->live)
      g_hash_table_insert(self->endpoints, endpoint->url, endpoint);
    else
      endpoint_free(endpoint);
  }
  for (guint i = 0; urls[i]; i++) {
    Endpoint *endpoint = g_hash_table_lookup(self->endpoints, urls[i]);
    if (endpoint && endpoint->live)
      gh_relay_scope_start(endpoint->live);
  }
}

static gint
compare_urls(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* The account's own kind-10050 relays for this generation: valid ws(s) URLs,
 * unique and sorted (so a reordered list is not a change); NULL if none. */
static GStrv
wanted_urls(GhDmInbox *self)
{
  if (gh_account_relays_get_generation(self->relays) != self->generation)
    return NULL;
  const gchar *const *inbox = gh_account_relays_get_inbox_relays(self->relays);
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; inbox && inbox[i]; i++) {
    g_autoptr(GError) error = NULL;
    gboolean duplicate = FALSE;
    for (guint j = 0; j < urls->len && !duplicate; j++)
      duplicate = g_str_equal(g_ptr_array_index(urls, j), inbox[i]);
    if (duplicate)
      continue;
    if (!gh_relay_url_validate(inbox[i], &error)) {
      g_message("Groundhog ignores DM inbox relay \"%s\": %s", inbox[i], error->message);
      continue;
    }
    if (urls->len == MAX_SCOPE_URLS) {
      g_message("Groundhog uses only the first %d DM inbox relays", MAX_SCOPE_URLS);
      break;
    }
    g_ptr_array_add(urls, g_strdup(inbox[i]));
  }
  if (urls->len == 0)
    return NULL;
  g_ptr_array_sort(urls, compare_urls);
  g_ptr_array_add(urls, NULL);
  return (GStrv)g_ptr_array_free(g_steal_pointer(&urls), FALSE);
}

/* ---- account binding ------------------------------------------------------- */

static void
on_auth_changed(GhDmInbox *self, const gchar *url)
{
  if (g_hash_table_contains(self->endpoints, url))
    emit_changed(self); /* a relay state (waiting for approval, declined) */
}

/* Saves the latest checkpoint through the grant, tears the session down and
 * drops the grant (storage mode). */
static void
withdraw_storage(GhDmInbox *self)
{
  if (!self->storage_granted)
    return;
  if (self->checkpoint > self->checkpoint_written)
    checkpoint_write(self);
  teardown_session(self);
  self->storage_granted = FALSE;
  self->storage_data = NULL;
  self->checkpoint = 0;
  self->checkpoint_written = 0;
}

static void
teardown_account(GhDmInbox *self)
{
  withdraw_storage(self);
  teardown_session(self);
  if (self->auth) {
    g_clear_signal_handler(&self->auth_handler, self->auth);
    gh_account_auth_revoke(self->auth);
    g_clear_object(&self->auth);
  }
  g_clear_pointer(&self->pubkey_hex, g_free);
  g_clear_pointer(&self->checkpoint_path, g_free);
  g_clear_pointer(&self->error, g_free);
  self->checkpoint = 0;
  self->checkpoint_written = 0;
  self->generation = 0;
  memset(&self->counters, 0, sizeof self->counters);
}

static void
bind_account(GhDmInbox *self, guint64 generation, const gchar *npub)
{
  self->generation = generation;
  self->pubkey_hex = gh_identity_pubkey_hex(npub);
  if (self->storage_mode) {
    /* Nothing is opened or bound here: the account store grants storage
     * once the account's store is open (gh_dm_inbox_set_storage). */
    if (!self->pubkey_hex) {
      self->error = g_strdup("The active account has no usable public key");
      return;
    }
    self->auth = gh_account_auth_new(self->accounts);
    if (self->auth)
      self->auth_handler = g_signal_connect_swapped(self->auth, "relay-changed",
                                                    G_CALLBACK(on_auth_changed), self);
    return;
  }
  GhNip17Seen *seen = NULL;
  g_autoptr(GError) error = NULL;
  if (!self->pubkey_hex) {
    self->error = g_strdup("The active account has no usable public key");
  } else if (g_mkdir_with_parents(self->state_dir, 0700) != 0) {
    self->error = g_strdup_printf("Could not create %s: %s", self->state_dir,
                                  g_strerror(errno));
  } else {
    g_autofree gchar *seen_name = g_strconcat(self->pubkey_hex, ".seen", NULL);
    g_autofree gchar *seen_path = g_build_filename(self->state_dir, seen_name, NULL);
    seen = gh_nip17_seen_open(seen_path, self->pubkey_hex, GH_DM_INBOX_SEEN_CAPACITY, &error);
    /* Fail closed: without a trustworthy seen-set every replayed wrap would
     * be a fresh signer prompt. */
    if (!seen)
      self->error = g_strdup_printf("The DM seen-set is unusable: %s", error->message);
  }
  /* The store drops the previous account's rooms before this one's appear
   * and owns the seen-set from here on; the inbox reaches it (including the
   * rejected namespace) through the store. */
  if (seen) {
    gh_conversation_store_set_account(self->store, self->pubkey_hex, &seen_delegate,
                                      seen, (GDestroyNotify)gh_nip17_seen_free);
  } else {
    gh_conversation_store_set_account(self->store, self->pubkey_hex, NULL, NULL, NULL);
    return;
  }
  g_autofree gchar *checkpoint_name = g_strconcat(self->pubkey_hex, ".checkpoint", NULL);
  self->checkpoint_path = g_build_filename(self->state_dir, checkpoint_name, NULL);
  self->checkpoint = checkpoint_load(self->checkpoint_path, self->pubkey_hex);
  self->checkpoint_written = self->checkpoint;
  /* Own inbox relays may ask for AUTH as the account (charter §4.3). */
  self->auth = gh_account_auth_new(self->accounts);
  if (self->auth)
    self->auth_handler = g_signal_connect_swapped(self->auth, "relay-changed",
                                                  G_CALLBACK(on_auth_changed), self);
}

static gboolean
strv_equal0(const gchar *const *a, const gchar *const *b)
{
  if (!a || !b)
    return a == b;
  return g_strv_equal(a, b);
}

/* Reconciles with the account controller and the relay lists. Both emit
 * "changed" on the main context in either order during a switch; the account
 * generation is compared first so no relay list of another account is used. */
static void
sync_binding(GhDmInbox *self)
{
  if (!self->accounts)
    return;
  guint64 generation = 0;
  const gchar *npub = NULL;
  if (gh_account_controller_get_state(self->accounts) == GH_ACCOUNT_STATE_ACTIVE) {
    generation = gh_account_controller_get_generation(self->accounts);
    npub = gh_account_controller_get_active_npub(self->accounts);
  }
  gboolean account_changed = generation != self->generation;
  if (account_changed) {
    teardown_account(self);
    if (generation)
      bind_account(self, generation, npub);
    else if (!self->storage_mode)
      gh_conversation_store_set_account(self->store, NULL, NULL, NULL, NULL);
  }
  if (self->generation && !self->error &&
      (!self->storage_mode || self->storage_granted)) {
    g_auto(GStrv) urls = wanted_urls(self);
    if (!strv_equal0((const gchar *const *)urls, (const gchar *const *)self->urls)) {
      teardown_session(self);
      if (urls)
        start_session(self, g_steal_pointer(&urls));
      account_changed = TRUE; /* the session changed: always report it */
    }
  }
  GhDmInboxState before = self->state;
  update_state(self);
  maybe_advance_checkpoint(self);
  if (account_changed || before != self->state)
    emit_changed(self);
}

/* ---- public ---------------------------------------------------------------- */

static GhDmInbox *
inbox_new(GhAccountController *accounts, GhAccountRelays *relays,
          GhConversationStore *store, gboolean storage_mode, const gchar *state_dir,
          const GhRelayTransport *transport, const GhRelayAuthTransport *auth_transport,
          gpointer transport_data)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts), NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_RELAYS(relays), NULL);
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(store), NULL);
  g_return_val_if_fail(!transport || (transport->open && transport->close), NULL);
  g_return_val_if_fail(!auth_transport || (transport && auth_transport->send_auth &&
                                           auth_transport->resubscribe), NULL);
  GhDmInbox *self = g_object_new(GH_TYPE_DM_INBOX, NULL);
  self->accounts = g_object_ref(accounts);
  self->relays = g_object_ref(relays);
  self->store = g_object_ref(store);
  self->storage_mode = storage_mode;
  if (!storage_mode)
    self->state_dir = state_dir ? g_strdup(state_dir)
                                : g_build_filename(g_get_user_state_dir(), "groundhog",
                                                   "nip17", NULL);
  if (transport) {
    self->transport = *transport;
    self->transport_data = transport_data;
    self->custom_transport = TRUE;
  }
  if (auth_transport) {
    self->auth_transport = *auth_transport;
    self->custom_auth = TRUE;
  }
  g_signal_connect_object(accounts, "changed", G_CALLBACK(sync_binding), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(relays, "changed", G_CALLBACK(sync_binding), self,
                          G_CONNECT_SWAPPED);
  sync_binding(self);
  return self;
}

GhDmInbox *
gh_dm_inbox_new(GhAccountController *accounts, GhAccountRelays *relays,
                GhConversationStore *store, const gchar *state_dir,
                const GhRelayTransport *transport, const GhRelayAuthTransport *auth_transport,
                gpointer transport_data)
{
  return inbox_new(accounts, relays, store, FALSE, state_dir, transport, auth_transport,
                   transport_data);
}

GhDmInbox *
gh_dm_inbox_new_with_storage(GhAccountController *accounts, GhAccountRelays *relays,
                             GhConversationStore *store, const GhRelayTransport *transport,
                             const GhRelayAuthTransport *auth_transport,
                             gpointer transport_data)
{
  return inbox_new(accounts, relays, store, TRUE, NULL, transport, auth_transport,
                   transport_data);
}

gboolean
gh_dm_inbox_set_storage(GhDmInbox *self, guint64 generation, const GhDmInboxStorage *storage,
                        gpointer data)
{
  g_return_val_if_fail(GH_IS_DM_INBOX(self), FALSE);
  g_return_val_if_fail(storage && storage->load_checkpoint && storage->save_checkpoint, FALSE);
  if (!self->storage_mode || !self->accounts || generation == 0)
    return FALSE;
  /* Bind to the controller's generation first, whichever "changed" handler
   * ran first during a switch. */
  sync_binding(self);
  if (self->generation != generation || !current(self) || !self->pubkey_hex)
    return FALSE;
  if (self->storage_granted && self->storage_data != data)
    withdraw_storage(self);
  self->storage = *storage;
  self->storage_data = data;
  if (!self->storage_granted) {
    gint64 saved = storage->load_checkpoint(data);
    self->checkpoint = saved > 0 ? saved : 0;
    self->checkpoint_written = self->checkpoint;
    self->storage_granted = TRUE;
  }
  sync_binding(self);
  emit_changed(self);
  return TRUE;
}

void
gh_dm_inbox_clear_storage(GhDmInbox *self)
{
  g_return_if_fail(GH_IS_DM_INBOX(self));
  if (!self->storage_granted)
    return;
  withdraw_storage(self);
  update_state(self);
  emit_changed(self);
}

gboolean
gh_dm_inbox_has_storage(GhDmInbox *self)
{
  g_return_val_if_fail(GH_IS_DM_INBOX(self), FALSE);
  return self->storage_granted;
}

void
gh_dm_inbox_set_max_in_flight(GhDmInbox *self, guint max_in_flight)
{
  g_return_if_fail(GH_IS_DM_INBOX(self));
  self->max_in_flight = CLAMP(max_in_flight, 1, GH_DM_INBOX_MAX_IN_FLIGHT);
  if (self->accounts)
    pump(self);
}

void
gh_dm_inbox_set_backfill_limits(GhDmInbox *self, guint req_limit, guint max_pages)
{
  g_return_if_fail(GH_IS_DM_INBOX(self));
  self->req_limit = CLAMP(req_limit, 1, GH_DM_INBOX_REQ_LIMIT);
  self->max_pages = CLAMP(max_pages, 1, GH_DM_INBOX_MAX_PAGES);
}

GhDmInboxState
gh_dm_inbox_get_state(GhDmInbox *self)
{
  g_return_val_if_fail(GH_IS_DM_INBOX(self), GH_DM_INBOX_INACTIVE);
  return self->state;
}

const gchar *
gh_dm_inbox_get_error(GhDmInbox *self)
{
  g_return_val_if_fail(GH_IS_DM_INBOX(self), NULL);
  if (self->state != GH_DM_INBOX_ERROR)
    return NULL;
  if (self->error)
    return self->error;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->endpoints);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    if (((Endpoint *)value)->auth_required)
      return "Your inbox relays deliver messages only after you sign in, and signing in "
             "was declined or failed";
  return "No inbox relay accepted the DM subscription";
}

guint64
gh_dm_inbox_get_generation(GhDmInbox *self)
{
  g_return_val_if_fail(GH_IS_DM_INBOX(self), 0);
  return self->generation;
}

const gchar *const *
gh_dm_inbox_get_relays(GhDmInbox *self)
{
  g_return_val_if_fail(GH_IS_DM_INBOX(self), NULL);
  return (const gchar *const *)self->urls;
}

GhDmInboxRelayState
gh_dm_inbox_get_relay_state(GhDmInbox *self, const gchar *url, const gchar **detail)
{
  g_return_val_if_fail(GH_IS_DM_INBOX(self), GH_DM_INBOX_RELAY_NONE);
  if (detail)
    *detail = NULL;
  Endpoint *endpoint = url ? g_hash_table_lookup(self->endpoints, url) : NULL;
  if (!endpoint)
    return GH_DM_INBOX_RELAY_NONE;
  GhAccountAuthRelayState auth = self->auth ? gh_account_auth_get_relay_state(self->auth, url)
                                            : GH_ACCOUNT_AUTH_RELAY_NONE;
  if (endpoint->status == ENDPOINT_FAILED) {
    if (detail)
      *detail = endpoint->auth_required && auth == GH_ACCOUNT_AUTH_RELAY_DECLINED
        ? "This relay delivers your messages only after you sign in, and signing in "
          "was declined for this session"
        : endpoint->detail;
    return endpoint->auth_required ? GH_DM_INBOX_RELAY_AUTH_REQUIRED
                                   : GH_DM_INBOX_RELAY_FAILED;
  }
  if (auth == GH_ACCOUNT_AUTH_RELAY_WAITING &&
      (endpoint->status == ENDPOINT_PENDING || endpoint->backfill == BACKFILL_PAGING))
    return GH_DM_INBOX_RELAY_WAITING_FOR_APPROVAL;
  if (endpoint->status == ENDPOINT_PENDING)
    return endpoint->answered ? GH_DM_INBOX_RELAY_BACKFILLING : GH_DM_INBOX_RELAY_CONNECTING;
  if (endpoint->check_pending || endpoint->backfill == BACKFILL_PAGING)
    return GH_DM_INBOX_RELAY_BACKFILLING;
  if (endpoint->backfill == BACKFILL_INCOMPLETE) {
    if (detail)
      *detail = "Some older messages on this relay could not be fetched this session";
    return GH_DM_INBOX_RELAY_INCOMPLETE;
  }
  return GH_DM_INBOX_RELAY_LIVE;
}

gint64
gh_dm_inbox_get_since(GhDmInbox *self)
{
  g_return_val_if_fail(GH_IS_DM_INBOX(self), 0);
  return self->since;
}

void
gh_dm_inbox_get_counters(GhDmInbox *self, GhDmInboxCounters *counters)
{
  g_return_if_fail(GH_IS_DM_INBOX(self));
  g_return_if_fail(counters != NULL);
  *counters = self->counters;
}

static void
gh_dm_inbox_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhDmInbox *self = GH_DM_INBOX(object);
  switch (id) {
  case PROP_STATE:
    g_value_set_enum(value, self->state);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_dm_inbox_dispose(GObject *object)
{
  GhDmInbox *self = GH_DM_INBOX(object);
  if (self->accounts) {
    g_signal_handlers_disconnect_by_data(self->accounts, self);
    g_signal_handlers_disconnect_by_data(self->relays, self);
    teardown_account(self);
    self->state = GH_DM_INBOX_INACTIVE;
    g_clear_object(&self->accounts);
    g_clear_object(&self->relays);
    g_clear_object(&self->store);
  }
  G_OBJECT_CLASS(gh_dm_inbox_parent_class)->dispose(object);
}

static void
gh_dm_inbox_finalize(GObject *object)
{
  GhDmInbox *self = GH_DM_INBOX(object);
  g_hash_table_unref(self->endpoints);
  g_hash_table_unref(self->pending_ids);
  g_hash_table_unref(self->deferred_ids);
  g_free(self->state_dir);
  G_OBJECT_CLASS(gh_dm_inbox_parent_class)->finalize(object);
}

static void
gh_dm_inbox_class_init(GhDmInboxClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = gh_dm_inbox_get_property;
  object_class->dispose = gh_dm_inbox_dispose;
  object_class->finalize = gh_dm_inbox_finalize;
  props[PROP_STATE] = g_param_spec_enum("state", NULL, NULL, GH_TYPE_DM_INBOX_STATE,
    GH_DM_INBOX_INACTIVE, G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);
  signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
                                         G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);
}

static void
gh_dm_inbox_init(GhDmInbox *self)
{
  self->max_in_flight = 1;
  self->req_limit = GH_DM_INBOX_REQ_LIMIT;
  self->max_pages = GH_DM_INBOX_MAX_PAGES;
  self->state = GH_DM_INBOX_INACTIVE;
  /* Keys are the endpoints' own URLs. */
  self->endpoints = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, endpoint_free);
  self->pending_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->deferred_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_queue_init(&self->queue);
}
