#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>
#include "nostr_secrets.h"
#include "nostr_dbus.h"

int nh_secrets_decrypt_via_signer(const char *ciphertext, char **plaintext_out){
  if (!plaintext_out) return -1;
  *plaintext_out = NULL;
  if (!ciphertext) return -1;

  const char *busname = nh_signer_bus_name();
  GError *err=NULL; GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (bus){
    /* Best-effort: call org.nostr.Signer.Decrypt(s)->(s). If not available, fall back. */
    GVariant *ret = g_dbus_connection_call_sync(bus, busname,
      "/org/nostr/Signer", "org.nostr.Signer", "Decrypt",
      g_variant_new("(s)", ciphertext), G_VARIANT_TYPE_TUPLE, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
    if (ret){
      const char *pt=NULL; g_variant_get(ret, "(s)", &pt);
      if (pt) *plaintext_out = strdup(pt);
      g_variant_unref(ret);
    }
    g_object_unref(bus);
  }
  if (!*plaintext_out){
    /* Fallback passthrough */
    *plaintext_out = strdup(ciphertext);
  }
  return *plaintext_out ? 0 : -1;
}
