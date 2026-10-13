#include "approval-controller.h"
#include "nostr-kinds.h"
#include "nostrc-test-bus.h"
#include <string.h>

typedef struct {
  NostrcTestBus *bus;
  GDBusConnection *server, *client;
  GApplication *app;
  GrottoApprovalController *controller;
  GPtrArray *rows;
  GDBusMethodInvocation *delayed;
  GVariant *delayed_rows;
  guint registration[4], lists, infos, decisions, messages, notification_count, last_notification,
      notification_activated;
  gchar *notification_title, *notification_body;
  gchar **notification_actions;
  gboolean delay, delay_info, delayed_is_info, fail_decision, fail_list, reconciled_ok;
  guint status_version, reconciles;
  gchar *last_message;
} Fixture;
static const char xml[] =
    "<node><interface name='org.nostr.Grotto.Control1'>"
    "<method name='GetStatus'><arg type='a{sv}' direction='out'/></method>"
    "<method name='ListRequests'><arg type='aa{sv}' direction='out'/></method>"
    "<method name='ListClients'><arg type='aa{sv}' direction='out'/></method>"
    "</interface><interface name='org.nostr.Signer'>"
    "<method name='GetApprovalInfo'><arg type='s' direction='in'/><arg type='a{sv}' "
    "direction='out'/></method>"
    "<method name='ApproveRequest'><arg type='s' direction='in'/><arg type='b' "
    "direction='in'/><arg type='b' direction='in'/><arg type='t' direction='in'/><arg type='b' "
    "direction='out'/></method>"
    "</interface></node>";
static GVariant *request(const char *id, guint remaining) {
  GVariantBuilder row;
  g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
#define S(key, value) g_variant_builder_add(&row, "{sv}", key, g_variant_new_string(value))
  S("request_id", id);
  S("principal", "org.test.Client");
  S("identity", "npub-pinned");
  S("state", "PENDING");
  S("operation", "sign_event");
  S("transport", "DBUS");
  S("flow_id", "flow");
#undef S
  g_variant_builder_add(&row, "{sv}", "remaining_seconds", g_variant_new_uint32(remaining));
  g_variant_builder_add(&row, "{sv}", "event_kind", g_variant_new_uint32(NOSTR_KIND_TEXT_NOTE));
  g_variant_builder_add(&row, "{sv}", "rememberable", g_variant_new_boolean(TRUE));
  return g_variant_ref_sink(g_variant_builder_end(&row));
}
static const char *id_of(GVariant *row) {
  const char *id;
  g_variant_lookup(row, "request_id", "&s", &id);
  return id;
}
static GVariant *snapshot(Fixture *f) {
  GVariantBuilder rows;
  g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
  for (guint i = 0; i < f->rows->len; i++)
    g_variant_builder_add_value(&rows, f->rows->pdata[i]);
  return g_variant_ref_sink(g_variant_builder_end(&rows));
}
static void emit_completed(Fixture *f, const char *id) {
  g_dbus_connection_emit_signal(f->server, NULL, "/org/nostr/signer", "org.nostr.Signer",
                                "ApprovalCompleted", g_variant_new("(sb)", id, TRUE), NULL);
}
static void method(GDBusConnection *bus, const char *sender, const char *path,
                   const char *interface, const char *name, GVariant *args,
                   GDBusMethodInvocation *invocation, gpointer data) {
  (void)bus;
  (void)sender;
  (void)path;
  (void)interface;
  Fixture *f = data;
  if (!strcmp(name, "GetStatus")) {
    GVariantBuilder row;
    g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&row, "{sv}", "control_version", g_variant_new_uint32(f->status_version));
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{sv})", &row));
  } else if (!strcmp(name, "ListClients")) {
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(aa{sv})", NULL));
  } else if (!strcmp(name, "ListRequests")) {
    f->lists++;
    if (f->fail_list) {
      g_dbus_method_invocation_return_dbus_error(invocation, "org.nostr.Signer.Error.Failed",
                                                 "Snapshot failed");
    } else if (f->delay) {
      g_assert_null(f->delayed);
      f->delayed = g_object_ref(invocation);
      f->delayed_rows = snapshot(f);
    } else
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(@aa{sv})", snapshot(f)));
  } else {
    const char *id;
    g_variant_get_child(args, 0, "&s", &id);
    GVariant *row = NULL;
    guint index = 0;
    for (; index < f->rows->len; index++)
      if (!strcmp(id_of(f->rows->pdata[index]), id)) {
        row = f->rows->pdata[index];
        break;
      }
    if (!row) {
      g_dbus_method_invocation_return_dbus_error(invocation, "org.nostr.Signer.Error.NotFound",
                                                 "No request");
      return;
    }
    if (!strcmp(name, "GetApprovalInfo")) {
      f->infos++;
      if (f->delay_info) {
        g_assert_null(f->delayed);
        f->delayed = g_object_ref(invocation);
        f->delayed_rows = g_variant_ref(row);
        f->delayed_is_info = TRUE;
      } else
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(@a{sv})", g_variant_ref(row)));
    } else if (!strcmp(name, "ApproveRequest")) {
      f->decisions++;
      if (f->fail_decision)
        g_dbus_method_invocation_return_dbus_error(invocation, "org.nostr.Signer.Error.Paused",
                                                   "Signing paused");
      else {
        emit_completed(f, id);
        g_ptr_array_remove_index(f->rows, index);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE));
      }
    }
  }
}
static const GDBusInterfaceVTable vtable = {.method_call = method};
static gboolean failure_bound(gpointer data) {
  (void)data;
  g_error("Approval controller test did not complete");
  return G_SOURCE_REMOVE;
}
#define WAIT(condition)                                                                            \
  do {                                                                                             \
    guint bound = g_timeout_add_seconds(15, failure_bound, NULL);                                  \
    while (!(condition))                                                                           \
      g_main_context_iteration(NULL, TRUE);                                                        \
    g_source_remove(bound);                                                                        \
  } while (0)
static guint pending(Fixture *f) {
  return g_list_model_get_n_items(grotto_approval_controller_get_pending(f->controller));
}
static gboolean ready(Fixture *f) {
  if (!pending(f))
    return FALSE;
  g_autoptr(GrottoApprovalRequest) r =
      g_list_model_get_item(grotto_approval_controller_get_pending(f->controller), 0);
  return grotto_approval_request_get_state(r) == GROTTO_APPROVAL_READY;
}
static void name(Fixture *f, gboolean claim) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) r =
      g_dbus_connection_call_sync(f->server, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                  "org.freedesktop.DBus", claim ? "RequestName" : "ReleaseName",
                                  claim ? g_variant_new("(su)", "org.nostr.Signer", 0)
                                        : g_variant_new("(s)", "org.nostr.Signer"),
                                  NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(r);
}
static void seen_message(GrottoApprovalController *controller, const char *text, gpointer data) {
  (void)controller;
  Fixture *f = data;
  f->messages++;
  g_free(f->last_message);
  f->last_message = g_strdup(text);
}
static void reconciled(GrottoApprovalController *s, gboolean success, gpointer data) {
  (void)s;
  Fixture *f = data;
  f->reconciles++;
  f->reconciled_ok = success;
}
static void setup(Fixture *f, gconstpointer data) {
  (void)data;
  nostrc_test_bus_tolerate_ebadf();
  f->bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(f->bus);
  f->server = nostrc_test_bus_connect(f->bus);
  f->client = nostrc_test_bus_connect(f->bus);
  f->rows = g_ptr_array_new_with_free_func((GDestroyNotify)g_variant_unref);
  f->status_version = 1;
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(xml, NULL);
  for (guint i = 0; i < 2; i++)
    f->registration[i] = g_dbus_connection_register_object(
        f->server, i ? "/org/nostr/signer" : "/org/nostr/signer/grotto", node->interfaces[i],
        &vtable, f, NULL, NULL);
  f->app = g_application_new("org.test.ApprovalController", G_APPLICATION_NON_UNIQUE);
  name(f, TRUE);
  f->controller = grotto_approval_controller_new(f->app, f->client);
  g_signal_connect(f->controller, "message", G_CALLBACK(seen_message), f);
  g_signal_connect(f->controller, "reconciled", G_CALLBACK(reconciled), f);
  WAIT(f->lists > 0 && grotto_approval_controller_get_holds(f->controller) == 0);
}
static void delayed_reply(Fixture *f) {
  g_assert_nonnull(f->delayed);
  f->delay = FALSE;
  g_dbus_method_invocation_return_value(f->delayed,
                                        g_variant_new(f->delayed_is_info ? "(@a{sv})" : "(@aa{sv})",
                                                      g_steal_pointer(&f->delayed_rows)));
  g_clear_object(&f->delayed);
  f->delayed_is_info = FALSE;
}
static void teardown(Fixture *f, gconstpointer data) {
  (void)data;
  grotto_approval_controller_stop(f->controller);
  if (f->delayed)
    delayed_reply(f);
  WAIT(grotto_approval_controller_get_holds(f->controller) == 0);
  /* Keep application/backends alive until their private connection is down. */
  for (guint i = 0; i < 2; i++)
    g_dbus_connection_unregister_object(f->server, f->registration[i]);
  for (guint i = 2; i < 4; i++)
    if (f->registration[i])
      g_dbus_connection_unregister_object(f->server, f->registration[i]);
  g_free(f->last_message);
  g_free(f->notification_title);
  g_free(f->notification_body);
  g_strfreev(f->notification_actions);
  g_ptr_array_unref(f->rows);
  g_object_unref(f->app);
  nostrc_test_bus_down(f->bus);
  g_object_unref(f->controller);
}
static void arrive(Fixture *f, const char *id) {
  g_dbus_connection_emit_signal(
      f->server, NULL, "/org/nostr/signer", "org.nostr.Signer", "ApprovalRequested",
      g_variant_new("(sssss)", "org.test.Client", "npub-pinned", "event", "preview", id), NULL);
}
static void dedup(Fixture *f, gconstpointer data) {
  (void)data;
  g_ptr_array_add(f->rows, request("one", 120));
  arrive(f, "one");
  arrive(f, "one");
  WAIT(ready(f));
  grotto_approval_controller_reconcile(f->controller);
  WAIT(f->lists >= 2 && grotto_approval_controller_get_holds(f->controller) == 1);
  g_assert_cmpuint(pending(f), ==, 1);
  emit_completed(f, "one");
  WAIT(pending(f) == 0);
  g_assert_cmpuint(grotto_approval_controller_get_holds(f->controller), ==, 0);
}
static void completed_before_snapshot(Fixture *f, gconstpointer data) {
  (void)data;
  g_ptr_array_add(f->rows, request("old", 120));
  f->delay = TRUE;
  grotto_approval_controller_reconcile(f->controller);
  WAIT(f->delayed != NULL);
  emit_completed(f, "old");
  g_ptr_array_set_size(f->rows, 0);
  /* Barrier: a subsequent signal arrival must be handled after completion. */
  g_ptr_array_add(f->rows, request("barrier", 120));
  arrive(f, "barrier");
  WAIT(ready(f));
  delayed_reply(f);
  WAIT(grotto_approval_controller_get_holds(f->controller) == 1);
  g_assert_cmpuint(pending(f), ==, 1);
  g_autoptr(GrottoApprovalRequest) r =
      g_list_model_get_item(grotto_approval_controller_get_pending(f->controller), 0);
  g_assert_cmpstr(grotto_approval_request_get_id(r), ==, "barrier");
}
static void completed_before_info(Fixture *f, gconstpointer data) {
  (void)data;
  g_ptr_array_add(f->rows, request("late-info", 120));
  f->delay_info = TRUE;
  arrive(f, "late-info");
  WAIT(f->delayed != NULL);
  emit_completed(f, "late-info");
  WAIT(pending(f) == 0);
  f->delay_info = FALSE;
  delayed_reply(f);
  /* A new metadata round-trip is a barrier after the discarded late reply. */
  g_ptr_array_set_size(f->rows, 0);
  g_ptr_array_add(f->rows, request("barrier", 120));
  arrive(f, "barrier");
  WAIT(ready(f));
  g_assert_cmpuint(pending(f), ==, 1);
  g_assert_cmpuint(grotto_approval_controller_get_holds(f->controller), ==, 1);
}
static void expiry(Fixture *f, gconstpointer data) {
  (void)data;
  g_ptr_array_add(f->rows, request("expires", 1));
  arrive(f, "expires");
  WAIT(ready(f));
  WAIT(pending(f) == 0);
  g_assert_cmpuint(grotto_approval_controller_get_holds(f->controller), ==, 0);
  /* Even a still-stale daemon list cannot renew the local authoritative age. */
  guint lists = f->lists;
  grotto_approval_controller_reconcile(f->controller);
  WAIT(f->lists > lists && grotto_approval_controller_get_holds(f->controller) == 0);
  g_assert_cmpuint(pending(f), ==, 0);
}
static void owner_change(Fixture *f, gconstpointer data) {
  (void)data;
  g_ptr_array_add(f->rows, request("reuse", 120));
  arrive(f, "reuse");
  WAIT(ready(f));
  f->delay = TRUE;
  grotto_approval_controller_reconcile(f->controller);
  WAIT(f->delayed != NULL);
  name(f, FALSE);
  WAIT(pending(f) == 0);
  g_ptr_array_set_size(f->rows, 0);
  delayed_reply(f);
  WAIT(grotto_approval_controller_get_holds(f->controller) == 0);
  guint lists = f->lists;
  g_ptr_array_add(f->rows, request("reuse", 120));
  name(f, TRUE);
  WAIT(f->lists > lists && ready(f));
  g_assert_cmpuint(pending(f), ==, 1);
}
typedef struct {
  guint done;
  gboolean ok;
  GError *error;
} Reply;
static void decided(GObject *source, GAsyncResult *result, gpointer data) {
  Reply *r = data;
  r->ok = grotto_approval_controller_decide_finish(GROTTO_APPROVAL_CONTROLLER(source), result,
                                                   &r->error);
  r->done++;
}
static void decision_rechecks(Fixture *f, gconstpointer data) {
  (void)data;
  g_ptr_array_add(f->rows, request("decision", 120));
  arrive(f, "decision");
  WAIT(ready(f));
  guint before = f->infos;
  Reply r = {0};
  grotto_approval_controller_decide_async(f->controller, "decision", TRUE, TRUE, 600, NULL, decided,
                                          &r);
  Reply second = {0};
  grotto_approval_controller_decide_async(f->controller, "decision", TRUE, FALSE, 0, NULL, decided,
                                          &second);
  WAIT(r.done && second.done);
  g_assert_true(r.ok);
  g_assert_no_error(r.error);
  g_assert_false(second.ok);
  g_clear_error(&second.error);
  g_assert_cmpuint(f->infos, >, before);
  g_assert_cmpuint(f->decisions, ==, 1);
  g_assert_cmpuint(pending(f), ==, 0);
}
static void failed_decision_and_snapshot_expires(Fixture *f, gconstpointer data) {
  (void)data;
  g_ptr_array_add(f->rows, request("double-failure", 1));
  arrive(f, "double-failure");
  WAIT(ready(f));
  f->fail_decision = f->fail_list = TRUE;
  guint lists = f->lists;
  Reply r = {0};
  gint64 before = g_get_monotonic_time();
  grotto_approval_controller_decide_async(f->controller, "double-failure", TRUE, FALSE, 0, NULL,
                                          decided, &r);
  WAIT(r.done && f->lists > lists && grotto_approval_controller_get_holds(f->controller) == 1);
  g_assert_false(r.ok);
  g_clear_error(&r.error);
  g_assert_false(ready(f));
  /* No completion signal, owner change, or successful recovery snapshot. */
  WAIT(pending(f) == 0 && grotto_approval_controller_get_holds(f->controller) == 0);
  g_assert_cmpint(g_get_monotonic_time() - before, <, 2 * G_USEC_PER_SEC);
  g_assert_cmpuint(f->decisions, ==, 1);
}
static void stale_review_terminal(Fixture *f, gconstpointer data) {
  (void)data;
  f->fail_list = TRUE;
  guint before = f->reconciles;
  grotto_approval_controller_review(f->controller, "exact-stale-id");
  WAIT(f->reconciles > before);
  g_assert_false(f->reconciled_ok);
  g_assert_cmpstr(f->last_message, ==, "That request is no longer waiting.");
  name(f, FALSE);
  WAIT(grotto_approval_controller_get_holds(f->controller) == 0);
  before = f->messages;
  grotto_approval_controller_review(f->controller, "offline-id");
  WAIT(f->messages > before);
  g_assert_cmpstr(f->last_message, ==, "That request is no longer waiting.");
  f->status_version = 99;
  name(f, TRUE);
  WAIT(f->messages > before + 1);
  before = f->messages;
  grotto_approval_controller_review(f->controller, "incompatible-id");
  WAIT(f->messages > before);
  g_assert_cmpstr(f->last_message, ==, "That request is no longer waiting.");
  g_assert_cmpuint(f->decisions, ==, 0);
}
static void missing_before_decision(Fixture *f, gconstpointer data) {
  (void)data;
  g_ptr_array_add(f->rows, request("stale", 120));
  arrive(f, "stale");
  WAIT(ready(f));
  g_ptr_array_set_size(f->rows, 0);
  Reply r = {0};
  grotto_approval_controller_decide_async(f->controller, "stale", TRUE, FALSE, 0, NULL, decided,
                                          &r);
  WAIT(r.done && pending(f) == 0);
  g_assert_false(r.ok);
  g_clear_error(&r.error);
  g_assert_cmpuint(f->decisions, ==, 0);
}
static void changed_identity(Fixture *f, gconstpointer data) {
  (void)data;
  g_ptr_array_add(f->rows, request("changed", 120));
  arrive(f, "changed");
  WAIT(ready(f));
  GVariantDict dict;
  g_variant_dict_init(&dict, f->rows->pdata[0]);
  g_variant_dict_insert(&dict, "identity", "s", "npub-other");
  GVariant *changed = g_variant_ref_sink(g_variant_dict_end(&dict));
  g_ptr_array_set_size(f->rows, 0);
  g_ptr_array_add(f->rows, changed);
  Reply r = {0};
  grotto_approval_controller_decide_async(f->controller, "changed", TRUE, FALSE, 0, NULL, decided,
                                          &r);
  WAIT(r.done);
  g_assert_false(r.ok);
  g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&r.error);
  g_assert_cmpuint(f->decisions, ==, 0);
  g_assert_cmpuint(pending(f), ==, 0);
}
static void missing_preview_blocks_approve(Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GVariant) base = request("auth", 120);
  GVariantDict dict;
  g_variant_dict_init(&dict, base);
  g_variant_dict_insert(&dict, "event_kind", "u", NOSTR_KIND_CLIENT_AUTHENTICATION);
  g_ptr_array_add(f->rows, g_variant_ref_sink(g_variant_dict_end(&dict)));
  arrive(f, "auth");
  WAIT(ready(f));
  Reply r = {0};
  grotto_approval_controller_decide_async(f->controller, "auth", TRUE, FALSE, 0, NULL, decided, &r);
  WAIT(r.done);
  g_assert_false(r.ok);
  g_clear_error(&r.error);
  g_assert_cmpuint(f->decisions, ==, 0);
  g_assert_true(ready(f));
  memset(&r, 0, sizeof r);
  grotto_approval_controller_decide_async(f->controller, "auth", FALSE, FALSE, 0, NULL, decided,
                                          &r);
  WAIT(r.done);
  g_assert_true(r.ok);
  g_assert_no_error(r.error);
  g_assert_cmpuint(f->decisions, ==, 1);
}
#ifndef __APPLE__
static void desktop_method(GDBusConnection *bus, const char *sender, const char *path,
                           const char *interface, const char *name, GVariant *args,
                           GDBusMethodInvocation *invocation, gpointer data) {
  (void)bus;
  (void)sender;
  (void)path;
  (void)interface;
  Fixture *f = data;
  if (!strcmp(name, "GetActive"))
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
  else if (!strcmp(name, "CloseNotification"))
    g_dbus_method_invocation_return_value(invocation, NULL);
  else if (!strcmp(name, "Notify")) {
    const char *title, *body;
    g_variant_get_child(args, 3, "&s", &title);
    g_variant_get_child(args, 4, "&s", &body);
    g_free(f->notification_title);
    g_free(f->notification_body);
    g_strfreev(f->notification_actions);
    f->notification_title = g_strdup(title);
    f->notification_body = g_strdup(body);
    g_autoptr(GVariant) actions = g_variant_get_child_value(args, 5);
    f->notification_actions = g_variant_dup_strv(actions, NULL);
    guint32 replace = 0;
    g_variant_get_child(args, 1, "u", &replace);
    f->notification_count++;
    f->last_notification = replace ? replace : f->notification_count;
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", f->last_notification));
  } else if (!strcmp(name, "GetCapabilities")) {
    const char *capabilities[] = {"actions", "body", NULL};
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(^as)", capabilities));
  }
}
static void notification_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
  (void)action;
  g_assert_cmpstr(g_variant_get_string(parameter, NULL), ==, "");
  ((Fixture *)data)->notification_activated++;
}
static void notifications(Fixture *f, gconstpointer data) {
  (void)data;
  static const char desktop_xml[] =
      "<node><interface name='org.freedesktop.Notifications'>"
      "<method name='Notify'><arg type='s' direction='in'/><arg type='u' direction='in'/><arg "
      "type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/><arg "
      "type='as' direction='in'/><arg type='a{sv}' direction='in'/><arg type='i' "
      "direction='in'/><arg type='u' direction='out'/></method>"
      "<method name='CloseNotification'><arg type='u' direction='in'/></method>"
      "<method name='GetCapabilities'><arg type='as' direction='out'/></method>"
      "</interface><interface name='org.gnome.ScreenSaver'><method name='GetActive'><arg type='b' "
      "direction='out'/></method></interface></node>";
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(desktop_xml, NULL);
  const char *paths[] = {"/org/freedesktop/Notifications", "/org/gnome/ScreenSaver"};
  const char *names[] = {"org.freedesktop.Notifications", "org.gnome.ScreenSaver"};
  static const GDBusInterfaceVTable desktop_vtable = {.method_call = desktop_method};
  for (guint i = 0; i < 2; i++) {
    f->registration[i + 2] = g_dbus_connection_register_object(
        f->server, paths[i], node->interfaces[i], &desktop_vtable, f, NULL, NULL);
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
        f->server, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "RequestName", g_variant_new("(su)", names[i], 0), NULL, 0, 5000, NULL, NULL);
    g_assert_nonnull(reply);
  }
  g_autoptr(GError) error = NULL;
  g_autoptr(GSimpleAction) action = g_simple_action_new("review-request", G_VARIANT_TYPE_STRING);
  g_signal_connect(action, "activate", G_CALLBACK(notification_action), f);
  g_action_map_add_action(G_ACTION_MAP(f->app), G_ACTION(action));
  g_assert_true(g_application_register(f->app, NULL, &error));
  g_assert_no_error(error);
  WAIT(grotto_approval_controller_notifications_available(f->controller));
  g_autoptr(GVariant) base = request("notification-one", 120);
  GVariantDict dict;
  g_variant_dict_init(&dict, base);
  g_variant_dict_insert(&dict, "claimed_app_id", "s", "attacker supplied name");
  g_variant_dict_insert(&dict, "reason", "s", "private reason");
  if (data) {
    g_variant_dict_insert(&dict, "verified", "b", TRUE);
    g_variant_dict_insert(&dict, "principal", "s", (const char *)data);
    g_variant_dict_insert(&dict, "transport", "s",
                          g_str_has_prefix(data, "nip46:") ? "NIP46" : "DBUS");
    g_variant_dict_insert(&dict, "app_id", "s", "");
  }
  g_ptr_array_add(f->rows, g_variant_ref_sink(g_variant_dict_end(&dict)));
  arrive(f, "notification-one");
  WAIT(f->notification_body && strstr(f->notification_body, "Text note"));
  g_assert_cmpstr(f->notification_title, ==, "Unverified app");
  gboolean review = FALSE, deny = FALSE;
  for (guint i = 1; f->notification_actions[i - 1] && f->notification_actions[i]; i += 2) {
    review |= !strcmp(f->notification_actions[i], "Review");
    deny |= !strcmp(f->notification_actions[i], "Deny once");
    g_assert_null(strstr(f->notification_actions[i], "Approve"));
  }
  g_assert_true(review);
  g_assert_true(deny);
  g_assert_null(strstr(f->notification_body, "private"));
  if (data) {
    g_assert_null(strstr(f->notification_title, data));
    g_assert_null(strstr(f->notification_body, data));
  }
  g_ptr_array_add(f->rows, request("notification-two", 120));
  arrive(f, "notification-two");
  WAIT(f->notification_body && g_str_has_prefix(f->notification_body, "2 requests"));
  for (guint i = 1; f->notification_actions[i - 1] && f->notification_actions[i]; i += 2)
    g_assert_null(strstr(f->notification_actions[i], "Deny"));
  g_dbus_connection_emit_signal(f->server, NULL, "/org/gnome/ScreenSaver", "org.gnome.ScreenSaver",
                                "ActiveChanged", g_variant_new("(b)", TRUE), NULL);
  WAIT(g_strcmp0(f->notification_body, "Grotto needs your approval") == 0);
  g_assert_cmpstr(f->notification_title, ==, "Grotto");
  /* Action delivery is an ACK barrier for Gio's asynchronous notification
   * backend, not a sleep. Review removes the summary, not either request. */
  g_dbus_connection_emit_signal(f->server, NULL, "/org/freedesktop/Notifications",
                                "org.freedesktop.Notifications", "ActionInvoked",
                                g_variant_new("(us)", f->last_notification, "default"), NULL);
  WAIT(f->notification_activated > 0);
  g_assert_cmpuint(pending(f), ==, 2);
}
#endif
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
#define ADD(name, fn)                                                                              \
  g_test_add("/signer/approval-controller/" name, Fixture, NULL, setup, fn, teardown)
  ADD("dedup-holds", dedup);
  ADD("completed-before-snapshot", completed_before_snapshot);
  ADD("expiry", expiry);
  ADD("owner-change", owner_change);
  ADD("decision-rechecks", decision_rechecks);
  ADD("failed-decision-and-snapshot-expires", failed_decision_and_snapshot_expires);
  ADD("stale-review-terminal", stale_review_terminal);
  ADD("missing-before-decision", missing_before_decision);
  ADD("completed-before-info", completed_before_info);
  ADD("changed-identity", changed_identity);
  ADD("missing-preview-blocks-approve", missing_preview_blocks_approve);
#undef ADD
#ifndef __APPLE__
  g_test_add("/signer/approval-controller/notifications", Fixture, NULL, setup, notifications,
             teardown);
  g_test_add("/signer/approval-controller/notifications-nameless-executable", Fixture,
             "exe:/home/alice/private-directory/client", setup, notifications, teardown);
  g_test_add("/signer/approval-controller/notifications-nameless-nip46", Fixture,
             "nip46:79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", setup,
             notifications, teardown);
#endif
  return g_test_run();
}
