/* Durable NIP-17 outbox (privacy charter G06): OB-1..OB-10, UX-5 (enum side)
 * and the store primitives the outbox added.
 *
 * Harnesses: a real SQLCipher GhStore (test hooks: H8 crash cut points), the
 * fake GhClock (H6) for every outbox timer and jitter, a fake GNetworkMonitor,
 * a fake GhInboxResolver, recording GhRelayScope/GhRelayPublish transports
 * (with the NIP-42 half) and, with libsoup, real local WebSocket relays (H2).
 * Signing runs against the mock org.nostr.Signer on a private bus
 * (gh-test-signer.h). Waits iterate the main context; their deadlines are
 * failure bounds only. The crash scenarios run in their own test subprocess,
 * which starts a private bus (nostrc-test-bus, no connection yet) and then
 * forks: the child connects, runs until it is SIGKILLed at the cut point, and
 * the parent connects to the same bus afterwards and resumes. The bus is
 * bound to the subprocess's lifetime, so no daemon outlives a test. */
#include "crash-harness.h"
#include "gh-outbox.h"
#include "gh-reaction-store.h"
#include "gh-auth-policy.h"
#include "gh-nip17-envelope.h"
#include "gh-store-conversations.h"
#include "gh-test-signer.h"
#include "nostr-tag.h"
#include "nostr/nip59/nip59.h"

#include <glib/gstdio.h>
#include <signal.h>
#include <unistd.h>

#ifdef GROUNDHOG_TEST_WIRE
#include "../relay/wire-relay.h"
#endif

#define DISC        "wss://discovery.test.invalid"
#define ALICE_INBOX "wss://alice-inbox.test.invalid"
#define SHARED      "wss://shared-inbox.test.invalid"
#define BOB_A       "wss://bob-inbox-a.test.invalid"
#define BOB_B       "wss://bob-inbox-b.test.invalid"
#define BOB_D       "wss://bob-inbox-d.test.invalid"
#define STORE_ID    "3f2a9c1e-7b4d-4e8a-9c0f-5d6e7f8a9b0c"
#define T0          G_GINT64_CONSTANT(1900000000)
#define KEY_ALICE   1
#define KEY_BOB     2

static gchar *hex_alice, *hex_bob, *npub_alice, *npub_bob;
static GhTestBus shared_bus;

/* ---- small helpers ------------------------------------------------------- */

static void
drain(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

static gboolean
never(gpointer data)
{
  (void) data;
  return FALSE;
}

static void
rm_rf(const gchar *path)
{
  if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
    GDir *dir = g_dir_open(path, 0, NULL);
    const gchar *name;
    while (dir && (name = g_dir_read_name(dir))) {
      g_autofree gchar *child = g_build_filename(path, name, NULL);
      rm_rf(child);
    }
    if (dir)
      g_dir_close(dir);
    g_rmdir(path);
  } else {
    g_unlink(path);
  }
}

static gchar *
signed_event(guint key, int kind, gint64 created_at, NostrTags *tags)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, "");
  nostr_event_set_tags(event, tags);
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[key]), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  gchar *copy = g_strdup(json);
  free(json);
  nostr_event_free(event);
  return copy;
}

/* ---- fake GNetworkMonitor ------------------------------------------------ */

#define FAKE_TYPE_MONITOR (fake_monitor_get_type())
G_DECLARE_FINAL_TYPE(FakeMonitor, fake_monitor, FAKE, MONITOR, GObject)

struct _FakeMonitor {
  GObject parent_instance;
  gboolean available;
};

enum { MONITOR_PROP_0, MONITOR_PROP_AVAILABLE, MONITOR_PROP_METERED, MONITOR_PROP_CONNECTIVITY };

static gboolean
fake_monitor_initable_init(GInitable *initable, GCancellable *cancellable, GError **error)
{
  (void) initable; (void) cancellable; (void) error;
  return TRUE;
}

static void
fake_monitor_initable_iface_init(GInitableIface *iface)
{
  iface->init = fake_monitor_initable_init;
}

static gboolean
fake_monitor_can_reach(GNetworkMonitor *monitor, GSocketConnectable *connectable,
                       GCancellable *cancellable, GError **error)
{
  (void) connectable; (void) cancellable;
  if (FAKE_MONITOR(monitor)->available)
    return TRUE;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE, "offline");
  return FALSE;
}

static void
fake_monitor_iface_init(GNetworkMonitorInterface *iface)
{
  iface->can_reach = fake_monitor_can_reach;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(FakeMonitor, fake_monitor, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(G_TYPE_INITABLE, fake_monitor_initable_iface_init)
  G_IMPLEMENT_INTERFACE(G_TYPE_NETWORK_MONITOR, fake_monitor_iface_init))

static void
fake_monitor_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  FakeMonitor *self = FAKE_MONITOR(object);
  switch (prop_id) {
  case MONITOR_PROP_AVAILABLE: g_value_set_boolean(value, self->available); break;
  case MONITOR_PROP_METERED: g_value_set_boolean(value, FALSE); break;
  case MONITOR_PROP_CONNECTIVITY:
    g_value_set_enum(value, self->available ? G_NETWORK_CONNECTIVITY_FULL
                                            : G_NETWORK_CONNECTIVITY_LOCAL);
    break;
  default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
fake_monitor_class_init(FakeMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = fake_monitor_get_property;
  g_object_class_override_property(object_class, MONITOR_PROP_AVAILABLE, "network-available");
  g_object_class_override_property(object_class, MONITOR_PROP_METERED, "network-metered");
  g_object_class_override_property(object_class, MONITOR_PROP_CONNECTIVITY, "connectivity");
}

static void
fake_monitor_init(FakeMonitor *self)
{
  self->available = TRUE;
}

static void
fake_monitor_set_available(FakeMonitor *self, gboolean available)
{
  self->available = available;
  g_object_notify(G_OBJECT(self), "network-available");
  g_signal_emit_by_name(self, "network-changed", available);
}

/* ---- fake GhInboxResolver ------------------------------------------------ */

#define FAKE_TYPE_RESOLVER (fake_resolver_get_type())
G_DECLARE_FINAL_TYPE(FakeResolver, fake_resolver, FAKE, RESOLVER, GObject)

struct _FakeResolver {
  GObject parent_instance;
  GHashTable *answers; /* pubkey -> GhInboxResult */
  guint calls;
};

static void
fake_resolve_async(GhInboxResolver *resolver, const gchar *pubkey, GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer data)
{
  FakeResolver *self = FAKE_RESOLVER(resolver);
  self->calls++;
  GTask *task = g_task_new(resolver, cancellable, callback, data);
  GhInboxResult *answer = g_hash_table_lookup(self->answers, pubkey);
  GhInboxResult *copy = NULL;
  if (answer) {
    copy = gh_inbox_result_copy(answer);
  } else {
    copy = g_new0(GhInboxResult, 1);
    copy->status = GH_INBOX_NOT_FOUND;
    copy->recipient = g_strdup(pubkey);
    copy->sources = copy->answered = 1;
  }
  g_task_return_pointer(task, copy, (GDestroyNotify) gh_inbox_result_free);
  g_object_unref(task);
}

static GhInboxResult *
fake_resolve_finish(GhInboxResolver *resolver, GAsyncResult *result, GError **error)
{
  (void) resolver;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
fake_resolver_iface_init(GhInboxResolverInterface *iface)
{
  iface->resolve_async = fake_resolve_async;
  iface->resolve_finish = fake_resolve_finish;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(FakeResolver, fake_resolver, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GH_TYPE_INBOX_RESOLVER, fake_resolver_iface_init))

static void
fake_resolver_finalize(GObject *object)
{
  g_hash_table_unref(FAKE_RESOLVER(object)->answers);
  G_OBJECT_CLASS(fake_resolver_parent_class)->finalize(object);
}

static void
fake_resolver_class_init(FakeResolverClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = fake_resolver_finalize;
}

static void
fake_resolver_init(FakeResolver *self)
{
  self->answers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        (GDestroyNotify) gh_inbox_result_free);
}

/* status FOUND with the NULL-terminated URLs, or NOT_FOUND/UNREACHABLE. */
static void
fake_resolver_set(FakeResolver *self, const gchar *pubkey, GhInboxStatus status, ...)
{
  GhInboxResult *result = g_new0(GhInboxResult, 1);
  result->status = status;
  result->recipient = g_strdup(pubkey);
  result->sources = 1;
  result->answered = status == GH_INBOX_UNREACHABLE ? 0 : 1;
  result->failed = status == GH_INBOX_UNREACHABLE ? 1 : 0;
  if (status == GH_INBOX_FOUND) {
    GPtrArray *urls = g_ptr_array_new();
    va_list args;
    va_start(args, status);
    for (const gchar *url = va_arg(args, const gchar *); url; url = va_arg(args, const gchar *))
      g_ptr_array_add(urls, g_strdup(url));
    va_end(args);
    g_ptr_array_add(urls, NULL);
    result->relays = (GStrv) g_ptr_array_free(urls, FALSE);
    result->event_id = g_strnfill(64, 'e');
    result->created_at = 100;
  }
  g_hash_table_replace(self->answers, g_strdup(pubkey), result);
}

/* ---- recording scope transport (the account's own lists) ------------------ */

typedef struct {
  GhRelayScope *scope;
  gchar *url;
  gchar *author;
  gboolean closed;
} ScopeOpen;

static void
scope_open_free(gpointer data)
{
  ScopeOpen *open = data;
  gh_relay_scope_unref(open->scope);
  g_free(open->url);
  g_free(open->author);
  g_free(open);
}

static gpointer
scope_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters, gpointer data,
           GError **error)
{
  (void) error;
  ScopeOpen *open = g_new0(ScopeOpen, 1);
  open->scope = gh_relay_scope_ref(scope);
  open->url = g_strdup(url);
  open->author = g_strdup(nostr_filter_authors_get(&filters->filters[0], 0));
  g_ptr_array_add(data, open);
  return open;
}

static void
scope_close(gpointer handle, gpointer data)
{
  (void) data;
  ((ScopeOpen *) handle)->closed = TRUE;
}

static const GhRelayTransport scope_transport = { scope_open, scope_close };

/* ---- recording publish transport, with scripted answers ------------------- */

typedef enum {
  ANSWER_MANUAL,      /* the test answers */
  ANSWER_OK,          /* an OK with the scripted accepted flag and message */
  ANSWER_FAIL_OPEN,   /* the connection cannot be opened */
  ANSWER_AUTH_REFUSE, /* challenge + auth-required, then the AUTH is refused */
  ANSWER_AUTH_ACCEPT  /* challenge + auth-required, AUTH accepted, re-send accepted */
} AnswerKind;

typedef struct {
  AnswerKind kind;
  gboolean accepted;
  gchar *message;
} Answer;

typedef struct _Transport Transport;

typedef struct {
  Transport *transport;
  GhRelayPublish *publish;
  gchar *url;
  gchar *p;          /* the wrap's recipient */
  gchar *event_id;
  gchar *event_json; /* exactly what was handed to the transport */
  gint64 opened_at;  /* fake clock, unix seconds */
  gboolean failed;   /* the open was refused */
  gboolean closed;
  gchar *challenge;
  GPtrArray *auth;   /* signed AUTH events sent on this connection */
  gchar *auth_id;    /* the last one's id */
  guint resends;
} PubOpen;

struct _Transport {
  GPtrArray *opens;     /* PubOpen */
  GHashTable *answers;  /* url -> Answer */
  GhClock *clock;
  GhStore *store;       /* when set: every published wrap is already in T-seal */
  guint challenges;
};

static void
answer_free(gpointer data)
{
  Answer *answer = data;
  g_free(answer->message);
  g_free(answer);
}

static void
pub_open_free(gpointer data)
{
  PubOpen *open = data;
  gh_relay_publish_unref(open->publish);
  g_free(open->url);
  g_free(open->p);
  g_free(open->event_id);
  g_free(open->event_json);
  g_free(open->challenge);
  g_free(open->auth_id);
  g_ptr_array_unref(open->auth);
  g_free(open);
}

static void
script(Transport *transport, const gchar *url, AnswerKind kind, gboolean accepted,
       const gchar *message)
{
  Answer *answer = g_new0(Answer, 1);
  answer->kind = kind;
  answer->accepted = accepted;
  answer->message = g_strdup(message ? message : "");
  g_hash_table_replace(transport->answers, g_strdup(url), answer);
}

/* An idle delivery to a publish, as a relay's frame would arrive. */
typedef struct {
  PubOpen *open;
  gchar *id;        /* OK for this event id */
  gboolean accepted;
  gchar *message;
  gboolean challenge_first;
} Delivery;

static gboolean
deliver(gpointer data)
{
  Delivery *d = data;
  if (!d->open->closed) {
    if (d->challenge_first)
      gh_relay_publish_auth_challenge(d->open->publish, d->open->url, d->open->challenge);
    gh_relay_publish_ok(d->open->publish, d->open->url, d->id, d->accepted, d->message);
  }
  g_free(d->id);
  g_free(d->message);
  g_free(d);
  return G_SOURCE_REMOVE;
}

static void
deliver_later(PubOpen *open, const gchar *id, gboolean accepted, const gchar *message,
              gboolean challenge_first)
{
  Delivery *d = g_new0(Delivery, 1);
  d->open = open;
  d->id = g_strdup(id);
  d->accepted = accepted;
  d->message = g_strdup(message);
  d->challenge_first = challenge_first;
  g_idle_add(deliver, d);
}

/* T-seal before any publish: the wrap handed to the transport is a stored
 * event of an outbox entry, byte for byte. */
static void
assert_stored(GhStore *store, const gchar *event_json)
{
  g_autoptr(GArray) ids = gh_store_outbox_list_unfinished(store, GH_STORE_BACKEND_NIP17, NULL);
  g_assert_nonnull(ids);
  for (guint i = 0; i < ids->len; i++) {
    g_autoptr(GhStoreOutboxEntry) entry =
      gh_store_outbox_load(store, g_array_index(ids, gint64, i), NULL);
    for (guint j = 0; entry && j < entry->events->len; j++)
      if (g_strcmp0(((GhStoreOutboxEvent *) g_ptr_array_index(entry->events, j))->event_json,
                    event_json) == 0)
        return;
  }
  g_error("a wrap was published before it was stored (T-seal)");
}

static gpointer
pub_open(GhRelayPublish *publish, const gchar *url, const gchar *event_json, gpointer data,
         GError **error)
{
  Transport *transport = data;
  if (transport->store)
    assert_stored(transport->store, event_json);
  NostrEvent *wrap = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(wrap, event_json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_get_kind(wrap), ==, 1059);
  g_assert_cmpstr(nostr_event_get_pubkey(wrap), !=, hex_alice); /* ephemeral outer key */
  PubOpen *open = g_new0(PubOpen, 1);
  open->transport = transport;
  char *p = nostr_nip59_get_recipient(wrap);
  open->p = g_strdup(p);
  free(p);
  nostr_event_free(wrap);
  open->publish = gh_relay_publish_ref(publish);
  open->url = g_strdup(url);
  open->event_id = g_strdup(gh_relay_publish_get_event_id(publish));
  open->event_json = g_strdup(event_json);
  open->opened_at = gh_clock_get_unix(transport->clock);
  open->auth = g_ptr_array_new_with_free_func(g_free);
  open->challenge = g_strdup_printf("challenge-%u", ++transport->challenges);
  g_ptr_array_add(transport->opens, open);
  Answer *answer = g_hash_table_lookup(transport->answers, url);
  AnswerKind kind = answer ? answer->kind : ANSWER_MANUAL;
  if (kind == ANSWER_FAIL_OPEN) {
    open->failed = open->closed = TRUE;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED, "connection refused");
    return NULL;
  }
  if (kind == ANSWER_OK)
    deliver_later(open, open->event_id, answer->accepted, answer->message, FALSE);
  else if (kind == ANSWER_AUTH_REFUSE || kind == ANSWER_AUTH_ACCEPT)
    deliver_later(open, open->event_id, FALSE, "auth-required: sign in first", TRUE);
  return open;
}

static void
pub_close(gpointer handle, gpointer data)
{
  (void) data;
  PubOpen *open = handle;
  g_assert_false(open->closed);
  open->closed = TRUE;
}

static gboolean
pub_send_auth(gpointer handle, const gchar *signed_json, gpointer data, GError **error)
{
  (void) data; (void) error;
  PubOpen *open = handle;
  g_ptr_array_add(open->auth, g_strdup(signed_json));
  gchar id[65] = { 0 };
  g_autoptr(GError) verify_error = NULL;
  g_assert_true(gh_relay_auth_verify_signed(signed_json, open->url, open->challenge, NULL,
                                            g_get_real_time() / G_USEC_PER_SEC, id,
                                            &verify_error));
  g_assert_no_error(verify_error);
  g_free(open->auth_id);
  open->auth_id = g_strdup(id);
  Answer *answer = g_hash_table_lookup(open->transport->answers, open->url);
  if (answer && answer->kind == ANSWER_AUTH_REFUSE)
    deliver_later(open, id, FALSE, "restricted: not a member", FALSE);
  else if (answer && answer->kind == ANSWER_AUTH_ACCEPT)
    deliver_later(open, id, TRUE, "", FALSE);
  return TRUE;
}

static gboolean
pub_resend(gpointer handle, gpointer data, GError **error)
{
  (void) data; (void) error;
  PubOpen *open = handle;
  open->resends++;
  Answer *answer = g_hash_table_lookup(open->transport->answers, open->url);
  if (answer && answer->kind == ANSWER_AUTH_ACCEPT)
    deliver_later(open, open->event_id, TRUE, "", FALSE);
  return TRUE;
}

static const GhRelayPublishTransport pub_transport = { pub_open, pub_close };
static const GhRelayPublishAuthTransport pub_auth_transport = { pub_send_auth, pub_resend };

/* Opens (successful or refused) of the wrap for p to url, in order. */
static guint
count_opens(Transport *transport, const gchar *p, const gchar *url)
{
  guint n = 0;
  for (guint i = 0; i < transport->opens->len; i++) {
    PubOpen *open = g_ptr_array_index(transport->opens, i);
    n += (!p || g_strcmp0(open->p, p) == 0) && (!url || g_strcmp0(open->url, url) == 0);
  }
  return n;
}

static PubOpen *
last_open(Transport *transport, const gchar *p, const gchar *url)
{
  for (guint i = transport->opens->len; i > 0; i--) {
    PubOpen *open = g_ptr_array_index(transport->opens, i - 1);
    if (g_strcmp0(open->p, p) == 0 && g_strcmp0(open->url, url) == 0)
      return open;
  }
  g_error("no publish of %s's wrap to %s", p, url);
}

static void
relay_ok(Transport *transport, const gchar *p, const gchar *url, gboolean accepted,
         const gchar *message)
{
  PubOpen *open = last_open(transport, p, url);
  g_assert_false(open->closed);
  gh_relay_publish_ok(open->publish, url, open->event_id, accepted, message);
}

/* ---- fixture --------------------------------------------------------------- */

typedef struct {
  GhTestBus *bus;
  GhTestBus own_bus;
  GhTestSigner mock;
  GSettings *settings;
  GhAccountController *accounts;
  GPtrArray *scopes;
  GhAccountRelays *relays;
  FakeResolver *resolver;
  GhDmSender *sender;
  GhClock *clock;
  gchar *data_dir;
  gboolean own_dir;
  GhStore *store;
  FakeMonitor *network;
  Transport transport;
  guint publish_deadline;  /* fixture_outbox()'s; 0: the default */
  GhOutbox *outbox;
} Fixture;

static GPtrArray *
fake_list(gpointer data, GError **error)
{
  (void) data; (void) error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify) gh_identity_info_free);
  const gchar *npubs[] = { npub_alice, npub_bob };
  for (guint i = 0; i < G_N_ELEMENTS(npubs); i++) {
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup(npubs[i]);
    info->label = g_strdup(i ? "Bob" : "Alice");
    g_ptr_array_add(ids, info);
  }
  return ids;
}

static gboolean
listed(gpointer data)
{
  return gh_account_controller_get_state(data) != GH_ACCOUNT_STATE_DISCOVERING;
}

static GhStore *
store_open(const gchar *data_dir, GhClock *clock)
{
  guint8 key[GH_STORE_KEY_SIZE];
  for (guint i = 0; i < sizeof key; i++)
    key[i] = (guint8) (0x5a ^ i);
  g_autoptr(GBytes) bytes = g_bytes_new(key, sizeof key);
  GhStoreConfig config = { data_dir, hex_alice, NULL, NULL, clock };
  g_autoptr(GError) error = NULL;
  GhStore *store = gh_store_open_with_key(&config, bytes, STORE_ID, GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(store);
  return store;
}

/* Connections to a crash scenario's bus, which this process's parent (or
 * this process, before the fork) started. The bus owns them: never closed
 * here, released by nostrc_test_bus_down() (or with the killed child). */
static void
bus_connect(GhTestBus *bus, NostrcTestBus *daemon)
{
  bus->bus = daemon;
  bus->client = nostrc_test_bus_connect(daemon);
  bus->owner = nostrc_test_bus_connect(daemon);
}

/* Everything but the outbox. data_dir NULL creates a fresh one. With a
 * crash scenario's bus, fresh connections to it are used; otherwise the
 * binary's shared bus. */
static void
fixture_up(Fixture *f, const gchar *data_dir, NostrcTestBus *daemon)
{
  memset(f, 0, sizeof *f);
  if (daemon) {
    bus_connect(&f->own_bus, daemon);
    f->bus = &f->own_bus;
  } else {
    f->bus = &shared_bus;
  }
  gh_test_signer_up(f->bus, &f->mock);
  f->settings = g_settings_new("org.nostr.Groundhog");
  const gchar *sources[] = { DISC, NULL };
  g_settings_set_strv(f->settings, "discovery-relays", sources);
  g_settings_set_string(f->settings, "signer-method", "auto");
  g_settings_set_string(f->settings, "current-npub", npub_alice);
  f->accounts = gh_account_controller_new_full(f->settings, f->bus->client, fake_list, NULL);
  gh_test_spin_until(listed, f->accounts);
  g_assert_cmpint(gh_account_controller_get_state(f->accounts), ==, GH_ACCOUNT_STATE_ACTIVE);
  f->scopes = g_ptr_array_new_with_free_func(scope_open_free);
  f->relays = gh_account_relays_new(f->accounts, f->settings, &scope_transport, f->scopes);
  f->resolver = g_object_new(FAKE_TYPE_RESOLVER, NULL);
  fake_resolver_set(f->resolver, hex_bob, GH_INBOX_FOUND, BOB_A, BOB_B, NULL);
  f->clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  f->transport.opens = g_ptr_array_new_with_free_func(pub_open_free);
  f->transport.answers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, answer_free);
  f->transport.clock = f->clock;
  f->sender = gh_dm_sender_new(f->accounts, f->relays, GH_INBOX_RESOLVER(f->resolver),
                               &pub_transport, &f->transport);
  if (data_dir) {
    f->data_dir = g_strdup(data_dir);
  } else {
    f->data_dir = g_dir_make_tmp("groundhog-outbox-XXXXXX", NULL);
    f->own_dir = TRUE;
  }
  g_assert_nonnull(f->data_dir);
  f->store = store_open(f->data_dir, f->clock);
  f->transport.store = f->store;
  f->network = g_object_new(FAKE_TYPE_MONITOR, NULL);
}

static void
fixture_outbox(Fixture *f)
{
  GhOutboxConfig config = {
    .store = f->store,
    .accounts = f->accounts,
    .account_relays = f->relays,
    .inboxes = GH_INBOX_RESOLVER(f->resolver),
    .sender = f->sender,
    .network = G_NETWORK_MONITOR(f->network),
    .transport = &pub_transport,
    .auth_transport = &pub_auth_transport,
    .transport_data = &f->transport,
    .publish_deadline = f->publish_deadline,
  };
  g_autoptr(GError) error = NULL;
  f->outbox = gh_outbox_new(&config, &error);
  g_assert_no_error(error);
  g_assert_nonnull(f->outbox);
}

static void
fixture_down(Fixture *f)
{
  if (f->outbox)
    gh_test_release(f->outbox);
  gh_test_release(f->sender);
  gh_test_release(f->relays);
  gh_test_release(f->accounts);
  GhTestSenders check = { f->bus, &f->mock };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  drain();
  for (guint i = 0; i < f->transport.opens->len; i++)
    g_assert_true(((PubOpen *) g_ptr_array_index(f->transport.opens, i))->closed);
  g_ptr_array_unref(f->transport.opens);
  g_hash_table_unref(f->transport.answers);
  g_ptr_array_unref(f->scopes);
  g_object_unref(f->resolver);
  g_object_unref(f->network);
  g_object_unref(f->settings);
  gh_store_close(f->store);
  gh_clock_unref(f->clock);
  gh_test_signer_down(f->bus, &f->mock);
  if (f->own_dir)
    rm_rf(f->data_dir);
  g_free(f->data_dir);
}

static ScopeOpen *
own_scope(Fixture *f)
{
  for (guint i = f->scopes->len; i > 0; i--) {
    ScopeOpen *open = g_ptr_array_index(f->scopes, i - 1);
    if (!open->closed && g_strcmp0(open->author, hex_alice) == 0)
      return open;
  }
  g_error("the account's own list discovery is not open");
}

/* The account's own 10050 lists these URLs (NULL-terminated). */
static void
settle_own(Fixture *f, ...)
{
  ScopeOpen *open = own_scope(f);
  NostrTags *tags = nostr_tags_new(0);
  va_list args;
  va_start(args, f);
  for (const gchar *url = va_arg(args, const gchar *); url; url = va_arg(args, const gchar *))
    nostr_tags_append(tags, nostr_tag_new("relay", url, NULL));
  va_end(args);
  g_autofree gchar *inbox = signed_event(KEY_ALICE, 10050, 100, tags);
  gh_relay_scope_event(open->scope, DISC, inbox);
  gh_relay_scope_eose(open->scope, DISC);
  g_assert_cmpint(gh_account_relays_get_state(f->relays), ==, GH_ACCOUNT_RELAYS_COMPLETE);
}

static GhOutboxItem *
send_text(Fixture *f, const gchar *recipient, const gchar *text)
{
  g_autoptr(GError) error = NULL;
  GhOutboxItem *item = gh_outbox_send(f->outbox, recipient, text, &error);
  g_assert_no_error(error);
  g_assert_nonnull(item);
  return item;
}

typedef struct {
  Transport *transport;
  guint count;
} OpenCount;

static gboolean
opens_reached(gpointer data)
{
  OpenCount *want = data;
  return want->transport->opens->len >= want->count;
}

static void
wait_opens(Fixture *f, guint count)
{
  OpenCount want = { &f->transport, count };
  gh_test_spin_until(opens_reached, &want);
  drain();
}

typedef struct {
  GhOutboxItem *item;
  GhMessageStatus status;
} StatusWait;

static gboolean
status_is(gpointer data)
{
  StatusWait *want = data;
  return gh_outbox_item_get_status(want->item) == want->status;
}

static void
wait_status(GhOutboxItem *item, GhMessageStatus status)
{
  StatusWait want = { item, status };
  gh_test_spin_until(status_is, &want);
}

static gboolean
state_settled(gpointer data)
{
  return gh_outbox_item_get_state(data) == GH_STORE_OUTBOX_SETTLED;
}

/* The round in progress ended (the outbox waits, settled or gave up). */
static gboolean
round_over(gpointer data)
{
  GhStoreOutboxState state = gh_outbox_item_get_state(data);
  return state != GH_STORE_OUTBOX_PUBLISHING && state != GH_STORE_OUTBOX_SEALED &&
         state != GH_STORE_OUTBOX_QUEUED && state != GH_STORE_OUTBOX_SEALING;
}

static gboolean
auth_sent(gpointer data)
{
  return ((PubOpen *) data)->auth->len > 0;
}

static gboolean
resent(gpointer data)
{
  return ((PubOpen *) data)->resends > 0;
}

typedef struct {
  GhTestSigner *mock;
  guint count;
} HeldWait;

static gboolean
held_reached(gpointer data)
{
  HeldWait *want = data;
  return want->mock->held->len >= want->count;
}

/* With the signer holding every call: approves the next n as they arrive
 * (the seal's encryptions and signatures), leaving later ones held. */
static void
approve_calls(Fixture *f, guint n)
{
  for (guint i = 0; i < n; i++) {
    HeldWait one = { &f->mock, 1 };
    gh_test_spin_until(held_reached, &one);
    gh_test_signer_release_one(&f->mock);
  }
}

static gboolean
flag_set(gpointer data)
{
  return *(gboolean *) data;
}

/* A barrier for "the publish deadline would have fired by now": a timer of
 * the deadline's length started after the publish opened. GLib's seconds
 * timeouts round to the same whole seconds and ready ones run in the order
 * they were attached, so an armed deadline runs first. */
static void
wait_past_deadline(guint seconds)
{
  gboolean passed = FALSE;
  g_timeout_add_seconds(seconds, gh_test_deadline_hit, &passed);
  gh_test_spin_until(flag_set, &passed);
}

/* The item's per-relay detail for url's copy of the wrap for pubkey (a
 * static string of gh-message-status.c). */
static const gchar *
target_description(GhOutboxItem *item, const gchar *pubkey, const gchar *url)
{
  g_autoptr(GPtrArray) targets = gh_outbox_item_dup_targets(item);
  for (guint i = 0; i < targets->len; i++) {
    GhOutboxTarget *target = g_ptr_array_index(targets, i);
    if (g_str_equal(target->pubkey, pubkey) && g_str_equal(target->url, url))
      return target->description;
  }
  g_error("%s is not a target of the wrap for %s", url, pubkey);
}

static GhStoreOutboxEntry *
load_entry(Fixture *f, gint64 outbox_id)
{
  g_autoptr(GError) error = NULL;
  GhStoreOutboxEntry *entry = gh_store_outbox_load(f->store, outbox_id, &error);
  g_assert_no_error(error);
  return entry;
}

static GhStoreOutboxEvent *
entry_event(GhStoreOutboxEntry *entry, GhStoreOutboxRole role)
{
  for (guint i = 0; i < entry->events->len; i++) {
    GhStoreOutboxEvent *event = g_ptr_array_index(entry->events, i);
    if (event->role == role)
      return event;
  }
  g_error("no stored event with role %d", role);
}

static GhStoreOutboxTarget *
event_target(GhStoreOutboxEvent *event, const gchar *url)
{
  for (guint i = 0; i < event->targets->len; i++) {
    GhStoreOutboxTarget *target = g_ptr_array_index(event->targets, i);
    if (g_strcmp0(target->relay_url, url) == 0)
      return target;
  }
  g_error("%s is not a target", url);
}

static GhTargetClass
stored_class(GhStoreOutboxTarget *target)
{
  return gh_target_classify((GhRelayPublishOutcome) target->outcome,
                            target->ok_prefix < 0 ? GH_RELAY_OK_PREFIX_NONE
                                                  : (GhRelayOkPrefix) target->ok_prefix,
                            target->attempts);
}

/* Advances the fake clock to its next timeout (the outbox's only timers). */
static void
advance_to_next(Fixture *f)
{
  gint64 next = gh_clock_fake_get_next_deadline(f->clock);
  g_assert_cmpint(next, >=, 0);
  gh_clock_fake_advance(f->clock, MAX(next - gh_clock_get_monotonic_time(f->clock), 0));
  drain();
}

static void
advance_seconds(Fixture *f, gint64 seconds)
{
  gh_clock_fake_advance(f->clock, seconds * G_USEC_PER_SEC);
  drain();
}

/* The rumor id inside a wrap, unwrapped with the receiver's key. */
static gchar *
wrapped_rumor_id(const gchar *wrap_json, guint receiver_key)
{
  NostrEvent *wrap = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(wrap, wrap_json, NULL), ==, 1);
  NostrEvent *seal = nostr_nip59_unwrap(wrap, gh_test_secret[receiver_key]);
  g_assert_nonnull(seal);
  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, gh_test_secret[receiver_key], sizeof sk));
  g_assert_true(nostr_hex2bin(pk, hex_alice, sizeof pk));
  guint8 *plaintext = NULL;
  size_t length = 0;
  g_assert_cmpint(nostr_nip44_decrypt_v2(sk, pk, nostr_event_get_content(seal), &plaintext,
                                         &length), ==, 0);
  g_autofree gchar *rumor_json = g_strndup((const gchar *) plaintext, length);
  free(plaintext);
  NostrEvent *rumor = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(rumor, rumor_json, NULL), ==, 1);
  gchar *id = g_strdup(rumor->id);
  nostr_event_free(rumor);
  nostr_event_free(seal);
  nostr_event_free(wrap);
  return id;
}

/* ---- store primitives (additive API for G06) ------------------------------ */

static gchar *
hex_of(const gchar *seed)
{
  return g_compute_checksum_for_string(G_CHECKSUM_SHA256, seed, -1);
}

static gint64
enqueue_plain(GhStore *store, gint64 conversation, const gchar *seed, gint64 *out_message)
{
  g_autofree gchar *op_id = gh_store_new_op_id();
  g_autofree gchar *rumor_id = hex_of(seed);
  GhStoreOutgoing outgoing = { conversation, op_id, rumor_id, hex_alice, 14, 50, "text",
                               "{\"kind\":14}", NULL, 0, FALSE, 0, NULL };
  gint64 outbox_id = 0;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_enqueue(store, &outgoing, &outbox_id, out_message, &error));
  g_assert_no_error(error);
  return outbox_id;
}

static void
assert_update(GhStore *store, gint64 id, GhStoreOutboxState state, gint64 next, gboolean count,
              const gchar *reason, gint expect_code)
{
  GhStoreOutboxUpdate update = { state, next, count, reason };
  g_autoptr(GError) error = NULL;
  gboolean ok = gh_store_outbox_update(store, id, &update, &error);
  if (expect_code < 0) {
    g_assert_no_error(error);
    g_assert_true(ok);
  } else {
    g_assert_error(error, GH_STORE_ERROR, expect_code);
    g_assert_false(ok);
  }
}

static void
test_store_api(void)
{
  g_autofree gchar *dir = g_dir_make_tmp("groundhog-outbox-store-XXXXXX", NULL);
  GhClock *clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  GhStore *store = store_open(dir, clock);
  g_autoptr(GError) error = NULL;
  gint64 conversation = 0, message = 0, other_message = 0;
  g_assert_true(gh_store_ensure_conversation(store, GH_STORE_BACKEND_NIP17, "k", 0,
                                             &conversation, &error));
  gint64 id = enqueue_plain(store, conversation, "first", &message);
  gint64 other = enqueue_plain(store, conversation, "second", &other_message);
  gint64 mls_conversation = 0, mls_message = 0;
  g_assert_true(gh_store_ensure_conversation(store, GH_STORE_BACKEND_MLS, "abcd", 0,
                                             &mls_conversation, &error));
  gint64 mls = enqueue_plain(store, mls_conversation, "mls", &mls_message);

  /* Each backend's engine lists only its own entries. */
  g_autoptr(GArray) ids = gh_store_outbox_list_unfinished(store, GH_STORE_BACKEND_NIP17, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(ids->len, ==, 2);
  g_assert_cmpint(g_array_index(ids, gint64, 0), ==, id);
  g_autoptr(GArray) mls_ids = gh_store_outbox_list_unfinished(store, GH_STORE_BACKEND_MLS, &error);
  g_assert_cmpuint(mls_ids->len, ==, 1);
  g_assert_cmpint(g_array_index(mls_ids, gint64, 0), ==, mls);
  g_autoptr(GhStoreOutboxEntry) queued = gh_store_outbox_load(store, id, &error);
  g_assert_no_error(error);
  g_assert_cmpint(queued->state, ==, GH_STORE_OUTBOX_QUEUED);
  g_assert_cmpint(queued->message_id, ==, message);
  g_assert_cmpint(queued->conversation_id, ==, conversation);
  g_assert_cmpint(queued->created_at, ==, T0);
  g_assert_cmpstr(queued->rumor_json, ==, "{\"kind\":14}");
  g_assert_cmpuint(queued->events->len, ==, 0);
  gint64 found = 0;
  g_assert_true(gh_store_outbox_find_by_message(store, message, &found, &error));
  g_assert_cmpint(found, ==, id);

  /* Unsealed: only QUEUED/SEALING, NEEDS_ATTENTION and CANCELLED. */
  assert_update(store, id, GH_STORE_OUTBOX_SEALING, 0, FALSE, NULL, -1);
  assert_update(store, id, GH_STORE_OUTBOX_PUBLISHING, 0, TRUE, NULL, GH_STORE_ERROR_STATE);
  assert_update(store, id, GH_STORE_OUTBOX_SETTLED, 0, FALSE, NULL, GH_STORE_ERROR_STATE);
  assert_update(store, id, GH_STORE_OUTBOX_SEALED, 0, FALSE, NULL, GH_STORE_ERROR_STATE);
  assert_update(store, id, GH_STORE_OUTBOX_QUEUED, 99, FALSE, "inbox-unreachable", -1);

  g_autofree gchar *wrap_id = hex_of("wrap");
  g_autofree gchar *self_id = hex_of("self");
  g_autofree gchar *recipient = hex_of("recipient");
  const gchar *wrap_urls[] = { "wss://b.test.invalid", "wss://a.test.invalid", NULL };
  const gchar *self_urls[] = { "wss://own.test.invalid", NULL };
  GhStoreSealedEvent events[] = {
    { GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP, recipient, wrap_id, "{\"wrap\":1}", 0, wrap_urls,
      FALSE },
    { GH_STORE_OUTBOX_ROLE_SELF_WRAP, hex_alice, self_id, "{\"wrap\":2}", T0 + 42, self_urls,
      FALSE },
  };
  g_assert_true(gh_store_seal(store, id, events, G_N_ELEMENTS(events), &error));
  g_assert_no_error(error);

  /* Sealed: never back to QUEUED/SEALING (republish, never re-seal). */
  assert_update(store, id, GH_STORE_OUTBOX_QUEUED, 0, FALSE, NULL, GH_STORE_ERROR_STATE);
  assert_update(store, id, GH_STORE_OUTBOX_SEALING, 0, FALSE, NULL, GH_STORE_ERROR_STATE);
  assert_update(store, id, GH_STORE_OUTBOX_PUBLISHING, 0, TRUE, NULL, -1);
  assert_update(store, id, GH_STORE_OUTBOX_WAITING_RETRY, T0 + 15, FALSE, NULL, -1);

  GhStoreTargetOutcome accepted = { "wss://a.test.invalid", GH_RELAY_PUBLISH_ACCEPTED,
                                    GH_RELAY_OK_PREFIX_DUPLICATE, "duplicate: have it", TRUE };
  GhStoreTargetOutcome added = { "wss://new.test.invalid", GH_RELAY_PUBLISH_PENDING, -1, NULL,
                                 FALSE };
  g_autoptr(GhStoreOutboxEntry) before = gh_store_outbox_load(store, id, &error);
  gint64 wrap_row = entry_event(before, GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP)->id;
  g_assert_true(gh_store_record_outcome(store, wrap_row, &accepted, &error));
  g_assert_true(gh_store_record_outcome(store, wrap_row, &added, &error));
  g_assert_no_error(error);

  g_autoptr(GhStoreOutboxEntry) sealed = gh_store_outbox_load(store, id, &error);
  g_assert_cmpint(sealed->state, ==, GH_STORE_OUTBOX_WAITING_RETRY);
  g_assert_cmpint(sealed->next_attempt_at, ==, T0 + 15);
  g_assert_cmpuint(sealed->attempts, ==, 1);
  g_assert_null(sealed->last_error);
  g_assert_cmpuint(sealed->events->len, ==, 2);
  GhStoreOutboxEvent *wrap = entry_event(sealed, GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP);
  g_assert_cmpstr(wrap->event_id, ==, wrap_id);
  g_assert_cmpstr(wrap->event_json, ==, "{\"wrap\":1}");
  g_assert_cmpstr(wrap->target_pubkey, ==, recipient);
  g_assert_cmpuint(wrap->targets->len, ==, 3);
  g_assert_cmpstr(((GhStoreOutboxTarget *) g_ptr_array_index(wrap->targets, 0))->relay_url, ==,
                  "wss://a.test.invalid"); /* ordered by URL */
  GhStoreOutboxTarget *a = event_target(wrap, "wss://a.test.invalid");
  g_assert_cmpint(a->outcome, ==, GH_RELAY_PUBLISH_ACCEPTED);
  g_assert_cmpint(a->ok_prefix, ==, GH_RELAY_OK_PREFIX_DUPLICATE);
  g_assert_cmpstr(a->ok_message, ==, "duplicate: have it");
  g_assert_cmpuint(a->attempts, ==, 1);
  g_assert_cmpint(a->last_attempt_at, ==, T0);
  GhStoreOutboxTarget *b = event_target(wrap, "wss://b.test.invalid");
  g_assert_cmpint(b->outcome, ==, GH_RELAY_PUBLISH_PENDING);
  g_assert_cmpint(b->ok_prefix, ==, -1);
  g_assert_cmpuint(b->attempts, ==, 0);
  g_assert_cmpint(event_target(wrap, "wss://new.test.invalid")->outcome, ==, 0);
  GhStoreOutboxEvent *self_copy = entry_event(sealed, GH_STORE_OUTBOX_ROLE_SELF_WRAP);
  g_assert_cmpint(self_copy->not_before, ==, T0 + 42);
  g_assert_cmpuint(self_copy->targets->len, ==, 1);

  assert_update(store, id, GH_STORE_OUTBOX_SETTLED, 0, FALSE, NULL, -1);
  g_autoptr(GArray) left = gh_store_outbox_list_unfinished(store, GH_STORE_BACKEND_NIP17, &error);
  g_assert_cmpuint(left->len, ==, 1);
  g_assert_cmpint(g_array_index(left, gint64, 0), ==, other);

  /* CANCELLED is final. */
  assert_update(store, other, GH_STORE_OUTBOX_CANCELLED, 0, FALSE, "cancelled", -1);
  assert_update(store, other, GH_STORE_OUTBOX_QUEUED, 0, FALSE, NULL, GH_STORE_ERROR_STATE);
  assert_update(store, other, GH_STORE_OUTBOX_NEEDS_ATTENTION, 0, FALSE, NULL,
                GH_STORE_ERROR_STATE);
  g_autoptr(GArray) none = gh_store_outbox_list_unfinished(store, GH_STORE_BACKEND_NIP17, &error);
  g_assert_cmpuint(none->len, ==, 0);

  /* Delete: entry, events, targets and the outgoing message go; seen stays. */
  g_assert_true(gh_store_outbox_delete(store, id, &error));
  g_assert_no_error(error);
  g_autoptr(GhStoreOutboxEntry) gone = gh_store_outbox_load(store, id, &error);
  g_assert_null(gone);
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND);
  g_clear_error(&error);
  g_assert_false(gh_store_outbox_find_by_message(store, message, &found, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND);
  g_clear_error(&error);
  gboolean seen = FALSE;
  g_autofree gchar *first_id = hex_of("first");
  g_assert_true(gh_store_seen_contains(store, GH_STORE_SEEN_RUMOR, first_id, &seen, &error));
  g_assert_true(seen);
  g_assert_true(gh_store_outbox_find_by_message(store, other_message, &found, &error));

  /* Unknown ids and bad arguments. */
  assert_update(store, 9999, GH_STORE_OUTBOX_CANCELLED, 0, FALSE, NULL, GH_STORE_ERROR_NOT_FOUND);
  assert_update(store, other, (GhStoreOutboxState) 42, 0, FALSE, NULL, GH_STORE_ERROR_INVALID);
  assert_update(store, other, GH_STORE_OUTBOX_CANCELLED, -1, FALSE, NULL, GH_STORE_ERROR_INVALID);
  g_assert_false(gh_store_outbox_delete(store, 9999, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND);
  g_clear_error(&error);

  gh_store_close(store);
  gh_clock_unref(clock);
  rm_rf(dir);
}

/* ---- OB-6 / OB-4 (classes) / UX-5: GTK-free status ------------------------ */

#define P GH_TARGET_CLASS_PENDING
#define A GH_TARGET_CLASS_ACCEPTED
#define T GH_TARGET_CLASS_TRANSIENT
#define R GH_TARGET_CLASS_RESUMABLE
#define X GH_TARGET_CLASS_TERMINAL

/* OB-6: two recipients x the five outcome classes, online, not given up. The
 * self-copy is no input of the status at all (it only drives the note), so
 * every self class yields this table; the note is checked separately. */
static const GhTargetClass classes[] = { P, A, T, R, X };
static const GhMessageStatus truth[5][5] = {
  /*         P                                A                                T                                R                                X */
  /* P */ { GH_MESSAGE_STATUS_SENDING,        GH_MESSAGE_STATUS_PARTIALLY_SENT, GH_MESSAGE_STATUS_SENDING,        GH_MESSAGE_STATUS_SENDING,        GH_MESSAGE_STATUS_SENDING },
  /* A */ { GH_MESSAGE_STATUS_PARTIALLY_SENT, GH_MESSAGE_STATUS_SENT,           GH_MESSAGE_STATUS_PARTIALLY_SENT, GH_MESSAGE_STATUS_PARTIALLY_SENT, GH_MESSAGE_STATUS_PARTIALLY_SENT },
  /* T */ { GH_MESSAGE_STATUS_SENDING,        GH_MESSAGE_STATUS_PARTIALLY_SENT, GH_MESSAGE_STATUS_RETRYING,       GH_MESSAGE_STATUS_SENDING,        GH_MESSAGE_STATUS_RETRYING },
  /* R */ { GH_MESSAGE_STATUS_SENDING,        GH_MESSAGE_STATUS_PARTIALLY_SENT, GH_MESSAGE_STATUS_SENDING,        GH_MESSAGE_STATUS_SENDING,        GH_MESSAGE_STATUS_SENDING },
  /* X */ { GH_MESSAGE_STATUS_SENDING,        GH_MESSAGE_STATUS_PARTIALLY_SENT, GH_MESSAGE_STATUS_RETRYING,       GH_MESSAGE_STATUS_SENDING,        GH_MESSAGE_STATUS_NOT_SENT },
};

static void
test_status_truth_table(void)
{
  for (guint i = 0; i < 5; i++) {
    for (guint j = 0; j < 5; j++) {
      GhTargetClass recipients[] = { classes[i], classes[j] };
      GhMessageStatusInput input = { .phase = GH_MESSAGE_PHASE_SEALED, .online = TRUE,
                                     .recipients = recipients, .n_recipients = 2 };
      GhMessageStatus expect = truth[i][j];
      g_assert_cmpint(gh_message_status_derive(&input), ==, expect);
      /* Offline: what still has to go out waits for the connection. */
      input.online = FALSE;
      GhMessageStatus offline = expect == GH_MESSAGE_STATUS_SENDING ||
                                expect == GH_MESSAGE_STATUS_RETRYING
                                  ? GH_MESSAGE_STATUS_QUEUED_OFFLINE : expect;
      g_assert_cmpint(gh_message_status_derive(&input), ==, offline);
      /* Given up (72 h): nobody reached is NOT_SENT; acceptance stays true. */
      input.online = TRUE;
      input.gave_up = TRUE;
      GhMessageStatus gave_up = expect == GH_MESSAGE_STATUS_SENT ||
                                expect == GH_MESSAGE_STATUS_PARTIALLY_SENT
                                  ? expect : GH_MESSAGE_STATUS_NOT_SENT;
      g_assert_cmpint(gh_message_status_derive(&input), ==, gave_up);
      /* nostrc-qp24.68: an approval the signer is asking the user for (the
       * self-copy's or a note to self's sign-in) is "Waiting for approval"
       * while nobody has the message and it is still going out, online or
       * not; it never hides that someone has it, or that it is not sent. */
      input.gave_up = FALSE;
      input.approval_pending = TRUE;
      GhMessageStatus waiting = expect == GH_MESSAGE_STATUS_SENDING ||
                                expect == GH_MESSAGE_STATUS_RETRYING
                                  ? GH_MESSAGE_STATUS_WAITING_FOR_SIGNER : expect;
      g_assert_cmpint(gh_message_status_derive(&input), ==, waiting);
      input.online = FALSE;
      g_assert_cmpint(gh_message_status_derive(&input), ==, waiting);
    }
  }
  /* The self-copy note, over every self class. */
  for (guint k = 0; k < 5; k++) {
    g_assert_cmpint(gh_message_status_self_copy_missing(classes[k], FALSE), ==, classes[k] == X);
    g_assert_cmpint(gh_message_status_self_copy_missing(classes[k], TRUE), ==, classes[k] != A);
  }
  /* One recipient's copy from its targets. */
  GhTargetClass xa[] = { X, T, A }, xp[] = { X, R, T }, xt[] = { X, T }, xx[] = { X, X };
  g_assert_cmpint(gh_target_class_combine(xa, 3), ==, A);
  g_assert_cmpint(gh_target_class_combine(xp, 3), ==, P);
  g_assert_cmpint(gh_target_class_combine(xt, 2), ==, T);
  g_assert_cmpint(gh_target_class_combine(xx, 2), ==, X);
  g_assert_cmpint(gh_target_class_combine(NULL, 0), ==, X);
  /* Unsealed phases. */
  GhMessageStatusInput unsealed = { .phase = GH_MESSAGE_PHASE_UNSEALED, .online = TRUE };
  g_assert_cmpint(gh_message_status_derive(&unsealed), ==, GH_MESSAGE_STATUS_WAITING_FOR_SIGNER);
  unsealed.online = FALSE;
  g_assert_cmpint(gh_message_status_derive(&unsealed), ==, GH_MESSAGE_STATUS_QUEUED_OFFLINE);
  unsealed.signer_pending = TRUE;
  g_assert_cmpint(gh_message_status_derive(&unsealed), ==, GH_MESSAGE_STATUS_WAITING_FOR_SIGNER);
  unsealed.signer_pending = FALSE;
  unsealed.online = TRUE;
  unsealed.retry_scheduled = TRUE;
  g_assert_cmpint(gh_message_status_derive(&unsealed), ==, GH_MESSAGE_STATUS_RETRYING);
  unsealed.gave_up = TRUE;
  g_assert_cmpint(gh_message_status_derive(&unsealed), ==, GH_MESSAGE_STATUS_NOT_SENT);
  unsealed.no_inbox = TRUE;
  g_assert_cmpint(gh_message_status_derive(&unsealed), ==, GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX);
  GhMessageStatusInput cancelled = { .phase = GH_MESSAGE_PHASE_CANCELLED, .online = TRUE };
  g_assert_cmpint(gh_message_status_derive(&cancelled), ==, GH_MESSAGE_STATUS_CANCELLED);
}

#undef P
#undef A
#undef T
#undef R
#undef X

/* OB-4, the table itself: every §3.6 row. */
static void
test_status_outcome_classes(void)
{
  const struct {
    GhRelayPublishOutcome outcome;
    const gchar *message; /* classified like the relay layer does */
    guint attempts;
    GhTargetClass expect;
  } rows[] = {
    { GH_RELAY_PUBLISH_ACCEPTED, "", 1, GH_TARGET_CLASS_ACCEPTED },
    { GH_RELAY_PUBLISH_ACCEPTED, "duplicate: already have it", 1, GH_TARGET_CLASS_ACCEPTED },
    { GH_RELAY_PUBLISH_REJECTED, "duplicate: already have it", 1, GH_TARGET_CLASS_ACCEPTED },
    { GH_RELAY_PUBLISH_REJECTED, "rate-limited: slow down", 1, GH_TARGET_CLASS_TRANSIENT },
    { GH_RELAY_PUBLISH_REJECTED, "rate-limited: slow down", 50, GH_TARGET_CLASS_TRANSIENT },
    { GH_RELAY_PUBLISH_REJECTED, "invalid: bad id", 1, GH_TARGET_CLASS_TERMINAL },
    { GH_RELAY_PUBLISH_REJECTED, "pow: difficulty 30", 1, GH_TARGET_CLASS_TERMINAL },
    { GH_RELAY_PUBLISH_REJECTED, "blocked: go away", 1, GH_TARGET_CLASS_TERMINAL },
    { GH_RELAY_PUBLISH_REJECTED, "restricted: members only", 1, GH_TARGET_CLASS_TERMINAL },
    { GH_RELAY_PUBLISH_REJECTED, "mute: muted", 1, GH_TARGET_CLASS_TERMINAL },
    { GH_RELAY_PUBLISH_REJECTED, "error: disk full", 1, GH_TARGET_CLASS_TRANSIENT },
    { GH_RELAY_PUBLISH_REJECTED, "error: disk full", 2, GH_TARGET_CLASS_TRANSIENT },
    { GH_RELAY_PUBLISH_REJECTED, "error: disk full", 3, GH_TARGET_CLASS_TERMINAL },
    { GH_RELAY_PUBLISH_REJECTED, "no prefix at all", 2, GH_TARGET_CLASS_TRANSIENT },
    { GH_RELAY_PUBLISH_REJECTED, "no prefix at all", 3, GH_TARGET_CLASS_TERMINAL },
    { GH_RELAY_PUBLISH_AUTH_REQUIRED, "auth-required: sign in", 1, GH_TARGET_CLASS_TERMINAL },
    { GH_RELAY_PUBLISH_CONNECTION_FAILED, NULL, 1, GH_TARGET_CLASS_TRANSIENT },
    { GH_RELAY_PUBLISH_CONNECTION_FAILED, NULL, 40, GH_TARGET_CLASS_TRANSIENT },
    { GH_RELAY_PUBLISH_CANCELLED, NULL, 0, GH_TARGET_CLASS_RESUMABLE },
    { GH_RELAY_PUBLISH_PENDING, NULL, 0, GH_TARGET_CLASS_PENDING },
  };
  for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
    GhRelayOkPrefix prefix = gh_relay_ok_prefix_classify(rows[i].message);
    g_assert_cmpint(gh_target_classify(rows[i].outcome, prefix, rows[i].attempts), ==,
                    rows[i].expect);
    g_assert_nonnull(gh_message_status_describe_target(rows[i].outcome, prefix));
  }
  /* A user's Retry still skips refusals that cannot change by retrying. */
  g_assert_true(gh_target_is_final_refusal(GH_RELAY_PUBLISH_REJECTED, GH_RELAY_OK_PREFIX_INVALID));
  g_assert_true(gh_target_is_final_refusal(GH_RELAY_PUBLISH_AUTH_REQUIRED, GH_RELAY_OK_PREFIX_NONE));
  g_assert_false(gh_target_is_final_refusal(GH_RELAY_PUBLISH_REJECTED, GH_RELAY_OK_PREFIX_ERROR));
  g_assert_false(gh_target_is_final_refusal(GH_RELAY_PUBLISH_CONNECTION_FAILED,
                                            GH_RELAY_OK_PREFIX_NONE));
}

static gboolean
has_word(const gchar *text, const gchar *word)
{
  g_autofree gchar *lower = g_utf8_strdown(text, -1);
  for (const gchar *p = strstr(lower, word); p; p = strstr(p + 1, word)) {
    gboolean start = p == lower || !g_ascii_isalpha(p[-1]);
    gboolean end = !g_ascii_isalpha(p[strlen(word)]);
    if (start && end)
      return TRUE;
  }
  return FALSE;
}

/* UX-5, enum side: every status has an icon, label and accessible
 * description, and none claims delivery or reading. */
static void
test_status_copy(void)
{
  /* The enum is contiguous from WAITING_FOR_SIGNER to CANCELLED; the G01
   * static check (message-status) pins that no value is DELIVERED/READ/SEEN. */
  g_assert_cmpint(GH_MESSAGE_STATUS_WAITING_FOR_SIGNER, ==, 0);
  g_assert_cmpint(GH_MESSAGE_STATUS_CANCELLED, ==, 8);
  for (gint value = GH_MESSAGE_STATUS_WAITING_FOR_SIGNER; value <= GH_MESSAGE_STATUS_CANCELLED;
       value++) {
    GhMessageStatus status = (GhMessageStatus) value;
    const gchar *texts[] = { gh_message_status_get_label(status),
                             gh_message_status_get_accessible_description(status) };
    const gchar *icon = gh_message_status_get_icon_name(status);
    g_assert_true(icon && g_str_has_suffix(icon, "-symbolic"));
    for (guint j = 0; j < G_N_ELEMENTS(texts); j++) {
      g_assert_true(texts[j] && *texts[j]);
      g_autofree gchar *lower = g_utf8_strdown(texts[j], -1);
      g_assert_null(strstr(lower, "deliver"));
      g_assert_false(has_word(texts[j], "read"));
      g_assert_false(has_word(texts[j], "seen"));
    }
  }
  g_assert_cmpstr(gh_message_status_get_label(GH_MESSAGE_STATUS_SENT), ==, "Sent");
  g_assert_cmpstr(gh_message_status_get_label(GH_MESSAGE_STATUS_WAITING_FOR_SIGNER), ==,
                  "Waiting for approval");
  g_assert_cmpstr(gh_message_status_get_self_copy_note(), ==, "Not saved to your other devices");
  g_assert_cmpstr(gh_message_status_describe_approval(), ==,
                  "Waiting for your approval in Grotto.");
}

/* ---- the outbox over the store ------------------------------------------- */

typedef struct {
  GPtrArray *statuses; /* GhMessageStatus, as notified */
} StatusLog;

static void
log_status(GObject *object, GParamSpec *pspec, gpointer data)
{
  (void) pspec;
  StatusLog *log = data;
  g_ptr_array_add(log->statuses,
                  GINT_TO_POINTER(gh_outbox_item_get_status(GH_OUTBOX_ITEM(object))));
}

/* T-enqueue before any signer call, T-seal before any publish, per-relay
 * T-outcome, SENT once each recipient has one relay's OK, EPHEMERAL AUTH on
 * a recipient relay that demands it (the account never signs an AUTH). */
static void
test_send_happy_path(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  settle_own(&f, ALICE_INBOX, NULL);
  fixture_outbox(&f);
  g_assert_true(gh_outbox_is_active(f.outbox));
  gint64 conversation = 0;
  g_autofree gchar *key = strcmp(hex_alice, hex_bob) < 0
    ? g_strconcat(hex_alice, ",", hex_bob, NULL) : g_strconcat(hex_bob, ",", hex_alice, NULL);
  g_assert_true(gh_store_ensure_conversation(f.store, GH_STORE_BACKEND_NIP17, key, 0,
                                             &conversation, NULL));
  g_assert_true(gh_store_set_draft(f.store, conversation, "hello b", NULL));

  StatusLog log = { g_ptr_array_new() };
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "hello bob");
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_QUEUED);
  g_signal_connect(item, "notify::status", G_CALLBACK(log_status), &log);
  g_assert_true(gh_outbox_item_get_can_retry(item) == FALSE);
  /* T-enqueue is durable and nothing has been signed. */
  g_assert_cmpuint(f.mock.calls, ==, 0);
  g_assert_cmpint(gh_outbox_item_get_conversation_id(item), ==, conversation);
  g_autofree gchar *draft = NULL;
  g_assert_true(gh_store_get_draft(f.store, conversation, &draft, NULL));
  g_assert_null(draft);
  g_autoptr(GhStoreOutboxEntry) queued = load_entry(&f, gh_outbox_item_get_outbox_id(item));
  g_assert_cmpint(queued->state, ==, GH_STORE_OUTBOX_QUEUED);
  g_assert_cmpint(queued->message_id, ==, gh_outbox_item_get_message_id(item));
  g_assert_nonnull(strstr(queued->rumor_json, "hello bob"));
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_WAITING_FOR_SIGNER);

  wait_opens(&f, 3);
  g_assert_cmpuint(f.mock.calls, ==, 4); /* two encryptions, two seal signatures */
  g_assert_cmpuint(count_opens(&f.transport, hex_bob, BOB_A), ==, 1);
  g_assert_cmpuint(count_opens(&f.transport, hex_bob, BOB_B), ==, 1);
  g_assert_cmpuint(count_opens(&f.transport, hex_alice, ALICE_INBOX), ==, 1);
  g_assert_cmpuint(f.transport.opens->len, ==, 3);
  g_autoptr(GhStoreOutboxEntry) sealed = load_entry(&f, gh_outbox_item_get_outbox_id(item));
  GhStoreOutboxEvent *wrap = entry_event(sealed, GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP);
  GhStoreOutboxEvent *self_copy = entry_event(sealed, GH_STORE_OUTBOX_ROLE_SELF_WRAP);
  g_assert_cmpstr(wrap->target_pubkey, ==, hex_bob);
  g_assert_cmpint(self_copy->not_before, ==, 0); /* disjoint inboxes: no D8 delay */
  g_assert_cmpstr(last_open(&f.transport, hex_bob, BOB_A)->event_json, ==, wrap->event_json);
  /* The wraps carry exactly the stored rumor. */
  g_autofree gchar *rumor_in_wrap = wrapped_rumor_id(wrap->event_json, KEY_BOB);
  g_autoptr(GhStoreOutboxEntry) with_rumor = load_entry(&f, gh_outbox_item_get_outbox_id(item));
  g_assert_nonnull(strstr(with_rumor->rumor_json, rumor_in_wrap));
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENDING);

  relay_ok(&f.transport, hex_bob, BOB_A, FALSE, "error: try later");
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENDING);

  /* BOB_B demands AUTH: one ephemeral AUTH (never the account), one re-send. */
  PubOpen *authed = last_open(&f.transport, hex_bob, BOB_B);
  gh_relay_publish_auth_challenge(authed->publish, BOB_B, authed->challenge);
  gh_relay_publish_ok(authed->publish, BOB_B, authed->event_id, FALSE,
                      "auth-required: sign in first");
  gh_test_spin_until(auth_sent, authed);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENDING);
  gh_relay_publish_ok(authed->publish, BOB_B, authed->auth_id, TRUE, "");
  gh_test_spin_until(resent, authed);
  gh_relay_publish_ok(authed->publish, BOB_B, authed->event_id, TRUE, "");
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpuint(authed->auth->len, ==, 1);
  g_assert_cmpuint(authed->resends, ==, 1);
  NostrEvent *auth = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(auth, g_ptr_array_index(authed->auth, 0), NULL),
                  ==, 1);
  g_assert_cmpstr(nostr_event_get_pubkey(auth), !=, hex_alice);
  nostr_event_free(auth);
  g_assert_cmpuint(f.mock.calls, ==, 4); /* the AUTH never reached the signer */
  g_assert_cmpstr(gh_outbox_item_get_label(item), ==, "Sent");
  g_assert_nonnull(strstr(gh_outbox_item_get_detail(item), "Accepted by 1 of 2"));
  g_assert_false(gh_outbox_item_get_self_copy_missing(item));

  /* The self-copy failing only adds the note; the status stays SENT. */
  relay_ok(&f.transport, hex_alice, ALICE_INBOX, FALSE, "blocked: not you");
  drain();
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENT);
  /* BOB_A's "error:" is retried; the rest is settled per relay. */
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_WAITING_RETRY);
  g_assert_true(gh_outbox_item_get_self_copy_missing(item));
  g_autoptr(GhStoreOutboxEntry) after = load_entry(&f, gh_outbox_item_get_outbox_id(item));
  GhStoreOutboxEvent *stored = entry_event(after, GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP);
  g_assert_cmpint(event_target(stored, BOB_B)->outcome, ==, GH_RELAY_PUBLISH_ACCEPTED);
  g_assert_cmpint(event_target(stored, BOB_A)->ok_prefix, ==, GH_RELAY_OK_PREFIX_ERROR);
  g_assert_cmpuint(after->attempts, ==, 1);

  script(&f.transport, BOB_A, ANSWER_OK, TRUE, "");
  advance_to_next(&f);
  gh_test_spin_until(state_settled, item);
  g_assert_cmpuint(count_opens(&f.transport, hex_bob, BOB_B), ==, 1); /* accepted: never again */
  g_assert_cmpuint(f.resolver->calls, ==, 2); /* the retry re-read Bob's list */
  g_assert_cmpuint(count_opens(&f.transport, hex_alice, ALICE_INBOX), ==, 1); /* terminal */
  g_assert_cmpuint(count_opens(&f.transport, hex_bob, BOB_A), ==, 2);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpuint(f.mock.calls, ==, 4);
  g_assert_cmpuint(gh_clock_fake_get_n_timeouts(f.clock), ==, 0);

  /* Honest transitions only: approval, sending, sent. */
  g_assert_cmpuint(log.statuses->len, >=, 2);
  for (guint i = 0; i < log.statuses->len; i++) {
    GhMessageStatus status = GPOINTER_TO_INT(g_ptr_array_index(log.statuses, i));
    g_assert_true(status == GH_MESSAGE_STATUS_SENDING || status == GH_MESSAGE_STATUS_SENT);
  }
  g_assert_cmpint(GPOINTER_TO_INT(g_ptr_array_index(log.statuses, log.statuses->len - 1)), ==,
                  GH_MESSAGE_STATUS_SENT);
  g_ptr_array_unref(log.statuses);
  g_autoptr(GPtrArray) targets = gh_outbox_item_dup_targets(item);
  g_assert_cmpuint(targets->len, ==, 3);
  g_assert_cmpint(((GhOutboxTarget *) g_ptr_array_index(targets, 2))->role, ==,
                  GH_STORE_OUTBOX_ROLE_SELF_WRAP);
  fixture_down(&f);
}

/* ---- OB-1..OB-3: crash (H8) and restart ----------------------------------- */

typedef struct {
  gchar *data_dir;
  NostrcTestBus *daemon;
} CrashEnv;

static void
crash_child_start(Fixture *f, CrashEnv *env)
{
  fixture_up(f, env->data_dir, env->daemon);
  settle_own(f, ALICE_INBOX, NULL);
  fixture_outbox(f);
}

/* OB-1 child: the signer holds its first request; the process dies then. */
static void
ob1_child(gpointer data)
{
  Fixture f;
  crash_child_start(&f, data);
  gint64 conversation = 0;
  g_autofree gchar *key = strcmp(hex_alice, hex_bob) < 0
    ? g_strconcat(hex_alice, ",", hex_bob, NULL) : g_strconcat(hex_bob, ",", hex_alice, NULL);
  g_assert_true(gh_store_ensure_conversation(f.store, GH_STORE_BACKEND_NIP17, key, 0,
                                             &conversation, NULL));
  g_assert_true(gh_store_set_draft(f.store, conversation, "draft that was sent", NULL));
  f.mock.hold = TRUE;
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "sent before the crash");
  HeldWait want = { &f.mock, 1 };
  gh_test_spin_until(held_reached, &want);
  kill(getpid(), SIGKILL);
}

static void
ob_seal_child(gpointer data)
{
  Fixture f;
  crash_child_start(&f, data);
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "sealed before the crash");
  gh_test_spin_until(never, NULL); /* killed at the cut point */
}

static void
ob3_child(gpointer data)
{
  Fixture f;
  crash_child_start(&f, data);
  script(&f.transport, BOB_A, ANSWER_OK, TRUE, "");
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "half published");
  gh_test_spin_until(never, NULL); /* killed after A's T-outcome */
}

static void
crash_run(CrashEnv *env, const gchar *cut_point, GhCrashScript child)
{
  env->data_dir = g_dir_make_tmp("groundhog-outbox-crash-XXXXXX", NULL);
  g_assert_nonnull(env->data_dir);
  /* Not a session bus: nostrc_test_bus_up() would then connect, and no GDBus
   * connection (or its worker thread) may exist before fork(). GhSigner
   * finds the bus through DBUS_SESSION_BUS_ADDRESS, exported by hand. The
   * child inherits the bus's lifeline, so its death leaves the bus up. */
  env->daemon = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NOT_SESSION);
  nostrc_test_bus_up(env->daemon);
  g_setenv("DBUS_SESSION_BUS_ADDRESS", nostrc_test_bus_get_address(env->daemon), TRUE);
  GhCrashOutcome outcome = gh_crash_harness_run(cut_point, 1, child, env);
  g_assert_cmpstr(gh_crash_outcome_to_string(outcome), ==,
                  gh_crash_outcome_to_string(GH_CRASH_KILLED));
}

static void
crash_env_clear(CrashEnv *env)
{
  nostrc_test_bus_down(g_steal_pointer(&env->daemon));
  g_unsetenv("DBUS_SESSION_BUS_ADDRESS");
  rm_rf(env->data_dir);
  g_free(env->data_dir);
}

#define CRASH_SUBPROCESS()                                                   \
  G_STMT_START {                                                             \
    if (!g_test_subprocess()) {                                              \
      g_test_trap_subprocess(NULL, 120 * G_USEC_PER_SEC,                     \
                             G_TEST_SUBPROCESS_INHERIT_STDERR);              \
      g_test_trap_assert_passed();                                           \
      return;                                                                \
    }                                                                        \
  } G_STMT_END

static gint64
only_entry(Fixture *f)
{
  g_autoptr(GArray) ids = gh_store_outbox_list_unfinished(f->store, GH_STORE_BACKEND_NIP17, NULL);
  g_assert_cmpuint(ids->len, ==, 1);
  return g_array_index(ids, gint64, 0);
}

/* OB-1: killed while the signer was asking. The message is queued, shows
 * "Waiting for approval", the signer is asked once more (one seal), the
 * wraps carry the stored rumor, and the draft stays cleared. */
static void
test_ob1_enqueue_durability(void)
{
  CRASH_SUBPROCESS();
  CrashEnv env = { 0 };
  crash_run(&env, "seal:after-commit", ob1_child); /* never reached: killed while held */
  Fixture f;
  fixture_up(&f, env.data_dir, env.daemon);
  gint64 id = only_entry(&f);
  g_autoptr(GhStoreOutboxEntry) before = load_entry(&f, id);
  g_assert_cmpint(before->state, ==, GH_STORE_OUTBOX_SEALING);
  g_assert_cmpuint(before->events->len, ==, 0);
  g_autofree gchar *draft = NULL;
  g_assert_true(gh_store_get_draft(f.store, before->conversation_id, &draft, NULL));
  g_assert_null(draft);

  settle_own(&f, ALICE_INBOX, NULL);
  f.mock.hold = TRUE;
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = gh_outbox_lookup(f.outbox, id);
  g_assert_nonnull(item);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_WAITING_FOR_SIGNER);
  g_assert_cmpstr(gh_outbox_item_get_label(item), ==, "Waiting for approval");
  HeldWait want = { &f.mock, 1 };
  gh_test_spin_until(held_reached, &want);
  g_assert_cmpuint(f.mock.calls, ==, 1);
  f.mock.hold = FALSE;
  gh_test_signer_release_all(&f.mock);
  wait_opens(&f, 3);
  g_assert_cmpuint(f.mock.calls, ==, 4); /* exactly one seal */
  g_autoptr(GhStoreOutboxEntry) sealed = load_entry(&f, id);
  g_assert_cmpuint(sealed->events->len, ==, 2);
  g_autofree gchar *inner = wrapped_rumor_id(
    entry_event(sealed, GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP)->event_json, KEY_BOB);
  g_autofree gchar *inner_self = wrapped_rumor_id(
    entry_event(sealed, GH_STORE_OUTBOX_ROLE_SELF_WRAP)->event_json, KEY_ALICE);
  g_assert_cmpstr(inner, ==, inner_self);
  g_assert_nonnull(strstr(sealed->rumor_json, inner));
  g_assert_nonnull(strstr(sealed->rumor_json, "sent before the crash"));
  g_autofree gchar *draft_after = NULL;
  g_assert_true(gh_store_get_draft(f.store, sealed->conversation_id, &draft_after, NULL));
  g_assert_null(draft_after);
  relay_ok(&f.transport, hex_bob, BOB_A, TRUE, "");
  wait_status(item, GH_MESSAGE_STATUS_SENT);
  fixture_down(&f);
  crash_env_clear(&env);
}

/* OB-2: killed right after T-seal. After restart the stored wraps are
 * republished byte for byte; the signer is never called. */
static void
test_ob2_no_reseal(void)
{
  CRASH_SUBPROCESS();
  CrashEnv env = { 0 };
  crash_run(&env, "seal:after-commit", ob_seal_child);
  Fixture f;
  fixture_up(&f, env.data_dir, env.daemon);
  gint64 id = only_entry(&f);
  g_autoptr(GhStoreOutboxEntry) stored = load_entry(&f, id);
  g_assert_cmpint(stored->state, ==, GH_STORE_OUTBOX_SEALED);
  g_assert_cmpuint(stored->attempts, ==, 0);
  GhStoreOutboxEvent *wrap = entry_event(stored, GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP);
  GhStoreOutboxEvent *self_copy = entry_event(stored, GH_STORE_OUTBOX_ROLE_SELF_WRAP);

  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = gh_outbox_lookup(f.outbox, id);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENDING);
  wait_opens(&f, 3);
  g_assert_cmpuint(f.mock.calls, ==, 0);
  for (guint i = 0; i < f.transport.opens->len; i++) {
    PubOpen *open = g_ptr_array_index(f.transport.opens, i);
    GhStoreOutboxEvent *event = g_strcmp0(open->p, hex_bob) == 0 ? wrap : self_copy;
    g_assert_cmpstr(open->event_id, ==, event->event_id);
    g_assert_cmpstr(open->event_json, ==, event->event_json);
  }
  relay_ok(&f.transport, hex_bob, BOB_B, TRUE, "");
  wait_status(item, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpuint(f.mock.calls, ==, 0);
  fixture_down(&f);
  crash_env_clear(&env);
}

/* OB-3: killed after A's T-outcome while B (and the self-copy) were still
 * pending: the restart publishes only to B and the own inbox. */
static void
test_ob3_resume_fanout(void)
{
  CRASH_SUBPROCESS();
  CrashEnv env = { 0 };
  crash_run(&env, "outcome:after-commit", ob3_child);
  Fixture f;
  fixture_up(&f, env.data_dir, env.daemon);
  gint64 id = only_entry(&f);
  g_autoptr(GhStoreOutboxEntry) stored = load_entry(&f, id);
  g_assert_cmpint(stored->state, ==, GH_STORE_OUTBOX_PUBLISHING);
  GhStoreOutboxEvent *wrap = entry_event(stored, GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP);
  g_assert_cmpint(event_target(wrap, BOB_A)->outcome, ==, GH_RELAY_PUBLISH_ACCEPTED);
  g_assert_cmpint(event_target(wrap, BOB_B)->outcome, ==, GH_RELAY_PUBLISH_PENDING);

  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = gh_outbox_lookup(f.outbox, id);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENT);
  wait_opens(&f, 2);
  g_assert_cmpuint(f.transport.opens->len, ==, 2);
  g_assert_cmpuint(count_opens(&f.transport, hex_bob, BOB_A), ==, 0);
  g_assert_cmpuint(count_opens(&f.transport, hex_bob, BOB_B), ==, 1);
  g_assert_cmpuint(count_opens(&f.transport, hex_alice, ALICE_INBOX), ==, 1);
  g_assert_cmpstr(last_open(&f.transport, hex_bob, BOB_B)->event_json, ==, wrap->event_json);
  g_assert_cmpuint(f.mock.calls, ==, 0);
  relay_ok(&f.transport, hex_bob, BOB_B, TRUE, "");
  relay_ok(&f.transport, hex_alice, ALICE_INBOX, TRUE, "");
  gh_test_spin_until(state_settled, item);
  fixture_down(&f);
  crash_env_clear(&env);
}

/* ---- OB-4: outcome classes through the outbox ------------------------------ */

#define RL_OK         "wss://ok.test.invalid"
#define RL_DUP        "wss://dup.test.invalid"
#define RL_DUP_FALSE  "wss://dup-false.test.invalid"
#define RL_RATE       "wss://rate.test.invalid"
#define RL_INVALID    "wss://invalid.test.invalid"
#define RL_POW        "wss://pow.test.invalid"
#define RL_BLOCKED    "wss://blocked.test.invalid"
#define RL_RESTRICTED "wss://restricted.test.invalid"
#define RL_MUTE       "wss://mute.test.invalid"
#define RL_ERROR      "wss://error.test.invalid"
#define RL_NOPREFIX   "wss://noprefix.test.invalid"
#define RL_AUTH       "wss://auth.test.invalid"
#define RL_DOWN       "wss://down.test.invalid"

static void
test_ob4_outcome_classes(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  fake_resolver_set(f.resolver, hex_bob, GH_INBOX_FOUND, RL_OK, RL_DUP, RL_DUP_FALSE, RL_RATE,
                    RL_INVALID, RL_POW, RL_BLOCKED, RL_RESTRICTED, RL_MUTE, RL_ERROR, RL_NOPREFIX,
                    RL_AUTH, RL_DOWN, NULL);
  script(&f.transport, RL_OK, ANSWER_OK, TRUE, "");
  script(&f.transport, RL_DUP, ANSWER_OK, TRUE, "duplicate: have it");
  script(&f.transport, RL_DUP_FALSE, ANSWER_OK, FALSE, "duplicate: have it");
  script(&f.transport, RL_RATE, ANSWER_OK, FALSE, "rate-limited: slow down");
  script(&f.transport, RL_INVALID, ANSWER_OK, FALSE, "invalid: bad");
  script(&f.transport, RL_POW, ANSWER_OK, FALSE, "pow: difficulty 30");
  script(&f.transport, RL_BLOCKED, ANSWER_OK, FALSE, "blocked: no");
  script(&f.transport, RL_RESTRICTED, ANSWER_OK, FALSE, "restricted: members");
  script(&f.transport, RL_MUTE, ANSWER_OK, FALSE, "mute: muted");
  script(&f.transport, RL_ERROR, ANSWER_OK, FALSE, "error: oops");
  script(&f.transport, RL_NOPREFIX, ANSWER_OK, FALSE, "something odd");
  script(&f.transport, RL_AUTH, ANSWER_AUTH_REFUSE, FALSE, NULL);
  script(&f.transport, RL_DOWN, ANSWER_FAIL_OPEN, FALSE, NULL);
  script(&f.transport, ALICE_INBOX, ANSWER_OK, TRUE, "");
  settle_own(&f, ALICE_INBOX, NULL);
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "classes");
  gint64 id = gh_outbox_item_get_outbox_id(item);

  const gchar *transient[] = { RL_RATE, RL_ERROR, RL_NOPREFIX, RL_DOWN };
  for (guint round = 1; round <= 4; round++) {
    if (round == 1) {
      wait_opens(&f, 14);
    } else {
      g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_WAITING_RETRY);
      guint before = f.transport.opens->len;
      advance_to_next(&f); /* the whole retry round runs here */
      g_assert_cmpuint(f.transport.opens->len, >, before);
    }
    gh_test_spin_until(round_over, item);
    g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENT);
    g_autoptr(GhStoreOutboxEntry) entry = load_entry(&f, id);
    GhStoreOutboxEvent *wrap = entry_event(entry, GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP);
    g_assert_cmpint(stored_class(event_target(wrap, RL_OK)), ==, GH_TARGET_CLASS_ACCEPTED);
    g_assert_cmpint(stored_class(event_target(wrap, RL_DUP)), ==, GH_TARGET_CLASS_ACCEPTED);
    g_assert_cmpint(stored_class(event_target(wrap, RL_DUP_FALSE)), ==, GH_TARGET_CLASS_ACCEPTED);
    g_assert_cmpint(stored_class(event_target(wrap, RL_RATE)), ==, GH_TARGET_CLASS_TRANSIENT);
    g_assert_cmpint(stored_class(event_target(wrap, RL_DOWN)), ==, GH_TARGET_CLASS_TRANSIENT);
    g_assert_cmpint(event_target(wrap, RL_DOWN)->outcome, ==, GH_RELAY_PUBLISH_CONNECTION_FAILED);
    const gchar *terminal[] = { RL_INVALID, RL_POW, RL_BLOCKED, RL_RESTRICTED, RL_MUTE, RL_AUTH };
    for (guint i = 0; i < G_N_ELEMENTS(terminal); i++)
      g_assert_cmpint(stored_class(event_target(wrap, terminal[i])), ==, GH_TARGET_CLASS_TERMINAL);
    g_assert_cmpint(event_target(wrap, RL_AUTH)->outcome, ==, GH_RELAY_PUBLISH_AUTH_REQUIRED);
    /* "error:" and prefix-less: transient three times, then terminal. */
    GhTargetClass by_count = round < 3 ? GH_TARGET_CLASS_TRANSIENT : GH_TARGET_CLASS_TERMINAL;
    guint expect_attempts = MIN(round, 3);
    g_assert_cmpint(stored_class(event_target(wrap, RL_ERROR)), ==, by_count);
    g_assert_cmpint(stored_class(event_target(wrap, RL_NOPREFIX)), ==, by_count);
    g_assert_cmpuint(event_target(wrap, RL_ERROR)->attempts, ==, expect_attempts);
    /* Exactly the non-accepted, non-terminal relays were retried. */
    for (guint i = 0; i < G_N_ELEMENTS(transient); i++) {
      guint expect = g_str_equal(transient[i], RL_ERROR) || g_str_equal(transient[i], RL_NOPREFIX)
                       ? MIN(round, 3) : round;
      g_assert_cmpuint(count_opens(&f.transport, hex_bob, transient[i]), ==, expect);
    }
    const gchar *once[] = { RL_OK, RL_DUP, RL_DUP_FALSE, RL_INVALID, RL_POW, RL_BLOCKED, RL_RESTRICTED,
                            RL_MUTE, RL_AUTH };
    for (guint i = 0; i < G_N_ELEMENTS(once); i++)
      g_assert_cmpuint(count_opens(&f.transport, hex_bob, once[i]), ==, 1);
  }
  /* The refused AUTH was one ephemeral AUTH, never escalated. */
  PubOpen *auth = last_open(&f.transport, hex_bob, RL_AUTH);
  g_assert_cmpuint(auth->auth->len, ==, 1);
  g_assert_cmpuint(auth->resends, ==, 0);
  g_assert_cmpuint(f.mock.calls, ==, 4);
  g_autoptr(GPtrArray) targets = gh_outbox_item_dup_targets(item);
  for (guint i = 0; i < targets->len; i++) {
    GhOutboxTarget *target = g_ptr_array_index(targets, i);
    if (g_str_equal(target->url, RL_RESTRICTED))
      g_assert_cmpstr(target->description, ==,
                      "This relay only accepts messages from signed-in users.");
    if (g_str_equal(target->url, RL_AUTH))
      g_assert_cmpstr(target->description, ==, "This relay requires sign-in.");
  }
  fixture_down(&f);
}

/* ---- OB-5: backoff on H6 ------------------------------------------------- */

static void
test_ob5_backoff(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  fake_resolver_set(f.resolver, hex_bob, GH_INBOX_FOUND, BOB_A, NULL);
  script(&f.transport, BOB_A, ANSWER_FAIL_OPEN, FALSE, NULL);
  script(&f.transport, ALICE_INBOX, ANSWER_OK, TRUE, "");
  settle_own(&f, ALICE_INBOX, NULL);
  fixture_outbox(&f);
  /* The first two retries take the jitter's extremes: x0.8, then x1.2. */
  gh_clock_fake_push_random(f.clock, 0);
  gh_clock_fake_push_random(f.clock, 400);
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "nobody home");
  wait_opens(&f, 2);
  wait_status(item, GH_MESSAGE_STATUS_RETRYING);
  g_assert_cmpint(last_open(&f.transport, hex_bob, BOB_A)->opened_at, ==, T0);

  static const gint64 schedule[] = { 15, 60, 300, 1800, 7200, 21600 };
  g_autoptr(GArray) times = g_array_new(FALSE, FALSE, sizeof(gint64));
  gint64 t = T0;
  g_array_append_val(times, t);
  while (gh_outbox_item_get_state(item) == GH_STORE_OUTBOX_WAITING_RETRY) {
    gint64 next = gh_outbox_item_get_next_attempt_at(item);
    g_assert_cmpint(next, >, gh_clock_get_unix(f.clock));
    g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_RETRYING);
    g_assert_true(gh_outbox_item_get_can_retry(item));
    guint before = count_opens(&f.transport, hex_bob, BOB_A);
    advance_to_next(&f);
    if (count_opens(&f.transport, hex_bob, BOB_A) > before) {
      gint64 at = last_open(&f.transport, hex_bob, BOB_A)->opened_at;
      g_assert_cmpint(at, ==, next);
      g_array_append_val(times, at);
    }
  }
  /* 72 h after T-enqueue: needs attention, NOT_SENT with Retry, no timer. */
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_NEEDS_ATTENTION);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_NOT_SENT);
  g_assert_cmpint(gh_clock_get_unix(f.clock), ==, T0 + 72 * 3600);
  g_assert_true(gh_outbox_item_get_can_retry(item));
  g_assert_cmpuint(gh_clock_fake_get_n_timeouts(f.clock), ==, 0);
  g_assert_cmpint(g_array_index(times, gint64, times->len - 1), <, T0 + 72 * 3600);
  g_assert_cmpuint(times->len, >=, 12);
  for (guint i = 1; i < times->len; i++) {
    gint64 interval = g_array_index(times, gint64, i) - g_array_index(times, gint64, i - 1);
    gint64 base = schedule[MIN(i, G_N_ELEMENTS(schedule)) - 1];
    if (i == 1)
      g_assert_cmpint(interval, ==, 12);  /* 15 s x 0.8 */
    else if (i == 2)
      g_assert_cmpint(interval, ==, 72);  /* 60 s x 1.2 */
    g_assert_cmpint(interval * 10, >=, base * 8);
    g_assert_cmpint(interval * 10, <=, base * 12);
  }
  g_assert_cmpuint(f.mock.calls, ==, 4);
  fixture_down(&f);
}

/* ---- OB-7: account switch mid-publish ------------------------------------- */

static void
test_ob7_account_switch(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  settle_own(&f, ALICE_INBOX, NULL);
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "mid publish");
  gint64 id = gh_outbox_item_get_outbox_id(item);
  wait_opens(&f, 3);
  PubOpen *stale = last_open(&f.transport, hex_bob, BOB_A);
  g_autofree gchar *wrap_id = g_strdup(stale->event_id);

  g_autoptr(GError) error = NULL;
  g_assert_true(gh_account_controller_select(f.accounts, npub_bob, &error));
  g_assert_no_error(error);
  drain();
  g_assert_false(gh_outbox_is_active(f.outbox));
  for (guint i = 0; i < f.transport.opens->len; i++)
    g_assert_true(((PubOpen *) g_ptr_array_index(f.transport.opens, i))->closed);
  /* A late OK from the old generation is not recorded. */
  gh_relay_publish_ok(stale->publish, BOB_A, stale->event_id, TRUE, "");
  drain();
  g_autoptr(GhStoreOutboxEntry) paused = load_entry(&f, id);
  g_assert_cmpint(paused->state, ==, GH_STORE_OUTBOX_PUBLISHING);
  GhStoreOutboxEvent *wrap = entry_event(paused, GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP);
  g_assert_cmpint(event_target(wrap, BOB_A)->outcome, ==, GH_RELAY_PUBLISH_PENDING);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_QUEUED_OFFLINE);
  /* Nothing is sent while the other account is active, whatever happens. */
  g_autoptr(GError) send_error = NULL;
  g_assert_null(gh_outbox_send(f.outbox, hex_bob, "not now", &send_error));
  g_assert_error(send_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  guint opens = f.transport.opens->len;
  advance_seconds(&f, 3600);
  fake_monitor_set_available(f.network, FALSE);
  fake_monitor_set_available(f.network, TRUE);
  drain();
  g_assert_cmpuint(f.transport.opens->len, ==, opens);

  /* Reselecting the account resumes the same stored wraps. */
  g_assert_true(gh_account_controller_select(f.accounts, npub_alice, &error));
  g_assert_no_error(error);
  wait_opens(&f, opens + 3);
  g_assert_true(gh_outbox_is_active(f.outbox));
  g_assert_cmpstr(last_open(&f.transport, hex_bob, BOB_A)->event_id, ==, wrap_id);
  g_assert_cmpstr(last_open(&f.transport, hex_bob, BOB_A)->event_json, ==, wrap->event_json);
  g_assert_cmpuint(f.mock.calls, ==, 4);
  relay_ok(&f.transport, hex_bob, BOB_A, TRUE, "");
  wait_status(item, GH_MESSAGE_STATUS_SENT);
  fixture_down(&f);
}

/* ---- OB-8: the recipient's 10050 changed after sealing -------------------- */

static void
test_ob8_new_inbox_relay(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  fake_resolver_set(f.resolver, hex_bob, GH_INBOX_FOUND, BOB_A, NULL);
  script(&f.transport, BOB_A, ANSWER_FAIL_OPEN, FALSE, NULL);
  script(&f.transport, ALICE_INBOX, ANSWER_OK, TRUE, "");
  settle_own(&f, ALICE_INBOX, NULL);
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "moved inbox");
  wait_status(item, GH_MESSAGE_STATUS_RETRYING);
  gh_test_spin_until(round_over, item);
  g_autofree gchar *wrap_id = g_strdup(last_open(&f.transport, hex_bob, BOB_A)->event_id);
  guint lookups = f.resolver->calls;

  fake_resolver_set(f.resolver, hex_bob, GH_INBOX_FOUND, BOB_A, BOB_D, NULL);
  script(&f.transport, BOB_D, ANSWER_OK, TRUE, "");
  advance_to_next(&f);
  wait_status(item, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpuint(f.resolver->calls, ==, lookups + 1);
  g_assert_cmpstr(last_open(&f.transport, hex_bob, BOB_D)->event_id, ==, wrap_id);
  g_assert_cmpuint(count_opens(&f.transport, hex_bob, BOB_A), ==, 2);
  g_assert_cmpuint(f.mock.calls, ==, 4); /* same wrap: no new seal */
  g_autoptr(GhStoreOutboxEntry) entry = load_entry(&f, gh_outbox_item_get_outbox_id(item));
  GhStoreOutboxEvent *wrap = entry_event(entry, GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP);
  g_assert_cmpstr(wrap->event_id, ==, wrap_id);
  g_assert_cmpint(event_target(wrap, BOB_D)->outcome, ==, GH_RELAY_PUBLISH_ACCEPTED);
  fixture_down(&f);
}

/* ---- OB-9: self-copy jitter (D8) ------------------------------------------ */

static void
check_self_copy_timing(gboolean overlap, guint32 draw, gint64 expect_delay)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  if (overlap) {
    fake_resolver_set(f.resolver, hex_bob, GH_INBOX_FOUND, BOB_A, SHARED, NULL);
    settle_own(&f, SHARED, NULL);
  } else {
    settle_own(&f, ALICE_INBOX, NULL);
  }
  const gchar *own = overlap ? SHARED : ALICE_INBOX;
  fixture_outbox(&f);
  if (overlap)
    gh_clock_fake_push_random(f.clock, draw);
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "self copy timing");
  wait_opens(&f, overlap ? 2 : 3);
  g_assert_cmpuint(count_opens(&f.transport, hex_bob, NULL), ==, 2);
  if (overlap) {
    g_assert_cmpuint(count_opens(&f.transport, hex_alice, NULL), ==, 0);
    g_autoptr(GhStoreOutboxEntry) entry = load_entry(&f, gh_outbox_item_get_outbox_id(item));
    gint64 not_before = entry_event(entry, GH_STORE_OUTBOX_ROLE_SELF_WRAP)->not_before;
    g_assert_cmpint(not_before, >=, T0 + 5);
    g_assert_cmpint(not_before, <=, T0 + 90);
    if (expect_delay >= 0)
      g_assert_cmpint(not_before, ==, T0 + expect_delay);
    advance_seconds(&f, not_before - T0 - 1);
    g_assert_cmpuint(count_opens(&f.transport, hex_alice, NULL), ==, 0);
    advance_seconds(&f, 1);
    wait_opens(&f, 3);
  }
  PubOpen *self_open = last_open(&f.transport, hex_alice, own);
  g_autoptr(GhStoreOutboxEntry) sealed = load_entry(&f, gh_outbox_item_get_outbox_id(item));
  gint64 due = entry_event(sealed, GH_STORE_OUTBOX_ROLE_SELF_WRAP)->not_before;
  g_assert_cmpint(self_open->opened_at, ==, overlap ? due : T0);
  if (!overlap)
    g_assert_cmpint(due, ==, 0);
  /* Its own connection, even to the shared relay. */
  for (guint i = 0; i < f.transport.opens->len; i++) {
    PubOpen *open = g_ptr_array_index(f.transport.opens, i);
    if (open != self_open)
      g_assert_true(open->publish != self_open->publish);
  }
  if (overlap)
    g_assert_cmpuint(count_opens(&f.transport, hex_bob, SHARED), ==, 1);
  fixture_down(&f);
}

static void
test_ob9_self_copy_jitter(void)
{
  check_self_copy_timing(TRUE, 0, 5);     /* the lower bound */
  check_self_copy_timing(TRUE, 85, 90);   /* the upper bound */
  check_self_copy_timing(TRUE, 40, 45);
  check_self_copy_timing(FALSE, 0, 0);    /* disjoint: immediately */
}

/* ---- G08 self-copy AUTH and G10 S2 re-targeting ----------------------------- */

static gchar *
auth_pubkey(PubOpen *open, guint index)
{
  NostrEvent *auth = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(auth, g_ptr_array_index(open->auth, index),
                                                  NULL), ==, 1);
  gchar *pubkey = g_strdup(nostr_event_get_pubkey(auth));
  nostr_event_free(auth);
  return pubkey;
}

/* W13 review 7a, decided in G08: an own inbox relay that accepts writes only
 * from signed-in members gets the self-copy after one AUTH as the account
 * (the signer asked once); the recipient's relay that demands AUTH only ever
 * sees a throwaway key. */
static void
test_self_copy_account_auth(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  settle_own(&f, ALICE_INBOX, NULL);
  script(&f.transport, BOB_A, ANSWER_OK, TRUE, "");
  script(&f.transport, BOB_B, ANSWER_AUTH_ACCEPT, TRUE, NULL);
  script(&f.transport, ALICE_INBOX, ANSWER_AUTH_ACCEPT, TRUE, NULL);
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "members-only inbox");
  gh_test_spin_until(state_settled, item);
  PubOpen *self_open = last_open(&f.transport, hex_alice, ALICE_INBOX);
  PubOpen *bob_open = last_open(&f.transport, hex_bob, BOB_B);
  g_assert_cmpuint(self_open->auth->len, ==, 1);
  g_assert_cmpuint(self_open->resends, ==, 1);
  g_assert_cmpuint(bob_open->auth->len, ==, 1);
  g_autofree gchar *self_signer = auth_pubkey(self_open, 0);
  g_autofree gchar *bob_signer = auth_pubkey(bob_open, 0);
  g_assert_cmpstr(self_signer, ==, hex_alice);
  g_assert_cmpstr(bob_signer, !=, hex_alice);
  g_assert_cmpuint(f.mock.calls, ==, 5); /* two encryptions, two seals, one AUTH */
  g_assert_false(gh_outbox_item_get_self_copy_missing(item));
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpint(gh_auth_policy_get_account_state(gh_auth_policy_get_for_accounts(f.accounts),
                                                   ALICE_INBOX), ==,
                  GH_AUTH_ACCOUNT_STATE_APPROVED);
  fixture_down(&f);
}

/* A self-copy target that has left the account's own 10050 since sealing is
 * somebody else's relay now: no account AUTH there. */
static void
test_self_copy_former_inbox(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  settle_own(&f, ALICE_INBOX, NULL);
  script(&f.transport, BOB_A, ANSWER_OK, TRUE, "");
  script(&f.transport, BOB_B, ANSWER_OK, TRUE, "");
  script(&f.transport, ALICE_INBOX, ANSWER_OK, FALSE, "error: busy");
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "moving house");
  gh_test_spin_until(round_over, item);
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_WAITING_RETRY);
  /* The account moves its inbox; the stored target stays and is retried. */
  NostrTags *tags = nostr_tags_new(1, nostr_tag_new("relay", SHARED, NULL));
  g_autofree gchar *moved = signed_event(KEY_ALICE, 10050, 200, tags);
  gh_relay_scope_event(own_scope(&f)->scope, DISC, moved);
  script(&f.transport, ALICE_INBOX, ANSWER_AUTH_REFUSE, FALSE, NULL);
  script(&f.transport, SHARED, ANSWER_OK, TRUE, "");
  guint calls = f.mock.calls;
  advance_to_next(&f);
  gh_test_spin_until(round_over, item);
  PubOpen *old = last_open(&f.transport, hex_alice, ALICE_INBOX);
  g_assert_cmpuint(old->auth->len, ==, 1);
  g_autofree gchar *signer = auth_pubkey(old, 0);
  g_assert_cmpstr(signer, !=, hex_alice);
  g_assert_cmpuint(f.mock.calls, ==, calls);
  fixture_down(&f);
}

/* nostrc-qp24.68, charter §4.4 R6: while Grotto asks the user to
 * approve signing in to the own inbox for the self-copy, the item says
 * "Waiting for approval" as long as nobody has the message, and the
 * self-copy's relay reads "Waiting for your approval in Grotto." in
 * the details. Bob's relay accepting it meanwhile is reported as "Sent" at
 * once: the approval never hides that the message went out. */
static void
test_self_copy_waiting_for_approval(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  settle_own(&f, ALICE_INBOX, NULL);
  script(&f.transport, ALICE_INBOX, ANSWER_AUTH_ACCEPT, TRUE, NULL);
  fixture_outbox(&f);
  GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(f.accounts);
  f.mock.hold = TRUE;
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "members-only inbox, slowly");
  approve_calls(&f, 4); /* two encryptions, two seals */
  HeldWait auth = { &f.mock, 1 };
  gh_test_spin_until(held_reached, &auth); /* the self-copy's sign-in */
  g_assert_cmpuint(f.mock.calls, ==, 5);
  g_assert_cmpint(gh_auth_policy_get_account_state(policy, ALICE_INBOX), ==,
                  GH_AUTH_ACCOUNT_STATE_WAITING);
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_PUBLISHING);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_WAITING_FOR_SIGNER);
  g_assert_cmpstr(gh_outbox_item_get_label(item), ==, "Waiting for approval");
  g_assert_cmpstr(gh_outbox_item_get_detail(item), ==,
                  "Approve signing in to your message relay in Grotto.");
  g_assert_cmpstr(target_description(item, hex_alice, ALICE_INBOX), ==,
                  gh_message_status_describe_approval());
  g_assert_cmpstr(target_description(item, hex_bob, BOB_A), ==, "Not answered yet.");

  /* Bob's relay accepts: sent, while the self-copy still waits. */
  relay_ok(&f.transport, hex_bob, BOB_A, TRUE, "");
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpstr(target_description(item, hex_alice, ALICE_INBOX), ==,
                  gh_message_status_describe_approval());
  g_assert_false(gh_outbox_item_get_self_copy_missing(item));
  relay_ok(&f.transport, hex_bob, BOB_B, TRUE, "");

  /* The user approves: the self-copy is signed in as Alice and stored. */
  f.mock.hold = FALSE;
  gh_test_signer_release_all(&f.mock);
  gh_test_spin_until(state_settled, item);
  PubOpen *self_open = last_open(&f.transport, hex_alice, ALICE_INBOX);
  g_assert_cmpuint(self_open->auth->len, ==, 1);
  g_autofree gchar *signer = auth_pubkey(self_open, 0);
  g_assert_cmpstr(signer, ==, hex_alice);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENT);
  g_assert_false(gh_outbox_item_get_self_copy_missing(item));
  g_assert_cmpstr(target_description(item, hex_alice, ALICE_INBOX), ==, "Accepted by this relay.");
  g_assert_cmpint(gh_auth_policy_get_account_state(policy, ALICE_INBOX), ==,
                  GH_AUTH_ACCOUNT_STATE_APPROVED);
  fixture_down(&f);
}

/* nostrc-qp24.68: the relay's publish deadline bounds the relay, never the
 * user. A note to self to an own inbox that demands sign-in waits for the
 * approval past the deadline (1 s here, 30 s by default), as "Waiting for
 * approval", and is stored once the user approves. Before, the deadline
 * ended it as "requires sign-in" (Not sent) while the user was deciding. */
static void
test_approval_outlives_publish_deadline(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  settle_own(&f, ALICE_INBOX, NULL);
  script(&f.transport, ALICE_INBOX, ANSWER_AUTH_ACCEPT, TRUE, NULL);
  f.publish_deadline = 1;
  fixture_outbox(&f);
  f.mock.hold = TRUE;
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_alice, "remember the milk");
  approve_calls(&f, 2); /* the note's encryption and seal */
  HeldWait auth = { &f.mock, 1 };
  gh_test_spin_until(held_reached, &auth);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_WAITING_FOR_SIGNER);
  wait_past_deadline(1);
  PubOpen *open = last_open(&f.transport, hex_alice, ALICE_INBOX);
  g_assert_false(open->closed);
  g_assert_cmpuint(open->auth->len, ==, 0);
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_PUBLISHING);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_WAITING_FOR_SIGNER);

  f.mock.hold = FALSE;
  gh_test_signer_release_all(&f.mock);
  gh_test_spin_until(state_settled, item);
  g_assert_cmpuint(open->auth->len, ==, 1);
  g_assert_cmpuint(open->resends, ==, 1);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpuint(f.mock.calls, ==, 3);
  fixture_down(&f);
}

/* Charter §4.5 S2 / NT-11 (outbox half): a background directory refresh that
 * finds a new relay for the recipient (resolver "changed") makes the outbox
 * publish the same stored wrap there, after the message settled and while it
 * is being sealed alike; nothing is re-sealed. */
static void
test_inbox_changed_signal(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  fake_resolver_set(f.resolver, hex_bob, GH_INBOX_FOUND, BOB_A, NULL);
  script(&f.transport, BOB_A, ANSWER_OK, TRUE, "");
  script(&f.transport, BOB_D, ANSWER_OK, TRUE, "");
  script(&f.transport, ALICE_INBOX, ANSWER_OK, TRUE, "");
  settle_own(&f, ALICE_INBOX, NULL);
  fixture_outbox(&f);

  /* Settled first, then the refresh reports BOB_D. */
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "settled then moved");
  gh_test_spin_until(state_settled, item);
  g_autofree gchar *wrap_id = g_strdup(last_open(&f.transport, hex_bob, BOB_A)->event_id);
  guint lookups = f.resolver->calls;
  fake_resolver_set(f.resolver, hex_bob, GH_INBOX_FOUND, BOB_A, BOB_D, NULL);
  gh_inbox_resolver_emit_changed(GH_INBOX_RESOLVER(f.resolver), hex_alice); /* not a recipient */
  drain();
  g_assert_cmpuint(f.resolver->calls, ==, lookups);
  gh_inbox_resolver_emit_changed(GH_INBOX_RESOLVER(f.resolver), hex_bob);
  wait_opens(&f, 3); /* BOB_A and the self-copy, then BOB_D */
  gh_test_spin_until(state_settled, item);
  g_assert_cmpstr(last_open(&f.transport, hex_bob, BOB_D)->event_id, ==, wrap_id);
  g_assert_cmpuint(count_opens(&f.transport, hex_bob, BOB_A), ==, 1); /* accepted: never again */
  g_assert_cmpuint(count_opens(&f.transport, hex_alice, ALICE_INBOX), ==, 1);
  g_assert_cmpuint(f.resolver->calls, ==, lookups + 1);
  g_assert_cmpuint(f.mock.calls, ==, 4);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENT);

  /* While the signer is still asked to seal: the list is read again before
   * the first publish, so the new relay gets the wrap right away. */
  fake_resolver_set(f.resolver, hex_bob, GH_INBOX_FOUND, BOB_A, NULL);
  f.mock.hold = TRUE;
  g_autoptr(GhOutboxItem) second = send_text(&f, hex_bob, "changed while sealing");
  HeldWait held = { &f.mock, 1 };
  gh_test_spin_until(held_reached, &held);
  fake_resolver_set(f.resolver, hex_bob, GH_INBOX_FOUND, BOB_A, BOB_D, NULL);
  gh_inbox_resolver_emit_changed(GH_INBOX_RESOLVER(f.resolver), hex_bob);
  f.mock.hold = FALSE;
  gh_test_signer_release_all(&f.mock);
  gh_test_spin_until(state_settled, second);
  g_assert_cmpuint(count_opens(&f.transport, hex_bob, BOB_D), ==, 2);
  g_autoptr(GhStoreOutboxEntry) entry = load_entry(&f, gh_outbox_item_get_outbox_id(second));
  GhStoreOutboxEvent *wrap = entry_event(entry, GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP);
  g_assert_cmpstr(last_open(&f.transport, hex_bob, BOB_D)->event_id, ==, wrap->event_id);

  /* Outside the retry window a change is ignored. */
  guint opens = f.transport.opens->len;
  advance_seconds(&f, 72 * 3600 + 1);
  fake_resolver_set(f.resolver, hex_bob, GH_INBOX_FOUND, BOB_A, BOB_D, BOB_B, NULL);
  gh_inbox_resolver_emit_changed(GH_INBOX_RESOLVER(f.resolver), hex_bob);
  drain();
  g_assert_cmpuint(f.transport.opens->len, ==, opens);
  fixture_down(&f);
}

/* ---- OB-10: offline ------------------------------------------------------- */

static void
test_ob10_offline(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  settle_own(&f, ALICE_INBOX, NULL);
  fake_monitor_set_available(f.network, FALSE);
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "offline");
  drain();
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_QUEUED_OFFLINE);
  g_assert_cmpstr(gh_outbox_item_get_label(item), ==, "Waiting for connection");
  advance_seconds(&f, 3600);
  g_assert_cmpuint(f.mock.calls, ==, 0);
  g_assert_cmpuint(f.resolver->calls, ==, 0);
  g_assert_cmpuint(f.transport.opens->len, ==, 0);

  script(&f.transport, BOB_A, ANSWER_FAIL_OPEN, FALSE, NULL);
  script(&f.transport, BOB_B, ANSWER_FAIL_OPEN, FALSE, NULL);
  script(&f.transport, ALICE_INBOX, ANSWER_OK, TRUE, "");
  fake_monitor_set_available(f.network, TRUE);
  wait_opens(&f, 3);
  wait_status(item, GH_MESSAGE_STATUS_RETRYING);
  gh_test_spin_until(round_over, item);
  guint opens = f.transport.opens->len;

  /* A retry that falls due while offline is not attempted. */
  fake_monitor_set_available(f.network, FALSE);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_QUEUED_OFFLINE);
  advance_to_next(&f);
  advance_seconds(&f, 600);
  g_assert_cmpuint(f.transport.opens->len, ==, opens);
  script(&f.transport, BOB_A, ANSWER_OK, TRUE, "");
  fake_monitor_set_available(f.network, TRUE);
  wait_status(item, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpuint(count_opens(&f.transport, hex_bob, BOB_A), ==, 2);
  fixture_down(&f);
}

/* ---- no inbox, signer refusal, Retry, cancel and delete ------------------- */

static void
test_no_inbox_and_retry(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  settle_own(&f, ALICE_INBOX, NULL);
  fake_resolver_set(f.resolver, hex_bob, GH_INBOX_NOT_FOUND);
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "no inbox yet");
  wait_status(item, GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX);
  g_assert_cmpuint(f.mock.calls, ==, 0); /* nothing sealed (PT-4b) */
  g_assert_cmpuint(f.transport.opens->len, ==, 0);
  g_assert_true(gh_outbox_item_get_can_retry(item));
  g_assert_nonnull(strstr(gh_outbox_item_get_detail(item), "hasn't set up private messaging"));

  /* Bob sets up his inbox; the user taps Retry. */
  fake_resolver_set(f.resolver, hex_bob, GH_INBOX_FOUND, BOB_A, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_outbox_retry(f.outbox, gh_outbox_item_get_outbox_id(item), &error));
  g_assert_no_error(error);
  wait_opens(&f, 2);
  relay_ok(&f.transport, hex_bob, BOB_A, TRUE, "");
  wait_status(item, GH_MESSAGE_STATUS_SENT);
  g_assert_false(gh_outbox_item_get_can_retry(item));
  g_assert_false(gh_outbox_retry(f.outbox, gh_outbox_item_get_outbox_id(item), &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  /* Bob's relay has it: it can no longer be "cancelled". */
  g_assert_false(gh_outbox_cancel(f.outbox, gh_outbox_item_get_outbox_id(item), &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENT);
  fixture_down(&f);
}

/* No recipient relay accepts and nothing is left to retry: the message
 * needs attention (never "settled"); the user's Retry reaches the relay that
 * failed with "error:" too often, not the one that refused it as invalid. */
static void
test_relays_refused(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  settle_own(&f, ALICE_INBOX, NULL);
  script(&f.transport, BOB_A, ANSWER_OK, FALSE, "invalid: nope");
  script(&f.transport, BOB_B, ANSWER_OK, FALSE, "error: try later");
  script(&f.transport, ALICE_INBOX, ANSWER_OK, TRUE, "");
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "nobody takes it");
  gint64 id = gh_outbox_item_get_outbox_id(item);
  wait_opens(&f, 3);
  gh_test_spin_until(round_over, item);
  for (guint round = 2; round <= 3; round++) {
    g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_WAITING_RETRY);
    advance_to_next(&f);
    gh_test_spin_until(round_over, item);
  }
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_NEEDS_ATTENTION);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_NOT_SENT);
  g_assert_true(gh_outbox_item_get_can_retry(item));
  g_assert_nonnull(strstr(gh_outbox_item_get_detail(item), "refused it as invalid"));
  g_assert_cmpuint(count_opens(&f.transport, hex_bob, BOB_A), ==, 1);
  g_assert_cmpuint(count_opens(&f.transport, hex_bob, BOB_B), ==, 3);
  g_assert_cmpuint(gh_clock_fake_get_n_timeouts(f.clock), ==, 0);

  script(&f.transport, BOB_B, ANSWER_OK, TRUE, "");
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_outbox_retry(f.outbox, id, &error));
  g_assert_no_error(error);
  gh_test_spin_until(state_settled, item);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpuint(count_opens(&f.transport, hex_bob, BOB_A), ==, 1);
  g_assert_cmpuint(count_opens(&f.transport, hex_bob, BOB_B), ==, 4);
  g_assert_cmpuint(f.mock.calls, ==, 4);
  fixture_down(&f);
}

/* The size bound: the largest text still seals into a wrap (the envelope
 * would fail otherwise); one more step is refused up front, draft kept. */
static void
test_size_bound(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  settle_own(&f, ALICE_INBOX, NULL);
  fixture_outbox(&f);
  g_autofree gchar *fits = g_strnfill(40600, 'x');
  g_autofree gchar *too_long = g_strnfill(40800, 'x');
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_outbox_send(f.outbox, hex_bob, too_long, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_autoptr(GArray) none = gh_store_outbox_list_unfinished(f.store, GH_STORE_BACKEND_NIP17, NULL);
  g_assert_cmpuint(none->len, ==, 0);
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, fits);
  wait_opens(&f, 3);
  g_assert_cmpuint(f.mock.calls, ==, 4);
  g_autoptr(GhStoreOutboxEntry) entry = load_entry(&f, gh_outbox_item_get_outbox_id(item));
  g_assert_cmpuint(entry->events->len, ==, 2);
  g_assert_cmpuint(strlen(entry_event(entry, GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP)->event_json), >,
                   65536);
  fixture_down(&f);
}

static void
test_signer_refusal_and_retry(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  settle_own(&f, ALICE_INBOX, NULL);
  fixture_outbox(&f);
  f.mock.deny = TRUE;
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "denied at first");
  wait_status(item, GH_MESSAGE_STATUS_NOT_SENT);
  g_assert_cmpuint(f.mock.calls, ==, 1);
  g_assert_cmpuint(f.transport.opens->len, ==, 0);
  g_assert_cmpint(gh_outbox_item_get_state(item), ==, GH_STORE_OUTBOX_NEEDS_ATTENTION);
  g_assert_nonnull(strstr(gh_outbox_item_get_detail(item), "Grotto"));
  f.mock.deny = FALSE;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_outbox_retry(f.outbox, gh_outbox_item_get_outbox_id(item), &error));
  wait_opens(&f, 3);
  g_assert_cmpuint(f.mock.calls, ==, 5);
  relay_ok(&f.transport, hex_bob, BOB_B, TRUE, "");
  wait_status(item, GH_MESSAGE_STATUS_SENT);
  fixture_down(&f);
}

static void
on_item_removed(GhOutbox *outbox, GhOutboxItem *item, gpointer data)
{
  (void) outbox;
  *(gint64 *) data = gh_outbox_item_get_outbox_id(item);
}

static void
test_cancel_and_delete(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  settle_own(&f, ALICE_INBOX, NULL);
  script(&f.transport, BOB_A, ANSWER_FAIL_OPEN, FALSE, NULL);
  script(&f.transport, BOB_B, ANSWER_FAIL_OPEN, FALSE, NULL);
  script(&f.transport, ALICE_INBOX, ANSWER_OK, TRUE, "");
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) first = send_text(&f, hex_bob, "to cancel");
  wait_status(first, GH_MESSAGE_STATUS_RETRYING);
  gh_test_spin_until(round_over, first);
  g_autoptr(GhOutboxItem) second = send_text(&f, hex_bob, "to delete");
  wait_status(second, GH_MESSAGE_STATUS_RETRYING);
  gh_test_spin_until(round_over, second);
  guint opens = f.transport.opens->len;

  g_autoptr(GError) error = NULL;
  gint64 first_id = gh_outbox_item_get_outbox_id(first);
  g_assert_true(gh_outbox_cancel(f.outbox, first_id, &error));
  g_assert_no_error(error);
  g_assert_cmpint(gh_outbox_item_get_status(first), ==, GH_MESSAGE_STATUS_CANCELLED);
  g_autoptr(GhStoreOutboxEntry) cancelled = load_entry(&f, first_id);
  g_assert_cmpint(cancelled->state, ==, GH_STORE_OUTBOX_CANCELLED);
  g_assert_cmpint(cancelled->message_id, >, 0); /* the text stays */

  gint64 removed = 0;
  g_signal_connect(f.outbox, "item-removed", G_CALLBACK(on_item_removed), &removed);
  gint64 second_id = gh_outbox_item_get_outbox_id(second);
  gint64 second_message = gh_outbox_item_get_message_id(second);
  g_assert_true(gh_outbox_delete(f.outbox, second_id, &error));
  g_assert_no_error(error);
  g_assert_cmpint(removed, ==, second_id);
  g_assert_cmpint(gh_outbox_item_get_status(second), ==, GH_MESSAGE_STATUS_CANCELLED);
  g_assert_null(gh_outbox_lookup(f.outbox, second_id));
  g_assert_null(gh_outbox_lookup_message(f.outbox, second_message));
  g_autoptr(GPtrArray) targets = gh_outbox_item_dup_targets(second);
  g_assert_cmpuint(targets->len, ==, 0);

  /* Neither is tried again. */
  advance_seconds(&f, 7 * 24 * 3600);
  g_assert_cmpuint(f.transport.opens->len, ==, opens);
  g_assert_cmpuint(gh_clock_fake_get_n_timeouts(f.clock), ==, 0);
  g_autoptr(GhOutboxItem) again = gh_outbox_lookup_message(f.outbox,
                                                           gh_outbox_item_get_message_id(first));
  g_assert_true(again == first);
  fixture_down(&f);
}

static void
test_reaction_same_second_toggle(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  fixture_outbox(&f);
  const gchar *recipients[] = { hex_bob, NULL };
  const gchar *target = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
  g_autoptr(GError) error = NULL;
  g_autofree gchar *first = gh_outbox_send_reaction_room(
    f.outbox, recipients, "+", target, "14", &error);
  g_assert_no_error(error);
  g_assert_nonnull(first);
  g_autofree gchar *deletion = gh_outbox_send_deletion_room(
    f.outbox, recipients, first, &error);
  g_assert_no_error(error);
  g_assert_nonnull(deletion);
  g_autofree gchar *again = gh_outbox_send_reaction_room(
    f.outbox, recipients, "+", target, "14", &error);
  g_assert_no_error(error);
  g_assert_nonnull(again);
  g_assert_cmpstr(first, !=, again);
  g_assert_cmpint(gh_clock_get_unix(f.clock), ==, T0);

  /* The delete references only the first id; the same-second re-add wins. */
  g_autoptr(GhReactionStore) reactions = gh_reaction_store_new();
  gh_reaction_store_set_account(reactions, hex_alice, NULL, NULL, NULL);
  g_autoptr(GhReaction) old =
    gh_reaction_new(target, first, hex_alice, "+", T0, "test-room");
  g_autoptr(GhReaction) latest =
    gh_reaction_new(target, again, hex_alice, "+", T0, "test-room");
  g_assert_true(gh_reaction_store_admit(reactions, old, NULL));
  g_assert_true(gh_reaction_store_remove(reactions, first, NULL));
  g_assert_true(gh_reaction_store_admit(reactions, latest, NULL));
  GhReactionSummary *summary = gh_reaction_store_lookup(reactions, target);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 1);
  g_assert_cmpstr(gh_reaction_summary_own_reaction_id(summary, "+"), ==, again);
  fixture_down(&f);
}

/* NIP-29 and MLS entries in the same store belong to their own engines:
 * the NIP-17 outbox neither resumes nor touches them. */
static void
test_other_backends_untouched(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  settle_own(&f, ALICE_INBOX, NULL);
  gint64 conversation = 0, message = 0;
  g_assert_true(gh_store_ensure_conversation(f.store, GH_STORE_BACKEND_MLS, "abcd", 0,
                                             &conversation, NULL));
  gint64 id = enqueue_plain(f.store, conversation, "mls commit", &message);
  fixture_outbox(&f);
  drain();
  g_autoptr(GPtrArray) items = gh_outbox_dup_items(f.outbox);
  g_assert_cmpuint(items->len, ==, 0);
  g_assert_null(gh_outbox_lookup(f.outbox, id));
  g_assert_null(gh_outbox_lookup_message(f.outbox, message));
  g_autoptr(GError) error = NULL;
  g_assert_false(gh_outbox_retry(f.outbox, id, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND);
  g_clear_error(&error);
  g_assert_false(gh_outbox_delete(f.outbox, id, &error));
  g_autoptr(GhStoreOutboxEntry) entry = load_entry(&f, id);
  g_assert_cmpint(entry->state, ==, GH_STORE_OUTBOX_QUEUED);
  g_assert_cmpuint(entry->attempts, ==, 0);
  g_assert_cmpuint(f.mock.calls, ==, 0);
  g_assert_cmpuint(f.transport.opens->len, ==, 0);
  fixture_down(&f);
}

/* A note to self: one wrap, to the own inbox, no self-copy leg. */
static void
test_note_to_self(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  settle_own(&f, ALICE_INBOX, NULL);
  script(&f.transport, ALICE_INBOX, ANSWER_OK, TRUE, "");
  fixture_outbox(&f);
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_alice, "remember the milk");
  wait_status(item, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpuint(f.mock.calls, ==, 2);
  g_assert_cmpuint(f.transport.opens->len, ==, 1);
  g_assert_false(gh_outbox_item_get_self_copy_missing(item));
  g_autoptr(GhStoreOutboxEntry) entry = load_entry(&f, gh_outbox_item_get_outbox_id(item));
  g_assert_cmpuint(entry->events->len, ==, 1);
  gh_test_spin_until(state_settled, item);
  fixture_down(&f);
}

/* ---- block lifted by writing again (W15 review B1) ------------------------ */

/* A NIP-17 rumor from sender to recipient as the model admits it, with the
 * wrap that delivered it (NULL: a local echo). */
static GhConversationAddResult
admit_rumor(GhConversationStore *model, const gchar *sender, const gchar *recipient,
            gint64 created_at, const gchar *text, const gchar *wrap_id)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *rumor = gh_nip17_rumor_new(sender, recipient, text, created_at, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GhMessage) message = gh_message_new_from_rumor(hex_alice, rumor, &error);
  g_assert_no_error(error);
  GhConversationAddResult result = gh_conversation_store_admit(model, message, wrap_id, &error);
  g_assert_no_error(error);
  return result;
}

static gboolean
room_blocked(GhStoreConversations *conversations, const gchar *room_id)
{
  gboolean blocked = FALSE;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_is_blocked(conversations, room_id, &blocked, &error));
  g_assert_no_error(error);
  return blocked;
}

/* Writing to someone blocked, the way the app sends (T-enqueue through the
 * durable outbox, then the send UI's local echo of the queued rumor), lifts
 * the block: the echo is listed and so is their reply. Before, T-enqueue
 * stored the message first, so its echo was a duplicate that never counted
 * as "new": the echo was hidden and every reply dropped for good. A
 * self-copy a relay delivers never lifts it. */
static void
test_send_lifts_block(void)
{
  Fixture f;
  fixture_up(&f, NULL, NULL);
  fixture_outbox(&f);
  fake_monitor_set_available(f.network, FALSE); /* only T-enqueue matters here */
  g_autoptr(GError) error = NULL;
  g_autoptr(GhConversationStore) model = gh_conversation_store_new();
  g_autoptr(GhStoreConversations) conversations = gh_store_conversations_new(f.store);
  g_assert_true(gh_store_conversations_attach(conversations, model, 0, &error));
  g_assert_no_error(error);
  g_autofree gchar *room_id = strcmp(hex_alice, hex_bob) < 0
    ? g_strconcat(hex_alice, ",", hex_bob, NULL) : g_strconcat(hex_bob, ",", hex_alice, NULL);

  g_autofree gchar *wrap_hello = hex_of("wrap hello");
  g_assert_cmpint(admit_rumor(model, hex_bob, hex_alice, T0 - 100, "hello", wrap_hello), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_true(gh_store_conversations_set_blocked(conversations, room_id, TRUE, &error));
  g_assert_no_error(error);
  g_assert_null(gh_conversation_store_lookup(model, room_id));

  /* A self-copy written on another device after the block: kept, unlisted,
   * and the room stays blocked (the block is this device's, P8). */
  g_autofree gchar *wrap_elsewhere = hex_of("wrap elsewhere");
  g_assert_cmpint(admit_rumor(model, hex_alice, hex_bob, T0 - 50, "from my phone",
                              wrap_elsewhere), ==, GH_CONVERSATION_ADD_HIDDEN);
  g_assert_true(room_blocked(conversations, room_id));

  /* Writing again from here: T-enqueue, then the echo (gh-send-ui.c echo()). */
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "sorry, let's talk");
  g_assert_false(room_blocked(conversations, room_id));
  g_autoptr(GhMessage) echo = gh_message_new_from_rumor(hex_alice,
                                                        gh_outbox_item_get_rumor_json(item),
                                                        &error);
  g_assert_no_error(error);
  g_assert_cmpint(gh_conversation_store_add_message(model, echo, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_no_error(error);
  GhConversation *room = gh_conversation_store_lookup(model, room_id);
  g_assert_nonnull(room);
  g_assert_false(gh_conversation_get_is_request(room));
  g_assert_nonnull(gh_conversation_store_lookup_message(model, gh_outbox_item_get_rumor_id(item)));

  /* Their reply is stored and listed, not recorded as seen only. */
  g_autofree gchar *wrap_reply = hex_of("wrap reply");
  g_assert_cmpint(admit_rumor(model, hex_bob, hex_alice, T0 + 10, "ok", wrap_reply), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 1);

  /* A restart lists the room with everything it kept. */
  g_autoptr(GhConversationStore) restored = gh_conversation_store_new();
  g_assert_true(gh_store_conversations_attach(conversations, restored, 0, &error));
  g_assert_no_error(error);
  room = gh_conversation_store_lookup(restored, room_id);
  g_assert_nonnull(room);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(room)), ==, 4);
  gh_store_conversations_close(conversations);
  fixture_down(&f);
}

#ifdef GROUNDHOG_TEST_WIRE
/* ---- OB-4 on H2: scripted OKs from real local relays, and ephemeral AUTH -- */

/* OK true with an empty message, otherwise OK false with the scripted one. */
static void
wire_scripted_ok(WireRelay *relay, SoupWebsocketConnection *connection, const gchar *event_id,
                 gpointer data)
{
  (void) relay;
  const gchar *message = data;
  wire_send_ok(connection, event_id, message[0] == '\0', message);
}

static void
test_ob4_wire(void)
{
  WireRelay ok = { 0 }, dup = { 0 }, rate = { 0 }, invalid = { 0 }, error_relay = { 0 },
            auth = { 0 }, own = { 0 };
  relay_init(&ok);
  relay_init(&dup);
  relay_init(&rate);
  relay_init(&invalid);
  relay_init(&error_relay);
  relay_init(&auth);
  relay_init(&own);
  ok.on_event = wire_scripted_ok;
  ok.on_event_data = (gpointer) "";
  dup.on_event = wire_scripted_ok;
  dup.on_event_data = (gpointer) "duplicate: already have it";
  rate.on_event = wire_scripted_ok;
  rate.on_event_data = (gpointer) "rate-limited: slow down";
  invalid.on_event = wire_scripted_ok;
  invalid.on_event_data = (gpointer) "invalid: nope";
  error_relay.on_event = wire_scripted_ok;
  error_relay.on_event_data = (gpointer) "error: oops";
  auth.require_auth = TRUE;
  auth.on_event = wire_scripted_ok;
  auth.on_event_data = (gpointer) "";
  own.on_event = wire_scripted_ok;
  own.on_event_data = (gpointer) "";

  Fixture f;
  fixture_up(&f, NULL, NULL);
  fake_resolver_set(f.resolver, hex_bob, GH_INBOX_FOUND, ok.url, dup.url, rate.url, invalid.url,
                    error_relay.url, auth.url, NULL);
  settle_own(&f, own.url, NULL);
  GhOutboxConfig config = {
    .store = f.store, .accounts = f.accounts, .account_relays = f.relays,
    .inboxes = GH_INBOX_RESOLVER(f.resolver), .sender = f.sender,
    .network = G_NETWORK_MONITOR(f.network), .publish_deadline = 10,
  };
  g_autoptr(GError) error = NULL;
  f.outbox = gh_outbox_new(&config, &error); /* the real gnostr transport */
  g_assert_no_error(error);
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "over the wire");
  wait_status(item, GH_MESSAGE_STATUS_SENT);
  gh_test_spin_until(round_over, item);
  g_autoptr(GhStoreOutboxEntry) entry = load_entry(&f, gh_outbox_item_get_outbox_id(item));
  GhStoreOutboxEvent *wrap = entry_event(entry, GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP);
  g_assert_cmpint(stored_class(event_target(wrap, ok.url)), ==, GH_TARGET_CLASS_ACCEPTED);
  g_assert_cmpint(event_target(wrap, dup.url)->ok_prefix, ==, GH_RELAY_OK_PREFIX_DUPLICATE);
  g_assert_cmpint(stored_class(event_target(wrap, dup.url)), ==, GH_TARGET_CLASS_ACCEPTED);
  g_assert_cmpint(event_target(wrap, rate.url)->ok_prefix, ==, GH_RELAY_OK_PREFIX_RATE_LIMITED);
  g_assert_cmpint(stored_class(event_target(wrap, rate.url)), ==, GH_TARGET_CLASS_TRANSIENT);
  g_assert_cmpint(stored_class(event_target(wrap, invalid.url)), ==, GH_TARGET_CLASS_TERMINAL);
  g_assert_cmpint(stored_class(event_target(wrap, error_relay.url)), ==,
                  GH_TARGET_CLASS_TRANSIENT);
  /* The AUTH-gated recipient relay took one ephemeral AUTH, not the account. */
  g_assert_cmpint(stored_class(event_target(wrap, auth.url)), ==, GH_TARGET_CLASS_ACCEPTED);
  g_assert_cmpuint(auth.auth_pubkeys->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(auth.auth_pubkeys, 0), !=, hex_alice);
  g_assert_cmpuint(own.events, ==, 1);
  g_assert_cmpuint(f.mock.calls, ==, 4);
  fixture_down(&f);
  WireRelay *relays[] = { &ok, &dup, &rate, &invalid, &error_relay, &auth, &own };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++)
    relay_clear(relays[i]);
}

/* nostrc-qp24.68 on H2: the real transports, an own inbox relay that takes
 * EVENTs only from signed-in connections, a 3 s publish deadline and a user
 * who takes longer than that to approve the self-copy's sign-in. The
 * self-copy's relay waits for the approval ("Waiting for your approval in
 * Grotto."), then the self-copy is signed in as Alice and stored. */
static void
test_wire_approval_outlives_deadline(void)
{
  WireRelay bob = { 0 }, own = { 0 };
  relay_init(&bob);
  relay_init(&own);
  bob.on_event = wire_scripted_ok;
  bob.on_event_data = (gpointer) "";
  own.require_auth = TRUE;
  own.on_event = wire_scripted_ok;
  own.on_event_data = (gpointer) "";

  Fixture f;
  fixture_up(&f, NULL, NULL);
  fake_resolver_set(f.resolver, hex_bob, GH_INBOX_FOUND, bob.url, NULL);
  settle_own(&f, own.url, NULL);
  GhOutboxConfig config = {
    .store = f.store, .accounts = f.accounts, .account_relays = f.relays,
    .inboxes = GH_INBOX_RESOLVER(f.resolver), .sender = f.sender,
    .network = G_NETWORK_MONITOR(f.network), .publish_deadline = 3,
  };
  g_autoptr(GError) error = NULL;
  f.outbox = gh_outbox_new(&config, &error); /* the real gnostr transport */
  g_assert_no_error(error);
  f.mock.hold = TRUE;
  g_autoptr(GhOutboxItem) item = send_text(&f, hex_bob, "held at the door");
  approve_calls(&f, 4); /* two encryptions, two seals */
  HeldWait auth = { &f.mock, 1 };
  gh_test_spin_until(held_reached, &auth); /* the self-copy's sign-in */
  g_assert_cmpstr(target_description(item, hex_alice, own.url), ==,
                  gh_message_status_describe_approval());
  wait_past_deadline(3);
  g_assert_cmpstr(target_description(item, hex_alice, own.url), ==,
                  gh_message_status_describe_approval());
  g_assert_cmpuint(own.auth_frames, ==, 0);
  g_assert_cmpuint(own.events, ==, 0);

  f.mock.hold = FALSE;
  gh_test_signer_release_all(&f.mock);
  gh_test_spin_until(state_settled, item);
  g_assert_cmpuint(own.events, ==, 1);
  g_assert_cmpuint(own.auth_pubkeys->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(own.auth_pubkeys, 0), ==, hex_alice);
  g_assert_cmpuint(bob.events, ==, 1);
  g_assert_cmpuint(bob.auth_frames, ==, 0);
  g_assert_cmpint(gh_outbox_item_get_status(item), ==, GH_MESSAGE_STATUS_SENT);
  g_assert_false(gh_outbox_item_get_self_copy_missing(item));
  g_assert_cmpuint(f.mock.calls, ==, 5);
  fixture_down(&f);
  relay_clear(&bob);
  relay_clear(&own);
}
#endif

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  hex_alice = gh_test_pub(KEY_ALICE);
  hex_bob = gh_test_pub(KEY_BOB);
  npub_alice = gh_test_npub(KEY_ALICE);
  npub_bob = gh_test_npub(KEY_BOB);
  /* A crash scenario's subprocess forks before any D-Bus use. */
  if (!g_test_subprocess())
    gh_test_bus_up(&shared_bus);

  g_test_add_func("/groundhog/outbox/store-api", test_store_api);
  g_test_add_func("/groundhog/outbox/status/truth-table", test_status_truth_table);
  g_test_add_func("/groundhog/outbox/status/outcome-classes", test_status_outcome_classes);
  g_test_add_func("/groundhog/outbox/status/copy", test_status_copy);
  g_test_add_func("/groundhog/outbox/send", test_send_happy_path);
  g_test_add_func("/groundhog/outbox/crash/enqueue-durability", test_ob1_enqueue_durability);
  g_test_add_func("/groundhog/outbox/crash/no-reseal", test_ob2_no_reseal);
  g_test_add_func("/groundhog/outbox/crash/resume-fanout", test_ob3_resume_fanout);
  g_test_add_func("/groundhog/outbox/outcome-classes", test_ob4_outcome_classes);
  g_test_add_func("/groundhog/outbox/backoff", test_ob5_backoff);
  g_test_add_func("/groundhog/outbox/account-switch", test_ob7_account_switch);
  g_test_add_func("/groundhog/outbox/inbox-changed", test_ob8_new_inbox_relay);
  g_test_add_func("/groundhog/outbox/self-copy-jitter", test_ob9_self_copy_jitter);
  g_test_add_func("/groundhog/outbox/self-copy-account-auth", test_self_copy_account_auth);
  g_test_add_func("/groundhog/outbox/self-copy-former-inbox", test_self_copy_former_inbox);
  g_test_add_func("/groundhog/outbox/self-copy-waiting-for-approval",
                  test_self_copy_waiting_for_approval);
  g_test_add_func("/groundhog/outbox/approval-outlives-publish-deadline",
                  test_approval_outlives_publish_deadline);
  g_test_add_func("/groundhog/outbox/inbox-changed-signal", test_inbox_changed_signal);
  g_test_add_func("/groundhog/outbox/offline", test_ob10_offline);
  g_test_add_func("/groundhog/outbox/no-inbox-retry", test_no_inbox_and_retry);
  g_test_add_func("/groundhog/outbox/signer-refusal", test_signer_refusal_and_retry);
  g_test_add_func("/groundhog/outbox/cancel-delete", test_cancel_and_delete);
  g_test_add_func("/groundhog/outbox/reaction-same-second-toggle",
                  test_reaction_same_second_toggle);
  g_test_add_func("/groundhog/outbox/note-to-self", test_note_to_self);
  g_test_add_func("/groundhog/outbox/other-backends", test_other_backends_untouched);
  g_test_add_func("/groundhog/outbox/relays-refused", test_relays_refused);
  g_test_add_func("/groundhog/outbox/size-bound", test_size_bound);
  g_test_add_func("/groundhog/outbox/send-lifts-block", test_send_lifts_block);
#ifdef GROUNDHOG_TEST_WIRE
  g_test_add_func("/groundhog/outbox/wire", test_ob4_wire);
  g_test_add_func("/groundhog/outbox/wire-approval-outlives-deadline",
                  test_wire_approval_outlives_deadline);
#endif
  int result = g_test_run();
  if (!g_test_subprocess())
    gh_test_bus_down(&shared_bus);
  return result;
}
