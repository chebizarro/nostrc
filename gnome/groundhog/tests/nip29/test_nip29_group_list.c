/* The account's NIP-51 kind-10009 group list (nostrc-90jof): published,
 * encrypted to the account, when a group is joined or left here; read on a
 * new store and offered for restoring (nothing joined until asked); newest
 * list wins, a group this device has is never dropped, public items another
 * client wrote stay public; and fail closed when the list is unreadable or a
 * source has not answered.
 *
 * Harnesses: local relays (nip29-relay.h): one NIP-29 group relay, and two
 * plain list relays (accept_lists) acting as the discovery relay (holding
 * the account's 10002) and the 10002 write relay; the real gnostr relay
 * transports; GhAccountRelays, GhNip29Service and GhNip29GroupList as the
 * application makes them; real SQLCipher stores (test hooks) in private
 * directories; a real account controller with the mock org.nostr.Signer on
 * the private test bus. "Another device" is the test signing a list with the
 * account's key. Waits iterate the main context; their deadlines are failure
 * bounds only. */
#include "gh-account-relays.h"
#include "gh-nip29-group-list.h"
#include "gh-nip29-service.h"
#include "gh-test-signer.h"
#include "nip29-relay.h"

#include <glib/gstdio.h>

#define STORE_ID "0b7e5d1a-9c2f-4a10-8e3b-1d2c3b4a5f60"
#define KEY_ALICE 1
#define KEY_BOB   2

static gchar *hex_alice, *npub_alice, *npub_bob;
static GhTestBus shared_bus;

/* ---- helpers --------------------------------------------------------------- */

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
  switch (prop_id) {
  case MONITOR_PROP_AVAILABLE: g_value_set_boolean(value, TRUE); break;
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
  (void)self;
}

/* ---- the account's own events (another device) ------------------------------------ */

static gchar *
self_encrypt(const gchar *plaintext)
{
  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, gh_test_secret[KEY_ALICE], sizeof sk));
  g_assert_true(nostr_hex2bin(pk, hex_alice, sizeof pk));
  char *ciphertext = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)plaintext, strlen(plaintext),
                                         &ciphertext), ==, 0);
  gchar *out = g_strdup(ciphertext);
  free(ciphertext);
  return out;
}

static gchar *
self_decrypt(const gchar *ciphertext)
{
  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, gh_test_secret[KEY_ALICE], sizeof sk));
  g_assert_true(nostr_hex2bin(pk, hex_alice, sizeof pk));
  guint8 *plaintext = NULL;
  size_t length = 0;
  g_assert_cmpint(nostr_nip44_decrypt_v2(sk, pk, ciphertext, &plaintext, &length), ==, 0);
  gchar *out = g_strndup((const gchar *)plaintext, length);
  free(plaintext);
  return out;
}

/* A signed event by the account, as JSON. tags: NULL-separated groups of
 * strings, ending with two NULLs; content as given. */
static gchar *
account_event(gint kind, gint64 created_at, NostrTags *tags, const gchar *content)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, content ? content : "");
  nostr_event_set_tags(event, tags ? tags : nostr_tags_new(0));
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[KEY_ALICE]), ==, 0);
  gchar *json = nip29_event_json(event);
  nostr_event_free(event);
  return json;
}

static void
relay_put(Nip29Relay *relay, const gchar *json)
{
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(event, json, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);
  nip29_store(relay, event);
}

static GPtrArray *
tags_of(const gchar *const *const *spec)
{
  GPtrArray *tags = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
  for (guint i = 0; spec[i]; i++)
    g_ptr_array_add(tags, g_strdupv((gchar **)spec[i]));
  return tags;
}

/* Another device's group list: public and private items (GStrv each). */
static gchar *
other_device_list(gint64 created_at, GPtrArray *public_tags, GPtrArray *private_tags)
{
  NostrTags *tags = nostr_tags_new(0);
  for (guint i = 0; public_tags && i < public_tags->len; i++) {
    const gchar *const *strv = g_ptr_array_index(public_tags, i);
    NostrTag *tag = nostr_tag_new(strv[0], NULL);
    for (guint j = 1; strv[j]; j++)
      nostr_tag_append(tag, strv[j]);
    nostr_tags_append(tags, tag);
  }
  g_autofree gchar *content = NULL;
  if (private_tags && private_tags->len) {
    g_autofree gchar *plaintext = gh_nip29_group_list_format_private(private_tags);
    content = self_encrypt(plaintext);
  }
  return account_event(GH_NIP29_GROUP_LIST_KIND, created_at, tags, content);
}

static NostrEvent *
parse(const gchar *json)
{
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(event, json, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);
  return event;
}

/* The account's stored group list on relay, as JSON, or NULL. */
static gchar *
stored_list(Nip29Relay *relay)
{
  NostrEvent *event = nip29_stored_list(relay, GH_NIP29_GROUP_LIST_KIND, hex_alice);
  return event ? nip29_event_json(event) : NULL;
}

static GPtrArray *
private_items(NostrEvent *event)
{
  const gchar *content = nostr_event_get_content(event);
  if (!content || !*content)
    return g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
  g_autofree gchar *plaintext = self_decrypt(content);
  g_autoptr(GError) error = NULL;
  GPtrArray *tags = gh_nip29_group_list_parse_private(plaintext, &error);
  g_assert_no_error(error);
  return tags;
}

static gboolean
has_tag(GPtrArray *tags, const gchar *key, const gchar *value, const gchar *third)
{
  for (guint i = 0; i < tags->len; i++) {
    const gchar *const *tag = g_ptr_array_index(tags, i);
    if (g_strcmp0(tag[0], key) == 0 && tag[1] && g_strcmp0(tag[1], value) == 0 &&
        (!third || g_strcmp0(tag[2], third) == 0))
      return TRUE;
  }
  return FALSE;
}

static gboolean
public_has_tag(NostrEvent *event, const gchar *key, const gchar *value)
{
  g_autoptr(GPtrArray) tags = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
  NostrTags *raw = nostr_event_get_tags(event);
  for (size_t i = 0; raw && i < nostr_tags_size(raw); i++) {
    NostrTag *tag = nostr_tags_get(raw, i);
    GStrv strv = g_new0(gchar *, nostr_tag_size(tag) + 1);
    for (size_t j = 0; j < nostr_tag_size(tag); j++)
      strv[j] = g_strdup(nostr_tag_get(tag, j));
    g_ptr_array_add(tags, strv);
  }
  return has_tag(tags, key, value, NULL);
}

/* ---- fixture ------------------------------------------------------------------ */

typedef struct {
  GhTestSigner mock;
  GSettings *settings;
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GhClock *clock;
  gchar *data_dir;
  GhStore *store;
  GhConversationStore *model;
  FakeMonitor *network;
  GhNip29Service *service;
  GhNip29GroupList *list;
  Nip29Relay group;   /* the NIP-29 group relay */
  Nip29Relay disc;    /* discovery relay: holds the account's 10002 */
  Nip29Relay write;   /* the 10002's write relay */
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

static gboolean
relays_complete(gpointer data)
{
  return gh_account_relays_get_state(data) == GH_ACCOUNT_RELAYS_COMPLETE &&
         gh_account_relays_get_write_relays(data) != NULL;
}

static void
relays_up(Fixture *f)
{
  nip29_relay_init(&f->group);
  nip29_relay_init(&f->disc);
  nip29_relay_init(&f->write);
  f->disc.accept_lists = TRUE;
  f->write.accept_lists = TRUE;
  /* The account's relay list: write to the write relay. */
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("r", f->write.url, "write", NULL));
  g_autofree gchar *relay_list = account_event(10002, g_get_real_time() / G_USEC_PER_SEC - 60,
                                               tags, NULL);
  relay_put(&f->disc, relay_list);
}

/* One device: a store in data_dir, its model, its group service and list. */
static void
device_up(Fixture *f)
{
  guint8 key[GH_STORE_KEY_SIZE];
  for (guint i = 0; i < sizeof key; i++)
    key[i] = (guint8)(0x10 ^ i);
  g_autoptr(GBytes) bytes = g_bytes_new(key, sizeof key);
  GhStoreConfig store_config = { f->data_dir, hex_alice, NULL, NULL, f->clock };
  g_autoptr(GError) error = NULL;
  f->store = gh_store_open_with_key(&store_config, bytes, STORE_ID, GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  f->model = gh_conversation_store_new();
  gh_conversation_store_set_account(f->model, hex_alice, NULL, NULL, NULL);
  GhNip29ServiceConfig config = {
    .store = f->store,
    .accounts = f->accounts,
    .conversations = f->model,
    .network = G_NETWORK_MONITOR(f->network),
    .publish_deadline = 10,
    .settings = f->settings,
  };
  f->service = gh_nip29_service_new(&config, &error);
  g_assert_no_error(error);
  /* As gh-app-outbox.c makes it beside the service. */
  GhNip29GroupListConfig list = {
    .accounts = f->accounts,
    .account_relays = f->relays,
    .settings = f->settings,
    .service = f->service,
    .publish_deadline = 10,
  };
  f->list = gh_nip29_group_list_new(&list);
  g_assert_nonnull(f->list);
}

static void
device_down(Fixture *f)
{
  gh_test_release(g_steal_pointer(&f->list));
  gh_test_release(g_steal_pointer(&f->service));
  g_clear_object(&f->model);
  gh_store_close(g_steal_pointer(&f->store));
}

static void
fixture_up(Fixture *f)
{
  memset(f, 0, sizeof *f);
  relays_up(f);
  gh_test_signer_up(&shared_bus, &f->mock);
  f->settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(f->settings, "signer-method", "auto");
  g_settings_set_string(f->settings, "current-npub", npub_alice);
  const gchar *discovery[] = { f->disc.url, NULL };
  g_settings_set_strv(f->settings, "discovery-relays", discovery);
  f->accounts = gh_account_controller_new_full(f->settings, shared_bus.client, fake_list, NULL);
  gh_test_spin_until(listed, f->accounts);
  g_assert_cmpint(gh_account_controller_get_state(f->accounts), ==, GH_ACCOUNT_STATE_ACTIVE);
  f->relays = gh_account_relays_new(f->accounts, f->settings, NULL, NULL);
  gh_test_spin_until(relays_complete, f->relays);
  f->clock = gh_clock_new_fake(g_get_real_time());
  f->data_dir = g_dir_make_tmp("groundhog-nip29-list-XXXXXX", NULL);
  f->network = g_object_new(FAKE_TYPE_MONITOR, NULL);
}

/* A new device of the same account: a fresh, empty store. */
static void
new_device(Fixture *f)
{
  device_down(f);
  rm_rf(f->data_dir);
  g_free(f->data_dir);
  f->data_dir = g_dir_make_tmp("groundhog-nip29-list-XXXXXX", NULL);
  device_up(f);
}

static void
fixture_down(Fixture *f)
{
  if (f->list)
    device_down(f);
  gh_test_release(g_steal_pointer(&f->relays));
  gh_test_release(f->accounts);
  GhTestSenders check = { &shared_bus, &f->mock };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_object_unref(f->network);
  g_object_unref(f->settings);
  gh_clock_unref(f->clock);
  gh_test_signer_down(&shared_bus, &f->mock);
  rm_rf(f->data_dir);
  g_free(f->data_dir);
  nip29_relay_clear(&f->group);
  nip29_relay_clear(&f->disc);
  nip29_relay_clear(&f->write);
}

/* ---- waits ---------------------------------------------------------------------- */

typedef struct {
  GhNip29GroupList *list;
  GhNip29GroupListState state;
} StateWait;

static gboolean
state_is(gpointer data)
{
  StateWait *wait = data;
  return gh_nip29_group_list_get_state(wait->list) == wait->state;
}

#define wait_state(l, s) \
  G_STMT_START { StateWait w_ = { (l), (s) }; gh_test_spin_until(state_is, &w_); } G_STMT_END

typedef struct {
  GhNip29Room *room;
  GhNip29JoinState state;
} JoinWait;

static gboolean
join_is(gpointer data)
{
  JoinWait *wait = data;
  return gh_nip29_room_get_join_state(wait->room) == wait->state;
}

#define wait_join(r, s) \
  G_STMT_START { JoinWait w_ = { (r), (s) }; gh_test_spin_until(join_is, &w_); } G_STMT_END

typedef struct {
  GhNip29GroupList *list;
  guint n;
} RestorableWait;

static gboolean
restorable_is(gpointer data)
{
  RestorableWait *wait = data;
  g_autoptr(GPtrArray) groups = gh_nip29_group_list_dup_restorable(wait->list);
  return groups->len == wait->n;
}

#define wait_restorable(l, count) \
  G_STMT_START { RestorableWait w_ = { (l), (count) }; gh_test_spin_until(restorable_is, &w_); } \
  G_STMT_END

/* The list has read json as the newest, and settled. */
typedef struct {
  GhNip29GroupList *list;
  const gchar *json;
} NewestWait;

static gboolean
newest_synced(gpointer data)
{
  NewestWait *wait = data;
  return g_strcmp0(gh_nip29_group_list_get_event_json(wait->list), wait->json) == 0 &&
         gh_nip29_group_list_get_state(wait->list) == GH_NIP29_GROUP_LIST_SYNCED;
}

typedef struct {
  GhAccountRelays *relays;
  const gchar *url;
} WriteWait;

static gboolean
write_relay_is(gpointer data)
{
  WriteWait *wait = data;
  const gchar *const *write = gh_account_relays_get_write_relays(wait->relays);
  return write && write[0] && g_strcmp0(write[0], wait->url) == 0 && !write[1];
}

/* The relay holds a group list other than the one with id old_id (NULL:
 * any). */
typedef struct {
  Nip29Relay *relay;
  const gchar *old_id;
} ListWait;

static gboolean
list_replaced(gpointer data)
{
  ListWait *wait = data;
  NostrEvent *event = nip29_stored_list(wait->relay, GH_NIP29_GROUP_LIST_KIND, hex_alice);
  return event && g_strcmp0(nip29_event_id(event), wait->old_id) != 0;
}

#define wait_list(relay, old) \
  G_STMT_START { ListWait w_ = { (relay), (old) }; gh_test_spin_until(list_replaced, &w_); } \
  G_STMT_END

static const gchar *
id_of(const gchar *json)
{
  NostrEvent *event = parse(json);
  const gchar *id = nip29_event_id(event);
  nostr_event_free(event);
  return id;
}

static GhNip29Room *
join(Fixture *f, const gchar *group_id)
{
  g_autoptr(GError) error = NULL;
  GhNip29Room *room = gh_nip29_service_join(f->service, f->group.url, group_id, NULL, NULL,
                                            &error);
  g_assert_no_error(error);
  g_assert_nonnull(room);
  return room;
}

/* ---- tests ---------------------------------------------------------------------- */

static void
test_private_items_roundtrip(void)
{
  const gchar *const group[] = { "group", "pizza", "wss://groups.example", "Pizza \"Lovers\"", NULL };
  const gchar *const r[] = { "r", "wss://groups.example", NULL };
  const gchar *const *const spec[] = { group, r, NULL };
  g_autoptr(GPtrArray) tags = tags_of(spec);
  g_autofree gchar *plaintext = gh_nip29_group_list_format_private(tags);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) back = gh_nip29_group_list_parse_private(plaintext, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(back->len, ==, 2);
  g_assert_true(g_strv_equal((const gchar *const *)g_ptr_array_index(back, 0), group));
  g_assert_true(g_strv_equal((const gchar *const *)g_ptr_array_index(back, 1), r));
  const gchar *bad[] = { "", "{}", "[[]]", "[[1]]", "[\"group\"]", "[[\"a\"],{}]", NULL };
  for (guint i = 0; bad[i]; i++) {
    g_autoptr(GError) parse_error = NULL;
    g_assert_null(gh_nip29_group_list_parse_private(bad[i], &parse_error));
    g_assert_error(parse_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  }
}

/* Joining publishes the list, encrypted, to the own relays only; leaving
 * removes the group; a newer list from another device wins over an older
 * local change. */
static void
test_publish_join_and_leave(void)
{
  Fixture f;
  fixture_up(&f);
  nip29_add_group(&f.group, "pizza", "Pizza Lovers");
  device_up(&f);
  /* No list anywhere and no group here: nothing to publish. */
  wait_state(f.list, GH_NIP29_GROUP_LIST_SYNCED);
  g_assert_null(stored_list(&f.write));
  g_assert_null(stored_list(&f.disc));

  g_autoptr(GhNip29Room) room = join(&f, "pizza");
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  wait_list(&f.write, NULL);
  wait_list(&f.disc, NULL);
  wait_state(f.list, GH_NIP29_GROUP_LIST_SYNCED);
  g_autofree gchar *first = stored_list(&f.write);
  g_autofree gchar *on_disc = stored_list(&f.disc);
  g_assert_cmpstr(id_of(first), ==, id_of(on_disc));
  /* Membership is private: nothing about the group in the clear. */
  g_assert_null(strstr(first, "pizza"));
  g_assert_null(strstr(first, f.group.url));
  NostrEvent *event = parse(first);
  g_assert_cmpuint(nostr_tags_size(nostr_event_get_tags(event)), ==, 0);
  g_autoptr(GPtrArray) items = private_items(event);
  g_assert_true(has_tag(items, "group", "pizza", f.group.url));
  g_assert_true(has_tag(items, "r", f.group.url, NULL));
  gint64 first_at = nostr_event_get_created_at(event);
  nostr_event_free(event);
  /* The group relay never sees the list. */
  NostrEvent *leaked = nip29_last_received(&f.group, GH_NIP29_GROUP_LIST_KIND);
  g_assert_null(leaked);

  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip29Op) op = gh_nip29_service_leave(f.service, room, NULL, &error);
  g_assert_no_error(error);
  wait_join(room, GH_NIP29_JOIN_LEFT);
  wait_list(&f.write, id_of(first));
  wait_state(f.list, GH_NIP29_GROUP_LIST_SYNCED);
  g_autofree gchar *second = stored_list(&f.write);
  event = parse(second);
  g_assert_cmpint(nostr_event_get_created_at(event), >, first_at - 1);
  g_assert_cmpstr(nostr_event_get_content(event), ==, "");
  g_assert_cmpuint(nostr_tags_size(nostr_event_get_tags(event)), ==, 0);
  gint64 second_at = nostr_event_get_created_at(event);
  nostr_event_free(event);

  /* Another device joined pizza again later: its newer list wins over this
   * device's older leave, and nothing is published over it. */
  const gchar *const group[] = { "group", "pizza", f.group.url, NULL };
  const gchar *const r[] = { "r", f.group.url, NULL };
  const gchar *const *const spec[] = { group, r, NULL };
  g_autoptr(GPtrArray) priv = tags_of(spec);
  g_autofree gchar *other = other_device_list(second_at + 30, NULL, priv);
  relay_put(&f.write, other);
  relay_put(&f.disc, other);
  NewestWait newest = { f.list, other };
  gh_test_spin_until(newest_synced, &newest);
  g_autofree gchar *kept = stored_list(&f.write);
  g_assert_cmpstr(id_of(kept), ==, id_of(other));
  /* This device has a record of pizza (left): it is not offered. */
  g_autoptr(GPtrArray) offer = gh_nip29_group_list_dup_restorable(f.list);
  g_assert_cmpuint(offer->len, ==, 0);
  fixture_down(&f);
}

/* A new store reads the list and offers its groups; nothing is joined until
 * asked; restoring joins through the service; public items stay public. */
static void
test_restore_on_new_store(void)
{
  Fixture f;
  fixture_up(&f);
  nip29_add_group(&f.group, "pizza", "Pizza Lovers");
  nip29_add_group(&f.group, "tacos", "Taco Tuesday");
  /* Another client listed tacos in the clear, Groundhog pizza privately. */
  const gchar *const tacos[] = { "group", "tacos", f.group.url, NULL };
  const gchar *const pizza[] = { "group", "pizza", f.group.url, "Pizza Lovers", NULL };
  const gchar *const r[] = { "r", f.group.url, NULL };
  const gchar *const *const pub_spec[] = { tacos, r, NULL };
  const gchar *const *const priv_spec[] = { pizza, NULL };
  g_autoptr(GPtrArray) pub = tags_of(pub_spec);
  g_autoptr(GPtrArray) priv = tags_of(priv_spec);
  g_autofree gchar *existing = other_device_list(g_get_real_time() / G_USEC_PER_SEC - 30, pub,
                                                 priv);
  relay_put(&f.write, existing);
  device_up(&f);

  wait_restorable(f.list, 2);
  wait_state(f.list, GH_NIP29_GROUP_LIST_SYNCED);
  g_autoptr(GPtrArray) offer = gh_nip29_group_list_dup_restorable(f.list);
  const GhNip29ListedGroup *first = g_ptr_array_index(offer, 0);
  const GhNip29ListedGroup *second = g_ptr_array_index(offer, 1);
  g_assert_cmpstr(first->group_id, ==, "tacos");
  g_assert_null(first->name);
  g_assert_cmpstr(second->group_id, ==, "pizza");
  g_assert_cmpstr(second->name, ==, "Pizza Lovers");
  g_assert_cmpstr(second->relay_url, ==, f.group.url);
  /* Nothing was joined, and the list was left alone. */
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.service)), ==, 0);
  g_assert_null(nip29_last_received(&f.group, 9021));
  g_autofree gchar *untouched = stored_list(&f.write);
  g_assert_cmpstr(id_of(untouched), ==, id_of(existing));

  /* Restore pizza only. */
  g_autoptr(GPtrArray) chosen = g_ptr_array_new();
  g_ptr_array_add(chosen, (gpointer)second);
  g_autoptr(GError) error = NULL;
  g_assert_cmpuint(gh_nip29_group_list_restore(f.list, chosen, &error), ==, 1);
  g_assert_no_error(error);
  g_autoptr(GhNip29Room) room = gh_nip29_service_lookup(f.service, f.group.url, "pizza");
  g_assert_nonnull(room);
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  wait_restorable(f.list, 1);
  wait_state(f.list, GH_NIP29_GROUP_LIST_SYNCED);
  /* Already listed: nothing to publish. */
  g_autofree gchar *still = stored_list(&f.write);
  g_assert_cmpstr(id_of(still), ==, id_of(existing));
  /* Not Now: tacos is not offered again this session. */
  gh_nip29_group_list_dismiss(f.list);
  wait_restorable(f.list, 0);

  /* Leaving pizza removes it; tacos stays listed, in the clear, with the
   * relay it still uses. */
  g_autoptr(GhNip29Op) op = gh_nip29_service_leave(f.service, room, NULL, &error);
  g_assert_no_error(error);
  wait_join(room, GH_NIP29_JOIN_LEFT);
  wait_list(&f.write, id_of(existing));
  wait_state(f.list, GH_NIP29_GROUP_LIST_SYNCED);
  g_autofree gchar *after = stored_list(&f.write);
  NostrEvent *event = parse(after);
  g_assert_true(public_has_tag(event, "group", "tacos"));
  g_assert_true(public_has_tag(event, "r", f.group.url));
  g_autoptr(GPtrArray) items = private_items(event);
  g_assert_false(has_tag(items, "group", "pizza", NULL));
  nostr_event_free(event);

  /* A new device again: tacos is offered; pizza is gone from the list. */
  new_device(&f);
  wait_restorable(f.list, 1);
  g_autoptr(GPtrArray) again = gh_nip29_group_list_dup_restorable(f.list);
  g_assert_cmpstr(((GhNip29ListedGroup *)g_ptr_array_index(again, 0))->group_id, ==, "tacos");
  fixture_down(&f);
}

/* A group this device has is never dropped: a newer list from another
 * device that lacks it is merged, not obeyed. */
static void
test_local_group_survives_newer_list(void)
{
  Fixture f;
  fixture_up(&f);
  nip29_add_group(&f.group, "pizza", "Pizza Lovers");
  device_up(&f);
  wait_state(f.list, GH_NIP29_GROUP_LIST_SYNCED);
  g_autoptr(GhNip29Room) room = join(&f, "pizza");
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  wait_list(&f.write, NULL);
  wait_state(f.list, GH_NIP29_GROUP_LIST_SYNCED);
  g_autofree gchar *mine = stored_list(&f.write);
  NostrEvent *event = parse(mine);
  gint64 mine_at = nostr_event_get_created_at(event);
  nostr_event_free(event);

  /* Another device writes a newer list naming only tacos. */
  const gchar *const tacos[] = { "group", "tacos", f.group.url, NULL };
  const gchar *const *const spec[] = { tacos, NULL };
  g_autoptr(GPtrArray) priv = tags_of(spec);
  g_autofree gchar *other = other_device_list(mine_at + 20, NULL, priv);
  relay_put(&f.write, other);
  relay_put(&f.disc, other);
  wait_list(&f.write, id_of(other));
  wait_state(f.list, GH_NIP29_GROUP_LIST_SYNCED);
  g_autofree gchar *merged = stored_list(&f.write);
  event = parse(merged);
  g_assert_cmpint(nostr_event_get_created_at(event), >, mine_at + 20);
  g_autoptr(GPtrArray) items = private_items(event);
  g_assert_true(has_tag(items, "group", "pizza", f.group.url));
  g_assert_true(has_tag(items, "group", "tacos", f.group.url));
  nostr_event_free(event);
  g_autofree gchar *on_disc = stored_list(&f.disc);
  g_assert_cmpstr(id_of(on_disc), ==, id_of(merged));
  /* And tacos, which this device lacks, is offered. */
  wait_restorable(f.list, 1);
  fixture_down(&f);
}

/* Fail closed: a list whose private part does not decrypt is never
 * replaced; nor is anything published before every source answered. */
static void
test_fail_closed(void)
{
  Fixture f;
  fixture_up(&f);
  nip29_add_group(&f.group, "pizza", "Pizza Lovers");
  g_autofree gchar *garbled = account_event(GH_NIP29_GROUP_LIST_KIND,
                                            g_get_real_time() / G_USEC_PER_SEC - 30, NULL,
                                            "not an encrypted list");
  relay_put(&f.write, garbled);
  device_up(&f);
  wait_state(f.list, GH_NIP29_GROUP_LIST_UNREADABLE);
  g_autoptr(GhNip29Room) room = join(&f, "pizza");
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  wait_state(f.list, GH_NIP29_GROUP_LIST_UNREADABLE);
  g_autofree gchar *kept = stored_list(&f.write);
  g_assert_cmpstr(id_of(kept), ==, id_of(garbled));
  g_assert_null(stored_list(&f.disc));
  device_down(&f);
  g_clear_object(&room);

  /* The write relay never ends its answer: still reading, nothing sent. */
  rm_rf(f.data_dir);
  g_free(f.data_dir);
  f.data_dir = g_dir_make_tmp("groundhog-nip29-list-XXXXXX", NULL);
  nip29_relay_clear(&f.write);
  nip29_relay_init(&f.write);
  f.write.accept_lists = TRUE;
  f.write.hold_eose = TRUE;
  /* The account's 10002 now names the new write relay. */
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("r", f.write.url, "write", NULL));
  g_autofree gchar *relay_list = account_event(10002, g_get_real_time() / G_USEC_PER_SEC, tags,
                                               NULL);
  relay_put(&f.disc, relay_list);
  WriteWait moved = { f.relays, f.write.url };
  gh_test_spin_until(write_relay_is, &moved);
  device_up(&f);
  wait_state(f.list, GH_NIP29_GROUP_LIST_READING);
  g_autoptr(GhNip29Room) held = join(&f, "pizza");
  wait_join(held, GH_NIP29_JOIN_MEMBER);
  wait_state(f.list, GH_NIP29_GROUP_LIST_READING);
  g_assert_null(stored_list(&f.write));
  g_assert_null(stored_list(&f.disc));
  fixture_down(&f);
}

/* Forgetting a group here removes it from the list; a group the relay
 * refuses is removed too. */
static void
test_forget_and_refused(void)
{
  Fixture f;
  fixture_up(&f);
  nip29_add_group(&f.group, "pizza", "Pizza Lovers");
  Nip29TestGroup *nope = nip29_add_group(&f.group, "nope", "No Entry");
  nope->join_policy = NIP29_JOIN_DENY;
  const gchar *const pizza[] = { "group", "pizza", f.group.url, NULL };
  const gchar *const no[] = { "group", "nope", f.group.url, NULL };
  const gchar *const r[] = { "r", f.group.url, NULL };
  const gchar *const *const spec[] = { pizza, no, r, NULL };
  g_autoptr(GPtrArray) priv = tags_of(spec);
  g_autofree gchar *existing = other_device_list(g_get_real_time() / G_USEC_PER_SEC - 30, NULL,
                                                 priv);
  relay_put(&f.write, existing);
  device_up(&f);
  wait_restorable(f.list, 2);
  wait_state(f.list, GH_NIP29_GROUP_LIST_SYNCED);
  g_autoptr(GPtrArray) offer = gh_nip29_group_list_dup_restorable(f.list);

  /* The signer declines the join: NOT_SENT, still listed. */
  f.mock.deny = TRUE;
  g_autoptr(GPtrArray) first = g_ptr_array_new();
  g_ptr_array_add(first, g_ptr_array_index(offer, 0));
  g_autoptr(GError) error = NULL;
  g_assert_cmpuint(gh_nip29_group_list_restore(f.list, first, &error), ==, 1);
  g_assert_no_error(error);
  g_autoptr(GhNip29Room) room = gh_nip29_service_lookup(f.service, f.group.url, "pizza");
  wait_join(room, GH_NIP29_JOIN_NOT_SENT);
  wait_state(f.list, GH_NIP29_GROUP_LIST_SYNCED);
  g_autofree gchar *same = stored_list(&f.write);
  g_assert_cmpstr(id_of(same), ==, id_of(existing));
  f.mock.deny = FALSE;

  /* Forgetting it removes it from the list. */
  g_assert_true(gh_nip29_service_forget(f.service, room, &error));
  g_assert_no_error(error);
  wait_list(&f.write, id_of(existing));
  wait_state(f.list, GH_NIP29_GROUP_LIST_SYNCED);
  g_autofree gchar *forgotten = stored_list(&f.write);
  NostrEvent *event = parse(forgotten);
  g_autoptr(GPtrArray) items = private_items(event);
  g_assert_false(has_tag(items, "group", "pizza", NULL));
  g_assert_true(has_tag(items, "group", "nope", f.group.url));
  g_assert_true(has_tag(items, "r", f.group.url, NULL));
  nostr_event_free(event);

  /* The relay refuses the other one for good: it is removed, and with it
   * the relay no group uses any more. */
  g_autoptr(GPtrArray) second = g_ptr_array_new();
  g_ptr_array_add(second, g_ptr_array_index(offer, 1));
  g_assert_cmpuint(gh_nip29_group_list_restore(f.list, second, &error), ==, 1);
  g_autoptr(GhNip29Room) refused = gh_nip29_service_lookup(f.service, f.group.url, "nope");
  wait_join(refused, GH_NIP29_JOIN_DENIED);
  wait_list(&f.write, id_of(forgotten));
  wait_state(f.list, GH_NIP29_GROUP_LIST_SYNCED);
  g_autofree gchar *emptied = stored_list(&f.write);
  event = parse(emptied);
  g_assert_cmpstr(nostr_event_get_content(event), ==, "");
  g_assert_cmpuint(nostr_tags_size(nostr_event_get_tags(event)), ==, 0);
  nostr_event_free(event);
  fixture_down(&f);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  hex_alice = gh_test_pub(KEY_ALICE);
  npub_alice = gh_test_npub(KEY_ALICE);
  npub_bob = gh_test_npub(KEY_BOB);
  gh_test_bus_up(&shared_bus);
  g_test_add_func("/groundhog/nip29-group-list/private-items", test_private_items_roundtrip);
  g_test_add_func("/groundhog/nip29-group-list/publish-join-and-leave",
                  test_publish_join_and_leave);
  g_test_add_func("/groundhog/nip29-group-list/restore-on-new-store", test_restore_on_new_store);
  g_test_add_func("/groundhog/nip29-group-list/local-group-survives-newer-list",
                  test_local_group_survives_newer_list);
  g_test_add_func("/groundhog/nip29-group-list/forget-and-refused", test_forget_and_refused);
  g_test_add_func("/groundhog/nip29-group-list/fail-closed", test_fail_closed);
  gint rc = g_test_run();
  gh_test_bus_down(&shared_bus);
  g_free(hex_alice);
  g_free(npub_alice);
  g_free(npub_bob);
  return rc;
}
