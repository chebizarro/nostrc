/* GhAccountStore at app level (privacy charter §8.2 G04; tests ST-8, KC-2,
 * KC-3, KC-4 and PT-6 of §9.2): a real account controller on a private bus
 * with the mock signer, the FakeSecret key backend (H5, which records every
 * call's flags), recording relay transports (H1), and the real NIP-17 inbox,
 * store delegate (G05), outbox (G06) and SQLCipher stores in private
 * directories. Nothing sleeps: every wait is on an observable condition.
 *
 * Two runs of this binary (two CTest entries): the default one, a bus per
 * case and no GTK; and --gui, the window's history paging after a restart
 * and the Start Fresh confirmation (W13b review B1 and non-blocking #3) on
 * the executable's wiring, with one bus for the process brought up before
 * GTK (which keeps its session connection); it exits 77 without a display. */
#include "account-store-outbox.h"
#include "gh-account-store.h"
#include "gh-app-outbox.h"
#include "gh-reaction-store.h"
#include "gh-conversation-list.h"
#include "gh-conversation-view.h"
#include "gh-conversation-private.h"
#include "gh-inbox-resolver.h"
#include "gh-nip17-inbox.h"
#include "gh-nip04-inbox.h"
#include "gh-status.h"
#include "gh-signer.h"
#include "gh-inbox-status.h"
#include "gh-store-status.h"
#include "gh-test-signer.h"
#include "gh-test-bunker.h"
#include "fake-secret.h"

#include "nostr-tag.h"
#include "nostr/nip59/nip59.h"
#include "nostr/nip04.h"
#include "secure_buf.h"

#include "nostrc-test-gdk-frame.h"

#include <fcntl.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <unistd.h>
#ifdef __linux__
#include <pthread.h>
#include <semaphore.h>
#include <sys/syscall.h>
#endif

#define DISCOVERY "wss://discovery.test.invalid"
#define INBOX_A "wss://inbox-a.test.invalid"
#define CANARY "GROUNDHOG-CANARY-account-store"

static gchar *npub[GH_TEST_KEYS];
static gchar *hex[GH_TEST_KEYS];
static gchar *xdg_root;
/* --gui: every case shares the one bus GTK was initialized with. */
static gboolean gui_mode;
static GhTestBus shared_bus;

void groundhog_register_resource(void);

/* ---- recording relay transport (H1) ------------------------------------------ */

typedef struct {
  GhRelayScope *scope;
  gchar *url;
  gint64 since;
  gboolean closed;
  gboolean legacy_outgoing;
} Req;

typedef struct {
  GPtrArray *reqs;      /* Req: every DM inbox REQ ever opened */
  GPtrArray *discovery; /* GhRelayScope: every relay-list discovery REQ */
  GPtrArray *legacy;    /* NIP-04 REQs */
} Recorder;

static gchar discovery_handle;

static void
req_free(gpointer data)
{
  Req *req = data;
  gh_relay_scope_unref(req->scope);
  g_free(req->url);
  g_free(req);
}

static gpointer
recorder_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters,
              gpointer data, GError **error)
{
  Recorder *rec = data;
  (void)error;
  g_assert_cmpuint(filters->count, ==, 1);
  const NostrFilter *filter = &filters->filters[0];
  if (nostr_filter_kinds_len(filter) == 2) {
    g_assert_cmpstr(url, ==, DISCOVERY);
    g_ptr_array_add(rec->discovery, gh_relay_scope_ref(scope));
    return &discovery_handle;
  }
  int kind = nostr_filter_kinds_get(filter, 0);
  g_assert_true(kind == 1059 || kind == 4);
  Req *req = g_new0(Req, 1);
  req->scope = gh_relay_scope_ref(scope);
  req->url = g_strdup(url);
  req->since = nostr_filter_get_since_i64(filter);
  if (kind == 4) {
    req->legacy_outgoing = nostr_filter_authors_len(filter) == 1;
    g_ptr_array_add(rec->legacy, req);
  } else {
    g_ptr_array_add(rec->reqs, req);
  }
  return req;
}

static void
recorder_close(gpointer handle, gpointer data)
{
  (void)data;
  if (handle != &discovery_handle)
    ((Req *)handle)->closed = TRUE;
}

static const GhRelayTransport recorder_transport = { recorder_open, recorder_close };

static gboolean
recorder_send_auth(gpointer handle, const gchar *json, gpointer data, GError **error)
{
  (void)handle; (void)json; (void)data; (void)error;
  return TRUE;
}

static void
recorder_resubscribe(gpointer handle, gpointer data)
{
  (void)handle; (void)data;
}

static const GhRelayAuthTransport recorder_auth = { recorder_send_auth, recorder_resubscribe };

static guint
open_reqs(Recorder *rec)
{
  guint n = 0;
  for (guint i = 0; i < rec->reqs->len; i++)
    n += !((Req *)g_ptr_array_index(rec->reqs, i))->closed;
  return n;
}

static Req *
open_req(Recorder *rec, const gchar *url)
{
  for (guint i = 0; i < rec->reqs->len; i++) {
    Req *req = g_ptr_array_index(rec->reqs, i);
    if (!req->closed && g_str_equal(req->url, url))
      return req;
  }
  return NULL;
}

/* Publish transport for the outbox: records, never answers. */
static gchar publish_handle;
static guint publish_opens;

static gpointer
pub_open(GhRelayPublish *publish, const gchar *url, const gchar *event_json, gpointer data,
         GError **error)
{
  (void)publish; (void)url; (void)event_json; (void)data; (void)error;
  publish_opens++;
  return &publish_handle;
}

static void
pub_close(gpointer handle, gpointer data)
{
  (void)handle; (void)data;
}

static const GhRelayPublishTransport pub_transport = { pub_open, pub_close };

/* ---- fake GhInboxResolver: nobody has an inbox ------------------------------ */

#define FAKE_TYPE_RESOLVER (fake_resolver_get_type())
G_DECLARE_FINAL_TYPE(FakeResolver, fake_resolver, FAKE, RESOLVER, GObject)

struct _FakeResolver {
  GObject parent_instance;
};

static void
fake_resolve_async(GhInboxResolver *resolver, const gchar *pubkey, GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer data)
{
  GTask *task = g_task_new(resolver, cancellable, callback, data);
  GhInboxResult *result = g_new0(GhInboxResult, 1);
  result->status = GH_INBOX_NOT_FOUND;
  result->recipient = g_strdup(pubkey);
  result->sources = result->answered = 1;
  g_task_return_pointer(task, result, (GDestroyNotify)gh_inbox_result_free);
  g_object_unref(task);
}

static GhInboxResult *
fake_resolve_finish(GhInboxResolver *resolver, GAsyncResult *result, GError **error)
{
  (void)resolver;
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

static void fake_resolver_class_init(FakeResolverClass *klass) { (void)klass; }
static void fake_resolver_init(FakeResolver *self) { (void)self; }

/* ---- fixture --------------------------------------------------------------------- */

typedef struct {
  GhTestBus bus;
  GhTestSigner signer;
  Recorder rec;
  GSettings *settings;
  GSettings *gnostr;
  FakeSecret *secret;
  gchar *root;
  gchar *data_dir;
  gchar *state_dir;
  gint64 list_time;
  gboolean with_outbox;
  gboolean remote;
  TestBunker bunker;
  /* The stack: what one Groundhog process owns. */
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GhConversationStore *model;
  GhDmInbox *inbox;
  GhNip04Inbox *nip04;
  GhStoreKey *store_key;
  FakeResolver *resolver;
  GhAppOutbox *sender;
  GhAccountStore *store;
  GhClock *clock;           /* NULL = the system clock */
  GPtrArray *events;        /* "opening <acct>", "closed <acct>", "outbox-gone <acct>" */
  gchar *other_store_path;  /* PT-6: must have no open fd when a store opens */
  gboolean fd_checked;
} Fixture;

static GPtrArray *
fake_list(gpointer data, GError **error)
{
  (void)data;
  (void)error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  for (guint key = 1; key <= 2; key++) {
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup(npub[key]);
    info->label = g_strdup_printf("Key %u", key);
    g_ptr_array_add(ids, info);
  }
  return ids;
}

static gboolean
listed(gpointer data)
{
  return gh_account_controller_get_state(data) != GH_ACCOUNT_STATE_DISCOVERING;
}

static GPtrArray *
remote_list(gpointer data, GError **error)
{
  (void)data;
  (void)error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
  info->npub = g_strdup(npub[2]);
  g_ptr_array_add(ids, info);
  return ids;
}

static GhNip46Session *
remote_session(const gchar *active_npub, gpointer data)
{
  Fixture *f = data;
  g_assert_cmpstr(active_npub, ==, npub[2]);
  const gchar *const relays[] = { DISCOVERY, NULL };
  g_autoptr(GError) error = NULL;
  GhNip46Session *session = gh_nip46_session_new(f->bunker.client_secret,
    f->bunker.signer_pubkey, relays, &bunker_scope_transport, NULL,
    &bunker_publish_transport, NULL, &f->bunker, &error);
  g_assert_no_error(error);
  return session;
}

/* Whether any open descriptor of this process refers to path (or its -wal
 * or -shm): independent evidence that a store is really closed. */
static gboolean
fd_open_on(const gchar *path)
{
  char real[PATH_MAX];
  if (!realpath(path, real))
    return FALSE;
  for (int fd = 0; fd < 4096; fd++) {
    char target[PATH_MAX] = { 0 };
#ifdef __APPLE__
    if (fcntl(fd, F_GETPATH, target) == -1)
      continue;
#else
    g_autofree gchar *link = g_strdup_printf("/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n < 0)
      continue;
    target[n] = '\0';
#endif
    if (g_str_has_prefix(target, real))
      return TRUE;
  }
  return FALSE;
}

static void
on_store_opening(GhAccountStore *store, const gchar *account, Fixture *f)
{
  (void)store;
  g_ptr_array_add(f->events, g_strdup_printf("opening %s", account));
  if (f->other_store_path) {
    g_assert_false(fd_open_on(f->other_store_path));
    f->fd_checked = TRUE;
  }
}

static void
on_store_closed(GhAccountStore *store, const gchar *account, Fixture *f)
{
  (void)store;
  /* The inbox let go of the store before it closed. */
  g_assert_false(gh_dm_inbox_has_storage(f->inbox));
  g_ptr_array_add(f->events, g_strdup_printf("closed %s", account));
}

static void
on_outbox_gone(gpointer data, GObject *outbox)
{
  Fixture *f = data;
  (void)outbox;
  g_ptr_array_add(f->events, g_strdup("outbox-gone"));
}

/* The application's factory (gh-app-outbox.c), watched. */
static GObject *
make_outbox(GhStore *store, gpointer data, GError **error)
{
  Fixture *f = data;
  GObject *outbox = gh_app_outbox_create(store, f->sender, error);
  if (outbox)
    g_object_weak_ref(outbox, on_outbox_gone, f);
  return outbox;
}

static void
stack_up(Fixture *f)
{
  if (f->remote) {
    g_settings_set_string(f->settings, "current-npub", "");
    g_settings_set_string(f->settings, "current-backend", "grotto");
    f->accounts = gh_account_controller_new_full_with_remote_list(
      f->settings, f->bus.client, fake_list, NULL, remote_list, f);
    gh_account_controller_set_session_factory_for_test(f->accounts, remote_session, f);
    gh_test_spin_until(listed, f->accounts);
    g_assert_true(gh_account_controller_select_backend(f->accounts,
      GH_SIGNER_BACKEND_NIP46, npub[2], NULL));
  } else {
    f->accounts = gh_account_controller_new_full(f->settings, f->bus.client, fake_list, NULL);
    gh_test_spin_until(listed, f->accounts);
  }
  f->relays = gh_account_relays_new(f->accounts, f->settings, &recorder_transport, &f->rec);
  f->model = gh_conversation_store_new();
  f->inbox = gh_dm_inbox_new_with_storage(f->accounts, f->relays, f->model,
                                          &recorder_transport, &recorder_auth, &f->rec);
  if (f->remote)
    f->nip04 = gh_nip04_inbox_new(f->accounts, f->relays, f->model, f->inbox,
                                  &recorder_transport, &f->rec);
  f->store_key = gh_store_key_new(GH_STORE_KEY_BACKEND(f->secret));
  f->resolver = g_object_new(FAKE_TYPE_RESOLVER, NULL);
  GhAppOutboxConfig outbox_config = {
    .accounts = f->accounts,
    .account_relays = f->relays,
    .inboxes = GH_INBOX_RESOLVER(f->resolver),
    .transport = &pub_transport,
  };
  f->sender = gh_app_outbox_new(&outbox_config);
  GhAccountStoreConfig config = {
    .accounts = f->accounts,
    .store_key = f->store_key,
    .conversations = f->model,
    .inbox = f->inbox,
    .settings = f->settings,
    .data_dir = f->data_dir,
    .legacy_state_dir = f->state_dir,
    .create_outbox = f->with_outbox ? make_outbox : NULL,
    .outbox_data = f,
    .clock = f->clock,
  };
  f->store = gh_account_store_new(&config);
  g_signal_connect(f->store, "store-opening", G_CALLBACK(on_store_opening), f);
  g_signal_connect(f->store, "store-closed", G_CALLBACK(on_store_closed), f);
}

static void
stack_down(Fixture *f)
{
  /* The app's teardown order (gh-app-services.c): account store first. */
  gh_test_release(g_steal_pointer(&f->store));
  g_clear_pointer(&f->sender, gh_app_outbox_free);
  g_clear_object(&f->resolver);
  gh_test_release(g_steal_pointer(&f->store_key));
  if (f->nip04)
    gh_test_release(g_steal_pointer(&f->nip04));
  gh_test_release(g_steal_pointer(&f->inbox));
  g_clear_object(&f->model);
  gh_test_release(g_steal_pointer(&f->relays));
  gh_test_release(g_steal_pointer(&f->accounts));
  GhTestSenders check = { &f->bus, &f->signer };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  g_ptr_array_set_size(f->rec.reqs, 0);
  g_ptr_array_set_size(f->rec.discovery, 0);
  g_ptr_array_set_size(f->rec.legacy, 0);
}

static void
fixture_up(Fixture *f, guint active_key)
{
  g_autoptr(GError) error = NULL;
  if (gui_mode)
    f->bus = shared_bus;
  else
    gh_test_bus_up(&f->bus);
  gh_test_signer_up(&f->bus, &f->signer);
  f->rec.reqs = g_ptr_array_new_with_free_func(req_free);
  f->rec.discovery = g_ptr_array_new_with_free_func((GDestroyNotify)gh_relay_scope_unref);
  f->rec.legacy = g_ptr_array_new_with_free_func(req_free);
  f->events = g_ptr_array_new_with_free_func(g_free);
  f->settings = g_settings_new("org.nostr.Groundhog");
  f->gnostr = g_settings_new("org.gnostr.Client");
  g_settings_set_string(f->gnostr, "current-npub", npub[1]);
  const gchar *sources[] = { DISCOVERY, NULL };
  g_settings_set_strv(f->settings, "discovery-relays", sources);
  g_settings_set_string(f->settings, "signer-method", "auto");
  g_settings_set_string(f->settings, "current-backend", "grotto");
  g_settings_set_int(f->settings, "backend-migration-version", 1);
  if (f->remote) {
    bunker_init(&f->bunker, gh_test_secret[3]);
    g_free(f->bunker.signer_secret);
    g_free(f->bunker.signer_pubkey);
    g_free(f->bunker.user_pubkey);
    f->bunker.signer_secret = g_strdup(gh_test_secret[2]);
    f->bunker.signer_pubkey = g_strdup(hex[2]);
    f->bunker.user_pubkey = g_strdup(hex[2]);
  }
  g_settings_set_string(f->settings, "current-npub", active_key && !f->remote ?
                        npub[active_key] : "");
  f->secret = fake_secret_new();
  f->root = g_dir_make_tmp("groundhog-account-store-XXXXXX", &error);
  g_assert_no_error(error);
  f->data_dir = g_build_filename(f->root, "data", NULL);
  f->state_dir = g_build_filename(f->root, "state", NULL);
  g_assert_cmpint(g_mkdir(f->data_dir, 0700), ==, 0);
  f->list_time = 1700000000;
}

static void
fixture_down(Fixture *f)
{
  if (f->store)
    stack_down(f);
  gh_test_signer_down(&f->bus, &f->signer);
  g_ptr_array_unref(f->rec.reqs);
  g_ptr_array_unref(f->rec.discovery);
  g_ptr_array_unref(f->rec.legacy);
  g_ptr_array_unref(f->events);
  g_settings_reset(f->gnostr, "current-npub");
  g_object_unref(f->gnostr);
  g_object_unref(f->settings);
  g_object_unref(f->secret);
  if (!gui_mode)
    gh_test_bus_down(&f->bus);
  gh_test_remove_tree(f->root);
  g_free(f->root);
  g_free(f->data_dir);
  g_free(f->state_dir);
  g_free(f->other_store_path);
  g_clear_pointer(&f->clock, gh_clock_unref);
  if (f->remote)
    bunker_clear(&f->bunker);
}

static gboolean
state_settled(gpointer data)
{
  GhAccountStoreState state = gh_account_store_get_state(data);
  return state != GH_ACCOUNT_STORE_OPENING;
}

static GhAccountStoreState
settle(Fixture *f)
{
  gh_test_spin_until(state_settled, f->store);
  gh_test_run_until_idle();
  return gh_account_store_get_state(f->store);
}

/* The account's own inbox list, published through the discovery scope. */
static void
publish_inbox_list(Fixture *f, guint key)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 10050);
  nostr_event_set_created_at(event, ++f->list_time);
  nostr_event_set_content(event, "");
  nostr_event_set_tags(event, nostr_tags_new(1, nostr_tag_new("relay", INBOX_A, NULL)));
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[key]), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_assert_cmpuint(f->rec.discovery->len, >, 0);
  gh_relay_scope_event(g_ptr_array_index(f->rec.discovery, f->rec.discovery->len - 1),
                       DISCOVERY, json);
  free(json);
  gh_test_run_until_idle();
}

static void
publish_nip04_relay_list(Fixture *f)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 10002);
  nostr_event_set_created_at(event, ++f->list_time);
  nostr_event_set_content(event, "");
  nostr_event_set_tags(event, nostr_tags_new(1, nostr_tag_new("r", INBOX_A, NULL)));
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[2]), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gh_relay_scope_event(g_ptr_array_index(f->rec.discovery, f->rec.discovery->len - 1),
                       DISCOVERY, json);
  free(json);
  gh_test_run_until_idle();
}

static Req *
legacy_incoming_req(Fixture *f)
{
  for (guint i = 0; i < f->rec.legacy->len; i++) {
    Req *req = g_ptr_array_index(f->rec.legacy, i);
    if (!req->closed && !req->legacy_outgoing && g_str_equal(req->url, INBOX_A))
      return req;
  }
  return NULL;
}

/* A NIP-17 message from key `from` to key `to`, gift-wrapped (NIP-59);
 * *rumor_id (nullable) is its rumor's id. */
static gchar *
craft_wrap(guint from, guint to, gint64 created_at, const gchar *content, gchar **rumor_id)
{
  NostrEvent *rumor = nostr_event_new();
  nostr_event_set_kind(rumor, 14);
  nostr_event_set_pubkey(rumor, hex[from]);
  nostr_event_set_created_at(rumor, created_at);
  nostr_event_set_content(rumor, content);
  nostr_event_set_tags(rumor, nostr_tags_new(1, nostr_tag_new("p", hex[to], NULL)));
  rumor->id = nostr_event_get_id(rumor);
  if (rumor_id)
    *rumor_id = g_strdup(rumor->id);
  char *rumor_json = nostr_event_serialize_compact(rumor);
  nostr_event_free(rumor);

  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, gh_test_secret[from], sizeof sk));
  g_assert_true(nostr_hex2bin(pk, hex[to], sizeof pk));
  char *ciphertext = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)rumor_json,
                                          strlen(rumor_json), &ciphertext), ==, 0);
  free(rumor_json);
  NostrEvent *seal = nostr_event_new();
  nostr_event_set_kind(seal, 13);
  nostr_event_set_pubkey(seal, hex[from]);
  nostr_event_set_content(seal, ciphertext);
  nostr_event_set_created_at(seal, created_at);
  nostr_event_set_tags(seal, nostr_tags_new(0));
  free(ciphertext);
  g_assert_cmpint(nostr_event_sign(seal, gh_test_secret[from]), ==, 0);

  guint8 ephemeral[32];
  g_assert_true(nostr_hex2bin(ephemeral, gh_test_secret[4], sizeof ephemeral));
  NostrEvent *wrap = nostr_nip59_wrap_with_key(seal, hex[to], ephemeral);
  nostr_event_free(seal);
  g_assert_nonnull(wrap);
  char *json = nostr_event_serialize_compact(wrap);
  nostr_event_free(wrap);
  gchar *out = g_strdup(json);
  free(json);
  return out;
}

static gchar *
craft_nip04(gint64 created_at, const gchar *content)
{
  char *ciphertext = NULL;
  nostr_secure_buf sk = secure_alloc(32);
  g_assert_true(nostr_hex2bin(sk.ptr, gh_test_secret[1], 32));
  g_assert_cmpint(nostr_nip04_encrypt_legacy_secure(content, hex[2], &sk,
                                                    &ciphertext, NULL), ==, 0);
  secure_free(&sk);
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 4);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, ciphertext);
  nostr_event_set_tags(event, nostr_tags_new(1, nostr_tag_new("p", hex[2], NULL)));
  free(ciphertext);
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[1]), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  gchar *copy = g_strdup(json);
  free(json);
  nostr_event_free(event);
  return copy;
}

static gchar *
wrap_id_of(const gchar *wrap_json)
{
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, wrap_json, NULL), ==, 1);
  char *id = nostr_event_get_id(event);
  nostr_event_free(event);
  gchar *out = g_strdup(id);
  free(id);
  return out;
}

static gchar *
room_of(guint a, guint b)
{
  return strcmp(hex[a], hex[b]) < 0 ? g_strconcat(hex[a], ",", hex[b], NULL)
                                    : g_strconcat(hex[b], ",", hex[a], NULL);
}

typedef struct {
  GhConversationStore *model;
  const gchar *room;
} RoomWait;

static gboolean
room_listed(gpointer data)
{
  RoomWait *wait = data;
  return gh_conversation_store_lookup(wait->model, wait->room) != NULL;
}

typedef struct {
  GhConversation *conversation;
  guint count;
} MessagesWait;

static gboolean
messages_reached(gpointer data)
{
  MessagesWait *wait = data;
  return g_list_model_get_n_items(G_LIST_MODEL(wait->conversation)) >= wait->count;
}

static gboolean
legacy_incoming_open(gpointer data)
{
  return legacy_incoming_req(data) != NULL;
}

/* key 1 sends the active account (key 2) a message on its inbox relay. */
static void
receive_message(Fixture *f, const gchar *content)
{
  Req *req = open_req(&f->rec, INBOX_A);
  g_assert_nonnull(req);
  g_autofree gchar *wrap = craft_wrap(1, 2, g_get_real_time() / G_USEC_PER_SEC - 60, content,
                                      NULL);
  gh_relay_scope_event(req->scope, INBOX_A, wrap);
  g_autofree gchar *room = room_of(1, 2);
  RoomWait wait = { f->model, room };
  gh_test_spin_until(room_listed, &wait);
}

static gchar *
store_db_path(Fixture *f, guint key)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dir = gh_store_account_dir_path(f->data_dir, hex[key], &error);
  g_assert_no_error(error);
  return g_build_filename(dir, "store.db", NULL);
}

static gchar *
file_sha256(const gchar *path)
{
  g_autofree gchar *contents = NULL;
  gsize length = 0;
  g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
  return g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)contents, length);
}

/* Every key of a schema, printed: what "unchanged" means for ST-8. */
static gchar *
dump_settings(GSettings *settings)
{
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(settings, "settings-schema", &schema, NULL);
  g_auto(GStrv) keys = g_settings_schema_list_keys(schema);
  GString *out = g_string_new(NULL);
  for (guint i = 0; keys[i]; i++) {
    g_autoptr(GVariant) value = g_settings_get_value(settings, keys[i]);
    g_autofree gchar *text = g_variant_print(value, TRUE);
    g_string_append_printf(out, "%s=%s\n", keys[i], text);
  }
  return g_string_free(out, FALSE);
}

static guint
count_events(Fixture *f, const gchar *prefix)
{
  guint n = 0;
  for (guint i = 0; i < f->events->len; i++)
    n += g_str_has_prefix(g_ptr_array_index(f->events, i), prefix);
  return n;
}

static void
select_key(Fixture *f, guint key)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_account_controller_select(f->accounts, npub[key], &error));
  g_assert_no_error(error);
  gh_test_run_until_idle();
}

/* ---- tests -------------------------------------------------------------------------- */

/* First open, receive through the granted inbox, restart restores the rooms
 * from the encrypted store before any relay answers. */
static void
test_open_receive_restore(void)
{
  Fixture f = { 0 };
  fixture_up(&f, 2);
  stack_up(&f);
  /* Nothing is subscribed until the store is open. */
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_NO_STORAGE);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_assert_cmpstr(gh_account_store_get_account(f.store), ==, hex[2]);
  g_assert_nonnull(gh_account_store_get_store(f.store));
  g_assert_nonnull(gh_account_store_get_conversations(f.store));
  g_assert_true(gh_dm_inbox_has_storage(f.inbox));
  /* KC-1 through the app: one item, never prompted. */
  g_assert_cmpuint(fake_secret_count(f.secret, hex[2]), ==, 1);
  g_assert_cmpuint(fake_secret_prompts(f.secret), ==, 0);
  g_autofree gchar *db = store_db_path(&f, 2);
  g_assert_true(g_file_test(db, G_FILE_TEST_IS_REGULAR));

  publish_inbox_list(&f, 2);
  g_assert_nonnull(open_req(&f.rec, INBOX_A));
  receive_message(&f, CANARY " first");
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.model)), ==, 1);

  stack_down(&f);
  g_assert_cmpuint(count_events(&f, "closed"), ==, 1);
  /* The encrypted files never hold the plaintext. */
  g_autofree gchar *contents = NULL;
  gsize length = 0;
  g_assert_true(g_file_get_contents(db, &contents, &length, NULL));
  g_assert_null(g_strstr_len(contents, length, CANARY));
  g_assert_false(g_file_test(f.state_dir, G_FILE_TEST_EXISTS));

  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  /* Restored from the store, with no relay involved yet. */
  g_assert_cmpuint(open_reqs(&f.rec), ==, 0);
  g_autofree gchar *room = room_of(1, 2);
  GhConversation *conversation = gh_conversation_store_lookup(f.model, room);
  g_assert_nonnull(conversation);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation)), ==, 1);
  g_assert_cmpuint(fake_secret_count(f.secret, hex[2]), ==, 1);
  fixture_down(&f);
}

typedef struct {
  TestBunker *bunker;
  guint count;
} BunkerPublishWait;

static gboolean
bunker_published(gpointer data)
{
  BunkerPublishWait *wait = data;
  return wait->bunker->publishes->len >= wait->count;
}

static void
reply_remote_decrypt(Fixture *f, guint count)
{
  BunkerPublishWait wait = { &f->bunker, count };
  gh_test_spin_until(bunker_published, &wait);
  bunker_accept(&f->bunker);
  g_autofree gchar *json = bunker_request_json(&f->bunker);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(json, &request), ==, 0);
  g_assert_true(g_str_equal(request.method, "nip44_decrypt") ||
                g_str_equal(request.method, "nip04_decrypt"));
  g_assert_cmpuint(request.n_params, ==, 2);
  g_autofree gchar *plaintext = NULL;
  if (g_str_equal(request.method, "nip44_decrypt")) {
    guint8 sk[32], pk[32], *plain = NULL;
    size_t length = 0;
    g_assert_true(nostr_hex2bin(sk, gh_test_secret[2], sizeof sk));
    g_assert_true(nostr_hex2bin(pk, request.params[0], sizeof pk));
    g_assert_cmpint(nostr_nip44_decrypt_v2(sk, pk, request.params[1], &plain, &length), ==, 0);
    plaintext = g_strndup((const gchar *)plain, length);
    free(plain);
  } else {
    char *plain = NULL;
    g_assert_cmpint(nostr_nip04_decrypt(request.params[1], request.params[0],
                                        gh_test_secret[2], &plain, NULL), ==, 0);
    plaintext = g_strdup(plain);
    free(plain);
  }
  g_autofree gchar *escaped = g_strescape(plaintext, NULL);
  g_autofree gchar *result_json = g_strdup_printf("\"%s\"", escaped);
  char *response = nostr_nip46_response_build_ok(request.id, result_json);
  g_assert_nonnull(response);
  bunker_reply(&f->bunker, response);
  free(response);
  nostr_nip46_request_free(&request);
}

static void
test_remote_cache_gate_and_offline_restart(void)
{
  Fixture f = { .remote = TRUE, .with_outbox = TRUE };
  fixture_up(&f, 2);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_assert_true(gh_dm_inbox_has_storage(f.inbox));
  g_assert_nonnull(gh_account_store_get_outbox(f.store));
  bunker_eose(&f.bunker);
  publish_inbox_list(&f, 2);
  Req *req = open_req(&f.rec, INBOX_A);
  g_assert_nonnull(req);
  g_autofree gchar *first = craft_wrap(1, 2, g_get_real_time() / G_USEC_PER_SEC - 60,
                                       CANARY " remote", NULL);
  gh_relay_scope_event(req->scope, INBOX_A, first);
  reply_remote_decrypt(&f, 1);
  reply_remote_decrypt(&f, 2);
  g_autofree gchar *room = room_of(1, 2);
  RoomWait listed_room = { f.model, room };
  gh_test_spin_until(room_listed, &listed_room);
  gh_relay_scope_eose(req->scope, INBOX_A);
  gh_test_run_until_idle();
  gint64 checkpoint = gh_dm_inbox_get_checkpoint(f.inbox);
  g_assert_cmpint(checkpoint, >, 0);
  publish_nip04_relay_list(&f);
  gh_test_spin_until(legacy_incoming_open, &f);
  g_autofree gchar *legacy = craft_nip04(g_get_real_time() / G_USEC_PER_SEC - 40,
                                          CANARY " legacy");
  Req *legacy_req = legacy_incoming_req(&f);
  gh_relay_scope_event(legacy_req->scope, INBOX_A, legacy);
  reply_remote_decrypt(&f, 3);
  GhConversation *first_room = gh_conversation_store_lookup(f.model, room);
  MessagesWait messages = { first_room, 2 };
  gh_test_spin_until(messages_reached, &messages);
  g_assert_cmpuint(f.bunker.publishes->len, ==, 3);

  stack_down(&f);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  GhConversation *restored = gh_conversation_store_lookup(f.model, room);
  g_assert_nonnull(restored);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(restored)), ==, 2);
  g_assert_cmpuint(f.bunker.publishes->len, ==, 3);
  bunker_eose(&f.bunker);
  publish_inbox_list(&f, 2);
  publish_nip04_relay_list(&f);
  gh_test_spin_until(legacy_incoming_open, &f);
  legacy_req = legacy_incoming_req(&f);
  gh_relay_scope_event(legacy_req->scope, INBOX_A, legacy);
  req = open_req(&f.rec, INBOX_A);
  g_assert_nonnull(req);
  gh_relay_scope_event(req->scope, INBOX_A, first);
  gh_test_run_until_idle();
  g_assert_cmpuint(f.bunker.publishes->len, ==, 3);
  g_autoptr(GhStatus) status = gh_status_new();
  gh_status_set_account_active(status, TRUE);
  gh_status_set_store(status, GH_STATUS_STORE_OPEN, NULL);
  gh_inbox_status_attach(status, f.inbox, f.relays);
  g_autofree gchar *new_wrap = craft_wrap(1, 2, g_get_real_time() / G_USEC_PER_SEC - 30,
                                          CANARY " offline", NULL);
  /* A settled inbox may have advanced after restart, before this wrap. */
  checkpoint = gh_dm_inbox_get_checkpoint(f.inbox);
  gh_relay_scope_event(req->scope, INBOX_A, new_wrap);
  guint backlog = 0;
  g_object_get(status, "inbox-backlog", &backlog, NULL);
  g_assert_cmpuint(backlog, ==, 1);
  g_autofree gchar *title = NULL;
  g_object_get(status, "banner-title", &title, NULL);
  g_assert_cmpstr(title, ==, "Decrypting 1 message with your signer");
  BunkerPublishWait waiting = { &f.bunker, 4 };
  gh_test_spin_until(bunker_published, &waiting);
  BunkerHandle *publish = bunker_last_publish(&f.bunker);
  gh_relay_publish_failed(publish->publish, publish->url, "offline");
  gh_test_run_until_idle();
  g_assert_cmpuint(gh_dm_inbox_get_locked(f.inbox), ==, 1);
  gh_relay_scope_eose(req->scope, INBOX_A);
  gh_test_run_until_idle();
  g_assert_cmpint(gh_dm_inbox_get_checkpoint(f.inbox), ==, checkpoint);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(restored)), ==, 2);
  fixture_down(&f);
}

static void
test_remote_failed_restore_keeps_gate_closed(void)
{
  Fixture f = { .remote = TRUE, .with_outbox = TRUE };
  fixture_up(&f, 2);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  bunker_eose(&f.bunker);
  publish_inbox_list(&f, 2);
  Req *req = open_req(&f.rec, INBOX_A);
  g_assert_nonnull(req);
  g_autofree gchar *wrap = craft_wrap(1, 2, g_get_real_time() / G_USEC_PER_SEC - 60,
                                      CANARY " damaged", NULL);
  gh_relay_scope_event(req->scope, INBOX_A, wrap);
  reply_remote_decrypt(&f, 1);
  reply_remote_decrypt(&f, 2);
  g_autofree gchar *room = room_of(1, 2);
  RoomWait listed_room = { f.model, room };
  gh_test_spin_until(room_listed, &listed_room);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_exec(gh_account_store_get_store(f.store),
    "ALTER TABLE conversations RENAME COLUMN read_seq TO broken_read_seq", &error));
  g_assert_no_error(error);
  stack_down(&f);

  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*could not restore every stored conversation*");
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_CORRUPT);
  g_test_assert_expected_messages();
  g_assert_nonnull(gh_account_store_get_error(f.store));
  g_assert_false(gh_dm_inbox_has_storage(f.inbox));
  g_assert_null(gh_account_store_get_outbox(f.store));
  g_assert_cmpuint(f.bunker.publishes->len, ==, 2);
  fixture_down(&f);
}

static void
test_remote_memory_only_refused(void)
{
  Fixture f = { .remote = TRUE, .with_outbox = TRUE };
  fixture_up(&f, 2);
  fake_secret_set_available(f.secret, FALSE);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_UNAVAILABLE);
  g_assert_false(gh_dm_inbox_has_storage(f.inbox));
  g_assert_null(gh_account_store_get_outbox(f.store));
  g_autoptr(GError) error = NULL;
  g_assert_false(gh_account_store_continue_without_saving(f.store, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_nonnull(g_strstr_len(error->message, -1, "encrypted message storage"));
  guint8 payload[99] = { 2 };
  g_autofree gchar *ciphertext = g_base64_encode(payload, sizeof payload);
  GAsyncResult *done = NULL;
  gh_account_controller_nip44_decrypt_async(f.accounts, ciphertext, hex[1],
                                             gh_test_store_result, &done);
  gh_test_wait(&done);
  g_clear_error(&error);
  g_assert_null(gh_account_controller_nip44_finish(done, &error));
  g_assert_error(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_UNAVAILABLE);
  g_object_unref(done);
  g_assert_cmpuint(f.bunker.publishes->len, ==, 0);
  fixture_down(&f);
}

static gboolean
store_seen(Fixture *f, GhStoreSeenNs ns, const gchar *id)
{
  gboolean has = FALSE;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_seen_contains(gh_account_store_get_store(f->store), ns, id, &has,
                                       &error));
  g_assert_no_error(error);
  return has;
}

static void
write_private(const gchar *path, const gchar *contents)
{
  g_autofree gchar *dir = g_path_get_dirname(path);
  g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
  g_assert_true(g_file_set_contents_full(path, contents, -1, G_FILE_SET_CONTENTS_CONSISTENT,
                                         0600, NULL));
}

/* The since of a first session: nothing carried over, the initial window. */
static void
assert_initial_since(gint64 since, gint64 before, gint64 after)
{
  g_assert_cmpint(since, >=, before - GH_DM_INBOX_INITIAL_BACKFILL);
  g_assert_cmpint(since, <=, after - GH_DM_INBOX_INITIAL_BACKFILL);
}

typedef struct {
  GhConversationStore *model;
  const gchar *rumor_id;
} MessageWait;

static gboolean
message_listed(gpointer data)
{
  MessageWait *wait = data;
  return gh_conversation_store_has_message(wait->model, wait->rumor_id);
}

static gboolean
inbox_live_and_settled(gpointer data)
{
  GhDmInboxCounters counters;
  gh_dm_inbox_get_counters(data, &counters);
  return gh_dm_inbox_get_state(data) == GH_DM_INBOX_LIVE && counters.pending == 0;
}

/* The relay delivers wrap on INBOX_A and then its EOSE; waits until the
 * inbox settled and the message is listed. */
static void
deliver_and_settle(Fixture *f, const gchar *wrap, const gchar *rumor_id)
{
  Req *req = open_req(&f->rec, INBOX_A);
  g_assert_nonnull(req);
  gh_relay_scope_event(req->scope, INBOX_A, wrap);
  gh_relay_scope_eose(req->scope, INBOX_A);
  gh_test_spin_until(inbox_live_and_settled, f->inbox);
  MessageWait wait = { f->model, rumor_id };
  gh_test_spin_until(message_listed, &wait);
}

/* The memory-only inbox names its file like the store's directory (§3.2). */
static void
test_pseudonymous_names(void)
{
  for (guint key = 1; key <= 2; key++) {
    g_autofree gchar *dir = gh_store_account_dir_name(hex[key]);
    g_autofree gchar *expected = g_strconcat(dir, ".seen", NULL);
    g_autofree gchar *name = gh_nip17_seen_file_name(hex[key]);
    g_assert_cmpstr(name, ==, expected);
    g_assert_null(strstr(name, hex[key]));
  }
  g_assert_null(gh_nip17_seen_file_name("npub1nope"));
}

/* ST-12 via the app (W13 review B1). The memory-only inbox's plaintext files
 * (Groundhog 0.6.0's <pubkey>.seen and .checkpoint; a store-less build's
 * <acct>.seen) held keys of messages it kept in memory only. Only their
 * rejected ids are imported; the "w"/"r" keys and the checkpoint are
 * dropped and every file is deleted, so the first encrypted-store session
 * asks for the initial window and stores what the relays still hold: the
 * message 0.6.0 showed once is listed again, and kept from then on. */
static void
test_legacy_import(void)
{
  Fixture f = { 0 };
  fixture_up(&f, 2);
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *rumor_id = NULL;
  g_autofree gchar *wrap = craft_wrap(1, 2, now - 120, CANARY " shown once by 0.6.0", &rumor_id);
  g_autofree gchar *wrap_id = wrap_id_of(wrap);
  g_autofree gchar *rejected = g_strnfill(64, 'a');
  g_autofree gchar *rejected2 = g_strnfill(64, 'b');
  g_autofree gchar *seen = g_strdup_printf("groundhog-nip17-seen 1 %s\nw %s\nr %s\nx %s\n",
                                           hex[2], wrap_id, rumor_id, rejected);
  g_autofree gchar *pseudonymous = g_strdup_printf("groundhog-nip17-seen 1 %s\nx %s\n", hex[2],
                                                   rejected2);
  g_autofree gchar *mark = g_strdup_printf("groundhog-dm-inbox-checkpoint 1 %s %" G_GINT64_FORMAT
                                           "\n", hex[2], now - 3600);
  g_autofree gchar *seen_path = g_strdup_printf("%s/%s.seen", f.state_dir, hex[2]);
  g_autofree gchar *mark_path = g_strdup_printf("%s/%s.checkpoint", f.state_dir, hex[2]);
  g_autofree gchar *name = gh_nip17_seen_file_name(hex[2]);
  g_autofree gchar *pseudonymous_path = g_build_filename(f.state_dir, name, NULL);
  write_private(seen_path, seen);
  write_private(mark_path, mark);
  write_private(pseudonymous_path, pseudonymous);

  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_assert_cmpuint(gh_test_count_files(f.state_dir), ==, 0);
  g_assert_true(store_seen(&f, GH_STORE_SEEN_REJECTED_WRAP, rejected));
  g_assert_true(store_seen(&f, GH_STORE_SEEN_REJECTED_WRAP, rejected2));
  g_assert_false(store_seen(&f, GH_STORE_SEEN_WRAP, wrap_id));
  g_assert_false(store_seen(&f, GH_STORE_SEEN_RUMOR, rumor_id));
  gint64 since = -1;
  g_assert_true(gh_store_get_cursor(gh_account_store_get_store(f.store),
                                    GH_ACCOUNT_STORE_INBOX_CURSOR, "", &since, NULL));
  g_assert_cmpint(since, ==, 0);
  gint64 before = g_get_real_time() / G_USEC_PER_SEC;
  publish_inbox_list(&f, 2);
  gint64 after = g_get_real_time() / G_USEC_PER_SEC;
  Req *req = open_req(&f.rec, INBOX_A);
  g_assert_nonnull(req);
  assert_initial_since(req->since, before, after);

  /* The relays' copy is unwrapped and stored this time. */
  guint calls = f.signer.calls;
  deliver_and_settle(&f, wrap, rumor_id);
  g_assert_cmpuint(f.signer.calls, ==, calls + 2);
  g_assert_true(store_seen(&f, GH_STORE_SEEN_WRAP, wrap_id));
  stack_down(&f);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_assert_true(gh_conversation_store_has_message(f.model, rumor_id));
  fixture_down(&f);
}

/* W13 review B1, on the executable's wiring (gh-app-services.c's controller,
 * relays, model, storage-mode inbox, store key, outbox and account store),
 * rebuilt over the same data and state directories as a new process would:
 *  - nothing subscribes before the account's store is open;
 *  - a message admitted in session 1 is listed again in session 2, from the
 *    store, before any relay answers;
 *  - the relays' copy then costs no signer call, and the inbox starts from
 *    the checkpoint the store kept;
 *  - nothing is written outside the encrypted store. */
static void
test_restart_relists(void)
{
  Fixture f = { 0 };
  f.with_outbox = TRUE;
  fixture_up(&f, 2);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  publish_inbox_list(&f, 2);
  g_autofree gchar *rumor_id = NULL;
  g_autofree gchar *wrap = craft_wrap(1, 2, g_get_real_time() / G_USEC_PER_SEC - 60,
                                      CANARY " session one", &rumor_id);
  deliver_and_settle(&f, wrap, rumor_id);
  guint calls = f.signer.calls;
  g_assert_cmpuint(calls, >=, 2);
  gint64 checkpoint = gh_dm_inbox_get_checkpoint(f.inbox);
  g_assert_cmpint(checkpoint, >, 0);
  stack_down(&f);

  /* Session 2, with the key lookup held: no inbox REQ until the store is
   * open, even though the inbox list is known. */
  fake_secret_set_hold(f.secret, TRUE);
  stack_up(&f);
  gh_test_run_until_idle();
  g_assert_cmpint(gh_account_store_get_state(f.store), ==, GH_ACCOUNT_STORE_OPENING);
  publish_inbox_list(&f, 2);
  g_assert_cmpuint(f.rec.reqs->len, ==, 0);
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_NO_STORAGE);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.model)), ==, 0);
  fake_secret_set_hold(f.secret, FALSE);
  while (fake_secret_release(f.secret))
    gh_test_run_until_idle();
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_assert_true(gh_conversation_store_has_message(f.model, rumor_id));
  g_autofree gchar *room = room_of(1, 2);
  g_assert_nonnull(gh_conversation_store_lookup(f.model, room));
  /* The checkpoint was saved in the store (not in a file) at shutdown. */
  gint64 saved = 0;
  g_assert_true(gh_store_get_cursor(gh_account_store_get_store(f.store),
                                    GH_ACCOUNT_STORE_INBOX_CURSOR, "", &saved, NULL));
  g_assert_cmpint(saved, >=, checkpoint);
  Req *req = open_req(&f.rec, INBOX_A);
  g_assert_nonnull(req);
  g_assert_cmpint(req->since, ==, saved - GH_DM_INBOX_WRAP_SKEW);
  gh_relay_scope_event(req->scope, INBOX_A, wrap);
  gh_relay_scope_eose(req->scope, INBOX_A);
  gh_test_spin_until(inbox_live_and_settled, f.inbox);
  GhDmInboxCounters counters;
  gh_dm_inbox_get_counters(f.inbox, &counters);
  g_assert_cmpuint(counters.skipped, ==, 1);
  g_assert_cmpuint(counters.admitted, ==, 0);
  g_assert_cmpuint(f.signer.calls, ==, calls);
  g_assert_true(gh_conversation_store_has_message(f.model, rumor_id));
  g_assert_false(g_file_test(f.state_dir, G_FILE_TEST_EXISTS));
  fixture_down(&f);
}

/* W13 review B1 in "Continue Without Saving Messages" (charter §3.4, KC-4):
 * the session writes nothing (no seen key, no checkpoint, no file at all),
 * and after a restart, the same choice made again, the relays' copy is
 * unwrapped again from the initial window and listed again. */
static void
test_ephemeral_restart_refetches(void)
{
  Fixture f = { 0 };
  f.with_outbox = TRUE;
  fixture_up(&f, 2);
  fake_secret_set_available(f.secret, FALSE);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_UNAVAILABLE);
  g_assert_true(gh_account_store_continue_without_saving(f.store, NULL));
  publish_inbox_list(&f, 2);
  g_autofree gchar *rumor_id = NULL;
  g_autofree gchar *wrap = craft_wrap(1, 2, g_get_real_time() / G_USEC_PER_SEC - 60,
                                      CANARY " in memory", &rumor_id);
  deliver_and_settle(&f, wrap, rumor_id);
  guint calls = f.signer.calls;
  g_assert_cmpint(gh_dm_inbox_get_checkpoint(f.inbox), >, 0);
  g_assert_cmpuint(gh_test_count_files(f.data_dir), ==, 0);
  g_assert_false(g_file_test(f.state_dir, G_FILE_TEST_EXISTS));
  g_assert_cmpuint(gh_test_count_files(xdg_root), ==, 0);
  stack_down(&f);
  g_assert_cmpuint(gh_test_count_files(f.data_dir), ==, 0);
  g_assert_cmpuint(gh_test_count_files(xdg_root), ==, 0);

  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_UNAVAILABLE);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.model)), ==, 0);
  g_assert_true(gh_account_store_continue_without_saving(f.store, NULL));
  gint64 before = g_get_real_time() / G_USEC_PER_SEC;
  publish_inbox_list(&f, 2);
  gint64 after = g_get_real_time() / G_USEC_PER_SEC;
  Req *req = open_req(&f.rec, INBOX_A);
  g_assert_nonnull(req);
  assert_initial_since(req->since, before, after);
  deliver_and_settle(&f, wrap, rumor_id);
  g_assert_cmpuint(f.signer.calls, ==, calls + 2);
  g_autofree gchar *room = room_of(1, 2);
  g_assert_nonnull(gh_conversation_store_lookup(f.model, room));
  g_assert_cmpuint(gh_test_count_files(f.data_dir), ==, 0);
  g_assert_false(g_file_test(f.state_dir, G_FILE_TEST_EXISTS));
  g_assert_cmpuint(gh_test_count_files(xdg_root), ==, 0);
  fixture_down(&f);
}

/* KC-2: a locked keyring in the background: no prompt (every call's flags
 * are recorded), STORE_LOCKED, nothing written, no inbox REQ; the banner's
 * Unlock is the one call that may prompt. */
static void
test_kc2_locked(void)
{
  Fixture f = { 0 };
  fixture_up(&f, 2);
  fake_secret_set_locked(f.secret, TRUE);
  stack_up(&f);
  g_autoptr(GhStatus) status = gh_status_new();
  gh_status_set_account_active(status, TRUE);
  gh_store_status_attach(status, f.store);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_LOCKED);
  g_assert_nonnull(gh_account_store_get_error(f.store));
  g_assert_null(gh_account_store_get_store(f.store));
  GPtrArray *calls = fake_secret_calls(f.secret);
  g_assert_cmpuint(calls->len, >, 0);
  for (guint i = 0; i < calls->len; i++)
    g_assert_cmpint(((FakeSecretCall *)g_ptr_array_index(calls, i))->flags, ==,
                    GH_STORE_KEY_FLAGS_NONE);
  g_assert_cmpuint(fake_secret_prompts(f.secret), ==, 0);
  /* Nothing written: no plaintext fallback, no store, no state file. */
  g_assert_cmpuint(gh_test_count_files(f.data_dir), ==, 0);
  g_assert_false(g_file_test(f.state_dir, G_FILE_TEST_EXISTS));
  /* Even with the inbox list known, no inbox REQ (the wraps stay on the relays). */
  publish_inbox_list(&f, 2);
  g_assert_cmpuint(f.rec.reqs->len, ==, 0);
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_NO_STORAGE);
  /* Status input and banner (§7.15 #14). */
  g_assert_cmpint(gh_status_get_store(status), ==, GH_STATUS_STORE_LOCKED);
  g_assert_cmpint(gh_status_get_banner(status), ==, GH_STATUS_BANNER_STORE_LOCKED);
  g_assert_cmpstr(gh_status_banner_get_action(GH_STATUS_BANNER_STORE_LOCKED), ==,
                  GH_STATUS_ACTION_STORE_UNLOCK);
  g_assert_cmpstr(gh_status_banner_get_button_label(GH_STATUS_BANNER_STORE_LOCKED), ==,
                  "Unlock");
  g_assert_true(gh_status_banner_is_problem(GH_STATUS_BANNER_STORE_LOCKED));
  /* It outranks being offline: nothing can be read either way. */
  gh_status_set_network_available(status, FALSE);
  g_assert_cmpint(gh_status_get_banner(status), ==, GH_STATUS_BANNER_STORE_LOCKED);
  gh_status_set_network_available(status, TRUE);
  /* Retry stays silent and locked; Unlock prompts once and opens. */
  g_assert_true(gh_account_store_retry(f.store));
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_LOCKED);
  g_assert_cmpuint(fake_secret_prompts(f.secret), ==, 0);
  g_assert_false(gh_account_store_continue_without_saving(f.store, NULL));
  g_assert_true(gh_account_store_unlock(f.store));
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_assert_cmpuint(fake_secret_prompts(f.secret), ==, 1);
  g_assert_false(gh_account_store_unlock(f.store));
  g_assert_nonnull(open_req(&f.rec, INBOX_A));
  g_assert_cmpint(gh_status_get_store(status), ==, GH_STATUS_STORE_OPEN);
  g_assert_cmpint(gh_status_get_banner(status), !=, GH_STATUS_BANNER_STORE_LOCKED);

  /* Locked again at the next start, with the store on disk: untouched. */
  receive_message(&f, CANARY " before lock");
  stack_down(&f);
  g_autofree gchar *db = store_db_path(&f, 2);
  g_autofree gchar *before = file_sha256(db);
  fake_secret_set_locked(f.secret, TRUE);
  fake_secret_clear_calls(f.secret);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_LOCKED);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.model)), ==, 0);
  g_autofree gchar *after = file_sha256(db);
  g_assert_cmpstr(before, ==, after);
  g_assert_cmpuint(fake_secret_prompts(f.secret), ==, 1);
  fixture_down(&f);
}

/* KC-3: the key item is gone but the store is there: STORE_KEY_MISSING, the
 * database untouched (never a new key for it); "Start fresh" crypto-shreds
 * the directory and creates a new store. */
static void
test_kc3_key_missing(void)
{
  Fixture f = { 0 };
  fixture_up(&f, 2);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_autofree gchar *old_id = g_strdup(gh_store_get_store_id(gh_account_store_get_store(f.store)));
  stack_down(&f);
  g_autofree gchar *db = store_db_path(&f, 2);
  g_autofree gchar *before = file_sha256(db);
  /* A new keyring: the account's item is gone. */
  g_object_unref(f.secret);
  f.secret = fake_secret_new();
  stack_up(&f);
  g_autoptr(GhStatus) status = gh_status_new();
  gh_status_set_account_active(status, TRUE);
  gh_store_status_attach(status, f.store);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_KEY_MISSING);
  g_assert_cmpuint(fake_secret_count(f.secret, hex[2]), ==, 0);
  g_autofree gchar *after = file_sha256(db);
  g_assert_cmpstr(before, ==, after);
  g_assert_cmpint(gh_status_get_banner(status), ==, GH_STATUS_BANNER_STORE_KEY_MISSING);
  g_assert_cmpstr(gh_status_get_store_error(status), ==, gh_account_store_get_error(f.store));
  /* Its button opens the confirmation (W13b review, non-blocking #3). */
  g_assert_cmpstr(gh_status_banner_get_action(GH_STATUS_BANNER_STORE_KEY_MISSING), ==,
                  GH_STATUS_ACTION_STORE_START_FRESH);
  g_assert_cmpstr(gh_status_banner_get_button_label(GH_STATUS_BANNER_STORE_KEY_MISSING), ==,
                  "Start Fresh on This Device…");
  /* Retry does not invent a key either. */
  g_assert_true(gh_account_store_retry(f.store));
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_KEY_MISSING);
  g_assert_cmpuint(fake_secret_count(f.secret, hex[2]), ==, 0);

  g_autoptr(GAsyncResult) result = NULL;
  gh_account_store_start_fresh_async(f.store, NULL, gh_test_store_result, &result);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_account_store_start_fresh_finish(f.store, gh_test_wait(&result), &error));
  g_assert_no_error(error);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_assert_cmpuint(fake_secret_count(f.secret, hex[2]), ==, 1);
  g_assert_cmpstr(gh_store_get_store_id(gh_account_store_get_store(f.store)), !=, old_id);
  g_autofree gchar *fresh = file_sha256(db);
  g_assert_cmpstr(fresh, !=, before);
  /* current-npub stays: this was not a forget. */
  g_autofree gchar *current = g_settings_get_string(f.settings, "current-npub");
  g_assert_cmpstr(current, ==, npub[2]);
  /* Not offered in any other state. */
  g_clear_object(&result);
  gh_account_store_start_fresh_async(f.store, NULL, gh_test_store_result, &result);
  g_assert_false(gh_account_store_start_fresh_finish(f.store, gh_test_wait(&result), &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  fixture_down(&f);
}

/* KC-4: no Secret Service: STORE_UNAVAILABLE; only the user's explicit
 * "Continue Without Saving Messages" starts an in-memory store, and then
 * the whole session (receive, rooms, the outbox) writes zero files. */
static void
test_kc4_unavailable(void)
{
  Fixture f = { 0 };
  f.with_outbox = TRUE;
  fixture_up(&f, 2);
  fake_secret_set_available(f.secret, FALSE);
  stack_up(&f);
  g_autoptr(GhStatus) status = gh_status_new();
  gh_status_set_account_active(status, TRUE);
  gh_store_status_attach(status, f.store);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_UNAVAILABLE);
  g_assert_cmpint(gh_status_get_banner(status), ==, GH_STATUS_BANNER_STORE_UNAVAILABLE);
  g_assert_cmpstr(gh_status_banner_get_action(GH_STATUS_BANNER_STORE_UNAVAILABLE), ==,
                  GH_STATUS_ACTION_STORE_EPHEMERAL);
  g_assert_cmpstr(gh_status_banner_get_button_label(GH_STATUS_BANNER_STORE_UNAVAILABLE), ==,
                  "Continue Without Saving Messages");
  /* Never automatic: still nothing, and no inbox REQ. */
  publish_inbox_list(&f, 2);
  g_assert_cmpuint(f.rec.reqs->len, ==, 0);
  g_assert_null(gh_account_store_get_outbox(f.store));

  g_autoptr(GError) error = NULL;
  g_assert_true(gh_account_store_continue_without_saving(f.store, &error));
  g_assert_no_error(error);
  g_assert_cmpint(gh_account_store_get_state(f.store), ==, GH_ACCOUNT_STORE_EPHEMERAL);
  g_assert_true(gh_store_is_ephemeral(gh_account_store_get_store(f.store)));
  g_assert_cmpint(gh_status_get_banner(status), ==, GH_STATUS_BANNER_STORE_EPHEMERAL);
  g_assert_nonnull(open_req(&f.rec, INBOX_A));
  receive_message(&f, CANARY " in memory");
  GObject *outbox = gh_account_store_get_outbox(f.store);
  g_assert_nonnull(outbox);
  g_assert_cmpint(test_outbox_send(outbox, hex[1], CANARY " reply"), >, 0);
  gh_test_run_until_idle();
  /* H7-style: not one file anywhere Groundhog could write. */
  g_assert_cmpuint(gh_test_count_files(f.data_dir), ==, 0);
  g_assert_false(g_file_test(f.state_dir, G_FILE_TEST_EXISTS));
  g_assert_cmpuint(gh_test_count_files(xdg_root), ==, 0);

  /* Not sticky: another activation asks the Secret Service again. */
  select_key(&f, 1);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_UNAVAILABLE);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.model)), ==, 0);
  select_key(&f, 2);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_UNAVAILABLE);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.model)), ==, 0);
  g_assert_cmpuint(gh_test_count_files(f.data_dir), ==, 0);
  g_assert_cmpuint(gh_test_count_files(xdg_root), ==, 0);
  /* Only offered when there is no keyring. */
  fake_secret_set_available(f.secret, TRUE);
  g_assert_true(gh_account_store_retry(f.store));
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_assert_false(gh_account_store_continue_without_saving(f.store, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  fixture_down(&f);
}

/* PT-6: on a switch the inbox stops, the outbox is disposed and the store is
 * closed (no descriptor left on it) before the next account's store opens;
 * the outbox's unsent message survives and resumes when the account returns. */
static void
test_pt6_switch_order(void)
{
  Fixture f = { 0 };
  f.with_outbox = TRUE;
  fixture_up(&f, 2);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  publish_inbox_list(&f, 2);
  receive_message(&f, CANARY " to two");
  g_autofree gchar *room = room_of(1, 2);
  GhConversation *conversation = gh_conversation_store_lookup(f.model, room);
  g_assert_nonnull(conversation);
  g_autoptr(GhMessage) target = g_list_model_get_item(G_LIST_MODEL(conversation), 0);
  g_assert_nonnull(target);
  g_autofree gchar *target_id = g_strdup(gh_message_get_rumor_id(target));
  GhReactionStore *reactions = gh_app_outbox_get_reactions(f.sender);
  g_assert_nonnull(reactions);
  g_autoptr(GhReaction) first = gh_reaction_new(target_id, "reaction-before-switch",
                                                 hex[1], "+", 10, room);
  g_autoptr(GError) reaction_error = NULL;
  g_assert_true(gh_reaction_store_admit(reactions, first, &reaction_error));
  g_assert_no_error(reaction_error);
  GObject *outbox = gh_account_store_get_outbox(f.store);
  g_assert_nonnull(outbox);
  gint64 outbox_id = test_outbox_send(outbox, hex[1], "queued before the switch");
  g_assert_cmpint(outbox_id, >, 0);
  g_autofree gchar *db2 = store_db_path(&f, 2);
  /* The probe sees an open store (so its "closed" verdict below means it). */
  g_assert_true(fd_open_on(db2));

  g_ptr_array_set_size(f.events, 0);
  f.other_store_path = g_strdup(db2);
  select_key(&f, 1);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_assert_true(f.fd_checked);
  g_assert_cmpuint(f.events->len, ==, 3);
  g_assert_cmpstr(g_ptr_array_index(f.events, 0), ==, "outbox-gone");
  g_autofree gchar *closed2 = g_strdup_printf("closed %s", hex[2]);
  g_autofree gchar *opening1 = g_strdup_printf("opening %s", hex[1]);
  g_assert_cmpstr(g_ptr_array_index(f.events, 1), ==, closed2);
  g_assert_cmpstr(g_ptr_array_index(f.events, 2), ==, opening1);
  g_assert_false(fd_open_on(db2));
  /* The old account's REQ is closed and its rooms are gone. */
  g_assert_null(open_req(&f.rec, INBOX_A));
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.model)), ==, 0);
  g_assert_cmpstr(gh_account_store_get_account(f.store), ==, hex[1]);

  /* Back: the same ordering the other way, and the message is resumed. */
  g_autofree gchar *db1 = store_db_path(&f, 1);
  g_free(f.other_store_path);
  f.other_store_path = g_strdup(db1);
  f.fd_checked = FALSE;
  g_ptr_array_set_size(f.events, 0);
  select_key(&f, 2);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_assert_true(f.fd_checked);
  g_autofree gchar *closed1 = g_strdup_printf("closed %s", hex[1]);
  g_autofree gchar *opening2 = g_strdup_printf("opening %s", hex[2]);
  g_assert_cmpstr(g_ptr_array_index(f.events, 1), ==, closed1);
  g_assert_cmpstr(g_ptr_array_index(f.events, 2), ==, opening2);
  outbox = gh_account_store_get_outbox(f.store);
  g_assert_true(test_outbox_has(outbox, outbox_id));
  g_assert_nonnull(gh_conversation_store_lookup(f.model, room));
  reactions = gh_app_outbox_get_reactions(f.sender);
  g_assert_nonnull(reactions);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(
    gh_reaction_store_lookup(reactions, target_id)), ==, 1);
  g_autoptr(GhReaction) second = gh_reaction_new(target_id, "reaction-after-switch",
                                                  hex[2], "❤", 11, room);
  g_assert_true(gh_reaction_store_admit(reactions, second, &reaction_error));
  g_assert_no_error(reaction_error);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(
    gh_reaction_store_lookup(reactions, target_id)), ==, 2);
  fixture_down(&f);
}

/* PT-6 with the key lookup still in flight: a switch makes it stale; its key
 * never opens a store, and the next account's store opens after it. */
static void
test_pt6_switch_mid_open(void)
{
  Fixture f = { 0 };
  fixture_up(&f, 2);
  fake_secret_set_hold(f.secret, TRUE);
  fake_secret_set_ignore_cancel(f.secret, TRUE);
  stack_up(&f);
  gh_test_run_until_idle();
  g_assert_cmpint(gh_account_store_get_state(f.store), ==, GH_ACCOUNT_STORE_OPENING);
  g_assert_cmpuint(fake_secret_pending_for(f.secret, hex[2]), ==, 1);
  select_key(&f, 1);
  g_assert_cmpint(gh_account_store_get_state(f.store), ==, GH_ACCOUNT_STORE_OPENING);
  /* Serialized: nothing for account 1 starts while 2's lookup is out. */
  g_assert_cmpuint(fake_secret_pending_for(f.secret, hex[1]), ==, 0);
  fake_secret_set_hold(f.secret, FALSE);
  while (fake_secret_release(f.secret))
    gh_test_run_until_idle();
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_assert_cmpstr(gh_account_store_get_account(f.store), ==, hex[1]);
  g_autofree gchar *opening1 = g_strdup_printf("opening %s", hex[1]);
  g_assert_cmpuint(f.events->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(f.events, 0), ==, opening1);
  g_autofree gchar *db2 = store_db_path(&f, 2);
  g_assert_false(g_file_test(db2, G_FILE_TEST_EXISTS));
  fixture_down(&f);
}

/* ST-8 Forget account: the store closes first, the key item and the
 * directory (and legacy files) are gone, current-npub is cleared only when it
 * matched, and Gnostr's settings are unchanged. */
static void
test_st8_forget(void)
{
  Fixture f = { 0 };
  fixture_up(&f, 1);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  select_key(&f, 2);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  publish_inbox_list(&f, 2);
  receive_message(&f, CANARY " forget me");
  g_autofree gchar *db1 = store_db_path(&f, 1);
  g_autofree gchar *db2 = store_db_path(&f, 2);
  g_autofree gchar *dir2 = g_path_get_dirname(db2);
  g_autofree gchar *legacy2 = g_strdup_printf("%s/%s.seen", f.state_dir, hex[2]);
  g_autofree gchar *legacy2_mark = g_strdup_printf("%s/%s.checkpoint", f.state_dir, hex[2]);
  g_autofree gchar *name2 = gh_nip17_seen_file_name(hex[2]);
  g_autofree gchar *pseudonymous2 = g_build_filename(f.state_dir, name2, NULL);
  g_assert_cmpint(g_mkdir_with_parents(f.state_dir, 0700), ==, 0);
  g_assert_true(g_file_set_contents(legacy2, "stale", -1, NULL));
  g_assert_true(g_file_set_contents(legacy2_mark, "stale", -1, NULL));
  g_assert_true(g_file_set_contents(pseudonymous2, "stale", -1, NULL));
  g_autofree gchar *gnostr_before = dump_settings(f.gnostr);
  g_autofree gchar *groundhog_before = dump_settings(f.settings);

  /* A non-active account first: current-npub (key 2) is kept. */
  g_autoptr(GAsyncResult) result = NULL;
  g_autoptr(GError) error = NULL;
  gh_account_store_forget_async(f.store, hex[1], NULL, gh_test_store_result, &result);
  g_assert_true(gh_account_store_forget_finish(f.store, gh_test_wait(&result), &error));
  g_assert_no_error(error);
  g_assert_false(g_file_test(db1, G_FILE_TEST_EXISTS));
  g_assert_cmpuint(fake_secret_count(f.secret, hex[1]), ==, 0);
  g_assert_cmpint(gh_account_store_get_state(f.store), ==, GH_ACCOUNT_STORE_OPEN);
  g_autofree gchar *still = dump_settings(f.settings);
  g_assert_cmpstr(still, ==, groundhog_before);

  /* The active account. */
  g_ptr_array_set_size(f.events, 0);
  g_clear_object(&result);
  gh_account_store_forget_async(f.store, hex[2], NULL, gh_test_store_result, &result);
  g_assert_true(gh_account_store_forget_finish(f.store, gh_test_wait(&result), &error));
  g_assert_no_error(error);
  gh_test_run_until_idle();
  g_autofree gchar *closed2 = g_strdup_printf("closed %s", hex[2]);
  g_assert_cmpuint(f.events->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(f.events, 0), ==, closed2);
  g_assert_false(g_file_test(dir2, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(legacy2, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(legacy2_mark, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(pseudonymous2, G_FILE_TEST_EXISTS));
  g_assert_cmpuint(fake_secret_count(f.secret, hex[2]), ==, 0);
  g_autofree gchar *current = g_settings_get_string(f.settings, "current-npub");
  g_assert_cmpstr(current, ==, "");
  g_assert_cmpint(gh_account_store_get_state(f.store), ==, GH_ACCOUNT_STORE_INACTIVE);
  g_assert_cmpuint(open_reqs(&f.rec), ==, 0);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.model)), ==, 0);
  /* Only current-npub changed in Groundhog's settings; Gnostr's not at all. */
  g_autofree gchar *gnostr_after = dump_settings(f.gnostr);
  g_assert_cmpstr(gnostr_after, ==, gnostr_before);
  g_settings_set_string(f.settings, "current-npub", npub[2]);
  gh_test_run_until_idle();
  g_autofree gchar *groundhog_after = dump_settings(f.settings);
  g_assert_cmpstr(groundhog_after, ==, groundhog_before);
  /* Selecting it again starts from nothing: a new key and an empty store. */
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_assert_cmpuint(fake_secret_count(f.secret, hex[2]), ==, 1);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.model)), ==, 0);

  /* A bad argument. */
  g_clear_object(&result);
  gh_account_store_forget_async(f.store, "npub1nope", NULL, gh_test_store_result, &result);
  g_assert_false(gh_account_store_forget_finish(f.store, gh_test_wait(&result), &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  fixture_down(&f);
}

/* W13b review 4a: forgetting the in-memory account ("Continue Without Saving
 * Messages") deletes everything and never asks the keyring, which never held
 * a key for it. */
static void
test_forget_in_memory(void)
{
  Fixture f = { 0 };
  fixture_up(&f, 2);
  fake_secret_set_available(f.secret, FALSE);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_UNAVAILABLE);
  g_assert_true(gh_account_store_continue_without_saving(f.store, NULL));
  publish_inbox_list(&f, 2);
  receive_message(&f, CANARY " in memory, then forgotten");
  fake_secret_clear_calls(f.secret);
  g_autoptr(GAsyncResult) result = NULL;
  g_autoptr(GError) error = NULL;
  gh_account_store_forget_async(f.store, hex[2], NULL, gh_test_store_result, &result);
  g_assert_true(gh_account_store_forget_finish(f.store, gh_test_wait(&result), &error));
  g_assert_no_error(error);
  g_assert_cmpuint(fake_secret_calls(f.secret)->len, ==, 0);
  g_assert_cmpuint(fake_secret_prompts(f.secret), ==, 0);
  gh_test_run_until_idle();
  g_assert_cmpint(gh_account_store_get_state(f.store), ==, GH_ACCOUNT_STORE_INACTIVE);
  g_autofree gchar *current = g_settings_get_string(f.settings, "current-npub");
  g_assert_cmpstr(current, ==, "");
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.model)), ==, 0);
  g_assert_cmpuint(gh_test_count_files(f.data_dir), ==, 0);
  fixture_down(&f);
}

/* W13b review 4a: the store files are deleted but the key item can't be (no
 * keyring now): KEY_KEPT, which says the messages are gone, not a failure to
 * delete them. */
static void
test_forget_key_kept(void)
{
  Fixture f = { 0 };
  fixture_up(&f, 2);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  publish_inbox_list(&f, 2);
  receive_message(&f, CANARY " forget me, key or not");
  g_autofree gchar *db = store_db_path(&f, 2);
  g_autofree gchar *dir = g_path_get_dirname(db);
  fake_secret_set_available(f.secret, FALSE);
  g_autoptr(GAsyncResult) result = NULL;
  g_autoptr(GError) error = NULL;
  gh_account_store_forget_async(f.store, hex[2], NULL, gh_test_store_result, &result);
  g_assert_false(gh_account_store_forget_finish(f.store, gh_test_wait(&result), &error));
  g_assert_error(error, GH_ACCOUNT_STORE_SHRED_ERROR, GH_ACCOUNT_STORE_SHRED_ERROR_KEY_KEPT);
  g_assert_nonnull(strstr(error->message, "couldn't be removed from the keyring"));
  gh_test_run_until_idle();
  g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
  g_autofree gchar *current = g_settings_get_string(f.settings, "current-npub");
  g_assert_cmpstr(current, ==, "");
  g_assert_cmpint(gh_account_store_get_state(f.store), ==, GH_ACCOUNT_STORE_INACTIVE);
  fixture_down(&f);
}

/* STORE_CORRUPT: a damaged store opens read-only for what can still be
 * read; nothing is received or sent from it (no inbox grant, no outbox);
 * "Reset Storage" crypto-shreds it and starts a new one. */
static void
test_corrupt_read_only(void)
{
  Fixture f = { 0 };
  f.with_outbox = TRUE;
  fixture_up(&f, 2);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_autoptr(GError) error = NULL;
  /* Enough pages that damage past the schema lands in table data. */
  g_assert_true(gh_store_exec(gh_account_store_get_store(f.store),
    "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 150) "
    "INSERT INTO contacts (pubkey, petname) SELECT printf('%064d', x), "
    "hex(randomblob(1000)) FROM c", &error));
  g_assert_no_error(error);
  g_autofree gchar *old_id = g_strdup(gh_store_get_store_id(gh_account_store_get_store(f.store)));
  stack_down(&f);
  g_autofree gchar *db = store_db_path(&f, 2);
  GStatBuf st;
  g_assert_cmpint(g_stat(db, &st), ==, 0);
  g_assert_cmpint(st.st_size, >, 64 * 4096);
  int fd = g_open(db, O_RDWR, 0);
  g_assert_cmpint(fd, >=, 0);
  for (goffset page = 2; page <= 12; page++) {
    const off_t offset = (off_t)(st.st_size - page * 4096 + 1000);
    guint8 byte = 0;
    g_assert_cmpint(pread(fd, &byte, 1, offset), ==, 1);
    byte ^= 0x40;
    g_assert_cmpint(pwrite(fd, &byte, 1, offset), ==, 1);
  }
  close(fd);

  stack_up(&f);
  g_autoptr(GhStatus) status = gh_status_new();
  gh_status_set_account_active(status, TRUE);
  gh_store_status_attach(status, f.store);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_CORRUPT);
  g_assert_nonnull(gh_account_store_get_store(f.store));
  g_assert_true(gh_store_is_read_only(gh_account_store_get_store(f.store)));
  g_assert_null(gh_account_store_get_outbox(f.store));
  g_assert_false(gh_dm_inbox_has_storage(f.inbox));
  publish_inbox_list(&f, 2);
  g_assert_cmpuint(f.rec.reqs->len, ==, 0);
  g_assert_cmpint(gh_status_get_banner(status), ==, GH_STATUS_BANNER_STORE_CORRUPT);
  /* Its button opens the Reset Storage confirmation (§7.15 #16). */
  g_assert_cmpstr(gh_status_banner_get_action(GH_STATUS_BANNER_STORE_CORRUPT), ==,
                  GH_STATUS_ACTION_STORE_START_FRESH);
  g_assert_cmpstr(gh_status_banner_get_button_label(GH_STATUS_BANNER_STORE_CORRUPT), ==,
                  "Reset Storage…");
  g_assert_false(gh_account_store_retry(f.store));

  g_autoptr(GAsyncResult) result = NULL;
  gh_account_store_start_fresh_async(f.store, NULL, gh_test_store_result, &result);
  g_assert_true(gh_account_store_start_fresh_finish(f.store, gh_test_wait(&result), &error));
  g_assert_no_error(error);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_assert_cmpstr(gh_store_get_store_id(gh_account_store_get_store(f.store)), !=, old_id);
  g_assert_true(gh_dm_inbox_has_storage(f.inbox));
  g_assert_nonnull(gh_account_store_get_outbox(f.store));
  g_assert_nonnull(open_req(&f.rec, INBOX_A));
  fixture_down(&f);
}

/* ---- the window on the executable's wiring (--gui) ------------------------------ */

static void
drain(void)
{
  for (int i = 0; i < 500 && g_main_context_iteration(NULL, FALSE); i++)
    ;
}

static gboolean
window_mapped(gpointer data)
{
  return gtk_widget_get_mapped(GTK_WIDGET(data));
}

/* A window of the process, attached as gh_app_services_attach_window()
 * attaches it: the conversation list, the store's status and its history. */
static GhWindow *
window_up(Fixture *f)
{
  GhWindow *window = gh_window_new(NULL);
  gh_conversation_list_attach(window, f->model, f->settings);
  gh_store_status_attach(gh_window_get_status(window), f->store);
  gh_store_status_attach_history(window, f->store);
  gtk_window_set_default_size(GTK_WINDOW(window), 1000, 700);
  gtk_window_present(GTK_WINDOW(window));
  gh_test_spin_until(window_mapped, window);
  drain();
  return window;
}

static void
window_down(GhWindow *window)
{
  gtk_window_destroy(GTK_WINDOW(window));
  drain();
}

static GhConversationView *
view_of(GhWindow *window)
{
  return GH_CONVERSATION_VIEW(gh_content_page_get_view(gh_window_get_content(window)));
}

typedef struct {
  GhAppOutbox *sender;
  GhConversationView *view;
} ReactionViewLink;

static void
share_reactions_on_store_change(GhAccountStore *store, ReactionViewLink *link)
{
  (void)store;
  gh_conversation_view_set_reaction_store(link->view,
                                          gh_app_outbox_get_reactions(link->sender));
}

static void
assert_view_reactions(GhConversationView *view, const gchar *target_id, guint count)
{
  GListModel *timeline = gh_conversation_view_get_timeline(view);
  g_assert_nonnull(timeline);
  for (guint i = 0; i < g_list_model_get_n_items(timeline); i++) {
    g_autoptr(GhTimelineItem) item = g_list_model_get_item(timeline, i);
    GhMessage *message = gh_timeline_item_get_message(item);
    if (message && g_strcmp0(gh_message_get_rumor_id(message), target_id) == 0) {
      GhReactionSummary *summary = gh_timeline_item_get_reaction_summary(item);
      g_assert_nonnull(summary);
      g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, count);
      return;
    }
  }
  g_error("reaction target is absent from the conversation view");
}

/* key 1's kind-14 rumor n to the account (key 2), admitted as the inbox
 * admits an unwrapped one (T-admit through the store's delegate), under a
 * synthetic wrap id. */
static void
admit_from_peer(Fixture *f, gint64 created_at, guint n)
{
  NostrEvent *rumor = nostr_event_new();
  nostr_event_set_kind(rumor, 14);
  nostr_event_set_pubkey(rumor, hex[1]);
  nostr_event_set_created_at(rumor, created_at);
  g_autofree gchar *content = g_strdup_printf(CANARY " message %u", n);
  nostr_event_set_content(rumor, content);
  nostr_event_set_tags(rumor, nostr_tags_new(1, nostr_tag_new("p", hex[2], NULL)));
  rumor->id = nostr_event_get_id(rumor);
  char *json = nostr_event_serialize_compact(rumor);
  nostr_event_free(rumor);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) message = gh_message_new_from_rumor(hex[2], json, &error);
  free(json);
  g_assert_no_error(error);
  g_autofree gchar *wrap_id = g_strdup_printf("%064x", n + 1);
  g_assert_cmpint(gh_conversation_store_admit(f->model, message, wrap_id, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_no_error(error);
}

static GhConversation *
history_room(Fixture *f)
{
  g_autofree gchar *room = room_of(1, 2);
  GhConversation *conversation = gh_conversation_store_lookup(f->model, room);
  g_assert_nonnull(conversation);
  return conversation;
}

/* The app-services store-change binding, exercised with a real window and
 * controller A→B→A selection: restored chips and a new reaction use A's
 * reopened store, not the old model. */
static void
test_reactions_switch_view(void)
{
  Fixture f = { .with_outbox = TRUE };
  fixture_up(&f, 2);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  admit_from_peer(&f, g_get_real_time() / G_USEC_PER_SEC - 60, 701);
  GhConversation *room = history_room(&f);
  gh_conversation_accept(room);
  g_autoptr(GhMessage) target = g_list_model_get_item(G_LIST_MODEL(room), 0);
  g_autofree gchar *target_id = g_strdup(gh_message_get_rumor_id(target));
  g_autofree gchar *room_id = g_strdup(gh_conversation_get_room_id(room));
  g_autoptr(GhReaction) first = gh_reaction_new(target_id, "view-reaction-one",
                                                 hex[1], "+", 10, room_id);
  g_assert_true(gh_reaction_store_admit(gh_app_outbox_get_reactions(f.sender), first, NULL));

  GhWindow *window = window_up(&f);
  GhConversationView *view = view_of(window);
  ReactionViewLink link = { f.sender, view };
  g_signal_connect(f.store, "changed", G_CALLBACK(share_reactions_on_store_change), &link);
  share_reactions_on_store_change(f.store, &link);
  gh_window_open_item(window, room);
  drain();
  assert_view_reactions(view, target_id, 1);

  select_key(&f, 1);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  select_key(&f, 2);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  room = history_room(&f);
  gh_window_open_item(window, room);
  drain();
  assert_view_reactions(view, target_id, 1);

  g_autoptr(GhReaction) second = gh_reaction_new(target_id, "view-reaction-two",
                                                  hex[2], "❤", 11, room_id);
  g_assert_true(gh_reaction_store_admit(gh_app_outbox_get_reactions(f.sender), second, NULL));
  assert_view_reactions(view, target_id, 2);
  g_signal_handlers_disconnect_by_data(f.store, &link);
  window_down(window);
  fixture_down(&f);
}

typedef struct {
  GhConversation *room;
  GhConversationView *view;
  guint listed;
} ListedWait;

static gboolean
listed_and_idle(gpointer data)
{
  ListedWait *wait = data;
  return g_list_model_get_n_items(G_LIST_MODEL(wait->room)) == wait->listed &&
         !gh_conversation_view_get_loading_older(wait->view);
}

#define HISTORY_READ   40 /* read in session 1 */
#define HISTORY_UNREAD 80 /* then received: more than a page of each */
#define PAGE           GH_STORE_CONVERSATIONS_PAGE_SIZE

/* W13b review B1 on the executable's wiring, over the same directories as a
 * new process would use: after a restart a room lists its newest page;
 * opening it reads only what is listed (the unloaded unread stay unread,
 * after another restart too, since no marker is written past them);
 * scrolling to the top lists the older page, which is then read and
 * written; reaching the top again lists the next. */
static void
test_restart_pages_history(void)
{
  Fixture f = { 0 };
  fixture_up(&f, 2);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  gint64 start = g_get_real_time() / G_USEC_PER_SEC - 100000;
  admit_from_peer(&f, start, 0);
  GhConversation *room = history_room(&f);
  gh_conversation_window_set_active(room, TRUE);
  for (guint i = 1; i < HISTORY_READ; i++)
    admit_from_peer(&f, start + i * 60, i);
  gh_conversation_accept(room);
  gh_conversation_mark_read(room);
  gh_conversation_window_set_active(room, FALSE);
  for (guint i = HISTORY_READ; i < HISTORY_READ + HISTORY_UNREAD; i++)
    admit_from_peer(&f, start + i * 60, i);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, HISTORY_UNREAD);
  stack_down(&f);

  /* Session 2: the newest page is listed; the count covers the rest. */
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  room = history_room(&f);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(room)), ==, PAGE);
  g_assert_true(gh_conversation_get_has_older(room));
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, HISTORY_UNREAD);

  /* Opening it reads the listed page and nothing that is not listed. */
  GhWindow *window = window_up(&f);
  g_assert_true(gh_sidebar_page_select_relative(gh_window_get_sidebar(window), 1));
  g_assert_true(gh_conversation_view_get_conversation(view_of(window)) == room);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(room)), ==, PAGE);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, HISTORY_UNREAD - PAGE);
  /* Nothing of that was written: after a restart all of it is unread. */
  window_down(window);
  stack_down(&f);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  room = history_room(&f);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, HISTORY_UNREAD);

  /* Scrolling to the top lists the older page from the store; its unread
   * messages are read once listed, and now the marker is written. */
  window = window_up(&f);
  GhConversationView *view = view_of(window);
  g_assert_true(gh_sidebar_page_select_relative(gh_window_get_sidebar(window), 1));
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, HISTORY_UNREAD - PAGE);
  GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW(
    gtk_widget_get_template_child(GTK_WIDGET(view), GH_TYPE_CONVERSATION_VIEW, "scroller"));
  gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(scroller), 0);
  ListedWait two_pages = { room, view, PAGE + GH_CONVERSATION_WINDOW_PAGE };
  gh_test_spin_until(listed_and_idle, &two_pages);
  /* GTK 4.14 updates the list's scroll anchor on the next layout frame. */
  gtk_test_widget_wait_for_draw(GTK_WIDGET(window));
  g_assert_false(gh_conversation_view_get_older_failed(view));
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==,
                   HISTORY_UNREAD - PAGE - GH_CONVERSATION_WINDOW_PAGE);
  g_assert_true(gh_conversation_get_has_older(room));
  /* The reader stays where they were, now a page of older messages below
   * the top; reaching the top again lists one more 25-message page (there
   * is no "Earlier Messages" button, nostrc-p15n5.2). Remaining stored
   * history continues to be available in further pages, never all at once. */
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(scroller);
  g_assert_cmpfloat(gtk_adjustment_get_value(adj), >, gtk_adjustment_get_page_size(adj));
  gtk_adjustment_set_value(adj, 0);
  ListedWait third_page = { room, view, PAGE + 2 * GH_CONVERSATION_WINDOW_PAGE };
  gh_test_spin_until(listed_and_idle, &third_page);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 0);
  g_autoptr(GError) paging_error = NULL;
  while (gh_conversation_get_has_older(room)) {
    guint loaded = 0;
    g_assert_true(gh_account_store_load_older(f.store, room,
      GH_CONVERSATION_WINDOW_PAGE, &loaded, &paging_error));
    g_assert_no_error(paging_error);
    g_assert_cmpuint(loaded, >, 0);
  }
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(room)), ==,
                   HISTORY_READ + HISTORY_UNREAD);
  g_assert_false(gh_conversation_get_has_older(room));
  window_down(window);
  stack_down(&f);

  /* Read for good: the next session starts with nothing unread. */
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  room = history_room(&f);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(room)), ==, PAGE);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 0);
  g_assert_false(g_file_test(f.state_dir, G_FILE_TEST_EXISTS));
  fixture_down(&f);
}

/* W13b review, non-blocking #3: KEY_MISSING's "Start Fresh on This Device…"
 * asks first (charter §3.4 copy, Cancel by default, Try Again offered), and
 * only its destructive response crypto-shreds the store and opens a new one. */
static void
test_start_fresh_dialog(void)
{
  Fixture f = { 0 };
  fixture_up(&f, 2);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_autofree gchar *old_id = g_strdup(gh_store_get_store_id(gh_account_store_get_store(f.store)));
  stack_down(&f);
  g_autofree gchar *db = store_db_path(&f, 2);
  g_autofree gchar *before = file_sha256(db);
  g_object_unref(f.secret);
  f.secret = fake_secret_new();
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_KEY_MISSING);
  GhWindow *window = window_up(&f);

  AdwAlertDialog *alert = gh_store_status_confirm_start_fresh(GTK_WIDGET(window), f.store);
  g_assert_nonnull(alert);
  g_assert_cmpstr(adw_alert_dialog_get_heading(alert), ==, "Start Fresh on This Device?");
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(alert), "encrypted group history can't"));
  g_assert_true(adw_alert_dialog_has_response(alert, "retry"));
  g_assert_cmpint(adw_alert_dialog_get_response_appearance(alert, "start-fresh"), ==,
                  ADW_RESPONSE_DESTRUCTIVE);
  g_assert_cmpstr(adw_alert_dialog_get_default_response(alert), ==, "cancel");
  g_assert_cmpstr(adw_alert_dialog_get_close_response(alert), ==, "cancel");
  g_signal_emit_by_name(alert, "response", "cancel");
  adw_dialog_force_close(ADW_DIALOG(alert));
  drain();
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_KEY_MISSING);
  g_autofree gchar *after = file_sha256(db);
  g_assert_cmpstr(before, ==, after);
  g_assert_cmpuint(fake_secret_count(f.secret, hex[2]), ==, 0);

  alert = gh_store_status_confirm_start_fresh(GTK_WIDGET(window), f.store);
  g_signal_emit_by_name(alert, "response", "start-fresh");
  adw_dialog_force_close(ADW_DIALOG(alert));
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_assert_cmpstr(gh_store_get_store_id(gh_account_store_get_store(f.store)), !=, old_id);
  g_assert_cmpuint(fake_secret_count(f.secret, hex[2]), ==, 1);
  /* Only for a missing key or damaged storage. */
  g_assert_null(gh_store_status_confirm_start_fresh(GTK_WIDGET(window), f.store));
  window_down(window);
  fixture_down(&f);
}

/* The status mapping is total and every store banner's copy exists. */
static void
test_status_mapping(void)
{
  static const struct {
    GhAccountStoreState state;
    GhStatusStore status;
    GhStatusBanner banner;
  } map[] = {
    { GH_ACCOUNT_STORE_INACTIVE, GH_STATUS_STORE_NONE, GH_STATUS_BANNER_NONE },
    { GH_ACCOUNT_STORE_OPENING, GH_STATUS_STORE_OPENING, GH_STATUS_BANNER_STORE_OPENING },
    { GH_ACCOUNT_STORE_OPEN, GH_STATUS_STORE_OPEN, GH_STATUS_BANNER_NONE },
    { GH_ACCOUNT_STORE_EPHEMERAL, GH_STATUS_STORE_EPHEMERAL, GH_STATUS_BANNER_STORE_EPHEMERAL },
    { GH_ACCOUNT_STORE_LOCKED, GH_STATUS_STORE_LOCKED, GH_STATUS_BANNER_STORE_LOCKED },
    { GH_ACCOUNT_STORE_UNAVAILABLE, GH_STATUS_STORE_UNAVAILABLE,
      GH_STATUS_BANNER_STORE_UNAVAILABLE },
    { GH_ACCOUNT_STORE_KEY_MISSING, GH_STATUS_STORE_KEY_MISSING,
      GH_STATUS_BANNER_STORE_KEY_MISSING },
    { GH_ACCOUNT_STORE_CORRUPT, GH_STATUS_STORE_CORRUPT, GH_STATUS_BANNER_STORE_CORRUPT },
    { GH_ACCOUNT_STORE_ERROR, GH_STATUS_STORE_ERROR, GH_STATUS_BANNER_STORE_ERROR },
  };
  g_autoptr(GhStatus) status = gh_status_new();
  gh_status_set_account_active(status, TRUE);
  for (guint i = 0; i < G_N_ELEMENTS(map); i++) {
    g_assert_cmpint(gh_store_status_map(map[i].state), ==, map[i].status);
    gh_status_set_store(status, map[i].status, "reason");
    g_assert_cmpint(gh_status_get_banner(status), ==, map[i].banner);
    if (map[i].banner != GH_STATUS_BANNER_NONE)
      g_assert_cmpstr(gh_status_banner_get_title(map[i].banner), !=, "");
    /* A button always has an action behind it. */
    g_assert_cmpint(gh_status_banner_get_button_label(map[i].banner) == NULL, ==,
                    gh_status_banner_get_action(map[i].banner) == NULL);
  }
  /* Progress and the in-memory note never hide an inbox problem. */
  gh_status_set_store(status, GH_STATUS_STORE_EPHEMERAL, NULL);
  gh_status_set_inbox(status, GH_STATUS_INBOX_UNREACHABLE, NULL);
  g_assert_cmpint(gh_status_get_banner(status), ==, GH_STATUS_BANNER_INBOX_UNREACHABLE);
  gh_status_set_inbox(status, GH_STATUS_INBOX_CONNECTING, NULL);
  g_assert_cmpint(gh_status_get_banner(status), ==, GH_STATUS_BANNER_STORE_EPHEMERAL);
  gh_status_set_account_active(status, FALSE);
  g_assert_cmpint(gh_status_get_banner(status), ==, GH_STATUS_BANNER_NONE);
}

#ifdef __linux__
/* ---- the open worker's OpenSSL state (nostrc-vpha) ------------------------------------
 * SQLCipher's key derivation and page cipher leave OpenSSL state on the thread
 * that opens a store (its random generators and error queue); OpenSSL frees it
 * from a thread-local key's destructor when the thread ends. The open worker
 * is a GTask pool thread, which may end after exit() has run OPENSSL_cleanup():
 * that deletes the key, nothing frees the state, and LeakSanitizer reports it
 * (EVP_RAND_CTX_new / ERR_set_mark <- libsqlcipher, groundhog-privacy-e2e under
 * load). The last case forces that order. hold_key, made first thing in
 * main() before OpenSSL makes its keys, has its destructor run first when the
 * worker ends and holds the worker there until release_held_worker(), which
 * atexit() runs after OPENSSL_cleanup() (registered before it). */
static pthread_key_t hold_key;
static sem_t hold_release;
static GThread *main_thread;
static gint held_tid; /* atomic: the held worker, 0 until one is */

static void
hold_worker_exit(gpointer value)
{
  (void)value;
  sem_wait(&hold_release);
}

static void
release_held_worker(void)
{
  const gint tid = g_atomic_int_get(&held_tid);
  if (!tid)
    return;
  sem_post(&hold_release);
  /* LeakSanitizer scans a live thread's thread-local storage (and would find
   * the state there): wait until the worker is gone. */
  char path[64];
  g_snprintf(path, sizeof path, "/proc/self/task/%d", tid);
  const gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (access(path, F_OK) == 0 && g_get_monotonic_time() < deadline)
    g_usleep(1000);
}

/* The system clock, but its first call off the main thread marks that thread
 * (the open worker: creating the schema reads the time, after SQLCipher keyed
 * the store) to be held when it ends. */
static gint64
marking_real_time(gpointer data)
{
  if (g_thread_self() != main_thread) {
    const gint tid = (gint)syscall(SYS_gettid);
    if (g_atomic_int_compare_and_exchange(&held_tid, 0, tid))
      g_assert_cmpint(pthread_setspecific(hold_key, GINT_TO_POINTER(1)), ==, 0);
  }
  return gh_clock_get_real_time(data);
}

static gint64
marking_monotonic_time(gpointer data)
{
  return gh_clock_get_monotonic_time(data);
}

static guint
marking_timeout_add(gpointer data, guint64 interval_ms, GSourceFunc func, gpointer func_data,
                    GDestroyNotify notify)
{
  return gh_clock_timeout_add(data, interval_ms, func, func_data, notify);
}

static gboolean
marking_source_remove(gpointer data, guint id)
{
  return gh_clock_source_remove(data, id);
}

static guint32
marking_random_uniform(gpointer data, guint32 upper_bound)
{
  return gh_clock_random_uniform(data, upper_bound);
}

/* The open worker frees its OpenSSL state before it reports the store open:
 * once the owner has the result it may exit, and the worker may end after. */
static void
test_open_worker_frees_openssl_state(void)
{
  static const GhClockVTable marking = {
    marking_real_time, marking_monotonic_time, marking_timeout_add,
    marking_source_remove, marking_random_uniform,
  };
  Fixture f = { 0 };
  fixture_up(&f, 2);
  f.clock = gh_clock_new(&marking, gh_clock_new_system(), (GDestroyNotify)gh_clock_unref);
  /* Pool threads end as soon as they are idle: the worker ends (and is held)
   * right after the open, not whenever the pool retires it. */
  g_thread_pool_set_max_unused_threads(0);
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  /* A worker created the schema, so it is the one held. */
  g_assert_cmpint(g_atomic_int_get(&held_tid), !=, 0);
  fixture_down(&f);
}
#endif

int
main(int argc, char **argv)
{
#ifdef __linux__
  /* Before anything initializes OpenSSL: see test_open_worker_frees_openssl_state. */
  main_thread = g_thread_self();
  g_assert_cmpint(pthread_key_create(&hold_key, hold_worker_exit), ==, 0);
  g_assert_cmpint(sem_init(&hold_release, 0, 0), ==, 0);
  g_assert_cmpint(atexit(release_held_worker), ==, 0);
#endif
  /* Private XDG homes before anything asks GLib for them: KC-4 proves no
   * file lands in any of them. */
  xdg_root = g_dir_make_tmp("groundhog-account-store-xdg-XXXXXX", NULL);
  g_assert_nonnull(xdg_root);
  static const gchar *const vars[] = { "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME",
                                       "XDG_CONFIG_HOME" };
  for (guint i = 0; i < G_N_ELEMENTS(vars); i++) {
    g_autofree gchar *dir = g_build_filename(xdg_root, vars[i], NULL);
    g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
    g_setenv(vars[i], dir, TRUE);
  }
  gui_mode = argc > 1 && g_str_equal(argv[1], "--gui");
  if (gui_mode) {
    argv[1] = argv[0];
    argv++;
    argc--;
    /* One bus for the process, up before GTK: GTK and libadwaita keep their
     * session connection (the settings portal lookup) for the life of the
     * process, so it is never brought down; nostrc-test-bus's lifeline
     * supervisor stops it when this process exits. No accessibility bus.
     * GTK starts before g_test_init(), as in the other GUI tests: a host
     * theme's parser warnings are not this test's to fail on. */
    g_setenv("GTK_A11Y", "none", TRUE);
    gh_test_bus_up(&shared_bus);
    if (!gtk_init_check()) {
      g_printerr("groundhog-account-store GUI tests skipped: no graphical display\n");
      gh_test_remove_tree(xdg_root);
      return 77;
    }
    adw_init();
    groundhog_register_resource();
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
  }
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    npub[key] = gh_test_npub(key);
    hex[key] = gh_test_pub(key);
  }
  if (gui_mode) {
    g_test_add_func("/groundhog/account-store-gui/restart-pages-history",
                    test_restart_pages_history);
    g_test_add_func("/groundhog/account-store-gui/start-fresh-dialog", test_start_fresh_dialog);
    g_test_add_func("/groundhog/account-store-gui/reactions-switch-view",
                    test_reactions_switch_view);
    int status = g_test_run();
    gh_test_remove_tree(xdg_root);
    return status;
  }
  g_test_add_func("/groundhog/account-store/status-mapping", test_status_mapping);
  g_test_add_func("/groundhog/account-store/open-receive-restore", test_open_receive_restore);
  g_test_add_func("/groundhog/account-store/remote-cache-gate-offline-restart",
                  test_remote_cache_gate_and_offline_restart);
  g_test_add_func("/groundhog/account-store/remote-memory-only-refused",
                  test_remote_memory_only_refused);
  g_test_add_func("/groundhog/account-store/remote-failed-restore",
                  test_remote_failed_restore_keeps_gate_closed);
  g_test_add_func("/groundhog/account-store/pseudonymous-names", test_pseudonymous_names);
  g_test_add_func("/groundhog/account-store/legacy-import", test_legacy_import);
  g_test_add_func("/groundhog/account-store/restart-relists", test_restart_relists);
  g_test_add_func("/groundhog/account-store/ephemeral-restart-refetches",
                  test_ephemeral_restart_refetches);
  g_test_add_func("/groundhog/account-store/kc2-locked", test_kc2_locked);
  g_test_add_func("/groundhog/account-store/kc3-key-missing", test_kc3_key_missing);
  g_test_add_func("/groundhog/account-store/kc4-unavailable", test_kc4_unavailable);
  g_test_add_func("/groundhog/account-store/pt6-switch-order", test_pt6_switch_order);
  g_test_add_func("/groundhog/account-store/pt6-switch-mid-open", test_pt6_switch_mid_open);
  g_test_add_func("/groundhog/account-store/st8-forget", test_st8_forget);
  g_test_add_func("/groundhog/account-store/forget-in-memory", test_forget_in_memory);
  g_test_add_func("/groundhog/account-store/forget-key-kept", test_forget_key_kept);
  g_test_add_func("/groundhog/account-store/corrupt-read-only", test_corrupt_read_only);
#ifdef __linux__
  /* Last: its worker ends at exit. */
  g_test_add_func("/groundhog/account-store/open-worker-frees-openssl-state",
                  test_open_worker_frees_openssl_state);
#endif
  int status = g_test_run();
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    g_free(npub[key]);
    g_free(hex[key]);
  }
  gh_test_remove_tree(xdg_root);
  g_free(xdg_root);
  return status;
}
