#include "nostr_dbus.h"
#include <gio/gio.h>

const char *nh_signer_bus_name(void){
  GError *err = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){
    if (err) g_error_free(err);
    return "org.nostr.Signer";
  }
  const char *preferred = "org.nostr.Signer";
  const char *alt = "com.nostr.Signer";
  gboolean has_pref = g_dbus_is_name(bus, preferred);
  if (has_pref){ g_object_unref(bus); return preferred; }
  gboolean has_alt = g_dbus_is_name(bus, alt);
  g_object_unref(bus);
  return has_alt ? alt : preferred;
}
