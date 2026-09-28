#include "gh-dm-inbox.h"
#include "gh-identity.h"
#include "gh-nip17-inbox.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <nostr-filter.h>
#include <string.h>

#define KIND_GIFT_WRAP 1059
#define CHECKPOINT_MAGIC "groundhog-dm-inbox-checkpoint 1"
/* A settled checkpoint is written at most this often; the in-memory value
 * (used by a same-process reopen) advances every time. */
#define CHECKPOINT_WRITE_INTERVAL 60
#define MAX_SCOPE_URLS 16

enum { ENDPOINT_PENDING, ENDPOINT_EOSE, ENDPOINT_FAILED };

typedef struct {
  guint status;
  gboolean answered; /* sent EOSE or a backfill event */
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
  gpointer transport_data;
  gboolean custom_transport;
  guint max_in_flight;

  /* Account binding: one per account generation. */
  guint64 generation;
  gchar *pubkey_hex;
  gchar *checkpoint_path;
  gint64 checkpoint;         /* 0: none */
  gint64 checkpoint_written; /* last value written this generation */
  gchar *error;
  GhDmInboxCounters counters;

  /* Session: one scope over one relay set. Bumped on every teardown. */
  guint64 session;
  GStrv urls;
  gint64 since;
  GhRelayScope *scope;
  GHashTable *endpoints;   /* url -> Endpoint */
  GCancellable *cancellable;
  GQueue queue;            /* Job, oldest first */
  GHashTable *pending_ids; /* wrap ids queued or in flight */
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

/* ---- state ----------------------------------------------------------------- */

static const gchar *
all_failed_message(void)
{
  return "No inbox relay accepted the DM subscription";
}

static GhDmInboxState
compute_state(GhDmInbox *self)
{
  if (!self->generation)
    return GH_DM_INBOX_INACTIVE;
  if (self->error)
    return GH_DM_INBOX_ERROR;
  if (!self->scope)
    return GH_DM_INBOX_NO_INBOX_RELAYS;
  gboolean pending = FALSE, answered = FALSE, eose = FALSE;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->endpoints);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Endpoint *endpoint = value;
    pending |= endpoint->status == ENDPOINT_PENDING;
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

/* Advances the checkpoint only once everything any inbox relay delivered has
 * been settled: every endpoint at EOSE, nothing queued or in flight, and no
 * wrap deferred during this session. */
static void
maybe_advance_checkpoint(GhDmInbox *self)
{
  if (self->state != GH_DM_INBOX_LIVE || self->hold_checkpoint ||
      self->counters.pending != 0)
    return;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->endpoints);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    if (((Endpoint *)value)->status != ENDPOINT_EOSE)
      return;
  gint64 now = now_seconds();
  if (now <= self->checkpoint)
    return;
  self->checkpoint = now;
  if (self->checkpoint - self->checkpoint_written >= CHECKPOINT_WRITE_INTERVAL)
    checkpoint_write(self);
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

/* Today's persistence delegate: messages live in the store's memory only and
 * the per-account GhNip17Seen file holds the seen keys. The encrypted store
 * (G05) replaces this table with its T-admit transaction. */
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
seen_admit(gpointer data, GhMessage *message, const gchar *wrap_id, GError **error)
{
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
  seen_has_wrap, seen_has_rumor, seen_admit
};

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
    result = gh_conversation_store_admit(self->store, parsed, message->wrap_id, &error);
  }
  switch (result) {
  case GH_CONVERSATION_ADD_NEW:
    self->counters.admitted++;
    break;
  case GH_CONVERSATION_ADD_DUPLICATE:
    self->counters.duplicates++; /* another relay copy or a re-wrap */
    break;
  case GH_CONVERSATION_ADD_FAILED:
    /* Not committed, so not seen: a later REQ retries it. */
    g_message("Groundhog could not store NIP-17 message %s: %s", message->rumor_id,
              error->message);
    self->counters.deferred++;
    self->hold_checkpoint = TRUE;
    break;
  case GH_CONVERSATION_ADD_REJECTED:
  default:
    /* The unwrap already validated it; this only guards the invariant. */
    g_warning("Groundhog refused NIP-17 message %s: %s", message->rumor_id,
              error ? error->message : "unknown error");
    self->counters.rejected++;
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
  } else {
    /* Signer denial or failure: not seen, so a later REQ (reopen or
     * restart) offers it again; hold the checkpoint so that REQ covers it. */
    g_debug("Groundhog deferred NIP-17 wrap %s: %s", call->job->wrap_id, error->message);
    self->counters.deferred++;
    self->hold_checkpoint = TRUE;
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
      g_hash_table_contains(self->pending_ids, update->event_id)) {
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

/* ---- relay scope ----------------------------------------------------------- */

static void
on_scope_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  GhDmInbox *self = data;
  /* The controller revokes a generation before its "changed" reaches us;
   * anything delivered in that window belongs to the previous account. */
  if (scope != self->scope || !self->accounts ||
      !gh_account_controller_is_current(self->accounts, self->generation))
    return;
  Endpoint *endpoint = g_hash_table_lookup(self->endpoints, update->url);
  if (!endpoint)
    return;
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT:
    if (update->backfill)
      endpoint->answered = TRUE;
    handle_wrap(self, update);
    break;
  case GH_RELAY_NOTICE_EOSE:
    endpoint->status = ENDPOINT_EOSE;
    endpoint->answered = TRUE;
    break;
  case GH_RELAY_NOTICE_ERROR:
    /* A relay that already answered stays answered while it redials. */
    if (endpoint->status == ENDPOINT_PENDING)
      endpoint->status = ENDPOINT_FAILED;
    break;
  case GH_RELAY_NOTICE_CLOSED:
    endpoint->status = ENDPOINT_FAILED;
    break;
  case GH_RELAY_NOTICE_DISCONNECTED:
    /* The REQ is re-issued on reconnect and must reach EOSE again. */
    endpoint->status = ENDPOINT_PENDING;
    endpoint->answered = FALSE;
    break;
  default:
    return;
  }
  update_state(self);
  maybe_advance_checkpoint(self);
  emit_changed(self);
}

static NostrFilters *
inbox_filters(const gchar *pubkey_hex, gint64 since)
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
  nostr_filter_set_limit(filter, GH_DM_INBOX_REQ_LIMIT);
  gboolean added = nostr_filters_add(filters, filter);
  nostr_filter_free(filter); /* contents moved into the vector */
  if (!added) {
    nostr_filters_free(filters);
    return NULL;
  }
  return filters;
}

static gint64
session_since(GhDmInbox *self)
{
  gint64 now = now_seconds();
  if (self->checkpoint > 0)
    return MIN(self->checkpoint, now) - GH_DM_INBOX_WRAP_SKEW;
  return now - GH_DM_INBOX_INITIAL_BACKFILL;
}

/* Cancel first: the scope revokes its generation and closes every transport,
 * and the cancellable revokes pending signer approvals, before anything for
 * the next session exists. Late unwrap callbacks see a newer session. */
static void
teardown_session(GhDmInbox *self)
{
  self->session++;
  if (self->scope) {
    gh_relay_scope_cancel(self->scope);
    g_clear_pointer(&self->scope, gh_relay_scope_unref);
  }
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  g_queue_clear_full(&self->queue, job_free);
  g_hash_table_remove_all(self->pending_ids);
  g_hash_table_remove_all(self->endpoints);
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
  NostrFilters *filters = inbox_filters(self->pubkey_hex, self->since);
  if (!filters) {
    self->error = g_strdup("Could not build the DM inbox subscription");
    return;
  }
  self->cancellable = g_cancellable_new();
  self->scope = self->custom_transport
    ? gh_relay_scope_new_with_transport(self->generation, filters, &self->transport,
                                        self->transport_data, on_scope_update, self)
    : gh_relay_scope_new(self->generation, filters, on_scope_update, self);
  for (guint i = 0; urls[i]; i++) {
    g_autoptr(GError) error = NULL;
    if (gh_relay_scope_add_url(self->scope, urls[i], &error))
      g_hash_table_insert(self->endpoints, g_strdup(urls[i]), g_new0(Endpoint, 1));
    else
      g_message("Groundhog ignores DM inbox relay \"%s\": %s", urls[i], error->message);
  }
  gh_relay_scope_start(self->scope);
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
teardown_account(GhDmInbox *self)
{
  teardown_session(self);
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
   * and owns the seen-set from here on. */
  if (seen)
    gh_conversation_store_set_account(self->store, self->pubkey_hex, &seen_delegate, seen,
                                      (GDestroyNotify)gh_nip17_seen_free);
  else
    gh_conversation_store_set_account(self->store, self->pubkey_hex, NULL, NULL, NULL);
  if (!seen)
    return;
  g_autofree gchar *checkpoint_name = g_strconcat(self->pubkey_hex, ".checkpoint", NULL);
  self->checkpoint_path = g_build_filename(self->state_dir, checkpoint_name, NULL);
  self->checkpoint = checkpoint_load(self->checkpoint_path, self->pubkey_hex);
  self->checkpoint_written = self->checkpoint;
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
    else
      gh_conversation_store_set_account(self->store, NULL, NULL, NULL, NULL);
  }
  if (self->generation && !self->error) {
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

GhDmInbox *
gh_dm_inbox_new(GhAccountController *accounts, GhAccountRelays *relays,
                GhConversationStore *store, const gchar *state_dir,
                const GhRelayTransport *transport, gpointer transport_data)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts), NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_RELAYS(relays), NULL);
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(store), NULL);
  g_return_val_if_fail(!transport || (transport->open && transport->close), NULL);
  GhDmInbox *self = g_object_new(GH_TYPE_DM_INBOX, NULL);
  self->accounts = g_object_ref(accounts);
  self->relays = g_object_ref(relays);
  self->store = g_object_ref(store);
  self->state_dir = state_dir ? g_strdup(state_dir)
                              : g_build_filename(g_get_user_state_dir(), "groundhog",
                                                 "nip17", NULL);
  if (transport) {
    self->transport = *transport;
    self->transport_data = transport_data;
    self->custom_transport = TRUE;
  }
  g_signal_connect_object(accounts, "changed", G_CALLBACK(sync_binding), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(relays, "changed", G_CALLBACK(sync_binding), self,
                          G_CONNECT_SWAPPED);
  sync_binding(self);
  return self;
}

void
gh_dm_inbox_set_max_in_flight(GhDmInbox *self, guint max_in_flight)
{
  g_return_if_fail(GH_IS_DM_INBOX(self));
  self->max_in_flight = CLAMP(max_in_flight, 1, GH_DM_INBOX_MAX_IN_FLIGHT);
  if (self->accounts)
    pump(self);
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
  return self->error ? self->error : all_failed_message();
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
  self->state = GH_DM_INBOX_INACTIVE;
  self->endpoints = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  self->pending_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_queue_init(&self->queue);
}
