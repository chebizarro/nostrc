#include "nostr_dbus.h"
#include <gio/gio.h>

static gboolean name_has_owner(const char *name){
  GError *err=NULL; gboolean owned = FALSE;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){ if (err) g_error_free(err); return FALSE; }
  GVariant *ret = g_dbus_connection_call_sync(bus,
      "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
      g_variant_new("(s)", name), G_VARIANT_TYPE_TUPLE, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (ret){ g_variant_get(ret, "(b)", &owned); g_variant_unref(ret); }
  if (err) g_error_free(err);
  g_object_unref(bus);
  return owned;
}

const char *nh_signer_bus_name(void){
  const char *preferred = "org.nostr.Signer";
  const char *alt = "com.nostr.Signer";
  if (name_has_owner(preferred)) return preferred;
  if (name_has_owner(alt)) return alt;
  return preferred;
}
