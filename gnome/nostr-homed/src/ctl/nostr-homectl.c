#include "nostr_homectl.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <gio/gio.h>

static const char *BUS_NAME = "org.nostr.Homed1";
static const char *OBJ_PATH = "/org/nostr/Homed1";
static const char *IFACE = "org.nostr.Homed1";

static int usage(const char *argv0){
  fprintf(stderr, "Usage: %s [--daemon]\n       %s <open-session|close-session|warm-cache|get-status> <arg>\n", argv0, argv0);
  return 2;
}

/* Library API (local execution) */
int nh_open_session(const char *username){
  printf("nostr-homectl: OpenSession %s (stub)\n", username);
  return 0;
}
int nh_close_session(const char *username){
  printf("nostr-homectl: CloseSession %s (stub)\n", username);
  return 0;
}
int nh_warm_cache(const char *npub_hex){
  printf("nostr-homectl: WarmCache %s (stub)\n", npub_hex);
  return 0;
}
int nh_get_status(const char *username, char *buf, size_t buflen){
  if (!buf || buflen==0) return -1;
  snprintf(buf, buflen, "user=%s status=ok", username ? username : "");
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

static const GDBusInterfaceVTable vtable = { on_method_call, NULL, NULL };

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
