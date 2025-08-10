#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

static GDBusProxy* ensure_proxy(GError **error){
  return g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
                                       G_DBUS_PROXY_FLAGS_NONE,
                                       NULL,
                                       "com.nostr.Signer",
                                       "/com/nostr/Signer",
                                       "com.nostr.Signer",
                                       NULL,
                                       error);
}

static int cmd_get_pubkey(void){
  GError *err=NULL; GDBusProxy *p = ensure_proxy(&err);
  if (!p){ fprintf(stderr, "proxy error: %s\n", err?err->message:"?"); g_clear_error(&err); return 1; }
  GVariant *ret = g_dbus_proxy_call_sync(p, "GetPublicKey", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (!ret){ fprintf(stderr, "GetPublicKey failed: %s\n", err?err->message:"?\n"); g_clear_error(&err); g_object_unref(p); return 2; }
  const gchar *npub=NULL; g_variant_get(ret, "(s)", &npub);
  printf("%s\n", npub?npub:""); g_variant_unref(ret); g_object_unref(p); return 0;
}

static int cmd_store_secret(const char *secret, const char *account){
  GError *err=NULL; GDBusProxy *p = ensure_proxy(&err);
  if (!p){ fprintf(stderr, "proxy error: %s\n", err?err->message:"?"); g_clear_error(&err); return 1; }
  GVariant *ret = g_dbus_proxy_call_sync(p, "StoreSecret", g_variant_new("(ss)", secret, account?account:"default"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (!ret){ fprintf(stderr, "StoreSecret failed: %s\n", err?err->message:"?\n"); g_clear_error(&err); g_object_unref(p); return 2; }
  gboolean ok=FALSE; g_variant_get(ret, "(b)", &ok); g_variant_unref(ret); g_object_unref(p);
  if (!ok){ fprintf(stderr, "StoreSecret returned false\n"); return 3; }
  printf("ok\n"); return 0;
}

static int cmd_clear_secret(const char *account){
  GError *err=NULL; GDBusProxy *p = ensure_proxy(&err);
  if (!p){ fprintf(stderr, "proxy error: %s\n", err?err->message:"?"); g_clear_error(&err); return 1; }
  GVariant *ret = g_dbus_proxy_call_sync(p, "ClearSecret", g_variant_new("(s)", account?account:"default"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (!ret){ fprintf(stderr, "ClearSecret failed: %s\n", err?err->message:"?\n"); g_clear_error(&err); g_object_unref(p); return 2; }
  gboolean ok=FALSE; g_variant_get(ret, "(b)", &ok); g_variant_unref(ret); g_object_unref(p);
  if (!ok){ fprintf(stderr, "ClearSecret returned false\n"); return 3; }
  printf("ok\n"); return 0;
}

static int cmd_sign(const char *json, const char *current_user, const char *requester){
  GError *err=NULL; GDBusProxy *p = ensure_proxy(&err);
  if (!p){ fprintf(stderr, "proxy error: %s\n", err?err->message:"?"); g_clear_error(&err); return 1; }
  GVariant *ret = g_dbus_proxy_call_sync(p, "SignEvent", g_variant_new("(sss)", json, current_user?current_user:"", requester?requester:""), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (!ret){ fprintf(stderr, "SignEvent failed: %s\n", err?err->message:"?\n"); g_clear_error(&err); g_object_unref(p); return 2; }
  const gchar *out=NULL; g_variant_get(ret, "(s)", &out); printf("%s\n", out?out:""); g_variant_unref(ret); g_object_unref(p); return 0;
}

static int cmd_nip04_enc(const char *plaintext, const char *peer, const char *current_user){
  GError *err=NULL; GDBusProxy *p = ensure_proxy(&err);
  if (!p){ fprintf(stderr, "proxy error: %s\n", err?err->message:"?"); g_clear_error(&err); return 1; }
  GVariant *ret = g_dbus_proxy_call_sync(p, "NIP04Encrypt", g_variant_new("(sss)", plaintext, peer, current_user?current_user:""), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (!ret){ fprintf(stderr, "NIP04Encrypt failed: %s\n", err?err->message:"?\n"); g_clear_error(&err); g_object_unref(p); return 2; }
  const gchar *out=NULL; g_variant_get(ret, "(s)", &out); printf("%s\n", out?out:""); g_variant_unref(ret); g_object_unref(p); return 0;
}

static int cmd_nip04_dec(const char *cipher_b64, const char *peer, const char *current_user){
  GError *err=NULL; GDBusProxy *p = ensure_proxy(&err);
  if (!p){ fprintf(stderr, "proxy error: %s\n", err?err->message:"?"); g_clear_error(&err); return 1; }
  GVariant *ret = g_dbus_proxy_call_sync(p, "NIP04Decrypt", g_variant_new("(sss)", cipher_b64, peer, current_user?current_user:""), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (!ret){ fprintf(stderr, "NIP04Decrypt failed: %s\n", err?err->message:"?\n"); g_clear_error(&err); g_object_unref(p); return 2; }
  const gchar *out=NULL; g_variant_get(ret, "(s)", &out); printf("%s\n", out?out:""); g_variant_unref(ret); g_object_unref(p); return 0;
}

static int cmd_nip44_enc(const char *plaintext, const char *peer, const char *current_user){
  GError *err=NULL; GDBusProxy *p = ensure_proxy(&err);
  if (!p){ fprintf(stderr, "proxy error: %s\n", err?err->message:"?"); g_clear_error(&err); return 1; }
  GVariant *ret = g_dbus_proxy_call_sync(p, "NIP44Encrypt", g_variant_new("(sss)", plaintext, peer, current_user?current_user:""), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (!ret){ fprintf(stderr, "NIP44Encrypt failed: %s\n", err?err->message:"?\n"); g_clear_error(&err); g_object_unref(p); return 2; }
  const gchar *out=NULL; g_variant_get(ret, "(s)", &out); printf("%s\n", out?out:""); g_variant_unref(ret); g_object_unref(p); return 0;
}

static int cmd_nip44_dec(const char *cipher_b64, const char *peer, const char *current_user){
  GError *err=NULL; GDBusProxy *p = ensure_proxy(&err);
  if (!p){ fprintf(stderr, "proxy error: %s\n", err?err->message:"?"); g_clear_error(&err); return 1; }
  GVariant *ret = g_dbus_proxy_call_sync(p, "NIP44Decrypt", g_variant_new("(sss)", cipher_b64, peer, current_user?current_user:""), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (!ret){ fprintf(stderr, "NIP44Decrypt failed: %s\n", err?err->message:"?\n"); g_clear_error(&err); g_object_unref(p); return 2; }
  const gchar *out=NULL; g_variant_get(ret, "(s)", &out); printf("%s\n", out?out:""); g_variant_unref(ret); g_object_unref(p); return 0;
}

static void usage(const char *argv0){
  fprintf(stderr,
    "Usage: %s <cmd> [args]\n\n"
    "Commands:\n"
    "  get-pubkey\n"
    "  store-secret <secret> [account]\n"
    "  clear-secret [account]\n"
    "  sign <json> [current_user] [requester]\n"
    "  nip04-encrypt <plaintext> <peer_hex> [current_user]\n"
    "  nip04-decrypt <cipher_b64> <peer_hex> [current_user]\n"
    "  nip44-encrypt <plaintext> <peer_hex> [current_user]\n"
    "  nip44-decrypt <cipher_b64> <peer_hex> [current_user]\n",
    argv0);
}

int main(int argc, char **argv){
  if (argc < 2){ usage(argv[0]); return 2; }
  const char *cmd = argv[1];
  if (strcmp(cmd, "get-pubkey")==0){ return cmd_get_pubkey(); }
  if (strcmp(cmd, "store-secret")==0){ if (argc<3){ usage(argv[0]); return 2; } return cmd_store_secret(argv[2], argc>3?argv[3]:"default"); }
  if (strcmp(cmd, "clear-secret")==0){ return cmd_clear_secret(argc>2?argv[2]:"default"); }
  if (strcmp(cmd, "sign")==0){ if (argc<3){ usage(argv[0]); return 2; } return cmd_sign(argv[2], argc>3?argv[3]:"", argc>4?argv[4]:""); }
  if (strcmp(cmd, "nip04-encrypt")==0){ if (argc<4){ usage(argv[0]); return 2; } return cmd_nip04_enc(argv[2], argv[3], argc>4?argv[4]:""); }
  if (strcmp(cmd, "nip04-decrypt")==0){ if (argc<4){ usage(argv[0]); return 2; } return cmd_nip04_dec(argv[2], argv[3], argc>4?argv[4]:""); }
  if (strcmp(cmd, "nip44-encrypt")==0){ if (argc<4){ usage(argv[0]); return 2; } return cmd_nip44_enc(argv[2], argv[3], argc>4?argv[4]:""); }
  if (strcmp(cmd, "nip44-decrypt")==0){ if (argc<4){ usage(argv[0]); return 2; } return cmd_nip44_dec(argv[2], argv[3], argc>4?argv[4]:""); }
  usage(argv[0]); return 2;
}
