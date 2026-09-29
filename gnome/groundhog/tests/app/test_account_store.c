/* GhAccountStore at app level (privacy charter §8.2 G04; tests ST-8, KC-2,
 * KC-3, KC-4 and PT-6 of §9.2): a real account controller on a private bus
 * with the mock signer, the FakeSecret key backend (H5, which records every
 * call's flags), recording relay transports (H1), and the real NIP-17 inbox,
 * store delegate (G05), outbox (G06) and SQLCipher stores in private
 * directories. Nothing sleeps: every wait is on an observable condition. */
#include "account-store-outbox.h"
#include "gh-account-store.h"
#include "gh-app-outbox.h"
#include "gh-inbox-resolver.h"
#include "gh-status.h"
#include "gh-store-status.h"
#include "gh-test-signer.h"
#include "fake-secret.h"

#include "nostr-tag.h"
#include "nostr/nip59/nip59.h"

#include <fcntl.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <unistd.h>

#define DISCOVERY "wss://discovery.test.invalid"
#define INBOX_A "wss://inbox-a.test.invalid"
#define CANARY "GROUNDHOG-CANARY-account-store"

static gchar *npub[GH_TEST_KEYS];
static gchar *hex[GH_TEST_KEYS];
static gchar *xdg_root;

/* ---- recording relay transport (H1) ------------------------------------------ */

typedef struct {
  GhRelayScope *scope;
  gchar *url;
  gint64 since;
  gboolean closed;
} Req;

typedef struct {
  GPtrArray *reqs;      /* Req: every DM inbox REQ ever opened */
  GPtrArray *discovery; /* GhRelayScope: every relay-list discovery REQ */
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
  /* Nothing but the DM inbox ever opens a REQ here. */
  g_assert_cmpint(nostr_filter_kinds_get(filter, 0), ==, 1059);
  Req *req = g_new0(Req, 1);
  req->scope = gh_relay_scope_ref(scope);
  req->url = g_strdup(url);
  req->since = nostr_filter_get_since_i64(filter);
  g_ptr_array_add(rec->reqs, req);
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
  /* The stack: what one Groundhog process owns. */
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GhConversationStore *model;
  GhDmInbox *inbox;
  GhStoreKey *store_key;
  FakeResolver *resolver;
  GhAppOutbox *sender;
  GhAccountStore *store;
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
  f->accounts = gh_account_controller_new_full(f->settings, f->bus.client, fake_list, NULL);
  gh_test_spin_until(listed, f->accounts);
  f->relays = gh_account_relays_new(f->accounts, f->settings, &recorder_transport, &f->rec);
  f->model = gh_conversation_store_new();
  f->inbox = gh_dm_inbox_new_with_storage(f->accounts, f->relays, f->model,
                                          &recorder_transport, &recorder_auth, &f->rec);
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
  gh_test_release(g_steal_pointer(&f->inbox));
  g_clear_object(&f->model);
  gh_test_release(g_steal_pointer(&f->relays));
  gh_test_release(g_steal_pointer(&f->accounts));
  GhTestSenders check = { &f->bus, &f->signer };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  g_ptr_array_set_size(f->rec.reqs, 0);
  g_ptr_array_set_size(f->rec.discovery, 0);
}

static void
fixture_up(Fixture *f, guint active_key)
{
  g_autoptr(GError) error = NULL;
  gh_test_bus_up(&f->bus);
  gh_test_signer_up(&f->bus, &f->signer);
  f->rec.reqs = g_ptr_array_new_with_free_func(req_free);
  f->rec.discovery = g_ptr_array_new_with_free_func((GDestroyNotify)gh_relay_scope_unref);
  f->events = g_ptr_array_new_with_free_func(g_free);
  f->settings = g_settings_new("org.nostr.Groundhog");
  f->gnostr = g_settings_new("org.gnostr.Client");
  g_settings_set_string(f->gnostr, "current-npub", npub[1]);
  const gchar *sources[] = { DISCOVERY, NULL };
  g_settings_set_strv(f->settings, "discovery-relays", sources);
  g_settings_set_string(f->settings, "signer-method", "auto");
  g_settings_set_string(f->settings, "current-npub", active_key ? npub[active_key] : "");
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
  g_ptr_array_unref(f->events);
  g_settings_reset(f->gnostr, "current-npub");
  g_object_unref(f->gnostr);
  g_object_unref(f->settings);
  g_object_unref(f->secret);
  gh_test_bus_down(&f->bus);
  gh_test_remove_tree(f->root);
  g_free(f->root);
  g_free(f->data_dir);
  g_free(f->state_dir);
  g_free(f->other_store_path);
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

/* A NIP-17 message from key `from` to key `to`, gift-wrapped (NIP-59). */
static gchar *
craft_wrap(guint from, guint to, gint64 created_at, const gchar *content)
{
  NostrEvent *rumor = nostr_event_new();
  nostr_event_set_kind(rumor, 14);
  nostr_event_set_pubkey(rumor, hex[from]);
  nostr_event_set_created_at(rumor, created_at);
  nostr_event_set_content(rumor, content);
  nostr_event_set_tags(rumor, nostr_tags_new(1, nostr_tag_new("p", hex[to], NULL)));
  rumor->id = nostr_event_get_id(rumor);
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

/* key 1 sends the active account (key 2) a message on its inbox relay. */
static void
receive_message(Fixture *f, const gchar *content)
{
  Req *req = open_req(&f->rec, INBOX_A);
  g_assert_nonnull(req);
  g_autofree gchar *wrap = craft_wrap(1, 2, g_get_real_time() / G_USEC_PER_SEC - 60, content);
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

/* ST-12 via the app: the old plaintext seen file and checkpoint are imported
 * into the store once and deleted; the inbox's since comes from the store. */
static void
test_legacy_import(void)
{
  Fixture f = { 0 };
  fixture_up(&f, 2);
  g_autofree gchar *wrap_id = g_strnfill(64, 'a');
  g_autofree gchar *seen = g_strdup_printf("groundhog-nip17-seen 1 %s\nw %s\n", hex[2], wrap_id);
  gint64 checkpoint = g_get_real_time() / G_USEC_PER_SEC - 3600;
  g_autofree gchar *mark = g_strdup_printf("groundhog-dm-inbox-checkpoint 1 %s %" G_GINT64_FORMAT
                                           "\n", hex[2], checkpoint);
  g_autofree gchar *seen_path = g_strdup_printf("%s/%s.seen", f.state_dir, hex[2]);
  g_autofree gchar *mark_path = g_strdup_printf("%s/%s.checkpoint", f.state_dir, hex[2]);
  g_assert_cmpint(g_mkdir(f.state_dir, 0700), ==, 0);
  g_assert_true(g_file_set_contents_full(seen_path, seen, -1, G_FILE_SET_CONTENTS_CONSISTENT,
                                         0600, NULL));
  g_assert_true(g_file_set_contents_full(mark_path, mark, -1, G_FILE_SET_CONTENTS_CONSISTENT,
                                         0600, NULL));
  stack_up(&f);
  g_assert_cmpint(settle(&f), ==, GH_ACCOUNT_STORE_OPEN);
  g_assert_false(g_file_test(seen_path, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(mark_path, G_FILE_TEST_EXISTS));
  GhStore *store = gh_account_store_get_store(f.store);
  gboolean has = FALSE;
  g_assert_true(gh_store_seen_contains(store, GH_STORE_SEEN_WRAP, wrap_id, &has, NULL));
  g_assert_true(has);
  gint64 since = 0;
  g_assert_true(gh_store_get_cursor(store, GH_ACCOUNT_STORE_INBOX_CURSOR, "", &since, NULL));
  g_assert_cmpint(since, ==, checkpoint);
  publish_inbox_list(&f, 2);
  Req *req = open_req(&f.rec, INBOX_A);
  g_assert_nonnull(req);
  g_assert_cmpint(req->since, ==, checkpoint - GH_DM_INBOX_WRAP_SKEW);
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
  g_autofree gchar *room = room_of(1, 2);
  g_assert_nonnull(gh_conversation_store_lookup(f.model, room));
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
  g_assert_cmpint(g_mkdir_with_parents(f.state_dir, 0700), ==, 0);
  g_assert_true(g_file_set_contents(legacy2, "stale", -1, NULL));
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
  /* Resetting needs a confirmation dialog, so the banner has no button. */
  g_assert_null(gh_status_banner_get_action(GH_STATUS_BANNER_STORE_CORRUPT));
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

int
main(int argc, char **argv)
{
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
  g_test_init(&argc, &argv, NULL);
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    npub[key] = gh_test_npub(key);
    hex[key] = gh_test_pub(key);
  }
  g_test_add_func("/groundhog/account-store/status-mapping", test_status_mapping);
  g_test_add_func("/groundhog/account-store/open-receive-restore", test_open_receive_restore);
  g_test_add_func("/groundhog/account-store/legacy-import", test_legacy_import);
  g_test_add_func("/groundhog/account-store/kc2-locked", test_kc2_locked);
  g_test_add_func("/groundhog/account-store/kc3-key-missing", test_kc3_key_missing);
  g_test_add_func("/groundhog/account-store/kc4-unavailable", test_kc4_unavailable);
  g_test_add_func("/groundhog/account-store/pt6-switch-order", test_pt6_switch_order);
  g_test_add_func("/groundhog/account-store/pt6-switch-mid-open", test_pt6_switch_mid_open);
  g_test_add_func("/groundhog/account-store/st8-forget", test_st8_forget);
  g_test_add_func("/groundhog/account-store/corrupt-read-only", test_corrupt_read_only);
  int status = g_test_run();
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    g_free(npub[key]);
    g_free(hex[key]);
  }
  gh_test_remove_tree(xdg_root);
  g_free(xdg_root);
  return status;
}
