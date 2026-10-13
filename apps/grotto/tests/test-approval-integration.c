#include "approval-controller.h"
#include "nostr-event.h"
#include "nostr-json.h"
#include "nostr-kinds.h"
#include "nostrc-test-bus.h"
#include "nostrc-test-gdk-frame.h"
#include <glib/gstdio.h>
#include <string.h>
#include <sys/resource.h>
#ifndef __APPLE__
#include "main_app.h"
#include "ui/page-permissions.h"
#include "ui/signer-window.h"
#include <signal.h>
#endif

typedef struct {
  guint done;
  GVariant *reply;
  GError *error;
} Reply;
static NostrcTestBus *active_bus;
static gchar *daemon_owner;
static gboolean bound(gpointer data) {
  if (active_bus) {
    nostrc_test_bus_dump_log(active_bus, "g07-daemon.log");
    nostrc_test_bus_dump_log(active_bus, "g07-gui.log");
  }
  g_error("Real-daemon approval did not complete (test line %d)", GPOINTER_TO_INT(data));
  return G_SOURCE_REMOVE;
}
static guint trust_changes, message_count;
static void controller_message(GrottoApprovalController *s, const char *text, gpointer data) {
  (void)s;
  (void)data;
  if (g_str_has_prefix(text, "Trust level changed."))
    trust_changes++;
  message_count++;
  g_printerr("Controller: %s\n", text);
}
#define WAIT(condition)                                                                            \
  do {                                                                                             \
    guint id = g_timeout_add_seconds(20, bound, GINT_TO_POINTER(__LINE__));                        \
    while (!(condition))                                                                           \
      g_main_context_iteration(NULL, TRUE);                                                        \
    g_source_remove(id);                                                                           \
  } while (0)
static void bus_done(GObject *source, GAsyncResult *result, gpointer data) {
  Reply *r = data;
  r->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &r->error);
  r->done++;
}
static void decision_done(GObject *source, GAsyncResult *result, gpointer data) {
  Reply *r = data;
  if (grotto_approval_controller_decide_finish(GROTTO_APPROVAL_CONTROLLER(source), result,
                                               &r->error))
    r->reply = g_variant_ref_sink(g_variant_new("(b)", TRUE));
  r->done++;
}
static void clear_reply(Reply *r) {
  g_clear_pointer(&r->reply, g_variant_unref);
  g_clear_error(&r->error);
  memset(r, 0, sizeof *r);
}
static void sign_tags(GDBusConnection *bus, guint32 kind, const char *tags, const char *content,
                      Reply *r) {
  g_autofree gchar *json = g_strdup_printf(
      "{\"kind\":%u,\"created_at\":1,\"tags\":%s,\"content\":\"%s\"}", kind, tags, content);
  g_dbus_connection_call(bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer",
                         "SignEvent", g_variant_new("(sss)", json, "", "spoofed-name"),
                         G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 30000, NULL, bus_done, r);
}
static void sign(GDBusConnection *bus, guint32 kind, const char *content, Reply *r) {
  sign_tags(bus, kind, "[]", content, r);
}
static gboolean ready(GrottoApprovalController *controller) {
  GListModel *pending = grotto_approval_controller_get_pending(controller);
  if (!g_list_model_get_n_items(pending))
    return FALSE;
  g_autoptr(GrottoApprovalRequest) r = g_list_model_get_item(pending, 0);
  return grotto_approval_request_get_state(r) == GROTTO_APPROVAL_READY;
}
static void owner_seen(GDBusConnection *bus, const char *name, const char *owner, gpointer data) {
  (void)bus;
  (void)name;
  if (!daemon_owner)
    daemon_owner = g_strdup(owner);
  *(gboolean *)data = TRUE;
}
static void verify(Reply *r, guint32 kind) {
  g_assert_no_error(r->error);
  g_assert_nonnull(r->reply);
  const char *json;
  g_variant_get(r->reply, "(&s)", &json);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize(event, json), ==, 0);
  g_assert_true(nostr_event_check_signature(event));
  g_assert_cmpint(nostr_event_get_kind(event), ==, (int)kind);
  nostr_event_free(event);
}
#ifndef __APPLE__
static gboolean count_frame(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)widget;
  (void)clock;
  return ++*(guint *)data < 2 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}
static GtkWidget *find_button(GtkWidget *root, const char *label) {
  if (GTK_IS_BUTTON(root) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), label) == 0)
    return root;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *button = find_button(child, label);
    if (button)
      return button;
  }
  return NULL;
}
static GtkWindow *management(GApplication *app) {
  for (GList *w = gtk_application_get_windows(GTK_APPLICATION(app)); w; w = w->next)
    if (SIGNER_IS_WINDOW(w->data))
      return w->data;
  return NULL;
}
static AdwDialog *review_dialog(GApplication *app) {
  for (GList *w = gtk_application_get_windows(GTK_APPLICATION(app)); w; w = w->next) {
    AdwDialog *dialog = ADW_IS_WINDOW(w->data) ? adw_window_get_visible_dialog(w->data) : NULL;
    if (dialog && GNOSTR_IS_APPROVAL_DIALOG(dialog))
      return dialog;
  }
  return NULL;
}
static GtkWidget *dialog_child(AdwDialog *dialog, const char *name) {
  return GTK_WIDGET(
      gtk_widget_get_template_child(GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, name));
}
static void wait_frames(GtkWidget *widget) {
  WAIT(gtk_widget_get_mapped(widget));
  guint ticks = 0;
  gtk_widget_add_tick_callback(widget, count_frame, &ticks, NULL);
  WAIT(ticks >= 2);
}
static void bus_name(GDBusConnection *bus, const char *name, gboolean claim) {
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
      bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      claim ? "RequestName" : "ReleaseName",
      claim ? g_variant_new("(su)", name, 0) : g_variant_new("(s)", name), NULL, 0, 5000, NULL,
      NULL);
  g_assert_nonnull(reply);
}
typedef struct {
  guint notifications, version, lists, decisions, infos, model_changes;
  gboolean fail_list, valid_request;
} Desktop;
static GVariant *other_request(void) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  const char *keys[] = {"request_id", "principal", "identity", "state", "operation", "transport"};
  const char *values[] = {"other-request", "org.test.Other", "npub-other",
                          "PENDING",       "get_public_key", "DBUS"};
  for (guint i = 0; i < G_N_ELEMENTS(keys); i++)
    g_variant_builder_add(&b, "{sv}", keys[i], g_variant_new_string(values[i]));
  g_variant_builder_add(&b, "{sv}", "remaining_seconds", g_variant_new_uint32(120));
  return g_variant_builder_end(&b);
}
static void desktop_call(GDBusConnection *bus, const char *sender, const char *path,
                         const char *iface, const char *name, GVariant *args,
                         GDBusMethodInvocation *inv, gpointer data) {
  (void)bus;
  (void)sender;
  (void)path;
  (void)iface;
  Desktop *d = data;
  if (!strcmp(name, "Notify")) {
    guint replace = 0;
    g_variant_get_child(args, 1, "u", &replace);
    d->notifications++;
    g_dbus_method_invocation_return_value(
        inv, g_variant_new("(u)", replace ? replace : d->notifications));
  } else if (!strcmp(name, "GetCapabilities")) {
    const char *caps[] = {"actions", "body", NULL};
    g_dbus_method_invocation_return_value(inv, g_variant_new("(^as)", caps));
  } else if (!strcmp(name, "GetStatus")) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "control_version", g_variant_new_uint32(d->version));
    g_dbus_method_invocation_return_value(inv, g_variant_new("(a{sv})", &b));
  } else if (!strcmp(name, "ListRequests")) {
    d->lists++;
    if (d->fail_list)
      g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Signer.Error.Failed",
                                                 "Snapshot unavailable");
    else {
      GVariantBuilder rows;
      g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
      if (d->valid_request)
        g_variant_builder_add_value(&rows, other_request());
      g_dbus_method_invocation_return_value(inv, g_variant_new("(aa{sv})", &rows));
    }
  } else if (!strcmp(name, "ListClients"))
    g_dbus_method_invocation_return_value(inv, g_variant_new("(aa{sv})", NULL));
  else if (!strcmp(name, "GetApprovalInfo")) {
    d->infos++;
    g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Signer.Error.NotFound",
                                               "No such request");
  } else if (!strcmp(name, "ApproveRequest")) {
    d->decisions++;
    g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", FALSE));
  } else
    g_dbus_method_invocation_return_value(inv, NULL);
}
static const char desktop_xml[] =
    "<node><interface name='org.freedesktop.Notifications'>"
    "<method name='Notify'><arg type='s' direction='in'/><arg type='u' direction='in'/><arg "
    "type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='as' direction='in'/>"
    "<arg type='a{sv}' direction='in'/><arg type='i' direction='in'/><arg type='u' "
    "direction='out'/></method>"
    "<method name='CloseNotification'><arg type='u' direction='in'/></method>"
    "<method name='GetCapabilities'><arg type='as' direction='out'/></method></interface>"
    "<interface name='org.nostr.Grotto.Control1'><method name='GetStatus'><arg type='a{sv}' "
    "direction='out'/></method>"
    "<method name='ListRequests'><arg type='aa{sv}' direction='out'/></method>"
    "<method name='ListClients'><arg type='aa{sv}' direction='out'/></method></interface>"
    "<interface name='org.nostr.Signer'><method name='GetApprovalInfo'><arg type='s' "
    "direction='in'/>"
    "<arg type='a{sv}' direction='out'/></method><method name='ApproveRequest'><arg type='s' "
    "direction='in'/>"
    "<arg type='b' direction='in'/><arg type='b' direction='in'/><arg type='t' direction='in'/>"
    "<arg type='b' direction='out'/></method></interface></node>";
static void model_changed(GListModel *m, guint p, guint r, guint a, gpointer data) {
  (void)m;
  (void)p;
  (void)r;
  (void)a;
  ((Desktop *)data)->model_changes++;
}
static void pending_reachability(GrottoApprovalController *controller, GApplication *app,
                                 GDBusConnection *bus, const char *id) {
  g_action_group_activate_action(G_ACTION_GROUP(app), "review-request", g_variant_new_string(""));
  GtkWindow *win = management(app);
  g_assert_nonnull(win);
  GtkWidget *group = g_object_get_data(G_OBJECT(win), "grotto-pending-group");
  g_assert_nonnull(group);
  wait_frames(group);
  g_assert_nonnull(strstr(adw_preferences_group_get_description(ADW_PREFERENCES_GROUP(group)),
                          "Notifications unavailable"));
  GtkWidget *page = GTK_WIDGET(
      gtk_widget_get_template_child(GTK_WIDGET(win), TYPE_SIGNER_WINDOW, "page_permissions"));
  GtkWidget *grants =
      GTK_WIDGET(gtk_widget_get_template_child(page, TYPE_PAGE_PERMISSIONS, "group_grants"));
  graphene_rect_t pending_bounds, grants_bounds;
  g_assert_true(gtk_widget_compute_bounds(group, page, &pending_bounds));
  g_assert_true(gtk_widget_compute_bounds(grants, page, &grants_bounds));
  g_assert_cmpfloat(pending_bounds.origin.y, <, grants_bounds.origin.y);
  g_assert_null(review_dialog(app)); /* Empty target opens the inbox, not another request. */
  g_action_group_activate_action(G_ACTION_GROUP(app), "review-request", g_variant_new_string(id));
  AdwDialog *dialog = review_dialog(app);
  g_assert_nonnull(dialog);
  wait_frames(GTK_WIDGET(dialog));
  g_signal_emit_by_name(dialog_child(dialog, "btn_not_now"), "clicked");
  WAIT(review_dialog(app) == NULL);
  g_assert_true(ready(controller));
  g_assert_cmpuint(g_list_model_get_n_items(grotto_approval_controller_get_pending(controller)), ==,
                   1);
  signer_window_show_page(SIGNER_WINDOW(win), "settings");
  g_application_activate(app); /* Ordinary launcher/reopen also reaches the deferred request. */
  WAIT(grotto_approval_controller_get_holds(controller) == 1);
  AdwViewStack *reopened_stack =
      ADW_VIEW_STACK(gtk_widget_get_template_child(GTK_WIDGET(win), TYPE_SIGNER_WINDOW, "stack"));
  g_assert_cmpstr(adw_view_stack_get_visible_child_name(reopened_stack), ==, "permissions");
  Desktop d = {0};
  g_autoptr(GDBusNodeInfo) xml = g_dbus_node_info_new_for_xml(desktop_xml, NULL);
  static const GDBusInterfaceVTable table = {.method_call = desktop_call};
  guint registration = g_dbus_connection_register_object(
      bus, "/org/freedesktop/Notifications", xml->interfaces[0], &table, &d, NULL, NULL);
  g_signal_connect(grotto_approval_controller_get_pending(controller), "items-changed",
                   G_CALLBACK(model_changed), &d);
  bus_name(bus, "org.freedesktop.Notifications", TRUE);
  WAIT(grotto_approval_controller_notifications_available(controller));
  g_assert_cmpstr(adw_preferences_group_get_description(ADW_PREFERENCES_GROUP(group)), ==, "");
  bus_name(bus, "org.freedesktop.Notifications", FALSE);
  WAIT(!grotto_approval_controller_notifications_available(controller));
  g_assert_nonnull(strstr(adw_preferences_group_get_description(ADW_PREFERENCES_GROUP(group)),
                          "Notifications unavailable"));
  bus_name(bus, "org.freedesktop.Notifications", TRUE);
  WAIT(grotto_approval_controller_notifications_available(controller));
  g_assert_cmpstr(adw_preferences_group_get_description(ADW_PREFERENCES_GROUP(group)), ==, "");
  g_assert_cmpuint(d.model_changes, ==, 0); /* Availability alone updates the banner. */
  g_assert_cmpuint(d.notifications, ==, 0); /* Deferred request is not re-notified. */
  g_signal_handlers_disconnect_by_data(grotto_approval_controller_get_pending(controller), &d);
  bus_name(bus, "org.freedesktop.Notifications", FALSE);
  WAIT(!grotto_approval_controller_notifications_available(controller));
  g_dbus_connection_unregister_object(bus, registration);
  signer_window_show_page(SIGNER_WINDOW(win), "settings");
  g_action_group_activate_action(G_ACTION_GROUP(app), "review-request", g_variant_new_string(""));
  AdwViewStack *stack =
      ADW_VIEW_STACK(gtk_widget_get_template_child(GTK_WIDGET(win), TYPE_SIGNER_WINDOW, "stack"));
  g_assert_cmpstr(adw_view_stack_get_visible_child_name(stack), ==, "permissions");
  GPtrArray *rows = g_object_get_data(G_OBJECT(group), "rows");
  g_assert_cmpuint(rows->len, ==, 1);
  g_assert_cmpstr(g_object_get_data(rows->pdata[0], "request-id"), ==, id);
  g_signal_emit_by_name(rows->pdata[0], "activated");
  dialog = review_dialog(app);
  g_assert_nonnull(dialog);
  wait_frames(GTK_WIDGET(dialog));
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(dialog_child(dialog, "content_preview"))), ==,
                  "first");
}
static GDBusConnection *consumer_connection(void) {
  GDBusConnection *bus =
      g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"),
                                             G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                 G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                             NULL, NULL, NULL);
  g_assert_nonnull(bus);
  return bus;
}
static void live_navigation(GrottoApprovalController *controller, GApplication *app) {
  GListModel *model = grotto_approval_controller_get_pending(controller);
  g_autoptr(GPtrArray) callers = g_ptr_array_new_with_free_func(g_object_unref);
  Reply results[3] = {{0}};
  gchar *ids[3] = {NULL};
  for (guint i = 0; i < 3; i++) {
    GDBusConnection *caller = consumer_connection();
    g_ptr_array_add(callers, caller);
    g_autofree gchar *json =
        g_strdup_printf("{\"kind\":%u,\"tags\":[[\"relay\",\"wss://"
                        "nos.lol\"],[\"challenge\",\"step-%u\"]],\"content\":\"\"}",
                        NOSTR_KIND_CLIENT_AUTHENTICATION, i);
    GVariantBuilder context;
    g_variant_builder_init(&context, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&context, "{sv}", "flow_id", g_variant_new_string("live-stepper"));
    g_dbus_connection_call(
        caller, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer", "CallWithContext",
        g_variant_new("(sv@a{sv})", "SignEvent", g_variant_new("(sss)", json, "", ""),
                      g_variant_builder_end(&context)),
        G_VARIANT_TYPE("(v)"), 0, 30000, NULL, bus_done, &results[i]);
    WAIT(g_list_model_get_n_items(model) == i + 1);
    g_autoptr(GrottoApprovalRequest) request = g_list_model_get_item(model, i);
    WAIT(grotto_approval_request_get_state(request) == GROTTO_APPROVAL_READY);
    ids[i] = g_strdup(grotto_approval_request_get_id(request));
    if (i == 1) {
      g_action_group_activate_action(G_ACTION_GROUP(app), "review-request",
                                     g_variant_new_string(ids[1]));
      AdwDialog *dialog = review_dialog(app);
      g_assert_nonnull(dialog);
      wait_frames(GTK_WIDGET(dialog));
      g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(dialog_child(dialog, "label_position"))), ==,
                      "2 of 2");
      g_assert_false(gtk_widget_get_sensitive(dialog_child(dialog, "btn_next")));
      GtkWidget *first = gtk_widget_get_first_child(dialog_child(dialog, "batch_box"));
      g_assert_nonnull(first);
      gtk_check_button_set_active(GTK_CHECK_BUTTON(first), TRUE);
    }
  }
  AdwDialog *dialog = review_dialog(app);
  g_assert_nonnull(dialog);
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(dialog_child(dialog, "label_position"))), ==,
                  "2 of 3");
  g_assert_true(gtk_widget_get_sensitive(dialog_child(dialog, "btn_next")));
  GtkWidget *batch = dialog_child(dialog, "batch_box"), *first = gtk_widget_get_first_child(batch);
  g_assert_true(gtk_check_button_get_active(GTK_CHECK_BUTTON(first)));
  g_assert_null(gtk_widget_get_next_sibling(
      gtk_widget_get_next_sibling(first))); /* Arrival was not added to the selection. */
  Reply decision = {0};
  grotto_approval_controller_decide_async(controller, ids[0], FALSE, FALSE, 0, NULL, decision_done,
                                          &decision);
  WAIT(decision.done && results[0].done && g_list_model_get_n_items(model) == 2);
  g_assert_no_error(decision.error);
  g_assert_nonnull(results[0].error);
  clear_reply(&decision);
  clear_reply(&results[0]);
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(dialog_child(dialog, "label_position"))), ==,
                  "1 of 2");
  g_assert_false(gtk_widget_get_sensitive(dialog_child(dialog, "btn_previous")));
  g_assert_true(gtk_widget_get_sensitive(dialog_child(dialog, "btn_next")));
  /* The only selected ID was externally completed. Never approve the unseen arrival. */
  g_signal_emit_by_name(dialog_child(dialog, "btn_approve_selected"), "clicked");
  WAIT(review_dialog(app) == NULL);
  g_assert_cmpuint(g_list_model_get_n_items(model), ==, 2);
  g_assert_cmpuint(results[1].done + results[2].done, ==, 0);
  for (guint i = 1; i < 3; i++) {
    grotto_approval_controller_decide_async(controller, ids[i], FALSE, FALSE, 0, NULL,
                                            decision_done, &decision);
    WAIT(decision.done && results[i].done);
    g_assert_no_error(decision.error);
    g_assert_nonnull(results[i].error);
    clear_reply(&decision);
    clear_reply(&results[i]);
  }
  WAIT(g_list_model_get_n_items(model) == 0);
  for (guint i = 0; i < 3; i++) {
    g_free(ids[i]);
    g_dbus_connection_close_sync(callers->pdata[i], NULL, NULL);
  }
}
static void availability_changed(GrottoApprovalController *s, gpointer data) {
  (void)s;
  ++*(guint *)data;
}
static gboolean settled(gpointer data) {
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}
static gboolean stale_notice(GtkWindow *win) {
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(win));
  return ADW_IS_ALERT_DIALOG(dialog) &&
         !g_strcmp0(adw_alert_dialog_get_body(ADW_ALERT_DIALOG(dialog)),
                    "That request is no longer waiting.");
}
static void stale_app_actions(GrottoApprovalController *controller, GApplication *app,
                              GDBusConnection *bus) {
  guint availability = 0;
  g_signal_connect(controller, "availability-changed", G_CALLBACK(availability_changed),
                   &availability);
  Reply stopped = {0};
  g_dbus_connection_call(bus, "org.test.GrottoCoordinator", "/org/test/GrottoCoordinator",
                         "org.test.GrottoCoordinator", "StopSigner", NULL, NULL, 0, 5000, NULL,
                         bus_done, &stopped);
  WAIT(stopped.done && availability > 0);
  g_assert_no_error(stopped.error);
  clear_reply(&stopped);
  Desktop d = {.version = 99, .fail_list = TRUE};
  g_autoptr(GDBusNodeInfo) xml = g_dbus_node_info_new_for_xml(desktop_xml, NULL);
  static const GDBusInterfaceVTable table = {.method_call = desktop_call};
  guint registration = g_dbus_connection_register_object(
      bus, "/org/nostr/signer/grotto", xml->interfaces[1], &table, &d, NULL, NULL);
  guint signer_registration = g_dbus_connection_register_object(
      bus, "/org/nostr/signer", xml->interfaces[2], &table, &d, NULL, NULL);
  for (guint mode = 0; mode < 3; mode++) {
    if (mode) {
      guint before = availability;
      d.version = mode == 1 ? 99 : 1;
      bus_name(bus, "org.nostr.Signer", TRUE);
      WAIT(availability > before && grotto_approval_controller_get_holds(controller) == 0);
    }
    for (guint deny = 0; deny < 2; deny++) {
      GtkWindow *win = management(app);
      g_assert_nonnull(win);
      signer_window_show_page(SIGNER_WINDOW(win), "settings");
      g_action_group_activate_action(G_ACTION_GROUP(app), deny ? "deny-request" : "review-request",
                                     g_variant_new_string("exact-stale-id"));
      WAIT(stale_notice(win));
      AdwViewStack *stack = ADW_VIEW_STACK(
          gtk_widget_get_template_child(GTK_WIDGET(win), TYPE_SIGNER_WINDOW, "stack"));
      g_assert_cmpstr(adw_view_stack_get_visible_child_name(stack), ==, "permissions");
      g_assert_true(gtk_widget_get_visible(GTK_WIDGET(win)));
      g_assert_null(review_dialog(app));
      while (adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(win))) {
        AdwDialog *alert = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(win));
        wait_frames(GTK_WIDGET(alert));
        adw_dialog_force_close(alert);
        WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(win)) != alert);
      }
    }
    if (mode) {
      guint before = availability;
      bus_name(bus, "org.nostr.Signer", FALSE);
      WAIT(availability > before && grotto_approval_controller_get_holds(controller) == 0);
    }
  }
  guint messages = message_count, before = availability;
  d.fail_list = FALSE;
  d.valid_request = TRUE;
  bus_name(bus, "org.nostr.Signer", TRUE);
  WAIT(availability > before && ready(controller) &&
       grotto_approval_controller_get_holds(controller) == 1);
  gboolean idle = FALSE;
  g_idle_add(settled, &idle); /* Earlier decision callbacks run before this idle barrier. */
  WAIT(idle);
  g_assert_null(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(management(app))));
  g_assert_cmpuint(message_count, ==,
                   messages); /* No queued action survived its terminal result. */
  g_assert_cmpuint(d.decisions, ==, 0);
  g_assert_cmpuint(d.infos, ==, 0);
  g_assert_null(review_dialog(app));
  bus_name(bus, "org.nostr.Signer", FALSE);
  WAIT(grotto_approval_controller_get_holds(controller) == 0);
  g_signal_handlers_disconnect_by_data(controller, &availability);
  g_dbus_connection_unregister_object(bus, registration);
  g_dbus_connection_unregister_object(bus, signer_registration);
}
static void click_review(GrottoApprovalController *controller, GApplication *app, const char *id,
                         gboolean allow, gboolean remember, const char *expected_preview) {
  (void)controller;
  g_action_group_activate_action(G_ACTION_GROUP(app), "review-request", g_variant_new_string(id));
  GList *windows = gtk_application_get_windows(GTK_APPLICATION(app));
  g_assert_nonnull(windows);
  g_autoptr(GtkWindow) host = g_object_ref(windows->data);
  AdwDialog *dialog = adw_window_get_visible_dialog(ADW_WINDOW(host));
  g_assert_nonnull(dialog);
  WAIT(gtk_widget_get_mapped(GTK_WIDGET(dialog)));
  guint frames = 0;
  gtk_widget_add_tick_callback(GTK_WIDGET(dialog), count_frame, &frames, NULL);
  WAIT(frames >= 2);
  GtkLabel *preview = GTK_LABEL(gtk_widget_get_template_child(
      GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "content_preview"));
  if (expected_preview)
    g_assert_nonnull(strstr(gtk_label_get_text(preview), expected_preview));
  g_assert_null(strstr(gtk_label_get_text(preview), "PRIVATE-CIPHERTEXT"));
  g_assert_null(strstr(gtk_label_get_text(preview), "PAIRING-TOKEN"));
  GtkCheckButton *check = GTK_CHECK_BUTTON(gtk_widget_get_template_child(
      GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "chk_remember"));
  gtk_check_button_set_active(check, remember);
  if (!allow) {
    GtkExpander *details = GTK_EXPANDER(gtk_widget_get_template_child(
        GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "details_expander"));
    gtk_expander_set_expanded(details, TRUE);
    GtkWidget *change = GTK_WIDGET(gtk_widget_get_template_child(
        GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "btn_change_trust"));
    WAIT(gtk_widget_get_sensitive(change) && gtk_widget_get_mapped(change));
    GtkDropDown *picker = GTK_DROP_DOWN(gtk_widget_get_template_child(
        GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "trust_dropdown"));
    gtk_drop_down_set_selected(picker, 1);
    guint before = trust_changes;
    g_signal_emit_by_name(change, "clicked");
    AdwDialog *alert = adw_window_get_visible_dialog(ADW_WINDOW(host));
    g_assert_true(ADW_IS_ALERT_DIALOG(alert));
    WAIT(gtk_widget_get_mapped(GTK_WIDGET(alert)));
    guint alert_frames = 0;
    gtk_widget_add_tick_callback(GTK_WIDGET(alert), count_frame, &alert_frames, NULL);
    WAIT(alert_frames >= 2);
    GtkWidget *confirm = find_button(GTK_WIDGET(alert), "Change trust level");
    g_assert_nonnull(confirm);
    g_signal_emit_by_name(confirm, "clicked");
    WAIT(trust_changes > before);
    AdwDialog *notice = adw_window_get_visible_dialog(ADW_WINDOW(host));
    g_test_message("Trust feedback on review host: %s",
                   notice ? G_OBJECT_TYPE_NAME(notice) : "none");
    if (ADW_IS_ALERT_DIALOG(notice)) {
      wait_frames(GTK_WIDGET(notice));
      GtkWidget *close = find_button(GTK_WIDGET(notice), "Close");
      g_assert_nonnull(close);
      g_signal_emit_by_name(close, "clicked");
      WAIT(adw_window_get_visible_dialog(ADW_WINDOW(host)) != notice);
    }
    g_test_message("Trust feedback dismissed; original review visible: %d",
                   review_dialog(app) == dialog);
    /* Tier mutation travels Control1; the required-confirmation contact-list
     * request remains pending, and Remember still travels ApproveRequest. */
    Reply clients = {0};
    g_dbus_connection_call(g_application_get_dbus_connection(app), "org.nostr.Signer",
                           "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "ListClients",
                           NULL, G_VARIANT_TYPE("(aa{sv})"), 0, 5000, NULL, bus_done, &clients);
    WAIT(clients.done);
    g_assert_no_error(clients.error);
    g_assert_nonnull(clients.reply);
    g_autoptr(GVariant) rows = g_variant_get_child_value(clients.reply, 0);
    gboolean medium = FALSE;
    for (gsize i = 0; i < g_variant_n_children(rows); i++) {
      g_autoptr(GVariant) row = g_variant_get_child_value(rows, i);
      const char *tier = NULL;
      if (g_variant_lookup(row, "trust-tier", "&s", &tier) && !strcmp(tier, "medium"))
        medium = TRUE;
    }
    g_assert_true(medium);
    g_assert_true(ready(controller));
    clear_reply(&clients);
  }
  GtkWidget *button = GTK_WIDGET(gtk_widget_get_template_child(
      GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, allow ? "btn_approve" : "btn_deny"));
  g_assert_true(gtk_widget_get_sensitive(button));
  g_signal_emit_by_name(button, "clicked");
}
#endif
static void run_flow(GDBusConnection *connection, GDBusConnection *consumer, GApplication *app,
                     gboolean gui) {
  GrottoApprovalController *installed =
      g_object_get_data(G_OBJECT(app), "grotto-approval-controller");
  g_autoptr(GrottoApprovalController) controller =
      installed ? g_object_ref(installed) : grotto_approval_controller_new(app, connection);
  g_signal_connect(controller, "message", G_CALLBACK(controller_message), NULL);
  g_print("daemon name acquired; waiting for first request\n");
  Reply result = {0}, decision = {0};
  sign(consumer, NOSTR_KIND_TEXT_NOTE, "first", &result);
  WAIT(ready(controller) || result.done);
  g_assert_no_error(result.error);
  g_assert_true(ready(controller));
  GListModel *pending = grotto_approval_controller_get_pending(controller);
  g_autoptr(GrottoApprovalRequest) request = g_list_model_get_item(pending, 0);
  GVariant *info = grotto_approval_request_get_info(request);
  const char *identity = NULL, *principal = NULL;
  g_assert_true(g_variant_lookup(info, "identity", "&s", &identity));
  g_assert_true(g_str_has_prefix(identity, "npub1"));
  g_assert_true(g_variant_lookup(info, "principal", "&s", &principal));
  g_assert_cmpstr(principal, !=, "spoofed-name");
#ifndef __APPLE__
  if (gui) {
    pending_reachability(controller, app, consumer, grotto_approval_request_get_id(request));
    click_review(controller, app, grotto_approval_request_get_id(request), TRUE, TRUE, "first");
    decision.done = 1;
  } else
#endif
    grotto_approval_controller_decide_async(controller, grotto_approval_request_get_id(request),
                                            TRUE, TRUE, 600, NULL, decision_done, &decision);
  WAIT(result.done && decision.done);
  verify(&result, NOSTR_KIND_TEXT_NOTE);
  g_assert_no_error(decision.error);
  WAIT(g_list_model_get_n_items(pending) == 0 &&
       grotto_approval_controller_get_holds(controller) == 0);
  clear_reply(&result);
  clear_reply(&decision);
  sign(consumer, NOSTR_KIND_TEXT_NOTE, "remembered", &result);
  WAIT(result.done);
  verify(&result, NOSTR_KIND_TEXT_NOTE);
  g_assert_cmpuint(g_list_model_get_n_items(pending), ==, 0);
  clear_reply(&result);
  const gchar *peer = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
  g_autofree gchar *target_tags =
      g_strdup_printf("[[\"e\",\"%s\"],[\"k\",\"%u\"]]", peer, NOSTR_KIND_TEXT_NOTE);
  g_autofree gchar *private_tags = g_strdup_printf("[[\"p\",\"%s\"]]", peer);
  struct {
    guint32 kind;
    const gchar *tags, *body, *preview, *key, *value;
  } cases[] = {{NOSTR_KIND_CLIENT_AUTHENTICATION,
                "[[\"relay\",\"wss://nos.lol\"],[\"challenge\",\"PAIRING-TOKEN\"]]", "",
                "wss://nos.lol", "auth_relay", "wss://nos.lol"},
               {NOSTR_KIND_DELETION, target_tags, "", "Delete 1 target(s)", NULL, NULL},
               {NOSTR_KIND_REPOST, target_tags, "", peer, "target_event_id", peer},
               {NOSTR_KIND_GENERIC_REPOST, target_tags, "", peer, "target_event_id", peer},
               {NOSTR_KIND_REACTION, target_tags, "♥", "Reaction: ♥", "reaction_value", "♥"},
               {NOSTR_KIND_ENCRYPTED_DIRECT_MESSAGE, private_tags, "PRIVATE-CIPHERTEXT",
                "Encrypted message", "recipient_peer", peer},
               {NOSTR_KIND_GIFT_WRAP, private_tags, "PRIVATE-CIPHERTEXT", "Encrypted message",
                "recipient_peer", peer}};
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autoptr(GDBusConnection) caller =
        g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"),
                                               G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                   G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                               NULL, NULL, NULL);
    g_assert_nonnull(caller);
    sign_tags(caller, cases[i].kind, cases[i].tags, cases[i].body, &result);
    WAIT(ready(controller) || result.done);
    g_assert_no_error(result.error);
    g_assert_true(ready(controller));
    g_clear_object(&request);
    request = g_list_model_get_item(pending, 0);
    g_autoptr(GVariant) summary = g_variant_lookup_value(grotto_approval_request_get_info(request),
                                                         "event_summary", G_VARIANT_TYPE_VARDICT);
    g_assert_nonnull(summary);
    if (cases[i].key) {
      const gchar *value = NULL;
      g_assert_true(g_variant_lookup(summary, cases[i].key, "&s", &value));
      g_assert_cmpstr(value, ==, cases[i].value);
    } else {
      guint count = 0;
      g_assert_true(g_variant_lookup(summary, "delete_target_count", "u", &count));
      g_assert_cmpuint(count, ==, 1);
    }
    g_autofree gchar *dump = g_variant_print(summary, FALSE);
    g_assert_null(strstr(dump, "PRIVATE-CIPHERTEXT"));
    g_assert_null(strstr(dump, "PAIRING-TOKEN"));
#ifndef __APPLE__
    if (gui) {
      click_review(controller, app, grotto_approval_request_get_id(request), TRUE, FALSE,
                   cases[i].preview);
      decision.done = 1;
    } else
#endif
      grotto_approval_controller_decide_async(controller, grotto_approval_request_get_id(request),
                                              TRUE, FALSE, 0, NULL, decision_done, &decision);
    WAIT(result.done && decision.done);
    verify(&result, cases[i].kind);
    g_assert_no_error(decision.error);
    WAIT(g_list_model_get_n_items(pending) == 0 &&
         grotto_approval_controller_get_holds(controller) == 0);
    clear_reply(&result);
    clear_reply(&decision);
    g_dbus_connection_close_sync(caller, NULL, NULL);
  }
  /* A new connection avoids the deliberate per-connection prompt-rate limit;
   * its verified principal is unchanged (no sleeps or policy bypass). */
  g_autoptr(GDBusConnection) extra = NULL;
  if (active_bus)
    consumer = nostrc_test_bus_connect(active_bus);
  else {
    extra =
        g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"),
                                               G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                   G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                               NULL, NULL, NULL);
    g_assert_nonnull(extra);
    consumer = extra;
  }
  sign(consumer, NOSTR_KIND_FOLLOW_LIST, "different scope", &result);
  WAIT(ready(controller) || result.done);
  g_assert_no_error(result.error);
  g_assert_true(ready(controller));
  g_clear_object(&request);
  request = g_list_model_get_item(pending, 0);
#ifndef __APPLE__
  if (gui) {
    click_review(controller, app, grotto_approval_request_get_id(request), FALSE, FALSE, NULL);
    decision.done = 1;
  } else
#endif
    grotto_approval_controller_decide_async(controller, grotto_approval_request_get_id(request),
                                            FALSE, FALSE, 0, NULL, decision_done, &decision);
  WAIT(result.done && decision.done);
  g_assert_nonnull(result.error);
  g_assert_no_error(decision.error);
  WAIT(g_list_model_get_n_items(pending) == 0);
  clear_reply(&result);
  clear_reply(&decision);
#ifndef __APPLE__
  if (gui) {
    live_navigation(controller, app);
    stale_app_actions(controller, app, connection);
  }
#endif
  grotto_approval_controller_stop(controller);
  WAIT(grotto_approval_controller_get_holds(controller) == 0);

  while (g_main_context_iteration(NULL, FALSE)) {
  }
  g_print("approval integration: %s review path exercised\n",
          gui ? "real GTK dialog" : "headless controller (platform UI skip)");
}
#ifndef __APPLE__
static void stop_signer(GDBusConnection *bus, const char *sender, const char *path,
                        const char *iface, const char *method, GVariant *args,
                        GDBusMethodInvocation *inv, gpointer data) {
  (void)sender;
  (void)path;
  (void)iface;
  (void)method;
  (void)args;
  (void)data;
  /* Bind to the original helper-owned daemon's unique bus peer, never a
   * later replacement or the lifeline supervisor (which must stay alive). */
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) peer = g_dbus_connection_call_sync(
      bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "GetConnectionUnixProcessID", g_variant_new("(s)", daemon_owner), G_VARIANT_TYPE("(u)"), 0,
      5000, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(peer);
  guint32 pid = 0;
  g_variant_get(peer, "(u)", &pid);
  g_assert_cmpuint(pid, >, 1);
  g_assert_cmpint(kill((pid_t)pid, SIGTERM), ==, 0);
  g_dbus_method_invocation_return_value(inv, NULL);
}
typedef struct {
  guint done;
  gboolean ok;
  GError *error;
} WorkerReply;
static void worker_done(GObject *source, GAsyncResult *result, gpointer data) {
  WorkerReply *r = data;
  r->ok = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &r->error);
  r->done++;
}
#endif
int main(int argc, char **argv) {
  gboolean worker = argc == 3 && !strcmp(argv[2], "--gui-worker");
  g_assert_true(argc == 2 || worker);
  argc = 2;
  g_assert_cmpstr(g_getenv("NOSTRC_TEST_ISOLATION"), ==, "1");
  if (!nostrc_test_bus_available())
    return 77;
  struct rlimit limit = {0, 0};
  setrlimit(RLIMIT_CORE, &limit);
  g_setenv("NOSTR_SIGNER_TEST_APPROVERS", argv[0], TRUE);
  g_setenv("NOSTR_SIGNER_SECKEY_HEX",
           "0000000000000000000000000000000000000000000000000000000000000001", TRUE);
  g_test_init(&argc, &argv, NULL);
  nostr_json_init();
  nostrc_test_bus_tolerate_ebadf();
#ifndef __APPLE__
  if (worker) {
    /* GTK/Adwaita have process-global bus proxies. Run the GUI in a short-lived
     * peer on the coordinator's private bus, not the developer's session bus;
     * the coordinator still checks that its own bus connections are released. */
    g_assert_nonnull(g_getenv("DBUS_SESSION_BUS_ADDRESS"));
    nostrc_test_tolerate_gdk_frame_warning();
    gtk_init();
    adw_init();
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    g_autoptr(GSettings) settings = g_settings_new("org.nostr.Grotto");
    g_settings_set_boolean(settings, "onboarding-completed", TRUE);
    g_autoptr(GApplication) app = G_APPLICATION(grotto_application_new(G_APPLICATION_NON_UNIQUE));
    g_assert_true(g_application_register(app, NULL, NULL));
    g_autoptr(GDBusConnection) connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    g_autoptr(GDBusConnection) consumer =
        g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"),
                                               G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                   G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                               NULL, NULL, NULL);
    g_assert_nonnull(consumer);
    WAIT(g_object_get_data(G_OBJECT(app), "grotto-approval-controller") != NULL);
    run_flow(connection, consumer, app, TRUE);
    g_signal_emit_by_name(app, "shutdown");
    while (gtk_application_get_windows(GTK_APPLICATION(app)))
      gtk_window_destroy(gtk_application_get_windows(GTK_APPLICATION(app))->data);
    g_clear_object(&app);
    nostr_json_cleanup();
    return 0;
  }
#endif
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  active_bus = bus;
  nostrc_test_bus_up(bus);
  /* Per-invocation profile, removed by the bus lifeline even after an abort. */
  g_autofree gchar *config = g_build_filename(nostrc_test_bus_get_dir(bus), "config", NULL);
  g_assert_cmpint(g_mkdir(config, 0700), ==, 0);
  g_setenv("XDG_CONFIG_HOME", config, TRUE);
  GDBusConnection *connection = nostrc_test_bus_connect(bus);
  g_autoptr(GVariant) owned = g_dbus_connection_call_sync(
      connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "RequestName", g_variant_new("(su)", "org.nostr.Grotto", 0), NULL, 0, 5000, NULL, NULL);
  g_assert_nonnull(owned);
  gboolean started = FALSE;
  guint watch = g_bus_watch_name_on_connection(connection, "org.nostr.Signer", 0, owner_seen, NULL,
                                               &started, NULL);
  const char *command[] = {argv[1], NULL};
  GSubprocess *daemon_supervisor =
      nostrc_test_bus_spawn_supervised(bus, "g07-daemon.log", NULL, command);
#ifndef __APPLE__
  g_autoptr(GDBusNodeInfo) coordinator =
      g_dbus_node_info_new_for_xml("<node><interface name='org.test.GrottoCoordinator'><method "
                                   "name='StopSigner'/></interface></node>",
                                   NULL);
  static const GDBusInterfaceVTable coordinator_table = {.method_call = stop_signer};
  guint coordinator_registration = g_dbus_connection_register_object(
      connection, "/org/test/GrottoCoordinator", coordinator->interfaces[0], &coordinator_table,
      daemon_supervisor, NULL, NULL);
  bus_name(connection, "org.test.GrottoCoordinator", TRUE);
#else
  (void)daemon_supervisor;
#endif
  WAIT(started);
#ifndef __APPLE__
  if (g_getenv("DISPLAY")) {
    WorkerReply r = {0};
    g_autoptr(GError) error = NULL;
    g_autoptr(GSubprocessLauncher) launcher =
        g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDERR_MERGE);
    g_autofree gchar *log = g_build_filename(nostrc_test_bus_get_dir(bus), "g07-gui.log", NULL);
    g_subprocess_launcher_set_stdout_file_path(launcher, log);
    g_autoptr(GSubprocess) peer =
        g_subprocess_launcher_spawn(launcher, &error, argv[0], argv[1], "--gui-worker", NULL);
    g_assert_no_error(error);
    g_subprocess_wait_check_async(peer, NULL, worker_done, &r);
    /* Let the worker's per-operation 20s failure bound report its own line. */
    guint worker_bound = g_timeout_add_seconds(60, bound, GINT_TO_POINTER(__LINE__));
    while (!r.done)
      g_main_context_iteration(NULL, TRUE);
    g_source_remove(worker_bound);
    nostrc_test_bus_dump_log(bus, "g07-gui.log");
    g_assert_no_error(r.error);
    g_assert_true(r.ok);
  } else
#endif
  {
    g_autoptr(GApplication) app =
        g_application_new("org.test.GrottoApproval", G_APPLICATION_NON_UNIQUE);
    run_flow(connection, nostrc_test_bus_connect(bus), app, FALSE);
  }
#ifndef __APPLE__
  g_dbus_connection_unregister_object(connection, coordinator_registration);
#endif
  g_bus_unwatch_name(watch);
  g_clear_pointer(&daemon_owner, g_free);
  nostrc_test_bus_dump_log(bus, "g07-daemon.log");
  nostrc_test_bus_down(bus);
  nostr_json_cleanup();
  return 0;
}
