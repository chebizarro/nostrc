/* Groundhog test harness (nostrc-sjl1): a headless, D-Bus-controlled MLS
 * service instance for the two-instance acceptance test.  Compiled only
 * with GH_MLS_TEST_HOOKS; never part of the production binary.
 *
 * Usage:  groundhog-test-harness <instance-name> <data-dir>
 *
 * Environment:
 *   DBUS_SESSION_BUS_ADDRESS   — the private test bus
 *   GSETTINGS_SCHEMA_DIR       — compiled GSettings schemas
 *   GSETTINGS_BACKEND=memory   — no dconf
 *
 * Claims org.nostr.Groundhog.<instance> on the session bus and registers
 * the org.nostr.Groundhog.TestControl interface.  Every method blocks
 * until it completes (spinning the GLib main loop). */

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>

#include "gh-account-controller.h"
#include "gh-account-relays.h"
#include "gh-conversation-store.h"
#include "gh-dm-inbox.h"
#include "gh-identity.h"
#include "gh-inbox-lookup.h"
#include "gh-mls-service.h"
#include "gh-store.h"
#include "gh-store-conversations.h"
#include "nostr-keys.h"
#include "nostr/nip19/nip19.h"

#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- state ---------------------------------------------------------------- */

typedef struct {
  GMainLoop *loop;
  GDBusConnection *bus;
  guint name_id;
  guint registration;
  GDBusNodeInfo *node;
  gchar *instance;
  gchar *data_dir;

  /* identity */
  gchar *npub;
  gchar *hex;

  /* MLS service stack */
  GSettings *settings;
  GhAccountController *accounts;
  GhClock *clock;
  GhStore *store;
  GhStoreConversations *rooms;
  GhConversationStore *model;
  GhAccountRelays *relays;
  GhInboxLookup *inboxes;
  GhDmInbox *inbox;
  GhMlsService *service;
  GObject *network;

  /* relay URLs */
  gchar *group_relay_url;

  /* tracking */
  guint invites;
  gboolean ready;
} Harness;

static Harness harness;

/* ---- fake GNetworkMonitor ------------------------------------------------- */

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

/* ---- spin helpers --------------------------------------------------------- */

static gboolean
deadline_hit(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static gboolean
tick_cb(gpointer data)
{
  (void)data;
  return G_SOURCE_CONTINUE;
}

static void
spin_until(gboolean (*pred)(gpointer), gpointer data, const gchar *what, int timeout_s)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds((guint)timeout_s, deadline_hit, &expired);
  guint tick = g_timeout_add(10, tick_cb, NULL);
  while (!pred(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_source_remove(tick);
  if (expired)
    g_error("test harness: %s did not happen within %d s", what, timeout_s);
  g_source_remove(timer);
}

static void
drain(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

/* ---- identity lister ------------------------------------------------------ */

static GPtrArray *
list_identity(gpointer data, GError **error)
{
  Harness *h = data;
  (void)error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
  info->npub = g_strdup(h->npub);
  info->label = g_strdup("Test Account");
  g_ptr_array_add(ids, info);
  return ids;
}

/* ---- predicates ----------------------------------------------------------- */

static gboolean
accounts_active(gpointer data)
{
  return gh_account_controller_get_state(data) == GH_ACCOUNT_STATE_ACTIVE;
}

static gboolean
relays_known(gpointer data)
{
  Harness *h = data;
  return gh_account_relays_get_inbox_relays(h->relays) &&
         gh_account_relays_get_write_relays(h->relays);
}

static gboolean
key_package_published(gpointer data)
{
  Harness *h = data;
  return gh_mls_service_get_key_package_state(h->service) == GH_MLS_KEY_PACKAGE_PUBLISHED;
}

static gboolean
has_invite(gpointer data)
{
  return ((Harness *)data)->invites > 0;
}

/* ---- inbox storage -------------------------------------------------------- */

static gint64
inbox_load_checkpoint(gpointer data)
{
  gint64 since = 0;
  gh_store_get_cursor(data, "nip17/inbox", "", &since, NULL);
  return since;
}

static gboolean
inbox_save_checkpoint(gpointer data, gint64 checkpoint, GError **error)
{
  return gh_store_set_cursor(data, "nip17/inbox", "", checkpoint, error);
}

static const GhDmInboxStorage inbox_storage = {
  .load_checkpoint = inbox_load_checkpoint,
  .save_checkpoint = inbox_save_checkpoint,
};

/* ---- invite signal -------------------------------------------------------- */

static void
on_invite(GhMlsService *service, const gchar *wrapper_id, gpointer data)
{
  (void)service; (void)wrapper_id;
  ((Harness *)data)->invites++;
}

/* ---- teardown ------------------------------------------------------------- */

static void
release_object(gpointer object)
{
  GObject **obj = object;
  if (*obj) {
    gpointer weak = *obj;
    g_object_add_weak_pointer(G_OBJECT(*obj), &weak);
    g_object_run_dispose(G_OBJECT(*obj));
    g_object_unref(*obj);
    gboolean expired = FALSE;
    guint timer = g_timeout_add_seconds(10, deadline_hit, &expired);
    guint tick = g_timeout_add(10, tick_cb, NULL);
    while (weak && !expired)
      g_main_context_iteration(NULL, TRUE);
    g_source_remove(tick);
    g_source_remove(timer);
    *obj = NULL;
  }
}

static void
teardown_services(Harness *h)
{
  if (h->service)
    release_object(&h->service);
  if (h->inbox) {
    gh_dm_inbox_clear_storage(h->inbox);
    release_object(&h->inbox);
  }
  drain();
  if (h->rooms) {
    gh_store_conversations_close(h->rooms);
    g_clear_object(&h->rooms);
  }
  g_clear_object(&h->model);
  if (h->store)
    gh_store_close(g_steal_pointer(&h->store));
  if (h->inboxes)
    release_object(&h->inboxes);
  if (h->relays)
    release_object(&h->relays);
  if (h->accounts)
    release_object(&h->accounts);
  drain();
  g_clear_object(&h->network);
  g_clear_object(&h->settings);
  if (h->clock) {
    gh_clock_unref(h->clock);
    h->clock = NULL;
  }
  h->ready = FALSE;
}

/* ---- D-Bus interface ------------------------------------------------------ */

static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='org.nostr.Groundhog.TestControl'>"
  "    <method name='Onboard'>"
  "      <arg type='s' name='npub' direction='in'/>"
  "      <arg type='s' name='discovery_url' direction='in'/>"
  "      <arg type='s' name='write_url' direction='in'/>"
  "      <arg type='s' name='inbox_url' direction='in'/>"
  "      <arg type='s' name='group_url' direction='in'/>"
  "    </method>"
  "    <method name='CreateGroup'>"
  "      <arg type='s' name='name' direction='in'/>"
  "      <arg type='as' name='invitee_hexes' direction='in'/>"
  "      <arg type='s' name='room_id' direction='out'/>"
  "    </method>"
  "    <method name='AcceptInvite'>"
  "      <arg type='s' name='room_id' direction='out'/>"
  "    </method>"
  "    <method name='SendText'>"
  "      <arg type='s' name='room_id' direction='in'/>"
  "      <arg type='s' name='text' direction='in'/>"
  "    </method>"
  "    <method name='WaitMessage'>"
  "      <arg type='s' name='room_id' direction='in'/>"
  "      <arg type='s' name='text' direction='in'/>"
  "    </method>"
  "    <method name='ListMessages'>"
  "      <arg type='s' name='room_id' direction='in'/>"
  "      <arg type='as' name='messages' direction='out'/>"
  "    </method>"
  "    <method name='Rename'>"
  "      <arg type='s' name='room_id' direction='in'/>"
  "      <arg type='s' name='new_name' direction='in'/>"
  "    </method>"
  "    <method name='RemoveMember'>"
  "      <arg type='s' name='room_id' direction='in'/>"
  "      <arg type='s' name='member_hex' direction='in'/>"
  "    </method>"
  "    <method name='AddMember'>"
  "      <arg type='s' name='room_id' direction='in'/>"
  "      <arg type='s' name='invitee_hex' direction='in'/>"
  "    </method>"
  "    <method name='Leave'>"
  "      <arg type='s' name='room_id' direction='in'/>"
  "    </method>"
  "    <method name='ListGroups'>"
  "      <arg type='as' name='room_ids' direction='out'/>"
  "    </method>"
  "    <method name='GetGroupName'>"
  "      <arg type='s' name='room_id' direction='in'/>"
  "      <arg type='s' name='name' direction='out'/>"
  "    </method>"
  "    <method name='AcceptContact'>"
  "      <arg type='s' name='hex' direction='in'/>"
  "    </method>"
  "    <method name='Quit'/>"
  "  </interface>"
  "</node>";

/* ---- async operation wait ------------------------------------------------- */

typedef struct {
  gboolean done;
  gpointer result;
  gboolean ok;
  GError *error;
} OpWait;

static gboolean
op_done(gpointer data)
{
  return ((OpWait *)data)->done;
}

static void
on_created(GObject *source, GAsyncResult *result, gpointer data)
{
  OpWait *wait = data;
  wait->result = gh_mls_service_create_group_finish(GH_MLS_SERVICE(source), result, &wait->error);
  wait->done = TRUE;
}

static void
on_changed(GObject *source, GAsyncResult *result, gpointer data)
{
  OpWait *wait = data;
  wait->ok = gh_mls_service_change_finish(GH_MLS_SERVICE(source), result, &wait->error);
  wait->done = TRUE;
}

/* ---- message wait predicate ----------------------------------------------- */

typedef struct {
  GhConversationStore *model;
  const gchar *room_id;
  const gchar *text;
} MessageWait;

static gboolean
message_listed(gpointer data)
{
  MessageWait *wait = data;
  GhConversation *room = gh_conversation_store_lookup(wait->model, wait->room_id);
  if (!room) return FALSE;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(room));
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhMessage) msg = g_list_model_get_item(G_LIST_MODEL(room), i);
    if (g_strcmp0(gh_message_get_content(msg), wait->text) == 0)
      return TRUE;
  }
  return FALSE;
}

/* ---- method handlers ------------------------------------------------------ */

static void
handle_onboard(Harness *h, GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *npub_arg, *discovery_url, *write_url, *inbox_url, *group_url;
  g_variant_get(parameters, "(&s&s&s&s&s)", &npub_arg, &discovery_url, &write_url,
                &inbox_url, &group_url);

  /* Tear down any prior session (restart scenario). */
  if (h->ready)
    teardown_services(h);

  g_free(h->npub);
  g_free(h->hex);
  g_free(h->group_relay_url);
  h->npub = g_strdup(npub_arg);
  h->hex = gh_identity_pubkey_hex(npub_arg);
  h->group_relay_url = g_strdup(group_url);
  h->invites = 0;

  if (!h->hex) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "invalid npub");
    return;
  }

  /* Settings (memory backend, no dconf). */
  g_autoptr(GSettingsBackend) backend = g_memory_settings_backend_new();
  h->settings = g_settings_new_with_backend("org.nostr.Groundhog", backend);
  g_settings_set_string(h->settings, "signer-method", "auto");
  g_settings_set_string(h->settings, "current-npub", h->npub);
  const gchar *discovery[] = { discovery_url, NULL };
  g_settings_set_strv(h->settings, "discovery-relays", discovery);

  /* Account controller (with our identity lister). */
  h->accounts = gh_account_controller_new_full(h->settings, h->bus, list_identity, h);
  spin_until(accounts_active, h->accounts, "account becoming active", 30);

  /* Clock, data dir, network monitor. */
  h->clock = gh_clock_new_system();
  g_assert_cmpint(g_mkdir_with_parents(h->data_dir, 0700), ==, 0);
  h->network = g_object_new(FAKE_TYPE_MONITOR, NULL);

  /* Relay lists. */
  h->relays = gh_account_relays_new(h->accounts, h->settings, NULL, NULL);
  spin_until(relays_known, h, "relay lists", 30);

  /* Inbox lookup. */
  h->inboxes = gh_inbox_lookup_new(h->accounts, h->settings, NULL, NULL);

  /* Open the store. */
  guint8 key_bytes[GH_STORE_KEY_SIZE];
  for (guint i = 0; i < GH_STORE_KEY_SIZE; i++)
    key_bytes[i] = (guint8)(0x41 + i);
  /* Mix in the instance name's first char for different instances. */
  if (h->instance && h->instance[0])
    key_bytes[0] ^= (guint8)h->instance[strlen(h->instance) - 1];
  g_autoptr(GBytes) key = g_bytes_new(key_bytes, sizeof key_bytes);
  GhStoreConfig config = { h->data_dir, h->hex, NULL, NULL, h->clock };
  /* Store ID must be a UUID; derive a deterministic one from the instance
   * name so restart reopens the same store. */
  g_autofree gchar *store_id = NULL;
  if (h->instance && g_strcmp0(h->instance, "testB") == 0)
    store_id = g_strdup("b0000000-0000-4000-8000-000000000002");
  else
    store_id = g_strdup("a0000000-0000-4000-8000-000000000001");
  g_autoptr(GError) error = NULL;
  h->store = gh_store_open_with_key(&config, key, store_id, GH_STORE_OPEN_CREATE, &error);
  if (!h->store) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", error->message);
    return;
  }

  /* Conversation model. */
  h->model = gh_conversation_store_new();
  h->rooms = gh_store_conversations_new(h->store);
  g_assert_true(gh_store_conversations_attach(h->rooms, h->model, 0, &error));

  /* DM inbox with storage. */
  h->inbox = gh_dm_inbox_new_with_storage(h->accounts, h->relays, h->model, NULL, NULL, NULL);
  g_assert_true(gh_dm_inbox_set_storage(h->inbox,
    gh_account_controller_get_generation(h->accounts), &inbox_storage, h->store));

  /* MLS service. */
  GhMlsServiceConfig mls_config = {
    .store = h->store,
    .accounts = h->accounts,
    .conversations = h->model,
    .account_relays = h->relays,
    .inboxes = GH_INBOX_RESOLVER(h->inboxes),
    .settings = h->settings,
    .inbox = h->inbox,
    .network = G_NETWORK_MONITOR(h->network),
    .publish_deadline = 20,
    .lookup_deadline = 20,
  };
  h->service = gh_mls_service_new(&mls_config, &error);
  if (!h->service) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", error->message);
    return;
  }
  g_signal_connect(h->service, "invite-received", G_CALLBACK(on_invite), h);

  /* Wait for KeyPackage publication. */
  spin_until(key_package_published, h, "KeyPackage publication", 60);

  h->ready = TRUE;
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
handle_create_group(Harness *h, GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *name;
  g_autoptr(GVariantIter) iter = NULL;
  g_variant_get(parameters, "(&sas)", &name, &iter);

  g_autoptr(GPtrArray) invitees = g_ptr_array_new_with_free_func(g_free);
  const gchar *invitee;
  while (g_variant_iter_next(iter, "&s", &invitee))
    g_ptr_array_add(invitees, g_strdup(invitee));
  g_ptr_array_add(invitees, NULL);

  const gchar *relays[] = { h->group_relay_url, NULL };
  OpWait wait = { 0 };
  gh_mls_service_create_group_async(h->service, name, "acceptance test", relays,
    (const gchar *const *)invitees->pdata, NULL, on_created, &wait);
  spin_until(op_done, &wait, "group creation", 90);

  if (wait.error) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", wait.error->message);
    g_error_free(wait.error);
    return;
  }

  GhMlsGroup *group = wait.result;
  const gchar *room_id = gh_mls_group_get_room_id(group);
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", room_id));
  g_object_unref(group); /* service keeps it */
}

static void
handle_accept_invite(Harness *h, GDBusMethodInvocation *invocation)
{
  /* Wait for an invitation to arrive. */
  spin_until(has_invite, h, "an invitation", 90);

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) invites = gh_mls_service_list_invites(h->service, &error);
  if (!invites || invites->len == 0) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "no invitation pending");
    return;
  }

  GhMlsInvite *invite = g_ptr_array_index(invites, 0);
  GhMlsGroup *group = gh_mls_service_accept_invite(h->service, invite->wrapper_id, &error);
  if (!group) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", error->message);
    return;
  }

  h->invites = 0;
  const gchar *room_id = gh_mls_group_get_room_id(group);
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", room_id));
}

static void
handle_send_text(Harness *h, GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id, *text;
  g_variant_get(parameters, "(&s&s)", &room_id, &text);

  GhMlsGroup *group = gh_mls_service_lookup(h->service, room_id);
  if (!group) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "group not found");
    return;
  }

  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) msg = gh_mls_service_send(h->service, group, text, &error);
  if (!msg) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", error->message);
    return;
  }

  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
handle_wait_message(Harness *h, GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id, *text;
  g_variant_get(parameters, "(&s&s)", &room_id, &text);

  MessageWait mw = { h->model, room_id, text };
  spin_until(message_listed, &mw, "message listed", 90);
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
handle_list_messages(Harness *h, GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id;
  g_variant_get(parameters, "(&s)", &room_id);

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));

  GhConversation *room = gh_conversation_store_lookup(h->model, room_id);
  if (room) {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(room));
    for (guint i = 0; i < n; i++) {
      g_autoptr(GhMessage) msg = g_list_model_get_item(G_LIST_MODEL(room), i);
      const gchar *content = gh_message_get_content(msg);
      if (content)
        g_variant_builder_add(&builder, "s", content);
    }
  }

  g_dbus_method_invocation_return_value(invocation, g_variant_new("(as)", &builder));
}

static void
handle_rename(Harness *h, GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id, *new_name;
  g_variant_get(parameters, "(&s&s)", &room_id, &new_name);

  GhMlsGroup *group = gh_mls_service_lookup(h->service, room_id);
  if (!group) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "group not found");
    return;
  }

  OpWait wait = { 0 };
  gh_mls_service_update_metadata_async(h->service, group, new_name, NULL, NULL, on_changed,
                                       &wait);
  spin_until(op_done, &wait, "rename", 90);

  if (wait.error) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", wait.error->message);
    g_error_free(wait.error);
    return;
  }
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
handle_remove_member(Harness *h, GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id, *member_hex;
  g_variant_get(parameters, "(&s&s)", &room_id, &member_hex);

  GhMlsGroup *group = gh_mls_service_lookup(h->service, room_id);
  if (!group) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "group not found");
    return;
  }

  const gchar *members[] = { member_hex, NULL };
  OpWait wait = { 0 };
  gh_mls_service_remove_members_async(h->service, group, members, NULL, on_changed, &wait);
  spin_until(op_done, &wait, "remove member", 90);

  if (wait.error) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", wait.error->message);
    g_error_free(wait.error);
    return;
  }
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
handle_add_member(Harness *h, GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id, *invitee_hex;
  g_variant_get(parameters, "(&s&s)", &room_id, &invitee_hex);

  GhMlsGroup *group = gh_mls_service_lookup(h->service, room_id);
  if (!group) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "group not found");
    return;
  }

  const gchar *invitees[] = { invitee_hex, NULL };
  OpWait wait = { 0 };
  gh_mls_service_add_members_async(h->service, group, invitees, NULL, on_changed, &wait);
  spin_until(op_done, &wait, "add member", 90);

  if (wait.error) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", wait.error->message);
    g_error_free(wait.error);
    return;
  }
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
handle_leave(Harness *h, GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id;
  g_variant_get(parameters, "(&s)", &room_id);

  GhMlsGroup *group = gh_mls_service_lookup(h->service, room_id);
  if (!group) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "group not found");
    return;
  }

  g_autoptr(GError) error = NULL;
  if (!gh_mls_service_leave(h->service, group, &error)) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", error->message);
    return;
  }
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
handle_list_groups(Harness *h, GDBusMethodInvocation *invocation)
{
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));

  if (h->service) {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(h->service));
    for (guint i = 0; i < n; i++) {
      g_autoptr(GhMlsGroup) group = g_list_model_get_item(G_LIST_MODEL(h->service), i);
      const gchar *room_id = gh_mls_group_get_room_id(group);
      if (room_id)
        g_variant_builder_add(&builder, "s", room_id);
    }
  }

  g_dbus_method_invocation_return_value(invocation, g_variant_new("(as)", &builder));
}

static void
handle_get_group_name(Harness *h, GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id;
  g_variant_get(parameters, "(&s)", &room_id);

  GhMlsGroup *group = gh_mls_service_lookup(h->service, room_id);
  if (!group) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "group not found");
    return;
  }

  const gchar *name = gh_mls_group_get_name(group);
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", name ? name : ""));
}

static void
handle_accept_contact(Harness *h, GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *peer_hex;
  g_variant_get(parameters, "(&s)", &peer_hex);

  /* Open a NIP-17 room with this peer — makes them an accepted contact. */
  const gchar *peers[] = { peer_hex, NULL };
  g_autoptr(GError) error = NULL;
  GhConversation *room = gh_conversation_store_open_room(h->model, peers, &error);
  if (!room) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error",
      error ? error->message : "failed to open contact room");
    return;
  }
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
handle_quit(Harness *h, GDBusMethodInvocation *invocation)
{
  g_dbus_method_invocation_return_value(invocation, NULL);
  teardown_services(h);
  g_main_loop_quit(h->loop);
}

/* ---- D-Bus dispatch ------------------------------------------------------- */

static void
method_call(GDBusConnection *connection, const gchar *sender, const gchar *object_path,
            const gchar *interface_name, const gchar *method_name, GVariant *parameters,
            GDBusMethodInvocation *invocation, gpointer user_data)
{
  Harness *h = user_data;
  (void)connection; (void)sender; (void)object_path; (void)interface_name;

  if (g_str_equal(method_name, "Onboard"))
    handle_onboard(h, invocation, parameters);
  else if (g_str_equal(method_name, "CreateGroup"))
    handle_create_group(h, invocation, parameters);
  else if (g_str_equal(method_name, "AcceptInvite"))
    handle_accept_invite(h, invocation);
  else if (g_str_equal(method_name, "SendText"))
    handle_send_text(h, invocation, parameters);
  else if (g_str_equal(method_name, "WaitMessage"))
    handle_wait_message(h, invocation, parameters);
  else if (g_str_equal(method_name, "ListMessages"))
    handle_list_messages(h, invocation, parameters);
  else if (g_str_equal(method_name, "Rename"))
    handle_rename(h, invocation, parameters);
  else if (g_str_equal(method_name, "RemoveMember"))
    handle_remove_member(h, invocation, parameters);
  else if (g_str_equal(method_name, "AddMember"))
    handle_add_member(h, invocation, parameters);
  else if (g_str_equal(method_name, "Leave"))
    handle_leave(h, invocation, parameters);
  else if (g_str_equal(method_name, "ListGroups"))
    handle_list_groups(h, invocation);
  else if (g_str_equal(method_name, "GetGroupName"))
    handle_get_group_name(h, invocation, parameters);
  else if (g_str_equal(method_name, "AcceptContact"))
    handle_accept_contact(h, invocation, parameters);
  else if (g_str_equal(method_name, "Quit"))
    handle_quit(h, invocation);
  else
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "unknown method");
}

static const GDBusInterfaceVTable vtable = { method_call, NULL, NULL, { 0 } };

/* ---- bus name callbacks --------------------------------------------------- */

static void
on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  (void)connection; (void)user_data;
  g_message("harness: acquired name %s", name);
}

static void
on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  (void)connection;
  g_warning("harness: lost name %s", name);
  Harness *h = user_data;
  g_main_loop_quit(h->loop);
}

/* ---- SIGTERM handler ------------------------------------------------------ */

static gboolean
on_terminate(gpointer data)
{
  Harness *h = data;
  teardown_services(h);
  g_main_loop_quit(h->loop);
  return G_SOURCE_REMOVE;
}

/* ---- main ----------------------------------------------------------------- */

int
main(int argc, char **argv)
{
  if (argc < 3) {
    g_printerr("Usage: groundhog-test-harness <instance-name> <data-dir>\n");
    return 1;
  }

  harness.instance = g_strdup(argv[1]);
  harness.data_dir = g_strdup(argv[2]);

  /* Build the bus name. */
  g_autofree gchar *bus_name = g_strdup_printf("org.nostr.Groundhog.%s", harness.instance);

  /* Warnings are not fatal (relay libsoup races); criticals are. */
  g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

  harness.loop = g_main_loop_new(NULL, FALSE);

  g_autoptr(GError) error = NULL;
  harness.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!harness.bus) {
    g_printerr("harness: cannot connect to session bus: %s\n", error->message);
    return 1;
  }

  /* Parse D-Bus introspection. */
  harness.node = g_dbus_node_info_new_for_xml(introspection_xml, &error);
  if (!harness.node) {
    g_printerr("harness: bad introspection XML: %s\n", error->message);
    return 1;
  }

  /* Register the test control object. */
  harness.registration = g_dbus_connection_register_object(harness.bus,
    "/org/nostr/Groundhog/TestControl", harness.node->interfaces[0],
    &vtable, &harness, NULL, &error);
  if (!harness.registration) {
    g_printerr("harness: cannot register object: %s\n", error->message);
    return 1;
  }

  /* Own the bus name. */
  harness.name_id = g_bus_own_name_on_connection(harness.bus, bus_name,
    G_BUS_NAME_OWNER_FLAGS_NONE, on_name_acquired, on_name_lost, &harness, NULL);

  guint sigterm = g_unix_signal_add(SIGTERM, on_terminate, &harness);
  guint sigint = g_unix_signal_add(SIGINT, on_terminate, &harness);

  g_main_loop_run(harness.loop);

  g_source_remove(sigterm);
  g_source_remove(sigint);
  if (harness.registration)
    g_dbus_connection_unregister_object(harness.bus, harness.registration);
  if (harness.name_id)
    g_bus_unown_name(harness.name_id);
  g_dbus_node_info_unref(harness.node);
  g_object_unref(harness.bus);
  g_main_loop_unref(harness.loop);
  g_free(harness.instance);
  g_free(harness.data_dir);
  g_free(harness.npub);
  g_free(harness.hex);
  g_free(harness.group_relay_url);

  return 0;
}
