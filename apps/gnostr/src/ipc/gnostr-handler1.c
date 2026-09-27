/*
 * gnostr-handler1.c — org.nostr.Handler1 export (nostrc-prqu.3).
 * See gnostr-handler1.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gnostr-handler1.h"

#include <string.h>

/* Mirrors gnome/dbus/org.nostr.Handler1.xml. */
static const char handler1_xml[] =
  "<node>"
  "  <interface name='org.nostr.Handler1'>"
  "    <method name='OpenEvent'>"
  "      <arg name='kind' type='u' direction='in'/>"
  "      <arg name='event_json' type='s' direction='in'/>"
  "      <arg name='relays' type='as' direction='in'/>"
  "    </method>"
  "  </interface>"
  "</node>";

/* Refuse absurd payloads before parsing: a signed event with a large
 * long-form body is well under this. */
#define HANDLER1_MAX_EVENT_BYTES (1024 * 1024)

/* Any same-user peer can call OpenEvent, and each accepted call brings a
 * view to the front: at most this many per window. A refused call makes
 * nostr-dispatcher fall back to launching the URI. */
#define HANDLER1_RATE_MAX     10
#define HANDLER1_RATE_WINDOW  (10 * G_USEC_PER_SEC)

struct _GnostrHandler1 {
  GDBusConnection *connection;
  GDBusNodeInfo *node;
  guint registration_id;
  guint owner_id;
  GnostrHandler1OpenEventFunc open_event;
  gpointer user_data;
  gint64 window_start;
  guint window_calls;
};

static gboolean
rate_ok(GnostrHandler1 *self)
{
  gint64 now = g_get_monotonic_time();
  if (now - self->window_start > HANDLER1_RATE_WINDOW) {
    self->window_start = now;
    self->window_calls = 0;
  }
  return ++self->window_calls <= HANDLER1_RATE_MAX;
}

static void
method_call(GDBusConnection *connection, const char *sender, const char *object_path,
            const char *interface_name, const char *method_name, GVariant *parameters,
            GDBusMethodInvocation *invocation, gpointer user_data)
{
  (void)connection; (void)object_path; (void)interface_name;
  GnostrHandler1 *self = user_data;

  if (g_strcmp0(method_name, "OpenEvent") != 0) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                          "Unknown method %s", method_name);
    return;
  }

  guint32 kind = 0;
  const char *event_json = NULL;
  g_autofree const char **relays = NULL;
  g_variant_get(parameters, "(u&s^a&s)", &kind, &event_json, &relays);

  if (kind > 65535 || strlen(event_json) > HANDLER1_MAX_EVENT_BYTES) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                          "Event rejected");
    return;
  }
  if (!rate_ok(self)) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_LIMITS_EXCEEDED,
                                          "Too many events; try again later");
    return;
  }

  g_autoptr(GError) error = NULL;
  if (!self->open_event(kind, event_json, relays, self->user_data, &error)) {
    g_debug("Handler1.OpenEvent from %s rejected: %s", sender ? sender : "?",
            error ? error->message : "unknown");
    g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR,
                                                  G_DBUS_ERROR_INVALID_ARGS,
                                                  error ? error->message : "Event rejected");
    return;
  }
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static const GDBusInterfaceVTable vtable = { method_call, NULL, NULL, { 0 } };

static void
on_name_lost(GDBusConnection *connection, const char *name, gpointer user_data)
{
  (void)user_data;
  /* NONE, not REPLACE/queue: never fight or silently take over later. If
   * another process holds the name, nostr-dispatcher hands it events
   * meant for GNostr, so say so loudly. */
  if (connection)
    g_warning("Handler1: another process owns %s; nostr: links handed over by "
              "nostr-dispatcher will not reach GNostr", name);
}

GnostrHandler1 *
gnostr_handler1_export(GDBusConnection *connection,
                       GnostrHandler1OpenEventFunc open_event,
                       gpointer user_data,
                       GError **error)
{
  g_return_val_if_fail(G_IS_DBUS_CONNECTION(connection), NULL);
  g_return_val_if_fail(open_event != NULL, NULL);

  GnostrHandler1 *self = g_new0(GnostrHandler1, 1);
  self->connection = g_object_ref(connection);
  self->open_event = open_event;
  self->user_data = user_data;
  self->node = g_dbus_node_info_new_for_xml(handler1_xml, error);
  if (!self->node)
    goto fail;

  /* Export BEFORE owning the name, so a call can never reach an
   * unexported object. */
  self->registration_id = g_dbus_connection_register_object(
      connection, GNOSTR_HANDLER1_OBJECT_PATH,
      g_dbus_node_info_lookup_interface(self->node, GNOSTR_HANDLER1_INTERFACE),
      &vtable, self, NULL, error);
  if (self->registration_id == 0)
    goto fail;

  self->owner_id = g_bus_own_name_on_connection(connection, GNOSTR_HANDLER1_BUS_NAME,
                                                G_BUS_NAME_OWNER_FLAGS_NONE,
                                                NULL, on_name_lost, NULL, NULL);
  return self;

fail:
  gnostr_handler1_unexport(self);
  return NULL;
}

void
gnostr_handler1_unexport(GnostrHandler1 *self)
{
  if (!self)
    return;
  if (self->owner_id)
    g_bus_unown_name(self->owner_id);
  if (self->registration_id)
    g_dbus_connection_unregister_object(self->connection, self->registration_id);
  g_clear_pointer(&self->node, g_dbus_node_info_unref);
  g_clear_object(&self->connection);
  g_free(self);
}
