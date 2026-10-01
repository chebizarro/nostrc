/* Shared fixture of the MLS service tests (nostrc-qp24.13 part 1): up to
 * three Groundhog accounts in one process, each composed as the app composes
 * one (account controller, own relay lists, the durable store with its
 * NIP-17 and MLS room delegates, the DM inbox in storage mode, a one-shot
 * 10050 lookup as the Welcome resolver, and the GhMlsService), against local
 * store-and-serve relays (wire-relay.h) through the real gnostr transports:
 *  - E, the discovery relay: everyone's kind 10002 (write: W) and 10050
 *    (inbox: X), and the discovery-relays setting of every account;
 *  - W, the write relay of every account: KeyPackages land here (and on X);
 *  - X, the inbox relay of every account: kind 1059 is served only to its
 *    authenticated recipient (NIP-17);
 *  - G, the group relay: demands NIP-42 AUTH for every REQ and EVENT, so
 *    MLS routing's ephemeral AUTH runs for real.
 * org.nostr.Signer is the mock signer on the private test bus
 * (gh-test-signer.h): it holds the test keys; Groundhog holds none. Nothing
 * sleeps: every wait is for an observable condition, bounded only to turn a
 * hang into a failure. Header-only: include it once per test executable. */
#ifndef GH_TEST_MLS_WORLD_H
#define GH_TEST_MLS_WORLD_H

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>

#include "gh-inbox-lookup.h"
#include "gh-mls-service.h"
#include "gh-store-conversations.h"
#include "gh-store-marmot.h"
#include "gh-test-signer.h"
#include "wire-relay.h"

#include <nostr-keys.h>

#include <glib/gstdio.h>

enum { ALICE = 1, BOB = 2, CAROL = 3, STRANGER = 4 };
#define N_APPS 4

/* A failure bound for one awaited condition (sanitizer builds are slow). */
#define WAIT_SECONDS 90

static gchar *hex[GH_TEST_KEYS];
static gchar *npub[GH_TEST_KEYS];
static GhTestBus test_bus;
/* The next world's accounts run on a fake store clock that does not move
 * (e.g. two sends within one second, deterministically). */
static gboolean world_fake_clock;
/* The next world's kind-10002 lists split relays by marker (nostrc-0bdg):
 * ["r", W, "write"], ["r", H] (unmarked: read and write) and ["r", R,
 * "read"] instead of an unmarked W. */
static gboolean world_split_lists;
/* The next world's services: KeyPackage rotation age and the longest a due
 * replacement waits for pending invitations (seconds; 0: the defaults). */
static gint64 world_key_package_lifetime;
/* The next world's accounts are fresh: no kind 10002 or 10050 seeded (they
 * set them up through GhInboxSetup, as onboarding does). */
static gboolean world_fresh_lists;
/* ...and set no discovery relay either: a real first run (PD-13). */
static gboolean world_no_discovery;
static gint64 world_key_package_max_hold;
/* The next world's accounts publish MDK 0.8 KeyPackages only (nostrc-lf62):
 * they make MDK 0.8-format groups with each other. */
static gboolean world_legacy_only;
/* The next world's admins commit the SelfRemove requirement on their own,
 * as the app does (nostrc-8ndz). Off by default: most tests count Commits
 * exactly or exercise the Remove-request path of groups without it. */
static gboolean world_self_remove_upgrade;

static G_GNUC_UNUSED void
spin_until_at(gboolean (*pred)(gpointer), gpointer data, const gchar *what, int line)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(WAIT_SECONDS, gh_test_deadline_hit, &expired);
  guint tick = g_timeout_add(10, gh_test_tick, NULL);
  while (!pred(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_source_remove(tick);
  if (expired)
    g_error("line %d: %s did not happen within %d s", line, what, WAIT_SECONDS);
  g_source_remove(timer);
}
#define spin_until(pred, data, what) spin_until_at((pred), (data), (what), __LINE__)

/* nostr_event_get_id() returns a malloc'd string: this is a GLib copy. */
static G_GNUC_UNUSED gchar *
event_id_dup(NostrEvent *event)
{
  char *id = nostr_event_get_id(event);
  gchar *copy = g_strdup(id);
  free(id);
  return copy;
}

static G_GNUC_UNUSED void
drain(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

static G_GNUC_UNUSED void
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

static G_GNUC_UNUSED gboolean
fake_monitor_initable_init(GInitable *initable, GCancellable *cancellable, GError **error)
{
  (void)initable; (void)cancellable; (void)error;
  return TRUE;
}

static G_GNUC_UNUSED void
fake_monitor_initable_iface_init(GInitableIface *iface)
{
  iface->init = fake_monitor_initable_init;
}

static G_GNUC_UNUSED gboolean
fake_monitor_can_reach(GNetworkMonitor *monitor, GSocketConnectable *connectable,
                       GCancellable *cancellable, GError **error)
{
  (void)monitor; (void)connectable; (void)cancellable; (void)error;
  return TRUE;
}

static G_GNUC_UNUSED void
fake_monitor_iface_init(GNetworkMonitorInterface *iface)
{
  iface->can_reach = fake_monitor_can_reach;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(FakeMonitor, fake_monitor, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(G_TYPE_INITABLE, fake_monitor_initable_iface_init)
  G_IMPLEMENT_INTERFACE(G_TYPE_NETWORK_MONITOR, fake_monitor_iface_init))

static G_GNUC_UNUSED void
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

static G_GNUC_UNUSED void
fake_monitor_class_init(FakeMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = fake_monitor_get_property;
  g_object_class_override_property(object_class, MONITOR_PROP_AVAILABLE, "network-available");
  g_object_class_override_property(object_class, MONITOR_PROP_METERED, "network-metered");
  g_object_class_override_property(object_class, MONITOR_PROP_CONNECTIVITY, "connectivity");
}

static G_GNUC_UNUSED void
fake_monitor_init(FakeMonitor *self)
{
  self->available = TRUE;
}

/* ---- signed fixtures ------------------------------------------------------------ */

static G_GNUC_UNUSED gchar *
sign_event(guint key, gint kind, gint64 created_at, const gchar *content, NostrTags *tags)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, content);
  nostr_event_set_tags(event, tags);
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[key]), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *out = g_strdup(json);
  free(json);
  return out;
}

/* The kind 10002 of world_split_lists: a write-only, an unmarked and a
 * read-only relay. */
static G_GNUC_UNUSED void
seed_split_list(WireRelay *discovery, guint key, const gchar *write_url, const gchar *both_url,
                const gchar *read_url)
{
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("r", write_url, "write", NULL));
  nostr_tags_append(tags, nostr_tag_new("r", both_url, NULL));
  nostr_tags_append(tags, nostr_tag_new("r", read_url, "read", NULL));
  g_autofree gchar *json = sign_event(key, 10002, g_get_real_time() / G_USEC_PER_SEC - 3600,
                                      "", tags);
  wire_relay_inject(discovery, json);
}

/* A signed kind 10050 (tag "relay") or 10002 (tag "r"), as the account's own
 * list publication would have put it on the discovery relay. */
static G_GNUC_UNUSED void
seed_list(WireRelay *discovery, guint key, gint kind, const gchar *url)
{
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new(kind == 10050 ? "relay" : "r", url, NULL));
  g_autofree gchar *json = sign_event(key, kind, g_get_real_time() / G_USEC_PER_SEC - 3600, "",
                                      tags);
  wire_relay_inject(discovery, json);
}

/* ---- the world ------------------------------------------------------------------ */

typedef struct _World World;

typedef struct {
  World *world;
  guint key;
  GSettings *settings;
  GhTestSigner *signer;
  GhAccountController *accounts;
  GhClock *clock;
  gchar *data_dir;
  GhStore *store;
  GhStoreConversations *rooms;
  GhConversationStore *model;
  GhAccountRelays *relays;
  GhInboxLookup *inboxes;
  GhDmInbox *inbox;
  FakeMonitor *network;
  GhMlsService *service;
  guint invites;      /* "invite-received" emissions */
} App;

struct _World {
  GhTestSigner signer;
  WireRelay e, w, x, g;
  WireRelay h;              /* a second group relay, for tests that name it; with
                             * world_split_lists also an unmarked 10002 relay */
  WireRelay r;              /* world_split_lists: every account's read-only relay */
  gchar *root;
  App apps[N_APPS];
};

static G_GNUC_UNUSED GPtrArray *
list_one(gpointer data, GError **error)
{
  (void)error;
  App *app = data;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
  info->npub = g_strdup(npub[app->key]);
  info->label = g_strdup_printf("Account %u", app->key);
  g_ptr_array_add(ids, info);
  return ids;
}

static G_GNUC_UNUSED gboolean
accounts_active(gpointer data)
{
  return gh_account_controller_get_state(data) == GH_ACCOUNT_STATE_ACTIVE;
}

static G_GNUC_UNUSED gboolean
relays_known(gpointer data)
{
  App *app = data;
  if (world_no_discovery)  /* nowhere to look yet */
    return gh_account_relays_get_state(app->relays) == GH_ACCOUNT_RELAYS_NO_SOURCES;
  if (world_fresh_lists)   /* nothing to find: the lookup has finished */
    return gh_account_relays_get_state(app->relays) == GH_ACCOUNT_RELAYS_COMPLETE;
  return gh_account_relays_get_inbox_relays(app->relays) &&
         gh_account_relays_get_write_relays(app->relays);
}

static gint64
inbox_load_checkpoint(gpointer data)
{
  gint64 since = 0;
  gh_store_get_cursor(data, "nip17/inbox", "", &since, NULL);
  return since;
}

static G_GNUC_UNUSED gboolean
inbox_save_checkpoint(gpointer data, gint64 checkpoint, GError **error)
{
  return gh_store_set_cursor(data, "nip17/inbox", "", checkpoint, error);
}

static const GhDmInboxStorage inbox_storage = {
  .load_checkpoint = inbox_load_checkpoint,
  .save_checkpoint = inbox_save_checkpoint,
};

static G_GNUC_UNUSED void
on_invite(GhMlsService *service, const gchar *wrapper_id, gpointer data)
{
  (void)service;
  (void)wrapper_id;
  ((App *)data)->invites++;
}

static G_GNUC_UNUSED void
store_open(App *app)
{
  guint8 key[GH_STORE_KEY_SIZE];
  for (guint i = 0; i < sizeof key; i++)
    key[i] = (guint8)(0x40 + app->key * 7 + i);
  g_autoptr(GBytes) bytes = g_bytes_new(key, sizeof key);
  GhStoreConfig config = { app->data_dir, hex[app->key], NULL, NULL, app->clock };
  g_autofree gchar *store_id = g_strdup_printf("3c0d2a51-3b8e-4f6a-9c1d-2e4f5a6b7c%02u", app->key);
  g_autoptr(GError) error = NULL;
  app->store = gh_store_open_with_key(&config, bytes, store_id, GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(app->store);
}

static G_GNUC_UNUSED void
service_up(App *app)
{
  GhMlsServiceConfig config = {
    .store = app->store,
    .accounts = app->accounts,
    .conversations = app->model,
    .account_relays = app->relays,
    .inboxes = GH_INBOX_RESOLVER(app->inboxes),
    .settings = app->settings,
    .inbox = app->inbox,
    .network = G_NETWORK_MONITOR(app->network),
    .publish_deadline = 20,
    .lookup_deadline = 20,
    .key_package_lifetime = world_key_package_lifetime,
    .key_package_max_hold = world_key_package_max_hold,
    .legacy_key_packages_only = world_legacy_only,
  };
  g_autoptr(GError) error = NULL;
  app->service = gh_mls_service_new(&config, &error);
  g_assert_no_error(error);
  g_assert_nonnull(app->service);
  g_signal_connect(app->service, "invite-received", G_CALLBACK(on_invite), app);
}

/* The store-bound half of an app: store, rooms, inbox and service. */
static G_GNUC_UNUSED void
app_store_up(App *app)
{
  store_open(app);
  app->model = gh_conversation_store_new();
  app->rooms = gh_store_conversations_new(app->store);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_attach(app->rooms, app->model, 0, &error));
  g_assert_no_error(error);
  app->inbox = gh_dm_inbox_new_with_storage(app->accounts, app->relays, app->model, NULL, NULL,
                                            NULL);
  g_assert_true(gh_dm_inbox_set_storage(app->inbox,
                                        gh_account_controller_get_generation(app->accounts),
                                        &inbox_storage, app->store));
  service_up(app);
}

static G_GNUC_UNUSED void
app_store_down(App *app)
{
  if (app->service)
    gh_test_release(g_steal_pointer(&app->service));
  if (app->inbox) {
    gh_dm_inbox_clear_storage(app->inbox);
    gh_test_release(g_steal_pointer(&app->inbox));
  }
  drain();
  if (app->rooms) {
    gh_store_conversations_close(app->rooms);
    g_clear_object(&app->rooms);
  }
  g_clear_object(&app->model);
  if (app->store)
    gh_store_close(g_steal_pointer(&app->store));
}

static G_GNUC_UNUSED void
app_up(World *w, guint key)
{
  App *app = &w->apps[key];
  app->world = w;
  app->key = key;
  /* g_settings_new_with_backend() takes its own reference. */
  g_autoptr(GSettingsBackend) backend = g_memory_settings_backend_new();
  app->settings = g_settings_new_with_backend("org.nostr.Groundhog", backend);
  g_settings_set_string(app->settings, "signer-method", "auto");
  g_settings_set_string(app->settings, "current-npub", npub[key]);
  const gchar *discovery[] = { w->e.url, NULL };
  if (!world_no_discovery)
    g_settings_set_strv(app->settings, "discovery-relays", discovery);
  app->accounts = gh_account_controller_new_full(app->settings, test_bus.client, list_one, app);
  spin_until(accounts_active, app->accounts, "the account becoming active");
  app->clock = world_fake_clock ? gh_clock_new_fake(g_get_real_time()) : gh_clock_new_system();
  g_autofree gchar *name = g_strdup_printf("app-%u", key);
  app->data_dir = g_build_filename(w->root, name, NULL);
  g_assert_cmpint(g_mkdir_with_parents(app->data_dir, 0700), ==, 0);
  app->network = g_object_new(FAKE_TYPE_MONITOR, NULL);
  app->relays = gh_account_relays_new(app->accounts, app->settings, NULL, NULL);
  spin_until(relays_known, app, "the account's own relay lists");
  app->inboxes = gh_inbox_lookup_new(app->accounts, app->settings, NULL, NULL);
  app_store_up(app);
}

/* A restart: the service, inbox and store go; all come back over the same
 * files (and a new, empty conversation model). */
static G_GNUC_UNUSED void
app_restart(App *app)
{
  app_store_down(app);
  app_store_up(app);
}

static G_GNUC_UNUSED void
app_down(App *app)
{
  if (!app->accounts)
    return;
  app_store_down(app);
  gh_test_release(g_steal_pointer(&app->inboxes));
  gh_test_release(g_steal_pointer(&app->relays));
  gh_test_release(g_steal_pointer(&app->accounts));
  drain();
  g_clear_object(&app->network);
  g_clear_object(&app->settings);
  gh_clock_unref(app->clock);
  g_free(app->data_dir);
  memset(app, 0, sizeof *app);
}

static G_GNUC_UNUSED void
world_up(World *w, const guint *keys, guint n_keys)
{
  memset(w, 0, sizeof *w);
  w->root = g_dir_make_tmp("groundhog-mls-XXXXXX", NULL);
  g_assert_nonnull(w->root);
  gh_test_signer_up(&test_bus, &w->signer);
  WireRelay *relays[] = { &w->e, &w->w, &w->x, &w->g, &w->h, &w->r };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++) {
    relays[i]->serve = TRUE;
    relays[i]->record = TRUE;
    relay_init(relays[i]);
  }
#ifdef GH_MLS_TEST_HOOKS
  gh_mls_service_test_set_self_remove_upgrade(world_self_remove_upgrade);
#endif
  w->x.auth_gate_dms = TRUE;   /* kind 1059 only to its signed-in recipient */
  w->g.require_auth = TRUE;    /* MLS routing: ephemeral AUTH, for real */
  for (guint key = 1; key < GH_TEST_KEYS && !world_fresh_lists; key++) {
    if (world_split_lists)
      seed_split_list(&w->e, key, w->w.url, w->h.url, w->r.url);
    else
      seed_list(&w->e, key, 10002, w->w.url);
    seed_list(&w->e, key, 10050, w->x.url);
  }
  for (guint i = 0; i < n_keys; i++)
    app_up(w, keys[i]);
}

static G_GNUC_UNUSED void
world_down(World *w)
{
  for (guint key = 1; key < N_APPS; key++)
    app_down(&w->apps[key]);
  GhTestSenders check = { &test_bus, &w->signer };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  drain();
  WireRelay *relays[] = { &w->e, &w->w, &w->x, &w->g, &w->h, &w->r };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++)
    relay_clear(relays[i]);
  drain();
  gh_test_signer_down(&test_bus, &w->signer);
  world_fake_clock = FALSE;
  world_self_remove_upgrade = FALSE;
  world_split_lists = FALSE;
  world_key_package_lifetime = 0;
  world_fresh_lists = FALSE;
  world_no_discovery = FALSE;
  world_key_package_max_hold = 0;
  world_legacy_only = FALSE;
  rm_rf(w->root);
  g_free(w->root);
}

/* ---- waits and lookups ------------------------------------------------------------ */

/* Whether W keeps the event id. */
static G_GNUC_UNUSED gboolean
stored_on_w(App *app, const gchar *id)
{
  GPtrArray *stored = app->world->w.stored;
  for (guint i = 0; i < stored->len; i++)
    if (g_strcmp0(((WireStored *)g_ptr_array_index(stored, i))->id, id) == 0)
      return TRUE;
  return FALSE;
}

/* The account's KeyPackages are published and on its write relay W (where
 * lookups find them; the state turns PUBLISHED on each format's first
 * relay OK, which may come from another write relay first): each format's
 * accepted event is kept on W (nostrc-lf62). After a restart that publishes
 * nothing, no id is known yet: then any of the account's kind 30443 on W. */
static G_GNUC_UNUSED gboolean
key_package_published(gpointer data)
{
  App *app = data;
  if (gh_mls_service_get_key_package_state(app->service) != GH_MLS_KEY_PACKAGE_PUBLISHED)
    return FALSE;
  gboolean known = FALSE;
  for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++) {
    const gchar *id = gh_mls_service_get_key_package_id_for_format(app->service, f);
    if (!id)
      continue;
    known = TRUE;
    if (!stored_on_w(app, id))
      return FALSE;
  }
  if (known)
    return TRUE;
  GPtrArray *stored = app->world->w.stored;
  for (guint i = 0; i < stored->len; i++) {
    WireStored *event = g_ptr_array_index(stored, i);
    if (nostr_event_get_kind(event->event) == 30443 &&
        g_strcmp0(nostr_event_get_pubkey(event->event), hex[app->key]) == 0)
      return TRUE;
  }
  return FALSE;
}

typedef struct {
  gboolean done;
  gpointer result;
  gboolean ok;
  GError *error;
} OpWait;

static G_GNUC_UNUSED gboolean
op_done(gpointer data)
{
  return ((OpWait *)data)->done;
}

static G_GNUC_UNUSED void
on_created(GObject *source, GAsyncResult *result, gpointer data)
{
  OpWait *wait = data;
  wait->result = gh_mls_service_create_group_finish(GH_MLS_SERVICE(source), result,
                                                    &wait->error);
  wait->done = TRUE;
}

static G_GNUC_UNUSED void
on_changed(GObject *source, GAsyncResult *result, gpointer data)
{
  OpWait *wait = data;
  wait->ok = gh_mls_service_change_finish(GH_MLS_SERVICE(source), result, &wait->error);
  wait->done = TRUE;
}

/* The account's listed message with this text in the room, or NULL. */
static G_GNUC_UNUSED GhMessage *
find_message(App *app, const gchar *room_id, const gchar *text)
{
  GhConversation *room = gh_conversation_store_lookup(app->model, room_id);
  guint n = room ? g_list_model_get_n_items(G_LIST_MODEL(room)) : 0;
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhMessage) message = g_list_model_get_item(G_LIST_MODEL(room), i);
    if (g_strcmp0(gh_message_get_content(message), text) == 0)
      return message;   /* the room keeps it */
  }
  return NULL;
}

typedef struct {
  App *app;
  const gchar *room_id;
  const gchar *text;
} MessageWait;

static G_GNUC_UNUSED gboolean
message_listed(gpointer data)
{
  MessageWait *wait = data;
  return find_message(wait->app, wait->room_id, wait->text) != NULL;
}

#define wait_message(app_, room_, text_) \
  G_STMT_START { MessageWait mw_ = { (app_), (room_), (text_) }; \
    spin_until(message_listed, &mw_, "message \"" text_ "\" listed"); } G_STMT_END

static G_GNUC_UNUSED gboolean
has_invite(gpointer data)
{
  return ((App *)data)->invites > 0;
}

/* The account's one pending invitation (asserted), its wrapper id. */
static G_GNUC_UNUSED gchar *
the_invite(App *app, guint inviter)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) invites = gh_mls_service_list_invites(app->service, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(invites->len, ==, 1);
  GhMlsInvite *invite = g_ptr_array_index(invites, 0);
  g_assert_cmpstr(invite->inviter, ==, hex[inviter]);
  return g_strdup(invite->wrapper_id);
}

typedef struct {
  GhMlsGroup *group;
  gint value;
} GroupWait;

static G_GNUC_UNUSED gboolean
read_is(gpointer data)
{
  GroupWait *wait = data;
  return (gint)gh_mls_group_get_read_state(wait->group) == wait->value;
}

static G_GNUC_UNUSED gboolean
epoch_at_least(gpointer data)
{
  GroupWait *wait = data;
  return gh_mls_group_get_epoch(wait->group) >= (guint64)wait->value;
}

static G_GNUC_UNUSED gboolean
members_are(gpointer data)
{
  GroupWait *wait = data;
  g_auto(GStrv) members = gh_mls_group_dup_members(wait->group);
  return g_strv_length(members) == (guint)wait->value;
}

#define wait_live(group_) \
  G_STMT_START { GroupWait gw_ = { (group_), GH_MLS_READ_LIVE }; \
    spin_until(read_is, &gw_, "the group read live"); } G_STMT_END
#define wait_epoch(group_, epoch_) \
  G_STMT_START { GroupWait gw_ = { (group_), (epoch_) }; \
    spin_until(epoch_at_least, &gw_, "the group's epoch"); } G_STMT_END
#define wait_members(group_, n_) \
  G_STMT_START { GroupWait gw_ = { (group_), (n_) }; \
    spin_until(members_are, &gw_, "the member count"); } G_STMT_END

/* Makes account `by` an accepted contact of `app` (an accepted, empty
 * NIP-17 room: what writing to them does). */
static G_GNUC_UNUSED void
accept_contact(App *app, guint other)
{
  const gchar *peers[] = { hex[other], NULL };
  g_autoptr(GError) error = NULL;
  g_assert_nonnull(gh_conversation_store_open_room(app->model, peers, &error));
  g_assert_no_error(error);
}

/* Creates a group of `app` on `relays` with `invitees` and waits for its Add. */
static G_GNUC_UNUSED GhMlsGroup *
create_group_on(App *app, const gchar *name, const gchar *const *relays, const guint *invitees,
                guint n)
{
  g_autoptr(GPtrArray) people = g_ptr_array_new();
  for (guint i = 0; i < n; i++)
    g_ptr_array_add(people, hex[invitees[i]]);
  g_ptr_array_add(people, NULL);
  OpWait wait = { 0 };
  gh_mls_service_create_group_async(app->service, name, "a test group", relays,
                                    (const gchar *const *)people->pdata, NULL, on_created,
                                    &wait);
  spin_until(op_done, &wait, "the group creation");
  g_assert_no_error(wait.error);
  g_assert_nonnull(wait.result);
  GhMlsGroup *group = wait.result;
  g_object_unref(group);   /* the service keeps it */
  return group;
}

/* Creates a group of `app` with `invitees` on G and waits for its Add. */
static G_GNUC_UNUSED GhMlsGroup *
create_group(App *app, const gchar *name, const guint *invitees, guint n)
{
  g_autoptr(GPtrArray) people = g_ptr_array_new();
  for (guint i = 0; i < n; i++)
    g_ptr_array_add(people, hex[invitees[i]]);
  g_ptr_array_add(people, NULL);
  const gchar *relays[] = { app->world->g.url, NULL };
  OpWait wait = { 0 };
  gh_mls_service_create_group_async(app->service, name, "a test group", relays,
                                    (const gchar *const *)people->pdata, NULL, on_created,
                                    &wait);
  spin_until(op_done, &wait, "the group creation");
  g_assert_no_error(wait.error);
  g_assert_nonnull(wait.result);
  GhMlsGroup *group = wait.result;
  g_object_unref(group);   /* the service keeps it */
  return group;
}

typedef struct {
  App *app;
  GhMlsKeyPackageFormat format;
  const gchar *old_id;
} ReplacedWait;

static G_GNUC_UNUSED gboolean
key_package_replaced(gpointer data)
{
  ReplacedWait *wait = data;
  const gchar *id = gh_mls_service_get_key_package_id_for_format(wait->app->service,
                                                                 wait->format);
  return id && g_strcmp0(id, wait->old_id) != 0 &&
         gh_mls_service_get_key_package_state(wait->app->service) ==
           GH_MLS_KEY_PACKAGE_PUBLISHED;
}

/* `app` accepts its one invitation from `inviter` and reads the group. The
 * join's KeyPackage replacement (nostrc-0bdg) -- of the format the group
 * used (nostrc-lf62) -- is awaited too, so a later invitation is made with
 * the new KeyPackage: its confirmation retires the old one, and a Welcome
 * to that would fail (the spec's trade-off). */
static G_GNUC_UNUSED GhMlsGroup *
join(App *app, guint inviter)
{
  spin_until(has_invite, app, "an invitation");
  g_autofree gchar *wrapper = the_invite(app, inviter);
  gchar *kp_before[GH_MLS_KEY_PACKAGE_N_FORMATS];
  for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++)
    kp_before[f] = g_strdup(gh_mls_service_get_key_package_id_for_format(app->service, f));
  g_autoptr(GError) error = NULL;
  GhMlsGroup *group = gh_mls_service_accept_invite(app->service, wrapper, &error);
  g_assert_no_error(error);
  g_assert_nonnull(group);
  app->invites = 0;
  wait_live(group);
  GhMlsKeyPackageFormat format = gh_mls_group_get_adopted(group)
                                   ? GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED
                                   : GH_MLS_KEY_PACKAGE_FORMAT_LEGACY;
  ReplacedWait replaced = { app, format, kp_before[format] };
  spin_until(key_package_replaced, &replaced, "the joiner's KeyPackage replacement");
  for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++)
    g_free(kp_before[f]);
  return group;
}

static G_GNUC_UNUSED void
send_text(App *app, GhMlsGroup *group, const gchar *text)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) message = gh_mls_service_send(app->service, group, text, &error);
  g_assert_no_error(error);
  g_assert_nonnull(message);
}

/* The account's network goes away or comes back (the fake monitor). */
static G_GNUC_UNUSED void
set_online(App *app, gboolean online)
{
  app->network->available = online;
  g_signal_emit_by_name(app->network, "network-changed", online);
}

static G_GNUC_UNUSED gboolean
unreadable_is(gpointer data)
{
  GroupWait *wait = data;
  return gh_mls_group_get_unreadable(wait->group) == (guint)wait->value;
}

#define wait_unreadable(group_, n_) \
  G_STMT_START { GroupWait gw_ = { (group_), (n_) }; \
    spin_until(unreadable_is, &gw_, "the unreadable count"); } G_STMT_END

static G_GNUC_UNUSED gint64
real_now(void)
{
  return g_get_real_time() / G_USEC_PER_SEC;
}


/* The envelope json re-signed under a fresh key with another created_at:
 * what a relay replay, or anyone copying a group event, can publish. */
static G_GNUC_UNUSED gchar *
resigned(const gchar *json, gint64 created_at)
{
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, json, NULL), ==, 1);
  nostr_event_set_created_at(event, created_at);
  char *key = nostr_key_generate_private();
  g_assert_cmpint(nostr_event_sign(event, key), ==, 0);
  free(key);
  char *out = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *copy = g_strdup(out);
  free(out);
  return copy;
}

/* A validly signed kind 445 with the group's routing h that is nothing:
 * anyone can publish one. */
static G_GNUC_UNUSED gchar *
junk_445(const gchar *h, guint n, gint64 created_at)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 445);
  nostr_event_set_created_at(event, created_at);
  g_autofree gchar *content = g_strdup_printf("AjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkA%u", n);
  nostr_event_set_content(event, content);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("h", h, NULL));
  nostr_event_set_tags(event, tags);
  char *key = nostr_key_generate_private();
  g_assert_cmpint(nostr_event_sign(event, key), ==, 0);
  free(key);
  char *out = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *copy = g_strdup(out);
  free(out);
  return copy;
}

/* The newest kind 445 the relay stored (arrival order), or NULL. */
static G_GNUC_UNUSED WireStored *
last_stored_445(WireRelay *relay)
{
  for (guint i = relay->stored->len; i > 0; i--) {
    WireStored *stored = g_ptr_array_index(relay->stored, i - 1);
    if (nostr_event_get_kind(stored->event) == 445)
      return stored;
  }
  return NULL;
}

/* ---- relay frames --------------------------------------------------------------------- */

/* The event of an ["EVENT", {...}] (client) or ["EVENT", sub, {...}] (relay)
 * frame, or NULL. */
static G_GNUC_UNUSED NostrEvent *
frame_event(const gchar *text, gboolean inbound)
{
  if (!g_str_has_prefix(text, "[\"EVENT\","))
    return NULL;
  const gchar *start = text + strlen("[\"EVENT\",");
  if (!inbound) {
    start = strchr(start, ',');
    if (!start)
      return NULL;
    start++;
  }
  gsize length = strlen(start);
  if (length < 2 || start[length - 1] != ']')
    return NULL;
  g_autofree gchar *json = g_strndup(start, length - 1);
  NostrEvent *event = nostr_event_new();
  if (nostr_event_deserialize_compact(event, json, NULL) != 1) {
    nostr_event_free(event);
    return NULL;
  }
  return event;
}

static G_GNUC_UNUSED gboolean
is_account(const gchar *pubkey)
{
  for (guint key = 1; key < GH_TEST_KEYS; key++)
    if (g_strcmp0(pubkey, hex[key]) == 0)
      return TRUE;
  return FALSE;
}

/* Every event a client published to relay, of kind (-1: any). */
static G_GNUC_UNUSED GPtrArray *
published(WireRelay *relay, gint kind)
{
  GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)nostr_event_free);
  for (guint i = 0; i < relay->frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay->frames, i);
    if (!frame->inbound)
      continue;
    NostrEvent *event = frame_event(frame->text, TRUE);
    if (event && (kind < 0 || nostr_event_get_kind(event) == kind))
      g_ptr_array_add(out, event);
    else if (event)
      nostr_event_free(event);
  }
  return out;
}

/* libmarmot's record of a processed Welcome (its gift wrap's id), if any:
 * its state (MarmotWelcomeState) and reason. */
static G_GNUC_UNUSED gboolean
processed_welcome(App *app, const gchar *wrap_id, gint *state, gchar **reason)
{
  g_autoptr(GError) error = NULL;
  MarmotStorage *storage = gh_store_marmot_new(app->store, &error);
  g_assert_no_error(error);
  guint8 wrapper[32];
  g_assert_true(nostr_hex2bin(wrapper, wrap_id, sizeof wrapper));
  bool found = false;
  int s = -1;
  char *why = NULL;
  g_assert_cmpint(storage->find_processed_welcome(storage->ctx, wrapper, &found, &s, &why),
                  ==, MARMOT_OK);
  marmot_storage_free(storage);
  if (found) {
    if (state)
      *state = s;
    if (reason)
      *reason = g_strdup(why);
  }
  free(why);
  return found;
}

/* Forward: defined below. */
static G_GNUC_UNUSED GhMlsKeyPackageFormat key_package_format(NostrEvent *event);

/* Withholds every kind 30443 of `key` in `format` the relay keeps (a relay
 * that keeps one back from REQs: review M2). How many. */
static G_GNUC_UNUSED guint
withhold_key_packages(WireRelay *relay, guint key, GhMlsKeyPackageFormat format)
{
  guint n = 0;
  for (guint i = 0; i < relay->stored->len; i++) {
    WireStored *stored = g_ptr_array_index(relay->stored, i);
    if (nostr_event_get_kind(stored->event) == 30443 &&
        g_strcmp0(nostr_event_get_pubkey(stored->event), hex[key]) == 0 &&
        key_package_format(stored->event) == format) {
      wire_relay_withhold(relay, stored->id);
      n++;
    }
  }
  return n;
}

/* Serves every event the relay withholds again. */
static G_GNUC_UNUSED void
release_all_withheld(WireRelay *relay)
{
  if (!relay->withheld)
    return;
  g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
  GHashTableIter iter;
  gpointer id;
  g_hash_table_iter_init(&iter, relay->withheld);
  while (g_hash_table_iter_next(&iter, &id, NULL))
    g_ptr_array_add(ids, g_strdup(id));
  for (guint i = 0; i < ids->len; i++)
    wire_relay_release(relay, g_ptr_array_index(ids, i));
}

/* A kind-30443 event's KeyPackage format (nostrc-lf62): the adopted profile
 * lists its components in an `app_components` tag, which the MDK 0.8
 * profile never has. */
static G_GNUC_UNUSED GhMlsKeyPackageFormat
key_package_format(NostrEvent *event)
{
  NostrTags *tags = nostr_event_get_tags(event);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++)
    if (g_strcmp0(nostr_tag_get(nostr_tags_get(tags, i), 0), "app_components") == 0)
      return GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED;
  return GH_MLS_KEY_PACKAGE_FORMAT_LEGACY;
}

/* The format of `key`'s newest kind 30443 the relay keeps, whatever its slot
 * (by created_at, then the lower id: what a slot-blind reader takes). */
static G_GNUC_UNUSED GhMlsKeyPackageFormat
newest_key_package_format(WireRelay *relay, guint key)
{
  WireStored *newest = NULL;
  for (guint i = 0; i < relay->stored->len; i++) {
    WireStored *stored = g_ptr_array_index(relay->stored, i);
    if (nostr_event_get_kind(stored->event) != 30443 ||
        g_strcmp0(nostr_event_get_pubkey(stored->event), hex[key]) != 0)
      continue;
    gint64 at = nostr_event_get_created_at(stored->event);
    gint64 best = newest ? nostr_event_get_created_at(newest->event) : 0;
    if (!newest || at > best || (at == best && g_strcmp0(stored->id, newest->id) < 0))
      newest = stored;
  }
  g_assert_nonnull(newest);
  return key_package_format(newest->event);
}

/* Whether any client frame to relay mentions text (e.g. a pubkey in a REQ). */
static G_GNUC_UNUSED gboolean
client_frames_mention(WireRelay *relay, const gchar *text)
{
  for (guint i = 0; i < relay->frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay->frames, i);
    if (frame->inbound && g_str_has_prefix(frame->text, "[\"REQ\"") &&
        strstr(frame->text, text))
      return TRUE;
  }
  return FALSE;
}

static G_GNUC_UNUSED void
mls_world_init(void)
{
  /* The local relays' libsoup server warns (structured logging, which no
   * test fatal handler sees) when a client closed its socket before the
   * server read the peer address, e.g. a one-shot publish that got its OK
   * and left; on Linux this is the test relay's race, not Groundhog's. So
   * warnings of other domains are not fatal, as in the UI tests; Groundhog's
   * own (the default domain) and every critical still are. */
  g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
  g_log_set_fatal_mask(NULL, G_LOG_FATAL_MASK | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL);
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    hex[key] = gh_test_pub(key);
    npub[key] = gh_test_npub(key);
  }
  gh_test_bus_up(&test_bus);
}

static G_GNUC_UNUSED void
mls_world_finish(void)
{
  gh_test_bus_down(&test_bus);
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    g_free(hex[key]);
    g_free(npub[key]);
  }
}

#endif
