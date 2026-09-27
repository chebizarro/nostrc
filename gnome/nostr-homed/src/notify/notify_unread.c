/*
 * notify_unread.c — see notify_unread.h.
 */
#include "notify_unread.h"
#include "notify-daemon1-xml.h" /* generated from gnome/dbus/org.nostr.NotifyDaemon1.xml */

#include <string.h>

#include "nostr/nip19/nip19.h"
#include "nostr/nip19/nostr-pointer.h"

#define PATH  "/org/nostr/NotifyDaemon1"
#define IFACE "org.nostr.NotifyDaemon1"

static struct {
  GDBusConnection *bus;
  GDBusNodeInfo   *node;
  guint            reg_id;
  uint32_t         unread;
  int64_t          last;
} U;

static void emit_changed(void) {
  if (!U.bus || !U.reg_id) return;
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "UnreadDirectMessages", g_variant_new_uint32(U.unread));
  g_variant_builder_add(&b, "{sv}", "LastDirectMessage", g_variant_new_uint64((guint64)MAX(U.last, 0)));
  g_dbus_connection_emit_signal(U.bus, NULL, PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                g_variant_new("(sa{sv}as)", IFACE, &b, NULL), NULL);
}

void nostr_notify_unread_add(int64_t when) {
  if (U.unread < G_MAXUINT32) U.unread++;
  U.last = when;
  emit_changed();
}

void nostr_notify_unread_reset(void) {
  if (U.unread == 0) return;
  U.unread = 0;
  emit_changed();
}

uint32_t nostr_notify_unread_count(void) { return U.unread; }
int64_t nostr_notify_unread_last(void) { return U.last; }

bool nostr_notify_uri_is_dm(const char *uri) {
  if (!uri || !g_str_has_prefix(uri, "nostr:nevent1")) return false;
  NostrEventPointer *p = NULL;
  if (nostr_nip19_decode_nevent(uri + strlen("nostr:"), &p) != 0 || !p) return false;
  bool dm = p->kind == 1059;
  nostr_event_pointer_free(p);
  return dm;
}

static void on_call(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                    const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer d) {
  (void)c; (void)sender; (void)path; (void)iface; (void)params; (void)d;
  if (g_str_equal(method, "MarkRead")) {
    nostr_notify_unread_reset();
    g_dbus_method_invocation_return_value(inv, NULL);
    return;
  }
  g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "%s", method);
}

static GVariant *on_get(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                        const gchar *prop, GError **error, gpointer d) {
  (void)c; (void)sender; (void)path; (void)iface; (void)error; (void)d;
  if (g_str_equal(prop, "UnreadDirectMessages")) return g_variant_new_uint32(U.unread);
  if (g_str_equal(prop, "LastDirectMessage")) return g_variant_new_uint64((guint64)MAX(U.last, 0));
  return NULL;
}

static const GDBusInterfaceVTable vtable = { .method_call = on_call, .get_property = on_get };

bool nostr_notify_unread_export(GDBusConnection *bus, GError **error) {
  if (U.reg_id) return true;
  if (!U.node && !(U.node = g_dbus_node_info_new_for_xml(notify_daemon1_xml, error))) return false;
  U.reg_id = g_dbus_connection_register_object(bus, PATH, U.node->interfaces[0], &vtable, NULL, NULL, error);
  if (!U.reg_id) return false;
  U.bus = g_object_ref(bus);
  return true;
}

void nostr_notify_unread_unexport(void) {
  if (U.reg_id) g_dbus_connection_unregister_object(U.bus, U.reg_id);
  U.reg_id = 0;
  g_clear_object(&U.bus);
}
