#include <gio/gio.h>
#include <stdlib.h>

#define SIGNER_NAME  "com.nostr.Signer"
#define SIGNER_PATH  "/com/nostr/Signer"
#define SIGNER_IFACE "com.nostr.Signer"

GDBusProxy *signer_client_new_sync(GError **error){
  return g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
                                       G_DBUS_PROXY_FLAGS_NONE,
                                       NULL,
                                       SIGNER_NAME,
                                       SIGNER_PATH,
                                       SIGNER_IFACE,
                                       NULL,
                                       error);
}

gchar *signer_client_get_public_key(GDBusProxy *proxy, GError **error){
  GVariant *ret = g_dbus_proxy_call_sync(proxy, "GetPublicKey", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  if (!ret) return NULL;
  const gchar *npub = NULL; g_variant_get(ret, "(s)", &npub);
  gchar *dup = g_strdup(npub);
  g_variant_unref(ret);
  return dup;
}

gchar *signer_client_sign_event(GDBusProxy *proxy, const gchar *event_json, const gchar *current_user, const gchar *app_id, GError **error){
  GVariant *ret = g_dbus_proxy_call_sync(proxy, "SignEvent", g_variant_new("(sss)", event_json?event_json:"", current_user?current_user:"", app_id?app_id:""), G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  if (!ret) return NULL; const gchar *sig=NULL; g_variant_get(ret, "(s)", &sig); gchar *dup=g_strdup(sig); g_variant_unref(ret); return dup;
}

gchar *signer_client_nip04_encrypt(GDBusProxy *proxy, const gchar *plaintext, const gchar *peer_pub_hex, const gchar *current_user, GError **error){
  GVariant *ret = g_dbus_proxy_call_sync(proxy, "NIP04Encrypt", g_variant_new("(sss)", plaintext?plaintext:"", peer_pub_hex?peer_pub_hex:"", current_user?current_user:""), G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  if (!ret) return NULL; const gchar *out=NULL; g_variant_get(ret, "(s)", &out); gchar *dup=g_strdup(out); g_variant_unref(ret); return dup;
}

gchar *signer_client_nip04_decrypt(GDBusProxy *proxy, const gchar *cipher_b64, const gchar *peer_pub_hex, const gchar *current_user, GError **error){
  GVariant *ret = g_dbus_proxy_call_sync(proxy, "NIP04Decrypt", g_variant_new("(sss)", cipher_b64?cipher_b64:"", peer_pub_hex?peer_pub_hex:"", current_user?current_user:""), G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  if (!ret) return NULL; const gchar *out=NULL; g_variant_get(ret, "(s)", &out); gchar *dup=g_strdup(out); g_variant_unref(ret); return dup;
}

gchar *signer_client_nip44_encrypt(GDBusProxy *proxy, const gchar *plaintext, const gchar *peer_pub_hex, const gchar *current_user, GError **error){
  GVariant *ret = g_dbus_proxy_call_sync(proxy, "NIP44Encrypt", g_variant_new("(sss)", plaintext?plaintext:"", peer_pub_hex?peer_pub_hex:"", current_user?current_user:""), G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  if (!ret) return NULL; const gchar *out=NULL; g_variant_get(ret, "(s)", &out); gchar *dup=g_strdup(out); g_variant_unref(ret); return dup;
}

gchar *signer_client_nip44_decrypt(GDBusProxy *proxy, const gchar *cipher_b64, const gchar *peer_pub_hex, const gchar *current_user, GError **error){
  GVariant *ret = g_dbus_proxy_call_sync(proxy, "NIP44Decrypt", g_variant_new("(sss)", cipher_b64?cipher_b64:"", peer_pub_hex?peer_pub_hex:"", current_user?current_user:""), G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  if (!ret) return NULL; const gchar *out=NULL; g_variant_get(ret, "(s)", &out); gchar *dup=g_strdup(out); g_variant_unref(ret); return dup;
}

gchar *signer_client_decrypt_zap_event(GDBusProxy *proxy, const gchar *event_json, const gchar *current_user, GError **error){
  GVariant *ret = g_dbus_proxy_call_sync(proxy, "DecryptZapEvent", g_variant_new("(ss)", event_json?event_json:"", current_user?current_user:""), G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  if (!ret) return NULL; const gchar *out=NULL; g_variant_get(ret, "(s)", &out); gchar *dup=g_strdup(out); g_variant_unref(ret); return dup;
}

gchar *signer_client_get_relays(GDBusProxy *proxy, GError **error){
  GVariant *ret = g_dbus_proxy_call_sync(proxy, "GetRelays", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  if (!ret) return NULL; const gchar *out=NULL; g_variant_get(ret, "(s)", &out); gchar *dup=g_strdup(out); g_variant_unref(ret); return dup;
}

gboolean signer_client_store_secret(GDBusProxy *proxy, const gchar *secret, const gchar *account, GError **error){
  GVariant *ret = g_dbus_proxy_call_sync(proxy, "StoreSecret", g_variant_new("(ss)", secret?secret:"", account?account:"default"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  if (!ret) return FALSE; gboolean ok=FALSE; g_variant_get(ret, "(b)", &ok); g_variant_unref(ret); return ok;
}

gboolean signer_client_clear_secret(GDBusProxy *proxy, const gchar *account, GError **error){
  GVariant *ret = g_dbus_proxy_call_sync(proxy, "ClearSecret", g_variant_new("(s)", account?account:"default"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  if (!ret) return FALSE; gboolean ok=FALSE; g_variant_get(ret, "(b)", &ok); g_variant_unref(ret); return ok;
}
