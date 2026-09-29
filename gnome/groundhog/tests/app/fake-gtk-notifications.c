#include "fake-gtk-notifications.h"

#define NAME "org.gtk.Notifications"
#define PATH "/org/gtk/Notifications"
#define FDO_NAME "org.freedesktop.Notifications"
#define FDO_PATH "/org/freedesktop/Notifications"

struct _FakeGtkNotifications {
  GDBusConnection *owner;
  GDBusNodeInfo *node;
  guint registration;
  guint sync_registration;
  guint fdo_registration;
  guint32 fdo_serial;
  GPtrArray *fdo_notifies;
  GhClock *clock;
  GPtrArray *calls;
};

static void
call_free(gpointer data)
{
  FakeNotificationCall *call = data;
  g_free(call->app_id);
  g_free(call->id);
  g_clear_pointer(&call->notification, g_variant_unref);
  g_free(call);
}

static void
method_call(GDBusConnection *connection, const gchar *sender, const gchar *path,
            const gchar *interface, const gchar *method, GVariant *parameters,
            GDBusMethodInvocation *invocation, gpointer user_data)
{
  FakeGtkNotifications *fake = user_data;
  (void)connection; (void)sender; (void)path;
  if (g_str_equal(interface, "org.nostrc.Test.Sync")) {
    g_dbus_method_invocation_return_value(invocation, NULL);
    return;
  }
  if (g_str_equal(interface, FDO_NAME)) {
    if (g_str_equal(method, "Notify")) {
      g_ptr_array_add(fake->fdo_notifies, g_variant_ref(parameters));
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", ++fake->fdo_serial));
    } else {
      g_dbus_method_invocation_return_value(invocation, NULL);
    }
    return;
  }
  FakeNotificationCall *call = g_new0(FakeNotificationCall, 1);
  call->added = g_str_equal(method, "AddNotification");
  if (call->added)
    g_variant_get(parameters, "(ss@a{sv})", &call->app_id, &call->id, &call->notification);
  else
    g_variant_get(parameters, "(ss)", &call->app_id, &call->id);
  call->at = fake->clock ? gh_clock_get_monotonic_time(fake->clock) : 0;
  g_ptr_array_add(fake->calls, call);
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static const GDBusInterfaceVTable vtable = { method_call, NULL, NULL, { 0 } };

FakeGtkNotifications *
fake_gtk_notifications_new(GDBusConnection *owner)
{
  g_autoptr(GError) error = NULL;
  FakeGtkNotifications *fake = g_new0(FakeGtkNotifications, 1);
  fake->owner = owner;
  fake->calls = g_ptr_array_new_with_free_func(call_free);
  fake->node = g_dbus_node_info_new_for_xml(
    "<node><interface name='org.gtk.Notifications'>"
    "<method name='AddNotification'><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='a{sv}' direction='in'/></method>"
    "<method name='RemoveNotification'><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/></method>"
    "</interface><interface name='org.nostrc.Test.Sync'><method name='Sync'/></interface>"
    "<interface name='org.freedesktop.Notifications'>"
    "<method name='Notify'><arg type='s' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='as' direction='in'/><arg type='a{sv}' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='u' direction='out'/></method>"
    "<method name='CloseNotification'><arg type='u' direction='in'/></method>"
    "</interface></node>", &error);
  g_assert_no_error(error);
  fake->registration = g_dbus_connection_register_object(owner, PATH, fake->node->interfaces[0],
                                                         &vtable, fake, NULL, &error);
  g_assert_no_error(error);
  fake->sync_registration = g_dbus_connection_register_object(owner, PATH,
    fake->node->interfaces[1], &vtable, fake, NULL, &error);
  g_assert_no_error(error);
  fake->fdo_registration = g_dbus_connection_register_object(owner, FDO_PATH,
    fake->node->interfaces[2], &vtable, fake, NULL, &error);
  g_assert_no_error(error);
  fake->fdo_notifies = g_ptr_array_new_with_free_func((GDestroyNotify)g_variant_unref);
  const gchar *const names[] = { NAME, FDO_NAME };
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(owner, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", names[i], 4u), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1,
      NULL, &error);
    g_assert_no_error(error);
    guint32 result = 0;
    g_variant_get(reply, "(u)", &result);
    g_assert_cmpuint(result, ==, 1); /* primary owner */
  }
  return fake;
}

void
fake_gtk_notifications_free(FakeGtkNotifications *fake)
{
  if (!fake)
    return;
  g_dbus_connection_unregister_object(fake->owner, fake->registration);
  g_dbus_connection_unregister_object(fake->owner, fake->sync_registration);
  g_dbus_connection_unregister_object(fake->owner, fake->fdo_registration);
  g_ptr_array_unref(fake->fdo_notifies);
  g_dbus_node_info_unref(fake->node);
  g_ptr_array_unref(fake->calls);
  g_clear_pointer(&fake->clock, gh_clock_unref);
  g_free(fake);
}

void
fake_gtk_notifications_set_clock(FakeGtkNotifications *fake, GhClock *clock)
{
  g_clear_pointer(&fake->clock, gh_clock_unref);
  fake->clock = clock ? gh_clock_ref(clock) : NULL;
}

static void
synced(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result,
                                                            &error);
  g_assert_no_error(error);
  *(gboolean *)data = TRUE;
}

static gboolean
deadline_hit(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

void
fake_gtk_notifications_sync(FakeGtkNotifications *fake, GDBusConnection *client)
{
  (void)fake;
  gboolean done = FALSE, expired = FALSE;
  g_dbus_connection_call(client, NAME, PATH, "org.nostrc.Test.Sync", "Sync", NULL, NULL,
                         G_DBUS_CALL_FLAGS_NONE, -1, NULL, synced, &done);
  guint deadline = g_timeout_add_seconds(10, deadline_hit, &expired);
  while (!done && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (expired)
    g_error("the fake notification server did not answer within 10 s");
  g_source_remove(deadline);
  /* The method calls it received before were dispatched before this one. */
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

GPtrArray *
fake_gtk_notifications_get_calls(FakeGtkNotifications *fake)
{
  return fake->calls;
}

void
fake_gtk_notifications_clear(FakeGtkNotifications *fake)
{
  g_ptr_array_set_size(fake->calls, 0);
  g_ptr_array_set_size(fake->fdo_notifies, 0);
}

GPtrArray *
fake_gtk_notifications_get_fdo_notifies(FakeGtkNotifications *fake)
{
  return fake->fdo_notifies;
}

GVariant *
fake_gtk_notifications_lookup(FakeGtkNotifications *fake, const gchar *id)
{
  for (guint i = fake->calls->len; i-- > 0;) {
    FakeNotificationCall *call = g_ptr_array_index(fake->calls, i);
    if (g_str_equal(call->id, id))
      return call->added ? call->notification : NULL;
  }
  return NULL;
}

guint
fake_gtk_notifications_count(FakeGtkNotifications *fake, gboolean added, const gchar *id)
{
  guint n = 0;
  for (guint i = 0; i < fake->calls->len; i++) {
    FakeNotificationCall *call = g_ptr_array_index(fake->calls, i);
    n += call->added == added && (!id || g_str_equal(call->id, id));
  }
  return n;
}

gchar *
fake_gtk_notifications_dump(FakeGtkNotifications *fake)
{
  GString *out = g_string_new(NULL);
  for (guint i = 0; i < fake->calls->len; i++) {
    FakeNotificationCall *call = g_ptr_array_index(fake->calls, i);
    g_string_append_printf(out, "%s %s %s ", call->added ? "add" : "remove", call->app_id,
                           call->id);
    if (call->notification) {
      g_autofree gchar *printed = g_variant_print(call->notification, TRUE);
      g_string_append(out, printed);
    }
    g_string_append_c(out, '\n');
  }
  return g_string_free(out, FALSE);
}
