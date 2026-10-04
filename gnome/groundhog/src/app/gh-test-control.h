/* gh-test-control.h — D-Bus TestControl interface for the real Groundhog
 * binary (compiled only with GH_MLS_TEST_HOOKS). Delegates every method
 * to the production GhAppServices objects, so the acceptance test exercises
 * the real app code path.
 *
 * Included from main.c; never linked into the production binary.
 * The interface is registered only when GH_TEST_CONTROL=1 is set. */

#ifndef GH_TEST_CONTROL_H
#define GH_TEST_CONTROL_H

#include "gh-app-services.h"
#include "gh-account-controller.h"
#include "gh-account-relays.h"
#include "gh-account-store.h"
#include "gh-conversation-store.h"
#include "gh-mls-service.h"
#include "gh-message.h"
#if GROUNDHOG_HAVE_BACKGROUND
#include "gh-background.h"
#endif

/* ---- spin helpers --------------------------------------------------------- */

static gboolean tc_deadline(gpointer data) { *(gboolean *)data = TRUE; return G_SOURCE_REMOVE; }
static gboolean tc_tick(gpointer data) { (void)data; return G_SOURCE_CONTINUE; }

static gboolean
tc_wait_until(gboolean (*pred)(gpointer), gpointer data, int timeout_s)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds((guint)timeout_s, tc_deadline, &expired);
  guint tick = g_timeout_add(10, tc_tick, NULL);
  while (!pred(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_source_remove(tick);
  if (!expired)
    g_source_remove(timer);
  return pred(data);
}

static void
tc_spin_until(gboolean (*pred)(gpointer), gpointer data, const gchar *what, int timeout_s)
{
  if (!tc_wait_until(pred, data, timeout_s))
    g_error("TestControl: %s did not happen within %d s", what, timeout_s);
}

/* ---- predicates ----------------------------------------------------------- */

static gboolean
tc_account_active(gpointer data)
{
  return gh_account_controller_get_state(data) == GH_ACCOUNT_STATE_ACTIVE;
}

static gboolean
tc_identities_listed(gpointer data)
{
  return gh_account_controller_get_identities(data) != NULL;
}

static gboolean
tc_relays_known(gpointer data)
{
  GhAccountRelays *relays = data;
  return gh_account_relays_get_inbox_relays(relays) &&
         gh_account_relays_get_write_relays(relays);
}

static gboolean
tc_kp_published(gpointer data)
{
  return gh_mls_service_get_key_package_state(data) == GH_MLS_KEY_PACKAGE_PUBLISHED;
}

static gboolean
tc_store_open(gpointer data)
{
  return gh_account_store_get_state(GH_ACCOUNT_STORE(data)) == GH_ACCOUNT_STORE_OPEN;
}

/* ---- async operation wait ------------------------------------------------- */

typedef struct { gboolean done; gpointer result; gboolean ok; GError *error; } TcOpWait;
static gboolean tc_op_done(gpointer data) { return ((TcOpWait *)data)->done; }

static void
tc_on_created(GObject *source, GAsyncResult *result, gpointer data)
{
  TcOpWait *wait = data;
  wait->result = gh_mls_service_create_group_finish(GH_MLS_SERVICE(source), result, &wait->error);
  wait->done = TRUE;
}

static void
tc_on_changed(GObject *source, GAsyncResult *result, gpointer data)
{
  TcOpWait *wait = data;
  wait->ok = gh_mls_service_change_finish(GH_MLS_SERVICE(source), result, &wait->error);
  wait->done = TRUE;
}

/* ---- message wait --------------------------------------------------------- */

typedef struct { GhConversationStore *model; const gchar *room_id; const gchar *text; } TcMsgWait;

static gboolean
tc_msg_listed(gpointer data)
{
  TcMsgWait *wait = data;
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

/* ---- invite tracking ------------------------------------------------------ */

static guint tc_invites;

static void
tc_on_invite(GhMlsService *service, const gchar *wrapper_id, gpointer data)
{
  (void)service; (void)wrapper_id; (void)data;
  tc_invites++;
}

static gboolean tc_has_invite(gpointer data) { (void)data; return tc_invites > 0; }

typedef struct {
  GhMlsGroup *group;
  const gchar *member;
  gboolean present;
} TcMemberWait;

static gboolean
tc_member_matches(gpointer data)
{
  TcMemberWait *wait = data;
  g_auto(GStrv) members = gh_mls_group_dup_members(wait->group);
  gboolean present = members && g_strv_contains((const gchar *const *)members, wait->member);
  return present == wait->present;
}

typedef struct { GhMlsGroup *group; GhMlsGroupEnd end; } TcEndWait;
static gboolean tc_end_matches(gpointer data)
{
  TcEndWait *wait = data;
  return gh_mls_group_get_end(wait->group) == wait->end;
}

/* ---- state ---------------------------------------------------------------- */

static GhAppServices *tc_services;
static GhMlsService *tc_mls;
static GhConversationStore *tc_conversations;
static gulong tc_invite_handler;
static GDBusConnection *tc_bus;
static GDBusNodeInfo *tc_node;
static guint tc_registration;

/* ---- D-Bus interface ------------------------------------------------------ */

static const gchar tc_introspection_xml[] =
  "<node>"
  "  <interface name='org.nostr.Groundhog.TestControl'>"
  "    <method name='Onboard'>"
  "      <arg type='s' name='npub' direction='in'/>"
  "      <arg type='s' name='discovery_url' direction='in'/>"
  "      <arg type='s' name='write_url' direction='in'/>"
  "      <arg type='s' name='inbox_url' direction='in'/>"
  "      <arg type='s' name='group_url' direction='in'/>"
  "    </method>"
  "    <method name='TryEnableBackground'>"
  "      <arg type='b' name='enabled' direction='out'/>"
  "    </method>"
  "    <method name='GetActiveNpub'>"
  "      <arg type='s' name='npub' direction='out'/>"
  "    </method>"
  "    <method name='WaitReady'>"
  "      <arg type='s' name='group_relay_url' direction='in'/>"
  "    </method>"
  "    <method name='AcceptContact'>"
  "      <arg type='s' name='hex' direction='in'/>"
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
  "    <method name='WaitMember'>"
  "      <arg type='s' name='room_id' direction='in'/>"
  "      <arg type='s' name='member_hex' direction='in'/>"
  "      <arg type='b' name='present' direction='in'/>"
  "    </method>"
  "    <method name='WaitGroupEnd'>"
  "      <arg type='s' name='room_id' direction='in'/>"
  "      <arg type='i' name='end' direction='in'/>"
  "    </method>"
  "    <method name='GetGroupName'>"
  "      <arg type='s' name='room_id' direction='in'/>"
  "      <arg type='s' name='name' direction='out'/>"
  "    </method>"
  "    <method name='Quit'/>"
  "  </interface>"
  "</node>";

static gchar *tc_group_relay_url;

/* ---- method handlers ------------------------------------------------------ */

#if GROUNDHOG_HAVE_BACKGROUND
static void
tc_background_done(GObject *source, GAsyncResult *result, gpointer data)
{
  TcOpWait *wait = data;
  wait->ok = gh_background_set_enabled_finish(GH_BACKGROUND(source), result, &wait->error);
  wait->done = TRUE;
}
#endif

static void
tc_handle_try_background(GDBusMethodInvocation *invocation)
{
#if GROUNDHOG_HAVE_BACKGROUND
  GhBackground *background = gh_background_get_for_application(g_application_get_default());
  if (background) {
    TcOpWait wait = { 0 };
    gh_background_set_enabled_async(background, TRUE, NULL, tc_background_done, &wait);
    tc_spin_until(tc_op_done, &wait, "background choice", 10);
    g_clear_error(&wait.error);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", wait.ok));
    return;
  }
#endif
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
}

static void
tc_handle_onboard(GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *npub, *discovery_url, *write_url, *inbox_url, *group_url;
  g_variant_get(parameters, "(&s&s&s&s&s)", &npub, &discovery_url, &write_url,
                &inbox_url, &group_url);
  (void)write_url;
  (void)inbox_url;
  (void)group_url;

  GSettings *settings = gh_app_services_get_settings(tc_services);
  GhAccountController *accounts = GH_ACCOUNT_CONTROLLER(gh_app_services_get_accounts(tc_services));
  if (!settings || !accounts) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "account services unavailable");
    return;
  }
  const gchar *discovery[] = { discovery_url, NULL };
  g_settings_set_string(settings, "signer-method", "auto");
  g_settings_set_strv(settings, "discovery-relays", discovery);
  tc_spin_until(tc_identities_listed, accounts, "signer identity listing", 30);
  g_autoptr(GError) error = NULL;
  if (!gh_account_controller_select(accounts, npub, &error)) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", error->message);
    return;
  }
  tc_spin_until(tc_account_active, accounts, "account becoming active", 30);
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
tc_handle_wait_ready(GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *group_relay_url;
  g_variant_get(parameters, "(&s)", &group_relay_url);
  g_free(tc_group_relay_url);
  tc_group_relay_url = g_strdup(group_relay_url);

  /* Wait for the account to become active. */
  GObject *accounts_obj = gh_app_services_get_accounts(tc_services);
  if (!accounts_obj) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "no account controller");
    return;
  }
  GhAccountController *accounts = GH_ACCOUNT_CONTROLLER(accounts_obj);
  tc_spin_until(tc_account_active, accounts, "account becoming active", 30);

  /* The real Secret Service key path must work. No ephemeral fallback. */
  GObject *store_obj = gh_app_services_get_account_store(tc_services);
  if (store_obj) {
    GhAccountStore *store = GH_ACCOUNT_STORE(store_obj);
    if (!tc_wait_until(tc_store_open, store, 30)) {
      g_dbus_method_invocation_return_dbus_error(invocation,
        "org.nostr.Groundhog.TestControl.Error", "encrypted store did not open through Secret Service");
      return;
    }
  }

  /* Wait for relay lists. */
  GObject *relays_obj = gh_app_services_get_account_relays(tc_services);
  if (relays_obj)
    tc_spin_until(tc_relays_known, GH_ACCOUNT_RELAYS(relays_obj), "relay lists", 30);

  /* Get the MLS service and wait for KeyPackage publication. */
  GObject *mls_obj = gh_app_services_get_mls_service(tc_services);
  if (!mls_obj) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "MLS service not available");
    return;
  }
  tc_mls = GH_MLS_SERVICE(mls_obj);
  tc_invite_handler = g_signal_connect(tc_mls, "invite-received",
                                        G_CALLBACK(tc_on_invite), NULL);
  tc_invites = 0;

  GObject *conv_obj = gh_app_services_get_conversations(tc_services);
  tc_conversations = conv_obj ? GH_CONVERSATION_STORE(conv_obj) : NULL;

  tc_spin_until(tc_kp_published, tc_mls, "KeyPackage publication", 60);

  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
tc_handle_accept_contact(GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *peer_hex;
  g_variant_get(parameters, "(&s)", &peer_hex);

  if (!tc_conversations) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "no conversation store");
    return;
  }

  const gchar *peers[] = { peer_hex, NULL };
  g_autoptr(GError) error = NULL;
  GhConversation *room = gh_conversation_store_open_room(tc_conversations, peers, &error);
  if (!room) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error",
      error ? error->message : "failed to open contact room");
    return;
  }
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
tc_handle_create_group(GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *name;
  g_autoptr(GVariantIter) iter = NULL;
  g_variant_get(parameters, "(&sas)", &name, &iter);

  g_autoptr(GPtrArray) invitees = g_ptr_array_new_with_free_func(g_free);
  const gchar *invitee;
  while (g_variant_iter_next(iter, "&s", &invitee))
    g_ptr_array_add(invitees, g_strdup(invitee));
  g_ptr_array_add(invitees, NULL);

  const gchar *relays[] = { tc_group_relay_url, NULL };
  TcOpWait wait = { 0 };
  gh_mls_service_create_group_async(tc_mls, name, "acceptance test", relays,
    (const gchar *const *)invitees->pdata, NULL, tc_on_created, &wait);
  tc_spin_until(tc_op_done, &wait, "group creation", 90);

  if (wait.error) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", wait.error->message);
    g_error_free(wait.error);
    return;
  }

  GhMlsGroup *group = wait.result;
  const gchar *room_id = gh_mls_group_get_room_id(group);
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", room_id));
  g_object_unref(group);
}

static void
tc_handle_accept_invite(GDBusMethodInvocation *invocation)
{
  tc_spin_until(tc_has_invite, NULL, "an invitation", 90);

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) invites = gh_mls_service_list_invites(tc_mls, &error);
  if (!invites || invites->len == 0) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "no invitation pending");
    return;
  }

  GhMlsInvite *invite = g_ptr_array_index(invites, 0);
  GhMlsGroup *group = gh_mls_service_accept_invite(tc_mls, invite->wrapper_id, &error);
  if (!group) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", error->message);
    return;
  }

  tc_invites = 0;
  const gchar *room_id = gh_mls_group_get_room_id(group);
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", room_id));
}

static void
tc_handle_send_text(GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id, *text;
  g_variant_get(parameters, "(&s&s)", &room_id, &text);

  GhMlsGroup *group = gh_mls_service_lookup(tc_mls, room_id);
  if (!group) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "group not found");
    return;
  }

  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) message = gh_mls_service_send(tc_mls, group, text, &error);
  if (!message) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", error->message);
    return;
  }
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
tc_handle_wait_message(GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id, *text;
  g_variant_get(parameters, "(&s&s)", &room_id, &text);

  TcMsgWait mw = { tc_conversations, room_id, text };
  tc_spin_until(tc_msg_listed, &mw, text, 60);
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
tc_handle_list_messages(GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id;
  g_variant_get(parameters, "(&s)", &room_id);

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));

  GhConversation *room = tc_conversations
    ? gh_conversation_store_lookup(tc_conversations, room_id) : NULL;
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
tc_handle_rename(GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id, *new_name;
  g_variant_get(parameters, "(&s&s)", &room_id, &new_name);

  GhMlsGroup *group = gh_mls_service_lookup(tc_mls, room_id);
  if (!group) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "group not found");
    return;
  }

  TcOpWait wait = { 0 };
  gh_mls_service_update_metadata_async(tc_mls, group, new_name, NULL, NULL, tc_on_changed, &wait);
  tc_spin_until(tc_op_done, &wait, "rename", 60);

  if (wait.error) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", wait.error->message);
    g_error_free(wait.error);
    return;
  }
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
tc_handle_remove_member(GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id, *member_hex;
  g_variant_get(parameters, "(&s&s)", &room_id, &member_hex);

  GhMlsGroup *group = gh_mls_service_lookup(tc_mls, room_id);
  if (!group) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "group not found");
    return;
  }

  const gchar *members[] = { member_hex, NULL };
  TcOpWait wait = { 0 };
  gh_mls_service_remove_members_async(tc_mls, group, members, NULL, tc_on_changed, &wait);
  tc_spin_until(tc_op_done, &wait, "remove member", 60);

  if (wait.error) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", wait.error->message);
    g_error_free(wait.error);
    return;
  }
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
tc_handle_add_member(GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id, *invitee_hex;
  g_variant_get(parameters, "(&s&s)", &room_id, &invitee_hex);

  GhMlsGroup *group = gh_mls_service_lookup(tc_mls, room_id);
  if (!group) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "group not found");
    return;
  }

  const gchar *members[] = { invitee_hex, NULL };
  TcOpWait wait = { 0 };
  gh_mls_service_add_members_async(tc_mls, group, members, NULL, tc_on_changed, &wait);
  tc_spin_until(tc_op_done, &wait, "add member", 60);

  if (wait.error) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", wait.error->message);
    g_error_free(wait.error);
    return;
  }
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
tc_handle_leave(GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id;
  g_variant_get(parameters, "(&s)", &room_id);

  GhMlsGroup *group = gh_mls_service_lookup(tc_mls, room_id);
  if (!group) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "group not found");
    return;
  }

  g_autoptr(GError) error = NULL;
  if (!gh_mls_service_leave(tc_mls, group, &error)) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", error->message);
    return;
  }
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
tc_handle_list_groups(GDBusMethodInvocation *invocation)
{
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));

  guint count = g_list_model_get_n_items(G_LIST_MODEL(tc_mls));
  for (guint i = 0; i < count; i++) {
    g_autoptr(GhMlsGroup) group = g_list_model_get_item(G_LIST_MODEL(tc_mls), i);
    g_variant_builder_add(&builder, "s", gh_mls_group_get_room_id(group));
  }
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(as)", &builder));
}

static void
tc_handle_wait_member(GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id, *member;
  gboolean present;
  g_variant_get(parameters, "(&s&sb)", &room_id, &member, &present);
  GhMlsGroup *group = gh_mls_service_lookup(tc_mls, room_id);
  if (!group) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "group not found");
    return;
  }
  TcMemberWait wait = { group, member, present };
  tc_spin_until(tc_member_matches, &wait, "member state", 60);
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
tc_handle_wait_group_end(GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id;
  gint end;
  g_variant_get(parameters, "(&si)", &room_id, &end);
  GhMlsGroup *group = gh_mls_service_lookup(tc_mls, room_id);
  if (!group) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "group not found");
    return;
  }
  TcEndWait wait = { group, (GhMlsGroupEnd)end };
  tc_spin_until(tc_end_matches, &wait, "group end state", 60);
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
tc_handle_get_group_name(GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const gchar *room_id;
  g_variant_get(parameters, "(&s)", &room_id);

  GhMlsGroup *group = gh_mls_service_lookup(tc_mls, room_id);
  const gchar *name = group ? gh_mls_group_get_name(group) : "";
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", name ? name : ""));
}

/* ---- D-Bus dispatch ------------------------------------------------------- */

static void
tc_method_call(GDBusConnection *connection, const gchar *sender,
               const gchar *object_path, const gchar *interface_name,
               const gchar *method_name, GVariant *parameters,
               GDBusMethodInvocation *invocation, gpointer user_data)
{
  (void)connection; (void)sender; (void)object_path; (void)interface_name;
  (void)user_data;

  if (g_str_equal(method_name, "Onboard"))
    tc_handle_onboard(invocation, parameters);
  else if (g_str_equal(method_name, "TryEnableBackground"))
    tc_handle_try_background(invocation);
  else if (g_str_equal(method_name, "GetActiveNpub")) {
    GhAccountController *accounts = GH_ACCOUNT_CONTROLLER(gh_app_services_get_accounts(tc_services));
    if (accounts)
      tc_spin_until(tc_account_active, accounts, "persisted account becoming active", 30);
    const gchar *npub = accounts ? gh_account_controller_get_active_npub(accounts) : NULL;
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", npub ? npub : ""));
  }
  else if (g_str_equal(method_name, "WaitReady"))
    tc_handle_wait_ready(invocation, parameters);
  else if (g_str_equal(method_name, "AcceptContact"))
    tc_handle_accept_contact(invocation, parameters);
  else if (g_str_equal(method_name, "CreateGroup"))
    tc_handle_create_group(invocation, parameters);
  else if (g_str_equal(method_name, "AcceptInvite"))
    tc_handle_accept_invite(invocation);
  else if (g_str_equal(method_name, "SendText"))
    tc_handle_send_text(invocation, parameters);
  else if (g_str_equal(method_name, "WaitMessage"))
    tc_handle_wait_message(invocation, parameters);
  else if (g_str_equal(method_name, "ListMessages"))
    tc_handle_list_messages(invocation, parameters);
  else if (g_str_equal(method_name, "Rename"))
    tc_handle_rename(invocation, parameters);
  else if (g_str_equal(method_name, "RemoveMember"))
    tc_handle_remove_member(invocation, parameters);
  else if (g_str_equal(method_name, "AddMember"))
    tc_handle_add_member(invocation, parameters);
  else if (g_str_equal(method_name, "Leave"))
    tc_handle_leave(invocation, parameters);
  else if (g_str_equal(method_name, "ListGroups"))
    tc_handle_list_groups(invocation);
  else if (g_str_equal(method_name, "WaitMember"))
    tc_handle_wait_member(invocation, parameters);
  else if (g_str_equal(method_name, "WaitGroupEnd"))
    tc_handle_wait_group_end(invocation, parameters);
  else if (g_str_equal(method_name, "GetGroupName"))
    tc_handle_get_group_name(invocation, parameters);
  else if (g_str_equal(method_name, "Quit"))
    { g_dbus_method_invocation_return_value(invocation, NULL);
      g_application_quit(G_APPLICATION(g_application_get_default())); }
  else
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Groundhog.TestControl.Error", "unknown method");
}

static const GDBusInterfaceVTable tc_vtable = { tc_method_call, NULL, NULL, { 0 } };

/* Register the TestControl interface on the app's D-Bus connection. */
static G_GNUC_UNUSED void
gh_test_control_register(GhAppServices *services, GDBusConnection *bus)
{
  tc_services = services;
  g_application_hold(g_application_get_default());
  g_autoptr(GError) error = NULL;
  tc_node = g_dbus_node_info_new_for_xml(tc_introspection_xml, &error);
  if (!tc_node) {
    g_warning("TestControl: bad introspection XML: %s", error->message);
    return;
  }
  tc_registration = g_dbus_connection_register_object(bus,
    "/org/nostr/Groundhog/TestControl", tc_node->interfaces[0],
    &tc_vtable, services, NULL, &error);
  if (!tc_registration)
    g_warning("TestControl: could not register: %s", error->message);
  else {
    tc_bus = g_object_ref(bus);
    g_message("TestControl: registered on D-Bus");
  }
}

static void
gh_test_control_unregister(void)
{
  if (tc_invite_handler && tc_mls)
    g_signal_handler_disconnect(tc_mls, tc_invite_handler);
  tc_invite_handler = 0;
  tc_mls = NULL;
  tc_conversations = NULL;
  tc_services = NULL;
  if (tc_registration && tc_bus)
    g_dbus_connection_unregister_object(tc_bus, tc_registration);
  tc_registration = 0;
  g_clear_object(&tc_bus);
  g_clear_pointer(&tc_node, g_dbus_node_info_unref);
  g_clear_pointer(&tc_group_relay_url, g_free);
  g_application_release(g_application_get_default());
}

#endif /* GH_TEST_CONTROL_H */
