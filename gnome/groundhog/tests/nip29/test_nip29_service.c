/* NIP-29 group service (charter §8.2 G20a; qp24.12 acceptance): the same h
 * on two relays isolated, admin vs non-admin, pending join then admission,
 * final denial and closed groups, duplicate, partial member lists,
 * AUTH/CLOSED, restart restore and the outbox resume, plus the NIP-11 relay
 * key fetch.
 *
 * Harnesses: local NIP-29 relays (nip29-relay.h: libsoup WebSocket + NIP-11,
 * relay-signed 39000-39003, membership-enforced writes, join policies, admin
 * rules, NIP-42), the real gnostr relay transports, real SQLCipher stores
 * (test hooks) in private directories, a real account controller with the
 * mock org.nostr.Signer on the private test bus (gh-test-signer.h), a fake
 * GNetworkMonitor and a fake GhClock that starts at the real time (the relay
 * signs with real timestamps). Waits iterate the main context; their
 * deadlines are failure bounds only. */
#include "gh-nip11.h"
#include "gh-nip29-service.h"
#include "gh-nip29-template.h"
#include "gh-store-nip29.h"
#include "gh-test-signer.h"
#include "nip29-relay.h"
#include "../gh-test-port.h"

#include <glib/gstdio.h>

#define STORE_ID "7d0c2a51-3b8e-4f6a-9c1d-2e4f5a6b7c8d"
#define KEY_ALICE 1
#define KEY_BOB   2
#define KEY_CAROL 3

static gchar *hex_alice, *hex_bob, *hex_carol, *npub_alice, *npub_bob;
static GhTestBus shared_bus;

/* ---- helpers --------------------------------------------------------------- */

static void
drain(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
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

/* ---- fake GNetworkMonitor ------------------------------------------------------ */

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
  (void)initable; (void)cancellable; (void)error;
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
  (void)monitor; (void)connectable; (void)cancellable; (void)error;
  return TRUE;
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
  case MONITOR_PROP_CONNECTIVITY: g_value_set_enum(value, G_NETWORK_CONNECTIVITY_FULL); break;
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

/* ---- fixture ------------------------------------------------------------------ */

typedef struct {
  GhTestSigner mock;
  GSettings *settings;
  GhAccountController *accounts;
  GhClock *clock;
  gchar *data_dir;
  GhStore *store;
  GhConversationStore *model;
  FakeMonitor *network;
  GhNip29Service *service;
} Fixture;

static GPtrArray *
fake_list(gpointer data, GError **error)
{
  (void)data; (void)error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
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
    key[i] = (guint8)(0x29 ^ i);
  g_autoptr(GBytes) bytes = g_bytes_new(key, sizeof key);
  GhStoreConfig config = { data_dir, hex_alice, NULL, NULL, clock };
  g_autoptr(GError) error = NULL;
  GhStore *store = gh_store_open_with_key(&config, bytes, STORE_ID, GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(store);
  return store;
}

static void
service_up(Fixture *f)
{
  GhNip29ServiceConfig config = {
    .store = f->store,
    .accounts = f->accounts,
    .conversations = f->model,
    .network = G_NETWORK_MONITOR(f->network),
    .publish_deadline = 10,
    .settings = f->settings,
  };
  g_autoptr(GError) error = NULL;
  f->service = gh_nip29_service_new(&config, &error);
  g_assert_no_error(error);
  g_assert_nonnull(f->service);
}

static void
model_up(Fixture *f)
{
  f->model = gh_conversation_store_new();
  gh_conversation_store_set_account(f->model, hex_alice, NULL, NULL, NULL);
}

static void
fixture_up(Fixture *f)
{
  memset(f, 0, sizeof *f);
  gh_test_signer_up(&shared_bus, &f->mock);
  f->settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(f->settings, "signer-method", "auto");
  g_settings_set_string(f->settings, "current-npub", npub_alice);
  f->accounts = gh_account_controller_new_full(f->settings, shared_bus.client, fake_list, NULL);
  gh_test_spin_until(listed, f->accounts);
  g_assert_cmpint(gh_account_controller_get_state(f->accounts), ==, GH_ACCOUNT_STATE_ACTIVE);
  f->clock = gh_clock_new_fake(g_get_real_time());
  f->data_dir = g_dir_make_tmp("groundhog-nip29-XXXXXX", NULL);
  f->store = store_open(f->data_dir, f->clock);
  f->network = g_object_new(FAKE_TYPE_MONITOR, NULL);
  model_up(f);
  service_up(f);
}

static void
service_down(Fixture *f)
{
  if (f->service)
    gh_test_release(g_steal_pointer(&f->service));
}

/* A restart: the service goes, the store closes, and both come back over a
 * new, empty conversation model. */
static void
restart(Fixture *f)
{
  service_down(f);
  drain();
  gh_store_close(f->store);
  g_clear_object(&f->model);
  f->store = store_open(f->data_dir, f->clock);
  model_up(f);
  service_up(f);
}

static void
fixture_down(Fixture *f)
{
  service_down(f);
  gh_test_release(f->accounts);
  GhTestSenders check = { &shared_bus, &f->mock };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  drain();
  g_clear_object(&f->model);
  g_object_unref(f->network);
  g_object_unref(f->settings);
  gh_store_close(f->store);
  gh_clock_unref(f->clock);
  gh_test_signer_down(&shared_bus, &f->mock);
  rm_rf(f->data_dir);
  g_free(f->data_dir);
}

/* ---- waits ---------------------------------------------------------------------- */

typedef struct {
  GhNip29Room *room;
  gint value;
} RoomWait;

static gboolean
join_is(gpointer data)
{
  RoomWait *wait = data;
  return (gint)gh_nip29_room_get_join_state(wait->room) == wait->value;
}

static gboolean
read_is(gpointer data)
{
  RoomWait *wait = data;
  return (gint)gh_nip29_room_get_read_state(wait->room) == wait->value;
}

static gboolean
members_is(gpointer data)
{
  RoomWait *wait = data;
  return (gint)gh_nip29_room_get_members_state(wait->room) == wait->value;
}

#define wait_join(room, state) \
  G_STMT_START { RoomWait w_ = { (room), (state) }; gh_test_spin_until(join_is, &w_); } G_STMT_END
#define wait_read(room, state) \
  G_STMT_START { RoomWait w_ = { (room), (state) }; gh_test_spin_until(read_is, &w_); } G_STMT_END
#define wait_members(room, state) \
  G_STMT_START { RoomWait w_ = { (room), (state) }; gh_test_spin_until(members_is, &w_); } G_STMT_END

typedef struct {
  GhNip29Room *room;
  const gchar *name;
} NameWait;

static gboolean
name_is(gpointer data)
{
  NameWait *wait = data;
  return g_strcmp0(gh_nip29_room_get_name(wait->room), wait->name) == 0;
}

#define wait_name(room, text) \
  G_STMT_START { NameWait w_ = { (room), (text) }; gh_test_spin_until(name_is, &w_); } G_STMT_END

typedef struct {
  GhNip29Op *op;
  GhNip29OpResult result;
} OpWait;

static gboolean
op_is(gpointer data)
{
  OpWait *wait = data;
  return gh_nip29_op_get_result(wait->op) == wait->result;
}

#define wait_op(op, value) \
  G_STMT_START { OpWait w_ = { (op), (value) }; gh_test_spin_until(op_is, &w_); } G_STMT_END

typedef struct {
  GhNip29Op *op;
  GhClock *clock;
  gboolean retried;
} OkWait;

/* The relay's OK arrived. A publish whose connection dropped before its OK
 * is retried on the store's (fake) clock: time is moved to the retry, as it
 * would pass, and the relay then answers "duplicate:" for the event it has. */
static gboolean
relay_ok(gpointer data)
{
  OkWait *wait = data;
  GhNip29OpResult result = gh_nip29_op_get_result(wait->op);
  if (result == GH_NIP29_OP_RETRYING) {
    wait->retried = TRUE;
    gint64 deadline = gh_clock_fake_get_next_deadline(wait->clock);
    if (deadline >= 0)
      gh_clock_fake_advance(wait->clock,
                            MAX(deadline - gh_clock_get_monotonic_time(wait->clock), 0));
    return FALSE;
  }
  return result == GH_NIP29_OP_ACCEPTED || (wait->retried && result == GH_NIP29_OP_DUPLICATE);
}

#define wait_relay_ok(fixture, operation) \
  G_STMT_START { \
    OkWait w_ = { (operation), (fixture)->clock, FALSE }; \
    gh_test_spin_until(relay_ok, &w_); \
  } G_STMT_END

/* Every operation of the outbox has its final answer. */
static gboolean
ops_settled(gpointer data)
{
  g_autoptr(GPtrArray) ops = gh_nip29_outbox_dup_ops(data);
  for (guint i = 0; i < ops->len; i++)
    if (!gh_nip29_op_result_is_final(gh_nip29_op_get_result(g_ptr_array_index(ops, i))))
      return FALSE;
  return TRUE;
}

typedef struct {
  const guint *counter;
  guint at_least;
} CountWait;

static gboolean
count_reached(gpointer data)
{
  CountWait *wait = data;
  return *wait->counter >= wait->at_least;
}

#define wait_count(counter, n) \
  G_STMT_START { CountWait w_ = { (counter), (n) }; gh_test_spin_until(count_reached, &w_); } G_STMT_END

typedef struct {
  GhConversationStore *model;
  const gchar *room_id;
  guint n;
} MessagesWait;

static gboolean
messages_reached(gpointer data)
{
  MessagesWait *wait = data;
  GhConversation *conversation = gh_conversation_store_lookup(wait->model, wait->room_id);
  return conversation && g_list_model_get_n_items(G_LIST_MODEL(conversation)) >= wait->n;
}

#define wait_messages(model, room_id, count) \
  G_STMT_START { MessagesWait w_ = { (model), (room_id), (count) }; \
                 gh_test_spin_until(messages_reached, &w_); } G_STMT_END

typedef struct {
  GhNip29Room *room;
  const gchar *pubkey;
} MemberWait;

static gboolean
member_listed(gpointer data)
{
  MemberWait *wait = data;
  const GhNip29Group *group = gh_nip29_room_get_group(wait->room);
  return group && gh_nip29_group_lookup_member(group, wait->pubkey) == GH_NIP29_MEMBERSHIP_LISTED;
}

static gboolean
member_gone(gpointer data)
{
  return !member_listed(data);
}

static gboolean
admins_known(gpointer data)
{
  const GhNip29Group *group = gh_nip29_room_get_group(data);
  return group && gh_nip29_group_get_snapshot_id(group, 39001, NULL) &&
         gh_nip29_group_get_snapshot_id(group, 39003, NULL);
}

static GhNip29Room *
join(Fixture *f, Nip29Relay *relay, const gchar *group_id, const gchar *code)
{
  g_autoptr(GError) error = NULL;
  GhNip29Room *room = gh_nip29_service_join(f->service, relay->url, group_id, NULL, code, &error);
  g_assert_no_error(error);
  g_assert_nonnull(room);
  return room;
}

/* A member's message: history (dated an hour ago) before the test's join,
 * live (dated now) after it. */
static NostrEvent *
post_at(Nip29Relay *relay, guint key, const gchar *group_id, const gchar *text, gint64 at)
{
  NostrEvent *event = nip29_member_event(gh_test_secret[key], 9, at, group_id, text);
  nip29_store(relay, event);
  return event; /* owned by the relay */
}
#define member_post(relay, key, group, text) post_at((relay), (key), (group), (text), nip29_past(relay))
#define live_post(relay, key, group, text) post_at((relay), (key), (group), (text), nip29_now(relay))

static GhConversation *
room_conversation(Fixture *f, GhNip29Room *room)
{
  return gh_conversation_store_lookup(f->model, gh_nip29_room_get_room_id(room));
}

/* ---- NIP-11 -------------------------------------------------------------------- */

typedef struct {
  gchar *key;
  GError *error;
  gboolean done;
} Fetch;

static void
on_fetched(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Fetch *fetch = data;
  fetch->key = gh_nip11_fetch_relay_key_finish(result, &fetch->error);
  fetch->done = TRUE;
}

static gboolean
fetch_done(gpointer data)
{
  return ((Fetch *)data)->done;
}

static void
fetch_key(GhNetHttp *http, const gchar *url, Fetch *fetch)
{
  memset(fetch, 0, sizeof *fetch);
  gh_nip11_fetch_relay_key_async(http, url, NULL, on_fetched, fetch);
  gh_test_spin_until(fetch_done, fetch);
}

static void
test_nip11(void)
{
  Nip29Relay relay;
  nip29_relay_init(&relay);
  /* Through GhNetHttp, the one HTTP client, in the configured network mode. */
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "network-mode", "system");
  g_autoptr(GhNetHttp) http = gh_net_http_new(settings);
  Fetch fetch;
  fetch_key(http, relay.url, &fetch);
  g_assert_no_error(fetch.error);
  g_assert_cmpstr(fetch.key, ==, relay.pk);
  g_free(fetch.key);
  g_assert_cmpuint(relay.nip11_gets, ==, 1);

  relay.nip11_no_key = TRUE;
  fetch_key(http, relay.url, &fetch);
  g_assert_error(fetch.error, GH_NIP11_ERROR, GH_NIP11_ERROR_NO_KEY);
  g_clear_error(&fetch.error);
  relay.nip11_no_key = FALSE;

  /* A redirect is never followed, to this host or another. */
  relay.nip11_redirect = TRUE;
  fetch_key(http, relay.url, &fetch);
  g_assert_error(fetch.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error(&fetch.error);
  relay.nip11_redirect = FALSE;

  relay.nip11_huge = TRUE;
  fetch_key(http, relay.url, &fetch);
  g_assert_error(fetch.error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
  g_clear_error(&fetch.error);
  relay.nip11_huge = FALSE;

  /* Tor mode with no Tor listening: an error, never a direct connection
   * (G09; the Tor path itself is tested in tests/net). */
  guint gets = relay.nip11_gets;
  g_autofree gchar *nowhere = g_strdup_printf("127.0.0.1:%u", gh_test_refused_port());
  g_settings_set_string(settings, "tor-socks-address", nowhere);
  g_settings_set_string(settings, "network-mode", "tor");
  fetch_key(http, relay.url, &fetch);
  g_assert_nonnull(fetch.error);
  g_clear_error(&fetch.error);
  g_assert_cmpuint(relay.nip11_gets, ==, gets);
  g_settings_reset(settings, "network-mode");
  g_settings_reset(settings, "tor-socks-address");

  /* A ws:// relay off loopback: no plaintext fetch, nothing is sent. */
  fetch_key(http, "ws://groups.example.org/relay", &fetch);
  g_assert_error(fetch.error, GH_NIP11_ERROR, GH_NIP11_ERROR_PLAINTEXT);
  g_clear_error(&fetch.error);

  g_autoptr(GError) error = NULL;
  g_autofree gchar *secure = gh_nip11_document_url("wss://groups.example.org/nip29", &error);
  g_assert_cmpstr(secure, ==, "https://groups.example.org/nip29");
  g_autofree gchar *plain = gh_nip11_document_url("ws://127.0.0.1:7777", &error);
  g_assert_cmpstr(plain, ==, "http://127.0.0.1:7777/");
  g_assert_null(gh_nip11_document_url("ws://groups.example.org", &error));
  g_assert_error(error, GH_NIP11_ERROR, GH_NIP11_ERROR_PLAINTEXT);
  g_clear_error(&error);
  g_assert_null(gh_nip11_document_url("https://example.org", &error));
  g_assert_error(error, GH_NIP11_ERROR, GH_NIP11_ERROR_INVALID_URL);
  g_clear_error(&error);
  g_assert_null(gh_nip11_document_url("wss://user:pw@example.org", &error));
  g_clear_error(&error);

  /* "self" only: the admin's "pubkey" is never the relay key (W15 review
   * non-blocking #4), whether "self" is missing or malformed. */
  g_autofree gchar *self_key = gh_nip11_parse_relay_key(
    "{\"self\":\"AAAA" "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
    "\"pubkey\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}", -1, &error);
  g_assert_cmpstr(self_key, ==, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  g_assert_null(gh_nip11_parse_relay_key(
    "{\"pubkey\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}", -1, &error));
  g_assert_error(error, GH_NIP11_ERROR, GH_NIP11_ERROR_NO_KEY);
  g_clear_error(&error);
  g_assert_null(gh_nip11_parse_relay_key(
    "{\"self\":\"nope\",\"pubkey\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}",
    -1, &error));
  g_assert_error(error, GH_NIP11_ERROR, GH_NIP11_ERROR_NO_KEY);
  g_clear_error(&error);
  g_assert_null(gh_nip11_parse_relay_key("[1,2]", -1, &error));
  g_assert_error(error, GH_NIP11_ERROR, GH_NIP11_ERROR_MALFORMED);
  nip29_relay_clear(&relay);
}

static gboolean
key_decided(gpointer data)
{
  return gh_nip29_room_get_relay_key_state(data) != GH_NIP29_RELAY_KEY_UNKNOWN;
}

/* A relay whose NIP-11 names only an administrator's "pubkey" (here Bob, a
 * member) and no "self": the relay key is unavailable, so nothing is
 * pinned, Bob cannot speak for the group, and its state stays unverified
 * (held) rather than trusted to a person (W15 review non-blocking #4). */
static void
test_nip11_admin_pubkey_only(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  relay.nip11_pubkey_only = TRUE;
  relay.nip11_pubkey_only_key = hex_bob;
  Nip29TestGroup *group = nip29_add_group(&relay, "pizza", "Pizza Lovers");
  nip29_set_member(&relay, group, hex_bob, NULL);
  g_autoptr(GhNip29Room) room = join(&f, &relay, "pizza", NULL);
  gh_test_spin_until(key_decided, room);
  g_assert_cmpint(gh_nip29_room_get_relay_key_state(room), ==, GH_NIP29_RELAY_KEY_UNAVAILABLE);
  g_assert_null(gh_nip29_room_get_group(room));
  wait_read(room, GH_NIP29_READ_LIVE);
  drain();
  /* The relay's 39000 arrived but was not admitted without a verified key. */
  g_assert_null(gh_nip29_room_get_group(room));
  g_assert_cmpstr(gh_nip29_room_get_name(room), !=, "Pizza Lovers");
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* ---- outcome classes ------------------------------------------------------------ */

static void
test_classify(void)
{
  g_assert_cmpint(gh_nip29_classify_answer(9, GH_RELAY_PUBLISH_ACCEPTED, GH_RELAY_OK_PREFIX_NONE,
                                           "", 1), ==, GH_NIP29_OP_ACCEPTED);
  g_assert_cmpint(gh_nip29_classify_answer(9, GH_RELAY_PUBLISH_ACCEPTED,
                                           GH_RELAY_OK_PREFIX_DUPLICATE, "duplicate: have it", 1),
                  ==, GH_NIP29_OP_DUPLICATE);
  g_assert_cmpint(gh_nip29_classify_answer(9, GH_RELAY_PUBLISH_REJECTED,
                                           GH_RELAY_OK_PREFIX_DUPLICATE, "duplicate: have it", 1),
                  ==, GH_NIP29_OP_DUPLICATE);
  g_assert_cmpint(gh_nip29_classify_answer(9021, GH_RELAY_PUBLISH_REJECTED,
                                           GH_RELAY_OK_PREFIX_DUPLICATE,
                                           "duplicate: already a member", 1),
                  ==, GH_NIP29_OP_DUPLICATE);
  g_assert_cmpint(gh_nip29_classify_answer(9021, GH_RELAY_PUBLISH_REJECTED,
                                           GH_RELAY_OK_PREFIX_RESTRICTED,
                                           "restricted: your request is Pending approval", 1),
                  ==, GH_NIP29_OP_PENDING_APPROVAL);
  /* Prefix-less "waiting for review" is pending too, not a transient error. */
  g_assert_cmpint(gh_nip29_classify_answer(9021, GH_RELAY_PUBLISH_REJECTED,
                                           GH_RELAY_OK_PREFIX_NONE, "an admin will review it", 1),
                  ==, GH_NIP29_OP_PENDING_APPROVAL);
  /* Only a join request can be pending. */
  g_assert_cmpint(gh_nip29_classify_answer(9, GH_RELAY_PUBLISH_REJECTED,
                                           GH_RELAY_OK_PREFIX_RESTRICTED,
                                           "restricted: pending approval", 1),
                  ==, GH_NIP29_OP_REJECTED);
  g_assert_cmpint(gh_nip29_classify_answer(9021, GH_RELAY_PUBLISH_REJECTED,
                                           GH_RELAY_OK_PREFIX_BLOCKED, "blocked: no", 1),
                  ==, GH_NIP29_OP_REJECTED);
  g_assert_cmpint(gh_nip29_classify_answer(9, GH_RELAY_PUBLISH_REJECTED,
                                           GH_RELAY_OK_PREFIX_RATE_LIMITED, "rate-limited: slow",
                                           5), ==, GH_NIP29_OP_RETRYING);
  g_assert_cmpint(gh_nip29_classify_answer(9, GH_RELAY_PUBLISH_REJECTED, GH_RELAY_OK_PREFIX_ERROR,
                                           "error: oops", 1), ==, GH_NIP29_OP_RETRYING);
  g_assert_cmpint(gh_nip29_classify_answer(9, GH_RELAY_PUBLISH_REJECTED, GH_RELAY_OK_PREFIX_ERROR,
                                           "error: oops", GH_MESSAGE_STATUS_ERROR_ATTEMPTS),
                  ==, GH_NIP29_OP_REJECTED);
  g_assert_cmpint(gh_nip29_classify_answer(9, GH_RELAY_PUBLISH_CONNECTION_FAILED,
                                           GH_RELAY_OK_PREFIX_NONE, NULL, 9),
                  ==, GH_NIP29_OP_RETRYING);
  g_assert_cmpint(gh_nip29_classify_answer(9, GH_RELAY_PUBLISH_AUTH_REQUIRED,
                                           GH_RELAY_OK_PREFIX_AUTH_REQUIRED,
                                           "auth-required: members only", 1),
                  ==, GH_NIP29_OP_NOT_SENT);
  g_assert_cmpint(gh_nip29_op_result_to_message_status(GH_NIP29_OP_ACCEPTED, TRUE), ==,
                  GH_MESSAGE_STATUS_SENT);
  g_assert_cmpint(gh_nip29_op_result_to_message_status(GH_NIP29_OP_QUEUED, FALSE), ==,
                  GH_MESSAGE_STATUS_QUEUED_OFFLINE);
  g_assert_cmpint(gh_nip29_op_result_to_message_status(GH_NIP29_OP_REJECTED, TRUE), ==,
                  GH_MESSAGE_STATUS_NOT_SENT);
}

/* ---- join, read and write ----------------------------------------------------------- */

static void
test_join_open_group_and_chat(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *group = nip29_add_group(&relay, "pizza", "Pizza Lovers");
  nip29_set_member(&relay, group, hex_bob, NULL);
  NostrEvent *hello = member_post(&relay, KEY_BOB, "pizza", "hello, pizza people");
  /* Nothing is asked of a group relay before the user joins a group on it. */
  drain();
  g_assert_cmpuint(relay.nip11_gets, ==, 0);
  g_assert_cmpuint(relay.reqs, ==, 0);

  g_autoptr(GhNip29Room) room = join(&f, &relay, "pizza", NULL);
  g_assert_cmpint(gh_nip29_room_get_join_state(room), ==, GH_NIP29_JOIN_REQUESTING);
  /* Listed as a conversation before its first message. */
  GhConversation *conversation = room_conversation(&f, room);
  g_assert_nonnull(conversation);
  g_assert_cmpint(gh_conversation_get_backend(conversation), ==, GH_CONVERSATION_BACKEND_NIP29);
  g_assert_false(gh_conversation_get_is_request(conversation));
  g_assert_null(gh_conversation_get_peers(conversation)[0]);

  wait_join(room, GH_NIP29_JOIN_MEMBER);
  wait_name(room, "Pizza Lovers");
  g_assert_cmpint(gh_nip29_room_get_relay_key_state(room), ==, GH_NIP29_RELAY_KEY_PINNED);
  g_assert_cmpuint(relay.nip11_gets, ==, 1);
  g_assert_cmpstr(gh_nip29_group_get_relay_pubkey(gh_nip29_room_get_group(room)), ==, relay.pk);
  wait_read(room, GH_NIP29_READ_LIVE);
  wait_messages(f.model, gh_nip29_room_get_room_id(room), 1);
  g_assert_cmpstr(gh_conversation_get_title(conversation), ==, "Pizza Lovers");
  g_assert_cmpuint(gh_conversation_get_unread_count(conversation), ==, 0); /* joined history */

  /* A chat message: listed at once, carries h and `previous`, and is
   * accepted by the group relay. */
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip29Op) op = gh_nip29_service_send(f.service, room, "hi all", &error);
  g_assert_no_error(error);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation)), ==, 2);
  wait_relay_ok(&f, op);
  GhMessage *echo = gh_conversation_store_lookup_message(f.model, gh_nip29_op_get_event_id(op));
  g_assert_nonnull(echo);
  g_assert_true(gh_message_is_self(echo));
  g_assert_cmpint(gh_message_get_status(echo), ==, GH_MESSAGE_STATUS_SENT);
  NostrEvent *sent = nip29_last_received(&relay, 9);
  g_assert_nonnull(sent);
  g_assert_cmpstr(nip29_tag_value(sent, "h"), ==, "pizza");
  /* `previous` cites events seen from the group (the relay checked each one
   * exists): Bob's history and the relay's 9000 admitting the account. */
  NostrTag *previous = nip29_tag_find(sent, "previous");
  g_assert_nonnull(previous);
  gboolean cites_hello = FALSE;
  for (size_t i = 1; i < nostr_tag_size(previous); i++)
    cites_hello |= g_str_has_prefix(nip29_event_id(hello), nostr_tag_get(previous, i));
  g_assert_true(cites_hello);
  g_assert_cmpstr(nip29_event_id(sent), ==, gh_nip29_op_get_event_id(op));
  nostr_event_free(sent);

  /* A live message after EOSE; our own event comes back without a copy. */
  live_post(&relay, KEY_BOB, "pizza", "welcome!");
  wait_messages(f.model, gh_nip29_room_get_room_id(room), 3);
  drain();
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation)), ==, 3);
  g_assert_cmpuint(gh_conversation_get_unread_count(conversation), ==, 1);
  /* Nothing here asked to sign in. */
  g_assert_cmpuint(relay.auth_pubkeys->len, ==, 0);
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* The same group id on two relays: two groups, two rooms, nothing shared,
 * and one relay's signed state is not admitted by the other's group. */
static void
test_same_id_two_relays_isolated(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay a, b;
  nip29_relay_init(&a);
  nip29_relay_init(&b);
  Nip29TestGroup *group_a = nip29_add_group(&a, "pizza", "Pizza on A");
  Nip29TestGroup *group_b = nip29_add_group(&b, "pizza", "Pizza on B");
  nip29_set_member(&a, group_a, hex_bob, NULL);
  nip29_set_member(&b, group_b, hex_bob, NULL);
  member_post(&a, KEY_BOB, "pizza", "only on A");
  member_post(&b, KEY_BOB, "pizza", "only on B");

  g_autoptr(GhNip29Room) room_a = join(&f, &a, "pizza", NULL);
  g_autoptr(GhNip29Room) room_b = join(&f, &b, "pizza", NULL);
  g_assert_true(room_a != room_b);
  g_assert_cmpstr(gh_nip29_room_get_room_id(room_a), !=, gh_nip29_room_get_room_id(room_b));
  wait_join(room_a, GH_NIP29_JOIN_MEMBER);
  wait_join(room_b, GH_NIP29_JOIN_MEMBER);
  wait_name(room_a, "Pizza on A");
  wait_name(room_b, "Pizza on B");
  wait_messages(f.model, gh_nip29_room_get_room_id(room_a), 1);
  wait_messages(f.model, gh_nip29_room_get_room_id(room_b), 1);
  wait_read(room_a, GH_NIP29_READ_LIVE);
  wait_read(room_b, GH_NIP29_READ_LIVE);
  GhConversation *conversation_a = room_conversation(&f, room_a);
  GhConversation *conversation_b = room_conversation(&f, room_b);
  g_assert_true(conversation_a != conversation_b);
  g_autoptr(GhMessage) first_a = g_list_model_get_item(G_LIST_MODEL(conversation_a), 0);
  g_autoptr(GhMessage) first_b = g_list_model_get_item(G_LIST_MODEL(conversation_b), 0);
  g_assert_cmpstr(gh_message_get_content(first_a), ==, "only on A");
  g_assert_cmpstr(gh_message_get_content(first_b), ==, "only on B");
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation_a)), ==, 1);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation_b)), ==, 1);

  /* A's relay-signed metadata replayed on B (a fork's state) is not B's. */
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("d", "pizza", NULL));
  nostr_tags_append(tags, nostr_tag_new("name", "Hijacked", NULL));
  nip29_store(&b, nip29_relay_sign(&a, 39000, tags, NULL));
  /* B's group asks B's NIP-11 again (once), finds the same key, drops it. */
  wait_count(&b.nip11_gets, 2);
  drain();
  g_assert_cmpstr(gh_nip29_room_get_name(room_b), ==, "Pizza on B");
  g_assert_cmpstr(gh_nip29_room_get_name(room_a), ==, "Pizza on A");
  g_assert_cmpstr(gh_nip29_group_get_relay_pubkey(gh_nip29_room_get_group(room_b)), ==, b.pk);

  /* A message on A never reaches B's room. */
  live_post(&a, KEY_BOB, "pizza", "second on A");
  wait_messages(f.model, gh_nip29_room_get_room_id(room_a), 2);
  drain();
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation_b)), ==, 1);
  fixture_down(&f);
  nip29_relay_clear(&a);
  nip29_relay_clear(&b);
}

static void
test_pending_then_admission(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *group = nip29_add_group(&relay, "garden", "Gardeners");
  group->join_policy = NIP29_JOIN_PENDING;
  nip29_set_member(&relay, group, hex_carol, "admin");

  g_autoptr(GhNip29Room) room = join(&f, &relay, "garden", NULL);
  wait_join(room, GH_NIP29_JOIN_PENDING);
  g_assert_nonnull(strstr(gh_nip29_room_get_detail(room), "pending approval"));
  g_assert_true(g_hash_table_contains(group->pending, hex_alice));
  wait_read(room, GH_NIP29_READ_LIVE);
  /* The admin admits the request: a relay-signed 9000 and a new 39002. */
  nip29_admit(&relay, group, hex_alice);
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  MemberWait listed_wait = { room, hex_alice };
  gh_test_spin_until(member_listed, &listed_wait);
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

static void
test_denied_and_closed(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *vip = nip29_add_group(&relay, "vip", "VIP");
  vip->join_policy = NIP29_JOIN_DENY;
  Nip29TestGroup *club = nip29_add_group(&relay, "club", "The Club");
  club->closed = TRUE;
  g_ptr_array_add(club->invites, g_strdup("SECRET-1"));
  nip29_publish_state(&relay, club);

  g_autoptr(GhNip29Room) denied = join(&f, &relay, "vip", NULL);
  wait_join(denied, GH_NIP29_JOIN_DENIED);
  g_assert_cmpstr(gh_nip29_room_get_detail(denied), ==, "blocked: you may not join this group");

  g_autoptr(GhNip29Room) closed = join(&f, &relay, "club", NULL);
  wait_join(closed, GH_NIP29_JOIN_CLOSED);
  g_assert_true(gh_nip29_room_get_is_closed(closed));
  /* Nothing is read from a group the account is not in. */
  wait_read(closed, GH_NIP29_READ_IDLE);

  /* With the invite code the same group admits the account. */
  g_autoptr(GhNip29Room) again = join(&f, &relay, "club", "SECRET-1");
  g_assert_true(again == closed);
  wait_join(closed, GH_NIP29_JOIN_MEMBER);
  NostrEvent *request = nip29_last_received(&relay, 9021);
  g_assert_cmpstr(nip29_tag_value(request, "code"), ==, "SECRET-1");
  nostr_event_free(request);

  /* A group that is not joined can be forgotten; one that is, cannot. */
  g_autoptr(GError) error = NULL;
  g_assert_false(gh_nip29_service_forget(f.service, closed, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  g_assert_true(gh_nip29_service_forget(f.service, denied, &error));
  g_assert_no_error(error);
  g_assert_null(room_conversation(&f, denied));
  g_autoptr(GhNip29Room) gone = gh_nip29_service_lookup(f.service, relay.url, "vip");
  g_assert_null(gone);
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* How often the quoted group id appears in a REQ frame. */
static guint
req_mentions(const gchar *frame, const gchar *group_id)
{
  g_autofree gchar *quoted = g_strdup_printf("\"%s\"", group_id);
  guint n = 0;
  for (const gchar *at = strstr(frame, quoted); at; at = strstr(at + 1, quoted))
    n++;
  return n;
}

/* The relay's latest answered REQ no longer asks for "club". */
static gboolean
club_dropped(gpointer data)
{
  Nip29Relay *relay = data;
  return relay->req_frames->len > 0 &&
         req_mentions(g_ptr_array_index(relay->req_frames, relay->req_frames->len - 1),
                      "club") == 0;
}

/* The held REQ that asks for "pub" (the rebuilt one), or NULL. */
static Nip29HeldReq *
held_for_pub(Nip29Relay *relay)
{
  for (guint i = 0; i < relay->held_reqs->len; i++) {
    Nip29HeldReq *held = g_ptr_array_index(relay->held_reqs, i);
    if (req_mentions(held->text, "pub") > 0)
      return held;
  }
  return NULL;
}

static gboolean
pub_asked(gpointer data)
{
  return held_for_pub(data) != NULL;
}

static gboolean
nothing_subscribed(gpointer data)
{
  return ((Nip29Relay *)data)->subs->len == 0;
}

/* nostrc-kfso: the relay's refusal (OK false, on the publish connection)
 * comes before the group state that says the group is closed (on the REQ's
 * connection). The refused room is DENIED at first and reads nothing of the
 * group but its 39000-39003 until that REQ's answer ends: the state makes it
 * CLOSED, and after the EOSE the group is no longer asked for. A group
 * joined meanwhile rebuilds the REQ with the refused group's state only. */
static void
test_refused_before_state(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *club = nip29_add_group(&relay, "club", "The Club");
  club->closed = TRUE;
  nip29_publish_state(&relay, club);
  nip29_add_group(&relay, "pub", "The Pub");
  relay.hold_reqs = TRUE;

  g_autoptr(GhNip29Room) closed = join(&f, &relay, "club", NULL);
  wait_join(closed, GH_NIP29_JOIN_DENIED);
  g_assert_false(gh_nip29_room_get_is_closed(closed));
  g_assert_cmpint(gh_nip29_room_get_read_state(closed), ==, GH_NIP29_READ_IDLE);
  g_assert_cmpstr(gh_nip29_room_get_detail(closed), ==, "restricted: this group is closed");
  /* Its REQ stays: the answer to it carries the state. */
  wait_count(&relay.held_reqs->len, 1);

  g_autoptr(GhNip29Room) open = join(&f, &relay, "pub", NULL);
  gh_test_spin_until(pub_asked, &relay);
  Nip29HeldReq *rebuilt = held_for_pub(&relay);
  g_assert_cmpuint(req_mentions(rebuilt->text, "club"), ==, 1); /* #d of 39000-39003 only */
  g_assert_cmpuint(req_mentions(rebuilt->text, "pub"), >, 1);   /* state and messages */

  nip29_release_reqs(&relay);
  wait_join(closed, GH_NIP29_JOIN_CLOSED);
  g_assert_true(gh_nip29_room_get_is_closed(closed));
  g_assert_cmpstr(gh_nip29_room_get_detail(closed), ==, "restricted: this group is closed");
  gh_test_spin_until(club_dropped, &relay);
  g_assert_cmpint(gh_nip29_room_get_read_state(closed), ==, GH_NIP29_READ_IDLE);
  wait_join(open, GH_NIP29_JOIN_MEMBER);
  wait_read(open, GH_NIP29_READ_LIVE);
  wait_name(open, "The Pub");
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* The relay key comes last (nostrc-kfso): the closed group's 39000 came
 * before the REQ's EOSE but is held back without a key; the refusal left the
 * group unread. When the key arrives the held state is admitted all the
 * same, and the refusal is CLOSED. */
static void
test_refused_state_after_eose(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *club = nip29_add_group(&relay, "club", "The Club");
  club->closed = TRUE;
  nip29_publish_state(&relay, club);
  relay.hold_nip11 = TRUE;

  g_autoptr(GhNip29Room) closed = join(&f, &relay, "club", NULL);
  wait_join(closed, GH_NIP29_JOIN_DENIED);
  wait_count(&relay.nip11_gets, 1);
  wait_count(&relay.reqs, 1);
  /* The REQ answered and closed: nothing of the group is read any more. */
  gh_test_spin_until(nothing_subscribed, &relay);
  g_assert_false(gh_nip29_room_get_is_closed(closed));
  g_assert_cmpint(gh_nip29_room_get_relay_key_state(closed), ==, GH_NIP29_RELAY_KEY_UNKNOWN);

  nip29_release_nip11(&relay);
  wait_join(closed, GH_NIP29_JOIN_CLOSED);
  g_assert_true(gh_nip29_room_get_is_closed(closed));
  g_assert_cmpint(gh_nip29_room_get_relay_key_state(closed), ==, GH_NIP29_RELAY_KEY_PINNED);
  g_assert_cmpint(gh_nip29_room_get_read_state(closed), ==, GH_NIP29_READ_IDLE);
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* "duplicate:" is accepted, never an error: a join of a member is MEMBER. */
static void
test_duplicate(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *group = nip29_add_group(&relay, "old-friends", "Old friends");
  nip29_set_member(&relay, group, hex_alice, NULL);
  g_autoptr(GhNip29Room) room = join(&f, &relay, "old-friends", NULL);
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  g_assert_null(gh_nip29_room_get_detail(room));
  g_autoptr(GPtrArray) ops = gh_nip29_outbox_dup_ops(gh_nip29_service_get_outbox(f.service));
  g_assert_cmpuint(ops->len, ==, 1);
  wait_op(g_ptr_array_index(ops, 0), GH_NIP29_OP_DUPLICATE);
  g_assert_cmpstr(gh_nip29_op_get_relay_message(g_ptr_array_index(ops, 0)), ==,
                  "duplicate: already a member");
  /* Joining a member group again sends nothing. */
  g_autoptr(GhNip29Room) same = join(&f, &relay, "old-friends", NULL);
  g_assert_true(same == room);
  g_autoptr(GPtrArray) after = gh_nip29_outbox_dup_ops(gh_nip29_service_get_outbox(f.service));
  g_assert_cmpuint(after->len, ==, 1);
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

static void
test_admin_and_non_admin(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *kitchen = nip29_add_group(&relay, "kitchen", "Kitchen");
  g_ptr_array_add(kitchen->extra_tags, g_strsplit("x-rules|no pineapple", "|", -1));
  nip29_set_member(&relay, kitchen, hex_alice, "admin");
  nip29_set_member(&relay, kitchen, hex_bob, NULL);
  Nip29TestGroup *diner = nip29_add_group(&relay, "diner", "Diner");
  nip29_set_member(&relay, diner, hex_carol, "admin");
  nip29_set_member(&relay, diner, hex_alice, NULL);
  Nip29TestGroup *cafe = nip29_add_group(&relay, "cafe", "Cafe");
  nip29_set_member(&relay, cafe, hex_carol, "admin");
  nip29_set_member(&relay, cafe, hex_alice, "moderator");
  NostrEvent *spam = member_post(&relay, KEY_BOB, "kitchen", "buy pineapple");
  g_autofree gchar *spam_id = g_strdup(nip29_event_id(spam));

  g_autoptr(GhNip29Room) kitchen_room = join(&f, &relay, "kitchen", NULL);
  g_autoptr(GhNip29Room) diner_room = join(&f, &relay, "diner", NULL);
  g_autoptr(GhNip29Room) cafe_room = join(&f, &relay, "cafe", NULL);
  wait_join(kitchen_room, GH_NIP29_JOIN_MEMBER);
  wait_join(diner_room, GH_NIP29_JOIN_MEMBER);
  wait_join(cafe_room, GH_NIP29_JOIN_MEMBER);
  gh_test_spin_until(admins_known, kitchen_room);
  gh_test_spin_until(admins_known, diner_room);
  gh_test_spin_until(admins_known, cafe_room);
  wait_messages(f.model, gh_nip29_room_get_room_id(kitchen_room), 1);
  /* The member lists can admit the account before its 9021s are answered:
   * let them be, so that "nothing is sent" counts from a settled relay. */
  gh_test_spin_until(ops_settled, gh_nip29_service_get_outbox(f.service));

  /* Non-admin: refused locally, nothing is signed or sent. */
  guint received = relay.received->len;
  guint signs = f.mock.calls;
  g_autoptr(GError) error = NULL;
  g_assert_cmpint(gh_nip29_room_check_permission(diner_room, NOSTR_PERMISSION_PUT_USER), ==,
                  GH_NIP29_AUTHZ_DENIED_NOT_ADMIN);
  g_assert_null(gh_nip29_service_put_user(f.service, diner_room, hex_bob, NULL, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&error);
  drain();
  g_assert_cmpuint(relay.received->len, ==, received);
  g_assert_cmpuint(f.mock.calls, ==, signs);

  /* Unknown role capabilities: attempted, the relay refuses, nothing
   * changes locally. */
  g_assert_cmpint(gh_nip29_room_check_permission(cafe_room, NOSTR_PERMISSION_PUT_USER), ==,
                  GH_NIP29_AUTHZ_UNKNOWN_POLICY);
  g_autoptr(GhNip29Op) refused = gh_nip29_service_put_user(f.service, cafe_room, hex_bob, NULL,
                                                           NULL, &error);
  g_assert_no_error(error);
  wait_op(refused, GH_NIP29_OP_REJECTED);
  g_assert_cmpstr(gh_nip29_op_get_relay_message(refused), ==,
                  "restricted: you are not allowed to do that");
  g_assert_cmpint(gh_nip29_group_lookup_member(gh_nip29_room_get_group(cafe_room), hex_bob), ==,
                  GH_NIP29_MEMBERSHIP_UNKNOWN);

  /* With the relay's role policy the moderator's rights are known. */
  GhNip29RolePolicy *policy = gh_nip29_role_policy_new();
  const nostr_permission_t all[] = { NOSTR_PERMISSION_PUT_USER, NOSTR_PERMISSION_REMOVE_USER,
                                     NOSTR_PERMISSION_EDIT_METADATA,
                                     NOSTR_PERMISSION_DELETE_EVENT,
                                     NOSTR_PERMISSION_CREATE_INVITE };
  const nostr_permission_t delete_only[] = { NOSTR_PERMISSION_DELETE_EVENT };
  g_assert_true(gh_nip29_role_policy_set_role(policy, "admin", all, G_N_ELEMENTS(all), NULL));
  g_assert_true(gh_nip29_role_policy_set_role(policy, "moderator", delete_only, 1, NULL));
  gh_nip29_service_set_role_policy(f.service, relay.url, policy);
  g_assert_cmpint(gh_nip29_room_check_permission(cafe_room, NOSTR_PERMISSION_PUT_USER), ==,
                  GH_NIP29_AUTHZ_DENIED_BY_POLICY);
  g_assert_null(gh_nip29_service_put_user(f.service, cafe_room, hex_bob, NULL, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&error);
  g_assert_cmpint(gh_nip29_room_check_permission(cafe_room, NOSTR_PERMISSION_DELETE_EVENT), ==,
                  GH_NIP29_AUTHZ_ALLOWED);

  /* Admin: every operation goes through and the relay's state follows. */
  g_assert_cmpint(gh_nip29_room_check_permission(kitchen_room, NOSTR_PERMISSION_PUT_USER), ==,
                  GH_NIP29_AUTHZ_ALLOWED);
  const gchar *const roles[] = { "moderator", NULL };
  g_autoptr(GhNip29Op) put = gh_nip29_service_put_user(f.service, kitchen_room, hex_carol, roles,
                                                       "welcome", &error);
  g_assert_no_error(error);
  wait_relay_ok(&f, put);
  MemberWait carol = { kitchen_room, hex_carol };
  gh_test_spin_until(member_listed, &carol);

  g_autoptr(GhNip29Metadata) metadata =
    gh_nip29_group_dup_metadata(gh_nip29_room_get_group(kitchen_room));
  g_free(metadata->name);
  metadata->name = g_strdup("Test Kitchen");
  g_autoptr(GhNip29Op) edit = gh_nip29_service_edit_metadata(f.service, kitchen_room, metadata,
                                                             NULL, &error);
  g_assert_no_error(error);
  wait_relay_ok(&f, edit);
  NostrEvent *edit_event = nip29_last_received(&relay, 9002);
  g_assert_cmpstr(nip29_tag_value(edit_event, "x-rules"), ==, "no pineapple"); /* kept */
  nostr_event_free(edit_event);
  wait_name(kitchen_room, "Test Kitchen");
  g_assert_cmpstr(gh_conversation_get_title(room_conversation(&f, kitchen_room)), ==,
                  "Test Kitchen");

  g_autoptr(GhNip29Op) removed = gh_nip29_service_delete_event(f.service, kitchen_room, spam_id,
                                                               "spam", &error);
  g_assert_no_error(error);
  wait_relay_ok(&f, removed);
  /* The deleted message leaves the room (and is never cited again). */
  g_assert_null(gh_conversation_store_lookup_message(f.model, spam_id));
  g_autoptr(GhNip29Op) invite = gh_nip29_service_create_invite(f.service, kitchen_room,
                                                               "KITCHEN-2", NULL, &error);
  g_assert_no_error(error);
  wait_relay_ok(&f, invite);
  g_assert_cmpstr(g_ptr_array_index(kitchen->invites, kitchen->invites->len - 1), ==, "KITCHEN-2");
  g_autoptr(GhNip29Op) kick = gh_nip29_service_remove_user(f.service, kitchen_room, hex_carol,
                                                           NULL, &error);
  g_assert_no_error(error);
  wait_relay_ok(&f, kick);
  gh_test_spin_until(member_gone, &carol);
  /* The moderator deletes in the cafe (ALLOWED by the policy). */
  NostrEvent *cafe_spam = live_post(&relay, KEY_CAROL, "cafe", "oops");
  g_autoptr(GhNip29Op) moderate = gh_nip29_service_delete_event(
    f.service, cafe_room, nip29_event_id(cafe_spam), NULL, &error);
  g_assert_no_error(error);
  wait_relay_ok(&f, moderate);
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* A 39002 that does not list the account is never "not a member". */
static void
test_partial_member_list(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *group = nip29_add_group(&relay, "crowd", "Big crowd");
  group->partial_members = TRUE;
  nip29_set_member(&relay, group, hex_carol, "admin");
  nip29_set_member(&relay, group, hex_bob, NULL);
  g_autoptr(GhNip29Room) room = join(&f, &relay, "crowd", NULL);
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  wait_members(room, GH_NIP29_MEMBERS_PARTIAL);
  MemberWait carol = { room, hex_carol };
  gh_test_spin_until(member_listed, &carol);
  g_assert_true(g_hash_table_contains(group->members, hex_alice));
  g_assert_cmpint(gh_nip29_group_lookup_member(gh_nip29_room_get_group(room), hex_alice), ==,
                  GH_NIP29_MEMBERSHIP_UNKNOWN);
  g_assert_cmpint(gh_nip29_group_lookup_member(gh_nip29_room_get_group(room), hex_bob), ==,
                  GH_NIP29_MEMBERSHIP_UNKNOWN);
  drain();
  g_assert_cmpint(gh_nip29_room_get_join_state(room), ==, GH_NIP29_JOIN_MEMBER);
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* A private group relay demands NIP-42 AUTH: reads and writes sign in as the
 * account (§4.3 GROUP); a relay that refuses the sign-in is shown as such. */
static void
test_auth_and_closed(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay, refusing;
  nip29_relay_init(&relay);
  nip29_relay_init(&refusing);
  relay.require_auth = TRUE;
  refusing.require_auth = TRUE;
  refusing.refuse_auth = TRUE;
  Nip29TestGroup *secret = nip29_add_group(&relay, "secret", "Secret society");
  secret->private_ = TRUE;
  nip29_set_member(&relay, secret, hex_bob, NULL);
  member_post(&relay, KEY_BOB, "secret", "members only");
  nip29_add_group(&refusing, "locked", "Locked");

  g_autoptr(GhNip29Room) room = join(&f, &relay, "secret", NULL);
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  wait_read(room, GH_NIP29_READ_LIVE);
  wait_messages(f.model, gh_nip29_room_get_room_id(room), 1);
  g_assert_true(gh_nip29_room_get_is_private(room));
  g_assert_cmpuint(relay.closed_reqs, >=, 1);
  g_assert_cmpuint(relay.auth_ok, >=, 2); /* the REQ and the join, each on its connection */
  for (guint i = 0; i < relay.auth_pubkeys->len; i++)
    g_assert_cmpstr(g_ptr_array_index(relay.auth_pubkeys, i), ==, hex_alice);

  g_autoptr(GhNip29Room) locked = join(&f, &refusing, "locked", NULL);
  wait_join(locked, GH_NIP29_JOIN_NOT_SENT);
  g_assert_nonnull(strstr(gh_nip29_room_get_detail(locked), "auth-required"));
  fixture_down(&f);
  nip29_relay_clear(&relay);
  nip29_relay_clear(&refusing);
}

typedef struct {
  GhTestSigner *mock;
  guint held;
} HeldWait;

static gboolean
signer_held(gpointer data)
{
  HeldWait *wait = data;
  return wait->mock->held->len >= wait->held;
}

/* Restart restores joined groups (state, verified snapshots, messages, key)
 * without asking NIP-11 again, and resumes from the sync cursor. */
static void
test_restart_restores(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *group = nip29_add_group(&relay, "pizza", "Pizza Lovers");
  nip29_set_member(&relay, group, hex_bob, NULL);
  member_post(&relay, KEY_BOB, "pizza", "before the restart");
  g_autoptr(GhNip29Room) room = join(&f, &relay, "pizza", NULL);
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  wait_name(room, "Pizza Lovers");
  wait_read(room, GH_NIP29_READ_LIVE);
  wait_messages(f.model, gh_nip29_room_get_room_id(room), 1);
  g_autofree gchar *room_id = g_strdup(gh_nip29_room_get_room_id(room));
  g_clear_object(&room);
  guint gets = relay.nip11_gets;
  guint reqs = relay.reqs;

  restart(&f);
  g_autoptr(GhNip29Room) restored = gh_nip29_service_lookup(f.service, relay.url, "pizza");
  g_assert_nonnull(restored);
  g_assert_cmpint(gh_nip29_room_get_join_state(restored), ==, GH_NIP29_JOIN_MEMBER);
  g_assert_cmpstr(gh_nip29_room_get_name(restored), ==, "Pizza Lovers");
  g_assert_cmpint(gh_nip29_room_get_relay_key_state(restored), ==, GH_NIP29_RELAY_KEY_PINNED);
  g_assert_cmpint(gh_nip29_room_get_members_state(restored), ==, GH_NIP29_MEMBERS_PARTIAL);
  GhConversation *conversation = gh_conversation_store_lookup(f.model, room_id);
  g_assert_nonnull(conversation);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation)), ==, 1);
  g_assert_cmpstr(gh_conversation_get_title(conversation), ==, "Pizza Lovers");
  g_assert_cmpuint(gh_conversation_get_unread_count(conversation), ==, 0);
  wait_count(&relay.reqs, reqs + 1);
  wait_read(restored, GH_NIP29_READ_LIVE);
  g_assert_cmpuint(relay.nip11_gets, ==, gets);
  const gchar *req = g_ptr_array_index(relay.req_frames, relay.req_frames->len - 1);
  g_assert_nonnull(strstr(req, "\"since\""));
  /* It stays subscribed: a new message arrives. */
  live_post(&relay, KEY_BOB, "pizza", "after the restart");
  wait_messages(f.model, room_id, 2);
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* The since of the last REQ's group message filter (0: none). */
static gint64
last_since(Nip29Relay *relay)
{
  g_assert_cmpuint(relay->req_frames->len, >, 0);
  const gchar *req = g_ptr_array_index(relay->req_frames, relay->req_frames->len - 1);
  const gchar *since = strstr(req, "\"since\":");
  return since ? g_ascii_strtoll(since + strlen("\"since\":"), NULL, 10) : 0;
}

/* The since-cursor moves only past what is stored with nothing missing
 * before it (W15 review non-blocking #1): a backfill cut off before its EOSE
 * (the connection dropped, or the app quit) leaves it where it was, so the
 * next REQ asks for that stretch again instead of skipping it for good;
 * the backfill's EOSE then commits it. */
static void
test_cursor_waits_for_eose(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *group = nip29_add_group(&relay, "pizza", "Pizza Lovers");
  nip29_set_member(&relay, group, hex_bob, NULL);
  NostrEvent *first = member_post(&relay, KEY_BOB, "pizza", "an hour ago");
  const gint64 first_at = nostr_event_get_created_at(first);
  g_autoptr(GhNip29Room) room = join(&f, &relay, "pizza", NULL);
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  wait_read(room, GH_NIP29_READ_LIVE);
  g_autofree gchar *room_id = g_strdup(gh_nip29_room_get_room_id(room));
  wait_messages(f.model, room_id, 1);
  g_clear_object(&room);

  /* A message while the app is away (stored, not pushed live). */
  NostrEvent *later = nip29_member_event(gh_test_secret[KEY_BOB], 9, nip29_now(&relay), "pizza",
                                         "while you were away");
  const gint64 later_at = nostr_event_get_created_at(later);
  g_ptr_array_add(relay.events, later);

  /* The next REQ asks from the first backfill's EOSE; this backfill stores
   * the new message but is cut off before its EOSE. */
  relay.hold_eose = TRUE;
  guint reqs = relay.reqs;
  restart(&f);
  wait_count(&relay.reqs, reqs + 1);
  g_assert_cmpint(last_since(&relay), ==, first_at - GH_NIP29_SERVICE_CURSOR_OVERLAP);
  wait_messages(f.model, room_id, 2);
  g_autoptr(GhNip29Room) cut = gh_nip29_service_lookup(f.service, relay.url, "pizza");
  g_assert_cmpint(gh_nip29_room_get_read_state(cut), ==, GH_NIP29_READ_SYNCING);
  g_clear_object(&cut);

  /* So the next REQ still asks from before that stretch. */
  relay.hold_eose = FALSE;
  reqs = relay.reqs;
  restart(&f);
  wait_count(&relay.reqs, reqs + 1);
  g_assert_cmpint(last_since(&relay), ==, first_at - GH_NIP29_SERVICE_CURSOR_OVERLAP);
  g_autoptr(GhNip29Room) whole = gh_nip29_service_lookup(f.service, relay.url, "pizza");
  wait_read(whole, GH_NIP29_READ_LIVE);
  g_clear_object(&whole);

  /* That backfill ended with EOSE: the cursor moved to what it stored. */
  reqs = relay.reqs;
  restart(&f);
  wait_count(&relay.reqs, reqs + 1);
  g_assert_cmpint(last_since(&relay), ==, later_at - GH_NIP29_SERVICE_CURSOR_OVERLAP);
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* A join queued while the signer waits survives a restart: the signer is
 * asked again (it was never signed) and the request then goes out. */
static void
test_outbox_resumes_after_restart(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  nip29_add_group(&relay, "later", "Later");
  f.mock.hold = TRUE;
  g_autoptr(GhNip29Room) room = join(&f, &relay, "later", NULL);
  HeldWait held = { &f.mock, 1 };
  gh_test_spin_until(signer_held, &held);
  g_autoptr(GPtrArray) ops = gh_nip29_outbox_dup_ops(gh_nip29_service_get_outbox(f.service));
  g_assert_cmpuint(ops->len, ==, 1);
  g_assert_cmpint(gh_nip29_op_get_result(g_ptr_array_index(ops, 0)), ==,
                  GH_NIP29_OP_WAITING_FOR_SIGNER);
  g_autofree gchar *event_id = g_strdup(gh_nip29_op_get_event_id(g_ptr_array_index(ops, 0)));
  g_clear_pointer(&ops, g_ptr_array_unref);
  g_clear_object(&room);
  g_assert_cmpuint(relay.events_seen, ==, 0);

  restart(&f);
  held.held = 2;
  gh_test_spin_until(signer_held, &held);
  f.mock.hold = FALSE;
  gh_test_signer_release_all(&f.mock);
  g_autoptr(GhNip29Room) restored = gh_nip29_service_lookup(f.service, relay.url, "later");
  wait_join(restored, GH_NIP29_JOIN_MEMBER);
  NostrEvent *request = nip29_last_received(&relay, 9021);
  g_assert_cmpstr(nip29_event_id(request), ==, event_id); /* the stored event, not a new one */
  nostr_event_free(request);
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* nostrc-cpwf (nostrc-x055): a relay that caps every REQ's stored answer (at
 * 25 here; strfry at 500) and answers newest first. Back after 120 messages,
 * the room's backfill is paged backwards with until, per filter, while the
 * live REQ stays open: every message arrives, the room is LIVE only after
 * the paging, and the cursor then moves past all of it. */
static void
test_backfill_paged_past_relay_cap(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *group = nip29_add_group(&relay, "pizza", "Pizza Lovers");
  nip29_set_member(&relay, group, hex_bob, NULL);
  member_post(&relay, KEY_BOB, "pizza", "before");
  g_autoptr(GhNip29Room) room = join(&f, &relay, "pizza", NULL);
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  wait_read(room, GH_NIP29_READ_LIVE);
  g_autofree gchar *room_id = g_strdup(gh_nip29_room_get_room_id(room));
  wait_messages(f.model, room_id, 1);
  g_clear_object(&room);

  const guint away = 120;
  gint64 newest = 0;
  for (guint i = 0; i < away; i++) {
    g_autofree gchar *text = g_strdup_printf("while away %u", i);
    NostrEvent *event = nip29_member_event(gh_test_secret[KEY_BOB], 9, nip29_now(&relay),
                                           "pizza", text);
    newest = nostr_event_get_created_at(event);
    g_ptr_array_add(relay.events, event);
  }
  relay.max_limit = 25;
  guint frames = relay.req_frames->len;
  restart(&f);
  wait_messages(f.model, room_id, 1 + away);
  g_autoptr(GhNip29Room) back = gh_nip29_service_lookup(f.service, relay.url, "pizza");
  wait_read(back, GH_NIP29_READ_LIVE);
  guint paged = 0;
  for (guint i = frames; i < relay.req_frames->len; i++)
    paged += strstr(g_ptr_array_index(relay.req_frames, i), "\"until\":") != NULL;
  g_assert_cmpuint(paged, >=, away / 25);
  /* Still live on the REQ that was open throughout. */
  live_post(&relay, KEY_BOB, "pizza", "live again");
  wait_messages(f.model, room_id, 2 + away);
  g_clear_object(&back);

  /* That backfill was complete: the next REQ asks from its newest. */
  frames = relay.req_frames->len;
  restart(&f);
  wait_count(&relay.req_frames->len, frames + 1);
  const gchar *req = g_ptr_array_index(relay.req_frames, frames);
  const gchar *since = strstr(req, "\"since\":");
  g_assert_nonnull(since);
  g_assert_cmpint(g_ascii_strtoll(since + strlen("\"since\":"), NULL, 10), >=,
                  newest - GH_NIP29_SERVICE_CURSOR_OVERLAP);
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

static void
test_leave_and_removed(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  nip29_add_group(&relay, "bye", "Goodbyes");
  Nip29TestGroup *strict = nip29_add_group(&relay, "strict", "Strict");
  g_autoptr(GhNip29Room) room = join(&f, &relay, "bye", NULL);
  g_autoptr(GhNip29Room) kicked = join(&f, &relay, "strict", NULL);
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  wait_join(kicked, GH_NIP29_JOIN_MEMBER);
  wait_read(kicked, GH_NIP29_READ_LIVE);

  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip29Op) leave = gh_nip29_service_leave(f.service, room, "moving on", &error);
  g_assert_no_error(error);
  g_assert_cmpint(gh_nip29_room_get_join_state(room), ==, GH_NIP29_JOIN_LEAVING);
  wait_relay_ok(&f, leave);
  wait_join(room, GH_NIP29_JOIN_LEFT);
  wait_read(room, GH_NIP29_READ_IDLE);
  g_assert_null(gh_nip29_service_send(f.service, room, "still here?", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED);
  g_clear_error(&error);

  /* An admin (here the relay) removes the account from the other group. */
  nip29_remove(&relay, strict, hex_alice, "spam");
  wait_join(kicked, GH_NIP29_JOIN_REMOVED);
  g_assert_cmpstr(gh_nip29_room_get_detail(kicked), ==, "spam");
  drain();
  g_assert_cmpint(gh_nip29_room_get_join_state(room), ==, GH_NIP29_JOIN_LEFT);
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  hex_alice = gh_test_pub(KEY_ALICE);
  hex_bob = gh_test_pub(KEY_BOB);
  hex_carol = gh_test_pub(KEY_CAROL);
  npub_alice = gh_test_npub(KEY_ALICE);
  npub_bob = gh_test_npub(KEY_BOB);
  gh_test_bus_up(&shared_bus);
  g_test_add_func("/groundhog/nip29-service/nip11", test_nip11);
  g_test_add_func("/groundhog/nip29-service/nip11-admin-pubkey-only",
                  test_nip11_admin_pubkey_only);
  g_test_add_func("/groundhog/nip29-service/classify", test_classify);
  g_test_add_func("/groundhog/nip29-service/join-and-chat", test_join_open_group_and_chat);
  g_test_add_func("/groundhog/nip29-service/same-id-two-relays", test_same_id_two_relays_isolated);
  g_test_add_func("/groundhog/nip29-service/pending-then-admission", test_pending_then_admission);
  g_test_add_func("/groundhog/nip29-service/denied-and-closed", test_denied_and_closed);
  g_test_add_func("/groundhog/nip29-service/refused-before-state", test_refused_before_state);
  g_test_add_func("/groundhog/nip29-service/refused-state-after-eose",
                  test_refused_state_after_eose);
  g_test_add_func("/groundhog/nip29-service/duplicate", test_duplicate);
  g_test_add_func("/groundhog/nip29-service/admin-and-non-admin", test_admin_and_non_admin);
  g_test_add_func("/groundhog/nip29-service/partial-member-list", test_partial_member_list);
  g_test_add_func("/groundhog/nip29-service/auth-and-closed", test_auth_and_closed);
  g_test_add_func("/groundhog/nip29-service/restart-restores", test_restart_restores);
  g_test_add_func("/groundhog/nip29-service/outbox-resumes", test_outbox_resumes_after_restart);
  g_test_add_func("/groundhog/nip29-service/cursor-waits-for-eose", test_cursor_waits_for_eose);
  g_test_add_func("/groundhog/nip29-service/backfill-paged-past-relay-cap",
                  test_backfill_paged_past_relay_cap);
  g_test_add_func("/groundhog/nip29-service/leave-and-removed", test_leave_and_removed);
  gint rc = g_test_run();
  gh_test_bus_down(&shared_bus);
  g_free(hex_alice);
  g_free(hex_bob);
  g_free(hex_carol);
  g_free(npub_alice);
  g_free(npub_bob);
  return rc;
}
