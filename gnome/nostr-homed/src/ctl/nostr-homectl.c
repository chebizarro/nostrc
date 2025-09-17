#include "nostr_homectl.h"
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <gio/gio.h>
#include "nostr_cache.h"
#include "relay_fetch.h"
#include "nostr_manifest.h"
#include "nostr_secrets.h"
#include <jansson.h>
#include <sys/statvfs.h>

static const char *BUS_NAME = "org.nostr.Homed1";
static const char *OBJ_PATH = "/org/nostr/Homed1";
static const char *IFACE = "org.nostr.Homed1";

static const char *get_namespace_env(const char *envname, const char *defv){
  const char *v = getenv(envname); return (v && *v) ? v : defv;
}

static int dbus_get_signer_npub(char **out_npub){
  if (out_npub) *out_npub = NULL;
  const char *busname = "org.nostr.Signer";
  GError *err=NULL; GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){ if (err) g_error_free(err); return -1; }
  GVariant *ret = g_dbus_connection_call_sync(bus, busname, "/org/nostr/Signer", "org.nostr.Signer", "GetPublicKey",
                   NULL, G_VARIANT_TYPE_TUPLE, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  int rc=-1; if (ret){ const char *npub=NULL; g_variant_get(ret, "(s)", &npub); if (npub){ *out_npub = g_strdup(npub); rc=0; } g_variant_unref(ret);} if (err) g_error_free(err);
  g_object_unref(bus); return rc;
}

static int is_mountpoint(const char *path){
  if (!path || !*path) return 0;
  struct stat st, pst;
  if (stat(path, &st) != 0) return 0;
  char parent[512];
  strncpy(parent, path, sizeof parent - 1); parent[sizeof parent - 1] = '\0';
  char *slash = strrchr(parent, '/'); if (!slash) return 0; if (slash == parent) { strcpy(parent, "/"); } else { *slash = '\0'; }
  if (stat(parent, &pst) != 0) return 0;
  return st.st_dev != pst.st_dev;
}

static int usage(const char *argv0){
  fprintf(stderr, "Usage: %s [--daemon]\n       %s <open-session|close-session|warm-cache|get-status> <arg>\n", argv0, argv0);
  return 2;
}

/* Library API (local execution) */
int nh_open_session(const char *username){
  if (!username || !*username) return -1;
  /* Ensure warmcache */
  nh_cache c0; if (nh_cache_open_configured(&c0, "/etc/nss_nostr.conf")==0){
    char wc[8]="";
    if (nh_cache_get_setting(&c0, "warmcache", wc, sizeof wc) != 0 || strcmp(wc, "1")!=0){
      nh_cache_close(&c0);
      /* Attempt to warm cache on-demand */
      (void)nh_warm_cache(NULL);
    } else {
      nh_cache_close(&c0);
    }
  }
  /* Mountpoint switched to /home/<user> */
  char mnt[256]; snprintf(mnt, sizeof mnt, "/home/%s", username);
  g_mkdir_with_parents(mnt, 0700);
  /* Start nostrfs via systemd template unit */
  GError *err=NULL;
  gchar *svc = g_strdup_printf("nostrfs@%s.service", username);
  GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &err, "systemctl", "start", svc, NULL);
  if (!proc){
    fprintf(stderr, "OpenSession: systemctl start %s failed: %s\n", svc, err?err->message:"error");
    if (err) g_error_free(err);
  } else {
    g_object_unref(proc);
  }
  /* User provisioning moved to WarmCache */
  /* Wait for mount readiness (up to ~5s) */
  for (int i=0; i<25; i++){
    if (is_mountpoint(mnt)) break;
    g_usleep(200 * 1000);
  }
  /* Persist status, pid and mountpoint */
  nh_cache c; if (nh_cache_open_configured(&c, "/etc/nss_nostr.conf")==0){
    char key[256];
    snprintf(key, sizeof key, "status.%s", username); nh_cache_set_setting(&c, key, "mounted");
    snprintf(key, sizeof key, "mount.%s", username); nh_cache_set_setting(&c, key, mnt);
    /* Store unit name as pid surrogate */
    snprintf(key, sizeof key, "pid.%s", username); nh_cache_set_setting(&c, key, svc);
    nh_cache_close(&c);
  }
  g_free(svc);
  printf("nostr-homectl: OpenSession %s (mounted %s)\n", username, mnt);
  return 0;
}
int nh_close_session(const char *username){
  if (!username || !*username) return -1;
  char mnt[256]=""; char spid[64]="";
  nh_cache c; if (nh_cache_open_configured(&c, "/etc/nss_nostr.conf")==0){
    char key[256];
    snprintf(key, sizeof key, "mount.%s", username); (void)nh_cache_get_setting(&c, key, mnt, sizeof mnt);
    snprintf(key, sizeof key, "pid.%s", username); (void)nh_cache_get_setting(&c, key, spid, sizeof spid);
    nh_cache_close(&c);
  }
  /* Stop systemd template unit; systemd will handle unmount */
  GError *err=NULL; gchar *svc = g_strdup_printf("nostrfs@%s.service", username);
  GSubprocess *u = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &err, "systemctl", "stop", svc, NULL);
  if (!u){ if (err) { g_error_free(err); } }
  else g_object_unref(u);
  g_free(svc);
  /* Update status */
  if (nh_cache_open_configured(&c, "/etc/nss_nostr.conf")==0){
    char key[256]; snprintf(key, sizeof key, "status.%s", username); nh_cache_set_setting(&c, key, "closed"); nh_cache_close(&c);
  }
  /* Wait for unmount (~5s) */
  for (int i=0; i<25; i++){
    char mnt[256]=""; nh_cache c2; if (nh_cache_open_configured(&c2, "/etc/nss_nostr.conf")==0){ char k2[256]; snprintf(k2, sizeof k2, "mount.%s", username); (void)nh_cache_get_setting(&c2, k2, mnt, sizeof mnt); nh_cache_close(&c2);} if (mnt[0]==0) break;
    if (!is_mountpoint(mnt)) {
      break;
    }
    g_usleep(200 * 1000);
  }
  printf("nostr-homectl: CloseSession %s\n", username);
  return 0;
}
int nh_warm_cache(const char *npub_hex){
  (void)npub_hex;
  const char *ns = get_namespace_env("HOMED_NAMESPACE", "personal");
  const char *relays_default[] = { "wss://relay.damus.io", "wss://nostr.wine" };
  const char **relays = relays_default; size_t relays_n = 2;
  int relays_owned = 0;
  /* Check settings for profile-provided relays */
  char relays_json[1024]; relays_json[0] = '\0';
  nh_cache cR; if (nh_cache_open_configured(&cR, "/etc/nss_nostr.conf")==0){
    char rkey[128]; snprintf(rkey, sizeof rkey, "relays.%s", ns);
    if (nh_cache_get_setting(&cR, rkey, relays_json, sizeof relays_json) == 0){
      json_error_t jerr; json_t *root = json_loads(relays_json, 0, &jerr);
      if (root && json_is_array(root)){
        size_t n = json_array_size(root);
        if (n>0 && n<32){
          const char **tmp = (const char**)calloc(n, sizeof(char*));
          size_t outn=0; for (size_t i=0;i<n;i++){ json_t *it = json_array_get(root, i); if (json_is_string(it)) tmp[outn++] = strdup(json_string_value(it)); }
          if (outn>0){ relays = tmp; relays_n = outn; relays_owned = 1; }
        }
      }
      if (root) json_decref(root);
    }
    nh_cache_close(&cR);
  }
  /* Try to fetch profile relays from network; if found, persist and prefer them */
  char **net_relays = NULL; size_t net_count = 0;
  if (nh_fetch_profile_relays(relays, relays_n, &net_relays, &net_count) == 0 && net_count > 0){
    /* Build JSON array */
    json_t *arr = json_array();
    for (size_t i=0;i<net_count;i++) json_array_append_new(arr, json_string(net_relays[i]));
    char *dump = json_dumps(arr, JSON_COMPACT);
    if (dump){ nh_cache cS; if (nh_cache_open_configured(&cS, "/etc/nss_nostr.conf")==0){ char rkey[128]; snprintf(rkey, sizeof rkey, "relays.%s", ns); nh_cache_set_setting(&cS, rkey, dump); nh_cache_close(&cS);} free(dump); }
    json_decref(arr);
    /* Replace current relay list */
    if (relays_owned){ for (size_t i=0;i<relays_n;i++) free((void*)relays[i]); free((void*)relays); }
    relays = (const char**)net_relays; relays_n = net_count; relays_owned = 1;
  }
  char *json = NULL; if (nh_fetch_latest_manifest_json(relays, relays_n, ns, &json) != 0){
    fprintf(stderr, "WarmCache: fetch failed\n"); return -1;
  }
  nh_manifest m; if (nh_manifest_parse_json(json, &m) != 0){ free(json); fprintf(stderr, "WarmCache: parse failed\n"); return -1; }
  /* Persist manifest JSON for later nostrfs consumption */
  nh_cache c0; if (nh_cache_open_configured(&c0, "/etc/nss_nostr.conf")==0){ char mkey[128]; snprintf(mkey, sizeof mkey, "manifest.%s", ns); nh_cache_set_setting(&c0, mkey, json); nh_cache_close(&c0); }
  nh_manifest_free(&m); free(json);
  if (relays_owned){ for (size_t i=0;i<relays_n;i++) free((void*)relays[i]); free((void*)relays); }
  /* Mount secrets tmpfs under /run/nostr-homed/secrets (best effort). */
  (void)nh_secrets_mount_tmpfs("/run/nostr-homed/secrets");
  /* Fetch and decrypt secrets (best effort). */
  do {
    char *se_j = NULL; if (nh_fetch_latest_secrets_json(relays, relays_n, &se_j) != 0) break;
    char *pt = NULL; if (nh_secrets_decrypt_via_signer(se_j, &pt) != 0 || !pt){ free(se_j); break; }
    free(se_j);
    FILE *fp = fopen("/run/nostr-homed/secrets/secrets.json", "wb");
    if (fp){ fwrite(pt, 1, strlen(pt), fp); fclose(fp); (void)chmod("/run/nostr-homed/secrets/secrets.json", 0600); }
    free(pt);
  } while (0);
  /* Mark warmed in cache */
  nh_cache c; if (nh_cache_open_configured(&c, "/etc/nss_nostr.conf")==0){ nh_cache_set_setting(&c, "warmcache", "1"); nh_cache_close(&c); }
  /* Provision deterministic UID/GID mapping for NSS if username is known */
  do {
    char userbuf[128]="";
    const char *user_env = getenv("HOMED_USERNAME");
    if (user_env && *user_env) strncpy(userbuf, user_env, sizeof userbuf - 1);
    if (!userbuf[0]){
      nh_cache cu; if (nh_cache_open_configured(&cu, "/etc/nss_nostr.conf")==0){
        char key[128]; snprintf(key, sizeof key, "username.%s", ns);
        (void)nh_cache_get_setting(&cu, key, userbuf, sizeof userbuf);
        nh_cache_close(&cu);
      }
    }
    if (!userbuf[0]) break; /* username unknown; skip provisioning */
    char *npub=NULL; if (dbus_get_signer_npub(&npub) != 0 || !npub) break;
    nh_cache cU; if (nh_cache_open_configured(&cU, "/etc/nss_nostr.conf")!=0){ g_free(npub); break; }
    uint32_t uid = nh_cache_map_npub_to_uid(&cU, npub);
    uint32_t gid = uid;
    char home[256]; snprintf(home, sizeof home, "/home/%s", userbuf);
    (void)nh_cache_ensure_primary_group(&cU, userbuf, gid);
    (void)nh_cache_upsert_user(&cU, uid, npub, userbuf, gid, home);
    nh_cache_close(&cU);
    g_free(npub);
  } while (0);
  printf("nostr-homectl: WarmCache completed\n");
  return 0;
}
int nh_get_status(const char *username, char *buf, size_t buflen){
  if (!buf || buflen==0) return -1;
  nh_cache c; char st[128]="unknown";
  if (nh_cache_open_configured(&c, "/etc/nss_nostr.conf")==0){
    char key[256]; snprintf(key, sizeof key, "status.%s", username?username:"");
    if (nh_cache_get_setting(&c, key, st, sizeof st) != 0) strcpy(st, "unknown");
    /* Live mount check as a fallback */
    if (username && *username){
      char mnt[256]=""; snprintf(key, sizeof key, "mount.%s", username); (void)nh_cache_get_setting(&c, key, mnt, sizeof mnt);
      if (mnt[0]){ if (is_mountpoint(mnt)) strncpy(st, "mounted", sizeof st - 1); }
    }
    nh_cache_close(&c);
  }
  snprintf(buf, buflen, "user=%s status=%s", username ? username : "", st);
  return 0;
}

/* DBus service implementation */
static void on_method_call(GDBusConnection *conn, const char *sender, const char *object_path,
                           const char *interface_name, const char *method_name, GVariant *params,
                           GDBusMethodInvocation *invocation, gpointer user_data){
  (void)conn; (void)sender; (void)object_path; (void)interface_name; (void)user_data;
  if (strcmp(method_name, "OpenSession")==0){
    const char *user=NULL; g_variant_get(params, "(s)", &user);
    gboolean ok = nh_open_session(user)==0;
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", ok));
    return;
  }
  if (strcmp(method_name, "CloseSession")==0){
    const char *user=NULL; g_variant_get(params, "(s)", &user);
    gboolean ok = nh_close_session(user)==0;
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", ok));
    return;
  }
  if (strcmp(method_name, "WarmCache")==0){
    const char *npub=NULL; g_variant_get(params, "(s)", &npub);
    gboolean ok = nh_warm_cache(npub)==0;
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", ok));
    return;
  }
  if (strcmp(method_name, "GetStatus")==0){
    const char *user=NULL; g_variant_get(params, "(s)", &user);
    char st[256]; nh_get_status(user, st, sizeof st);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", st));
    return;
  }
  g_dbus_method_invocation_return_dbus_error(invocation, "org.nostr.Homed1.Error.UnknownMethod", method_name);
}

static const GDBusInterfaceVTable vtable = {
  .method_call = on_method_call,
  .get_property = NULL,
  .set_property = NULL,
  .padding = {0}
};

static GDBusNodeInfo *introspection = NULL;
static const gchar introspection_xml[] =
  "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" \n"
  "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
  "<node>\n"
  "  <interface name='org.nostr.Homed1'>\n"
  "    <method name='OpenSession'><arg type='s' name='user' direction='in'/><arg type='b' name='ok' direction='out'/></method>\n"
  "    <method name='CloseSession'><arg type='s' name='user' direction='in'/><arg type='b' name='ok' direction='out'/></method>\n"
  "    <method name='WarmCache'><arg type='s' name='npub' direction='in'/><arg type='b' name='ok' direction='out'/></method>\n"
  "    <method name='GetStatus'><arg type='s' name='user' direction='in'/><arg type='s' name='status' direction='out'/></method>\n"
  "    <property name='Version' type='s' access='read'/>\n"
  "  </interface>\n"
  "</node>\n";

static void on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data){
  (void)name; (void)user_data;
  guint reg_id = g_dbus_connection_register_object(connection, OBJ_PATH,
                    introspection->interfaces[0], &vtable, NULL, NULL, NULL);
  if (reg_id == 0) fprintf(stderr, "nostr-homectl: failed to register object\n");
}

int main(int argc, char **argv){
  if (argc >= 2 && strcmp(argv[1], "--daemon")==0){
    GMainLoop *loop = NULL;
    introspection = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
    GBusNameOwnerFlags flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE;
    guint owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, BUS_NAME, flags, on_bus_acquired, NULL, NULL, NULL, NULL);
    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    g_bus_unown_name(owner_id);
    g_main_loop_unref(loop);
    g_dbus_node_info_unref(introspection);
    return 0;
  }

  if (argc < 3) return usage(argv[0]);
  const char *cmd = argv[1];
  GError *err=NULL; GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){ if (err){ fprintf(stderr, "%s\n", err->message); g_error_free(err);} return 1; }
  if (strcmp(cmd, "open-session")==0){
    GVariant *ret = g_dbus_connection_call_sync(bus, BUS_NAME, OBJ_PATH, IFACE, "OpenSession",
                    g_variant_new("(s)", argv[2]), G_VARIANT_TYPE_TUPLE, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
    if (!ret){ fprintf(stderr, "OpenSession failed: %s\n", err?err->message:"err"); if (err) g_error_free(err); g_object_unref(bus); return 1; }
    gboolean ok; g_variant_get(ret, "(b)", &ok); g_variant_unref(ret); g_object_unref(bus); return ok?0:1;
  }
  if (strcmp(cmd, "close-session")==0){
    GVariant *ret = g_dbus_connection_call_sync(bus, BUS_NAME, OBJ_PATH, IFACE, "CloseSession",
                    g_variant_new("(s)", argv[2]), G_VARIANT_TYPE_TUPLE, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
    if (!ret){ fprintf(stderr, "CloseSession failed: %s\n", err?err->message:"err"); if (err) g_error_free(err); g_object_unref(bus); return 1; }
    gboolean ok; g_variant_get(ret, "(b)", &ok); g_variant_unref(ret); g_object_unref(bus); return ok?0:1;
  }
  if (strcmp(cmd, "warm-cache")==0){
    GVariant *ret = g_dbus_connection_call_sync(bus, BUS_NAME, OBJ_PATH, IFACE, "WarmCache",
                    g_variant_new("(s)", argv[2]), G_VARIANT_TYPE_TUPLE, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
    if (!ret){ fprintf(stderr, "WarmCache failed: %s\n", err?err->message:"err"); if (err) g_error_free(err); g_object_unref(bus); return 1; }
    gboolean ok; g_variant_get(ret, "(b)", &ok); g_variant_unref(ret); g_object_unref(bus); return ok?0:1;
  }
  if (strcmp(cmd, "get-status")==0){
    GVariant *ret = g_dbus_connection_call_sync(bus, BUS_NAME, OBJ_PATH, IFACE, "GetStatus",
                    g_variant_new("(s)", argv[2]), G_VARIANT_TYPE_TUPLE, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
    if (!ret){ fprintf(stderr, "GetStatus failed: %s\n", err?err->message:"err"); if (err) g_error_free(err); g_object_unref(bus); return 1; }
    const char *st=NULL; g_variant_get(ret, "(s)", &st); printf("%s\n", st?st:""); g_variant_unref(ret); g_object_unref(bus); return 0;
  }
  g_object_unref(bus);
  return usage(argv[0]);
}
