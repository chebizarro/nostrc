/*
 * nostr-dispatcher — sole owner of x-scheme-handler/nostr and web+nostr.
 *
 *   nostr-dispatcher open <nostr:uri | web+nostr:uri | file | file://uri>
 *   nostr-dispatcher resolve <uri>          (dry run: kind + handler)
 *   nostr-dispatcher query-default <kind|*>
 *   nostr-dispatcher set-default <kind|A-B|*> <desktop-id>
 *   nostr-dispatcher daemon                 (org.nostr.Dispatcher1 service)
 *
 * `open <uri>` forwards to the org.nostr.Dispatcher1 service (D-Bus
 * activated through nostr-dispatcher.service) and only dispatches
 * in-process when no session bus / service is available. The CLI never
 * owns the bus name. Event files (application/vnd.nostr.event+json) are
 * always handled in-process — they need no network.
 */
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "nd-dispatch.h"
#include "nd-error.h"
#include "nd-event.h"
#include "nd-introspection.h"
#include "nd-registry.h"

#define BUS_NAME "org.nostr.Dispatcher1"
#define OBJ_PATH "/org/nostr/Dispatcher1"
#define IFACE "org.nostr.Dispatcher1"
#define INACTIVITY_TIMEOUT_MS 30000

static const GDBusErrorEntry nd_dbus_errors[] = {
    {ND_ERROR_INVALID_URI, "org.nostr.Dispatcher1.Error.InvalidUri"},
    {ND_ERROR_FORBIDDEN, "org.nostr.Dispatcher1.Error.Forbidden"},
    {ND_ERROR_NOT_FOUND, "org.nostr.Dispatcher1.Error.NotFound"},
    {ND_ERROR_NO_HANDLER, "org.nostr.Dispatcher1.Error.NoHandler"},
    {ND_ERROR_INVALID_EVENT, "org.nostr.Dispatcher1.Error.InvalidEvent"},
    {ND_ERROR_LAUNCH_FAILED, "org.nostr.Dispatcher1.Error.LaunchFailed"},
};

static void register_dbus_errors(void) {
  static gsize once = 0;
  g_dbus_error_register_error_domain("nostr-dispatcher-error-quark", &once,
                                     nd_dbus_errors, G_N_ELEMENTS(nd_dbus_errors));
}

/* Whitelist platform_data: only string activation tokens survive. */
static char *activation_token_from(GVariant *platform_data) {
  if (!platform_data) return NULL;
  const char *keys[] = {"activation-token", "desktop-startup-id"};
  for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
    const char *v = NULL;
    if (g_variant_lookup(platform_data, keys[i], "&s", &v) && v && *v &&
        strlen(v) < 256 && g_utf8_validate(v, -1, NULL))
      return g_strdup(v);
  }
  return NULL;
}

/* ================================================================== */
/* Daemon                                                              */
/* ================================================================== */

#define ND_TYPE_APP (nd_app_get_type())
G_DECLARE_FINAL_TYPE(NdApp, nd_app, ND, APP, GApplication)

struct _NdApp {
  GApplication parent;
  GDBusNodeInfo *node;
  guint reg_id;
};

G_DEFINE_FINAL_TYPE(NdApp, nd_app, G_TYPE_APPLICATION)

typedef struct {
  GApplication *app;
  GDBusMethodInvocation *inv;
  gboolean want_kind; /* Resolve */
} CallCtx;

static void notify_no_handler(GApplication *app, const GError *error) {
  if (!g_error_matches(error, ND_ERROR, ND_ERROR_NO_HANDLER)) return;
  g_autoptr(GNotification) n = g_notification_new("No application for this Nostr link");
  g_notification_set_body(n, error->message);
  g_notification_set_priority(n, G_NOTIFICATION_PRIORITY_LOW);
  g_application_send_notification(app, "no-handler", n);
}

static void call_done(GObject *src, GAsyncResult *res, gpointer user_data) {
  CallCtx *c = user_data;
  GError *err = NULL;
  NdOpenResult *r = nd_dispatch_open_finish(res, &err);
  if (!r) {
    g_message("nostr-dispatcher: %s", err->message);
    if (!c->want_kind) notify_no_handler(c->app, err);
    g_dbus_method_invocation_take_error(c->inv, err);
  } else {
    g_message("nostr-dispatcher: kind %d -> %s (%s)%s", r->kind, r->desktop_id,
              nd_source_name(r->source),
              c->want_kind ? " [resolve]" : (r->handed_off ? " [Handler1]" : ""));
    if (c->want_kind)
      g_dbus_method_invocation_return_value(c->inv,
                                            g_variant_new("(is)", r->kind, r->desktop_id));
    else
      g_dbus_method_invocation_return_value(c->inv, g_variant_new("(s)", r->desktop_id));
    nd_open_result_free(r);
  }
  g_application_release(c->app);
  g_free(c);
}

static void method_call(GDBusConnection *conn, const char *sender, const char *path,
                        const char *iface, const char *method, GVariant *params,
                        GDBusMethodInvocation *inv, gpointer user_data) {
  GApplication *app = G_APPLICATION(user_data);

  if (strcmp(method, "QueryDefault") == 0) {
    gint32 kind = -1;
    g_variant_get(params, "(i)", &kind);
    g_autoptr(NdRegistry) reg = nd_registry_new_default();
    g_autofree char *id = nd_registry_choose(reg, kind < 0 ? -1 : kind, NULL);
    if (id)
      g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", id));
    else
      g_dbus_method_invocation_return_error(inv, ND_ERROR, ND_ERROR_NO_HANDLER,
                                            "no handler for kind %d", kind);
    return;
  }

  CallCtx *c = g_new0(CallCtx, 1);
  c->app = app;
  c->inv = inv;
  /* Hold the service for the whole request so the inactivity timeout
   * cannot exit mid-fetch. */
  g_application_hold(app);

  NdOpenOptions o = {0};
  if (strcmp(method, "Open") == 0) {
    const char *uri = NULL;
    g_autoptr(GVariant) pd = NULL;
    g_variant_get(params, "(&s@a{sv})", &uri, &pd);
    o.activation_token = activation_token_from(pd);
    nd_dispatch_open_uri_async(nd_registry_new_default(), uri, &o, NULL, call_done, c);
  } else if (strcmp(method, "OpenEvent") == 0) {
    const char *json = NULL;
    g_autofree const char **relays = NULL;
    g_autoptr(GVariant) pd = NULL;
    g_variant_get(params, "(&s^a&s@a{sv})", &json, &relays, &pd);
    o.activation_token = activation_token_from(pd);
    nd_dispatch_open_event_async(nd_registry_new_default(), json, relays, NULL, &o, NULL,
                                 call_done, c);
  } else if (strcmp(method, "Resolve") == 0) {
    const char *uri = NULL;
    g_variant_get(params, "(&s)", &uri);
    c->want_kind = TRUE;
    o.dry_run = TRUE;
    nd_dispatch_open_uri_async(nd_registry_new_default(), uri, &o, NULL, call_done, c);
  } else {
    g_application_release(app);
    g_free(c);
    g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                          "unknown method %s", method);
    return;
  }
  g_free(o.activation_token);
}

static const GDBusInterfaceVTable vtable = {method_call, NULL, NULL, {0}};

/* Export BEFORE the bus name is requested (GApplication calls dbus_register
 * first), so an activating call can never hit an unexported object. */
static gboolean nd_app_dbus_register(GApplication *gapp, GDBusConnection *conn,
                                     const char *object_path, GError **error) {
  NdApp *self = ND_APP(gapp);
  if (!G_APPLICATION_CLASS(nd_app_parent_class)->dbus_register(gapp, conn, object_path, error))
    return FALSE;
  self->node = g_dbus_node_info_new_for_xml(ND_DISPATCHER1_XML, error);
  if (!self->node) return FALSE;
  self->reg_id = g_dbus_connection_register_object(
      conn, OBJ_PATH, g_dbus_node_info_lookup_interface(self->node, IFACE), &vtable,
      self, NULL, error);
  return self->reg_id != 0;
}

static void nd_app_dbus_unregister(GApplication *gapp, GDBusConnection *conn,
                                   const char *object_path) {
  NdApp *self = ND_APP(gapp);
  if (self->reg_id) g_dbus_connection_unregister_object(conn, self->reg_id);
  self->reg_id = 0;
  G_APPLICATION_CLASS(nd_app_parent_class)->dbus_unregister(gapp, conn, object_path);
}

static void nd_app_finalize(GObject *obj) {
  NdApp *self = ND_APP(obj);
  g_clear_pointer(&self->node, g_dbus_node_info_unref);
  G_OBJECT_CLASS(nd_app_parent_class)->finalize(obj);
}

static void nd_app_activate(GApplication *app) { /* service: nothing to show */ }

static void nd_app_class_init(NdAppClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = nd_app_finalize;
  G_APPLICATION_CLASS(klass)->dbus_register = nd_app_dbus_register;
  G_APPLICATION_CLASS(klass)->dbus_unregister = nd_app_dbus_unregister;
  G_APPLICATION_CLASS(klass)->activate = nd_app_activate;
}

static void nd_app_init(NdApp *self) {}

static int run_daemon(const char *argv0) {
  g_autoptr(GApplication) app = g_object_new(ND_TYPE_APP, "application-id", BUS_NAME,
                                             "flags", G_APPLICATION_IS_SERVICE, NULL);
  g_application_set_inactivity_timeout(app, INACTIVITY_TIMEOUT_MS);
  char *argv[] = {(char *)argv0, NULL};
  return g_application_run(app, 1, argv);
}

/* ================================================================== */
/* CLI                                                                 */
/* ================================================================== */

typedef struct {
  GMainLoop *loop;
  int status;
  gboolean resolve;
} LocalRun;

static void local_done(GObject *src, GAsyncResult *res, gpointer user_data) {
  LocalRun *lr = user_data;
  g_autoptr(GError) err = NULL;
  g_autoptr(NdOpenResult) r = nd_dispatch_open_finish(res, &err);
  if (!r) {
    g_printerr("nostr-dispatcher: %s\n", err->message);
    lr->status = 1;
  } else if (lr->resolve) {
    g_print("%d\t%s\t%s\n", r->kind, r->desktop_id, nd_source_name(r->source));
  } else {
    g_print("%s\n", r->desktop_id);
    g_printerr("nostr-dispatcher: kind %d -> %s (%s)%s\n", r->kind, r->desktop_id,
               nd_source_name(r->source), r->handed_off ? " via Handler1" : "");
  }
  g_main_loop_quit(lr->loop);
}

static char *env_activation_token(void) {
  const char *t = g_getenv("XDG_ACTIVATION_TOKEN");
  if (!t || !*t) t = g_getenv("DESKTOP_STARTUP_ID");
  return (t && *t) ? g_strdup(t) : NULL;
}

static int local_open_uri(const char *uri, gboolean resolve) {
  LocalRun lr = {g_main_loop_new(NULL, FALSE), 0, resolve};
  NdOpenOptions o = {0};
  o.dry_run = resolve;
  o.activation_token = env_activation_token();
  nd_dispatch_open_uri_async(nd_registry_new_default(), uri, &o, NULL, local_done, &lr);
  g_free(o.activation_token);
  g_main_loop_run(lr.loop);
  g_main_loop_unref(lr.loop);
  return lr.status;
}

static int local_open_file(GFile *file) {
  g_autofree char *contents = NULL;
  gsize len = 0;
  g_autoptr(GError) err = NULL;
  g_autoptr(GFileInfo) fi = g_file_query_info(
      file, G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
      G_FILE_QUERY_INFO_NONE, NULL, &err);
  if (!fi) {
    g_printerr("nostr-dispatcher: %s\n", err->message);
    return 1;
  }
  if (g_file_info_get_file_type(fi) != G_FILE_TYPE_REGULAR) {
    g_printerr("nostr-dispatcher: not a regular file\n");
    return 1;
  }
  if (g_file_info_get_size(fi) > ND_EVENT_MAX_JSON) {
    g_printerr("nostr-dispatcher: event file larger than 1 MiB\n");
    return 1;
  }
  if (!g_file_load_contents(file, NULL, &contents, &len, NULL, &err)) {
    g_printerr("nostr-dispatcher: %s\n", err->message);
    return 1;
  }
  g_autofree char *file_uri = g_file_get_uri(file);
  g_autofree char *json = g_strndup(contents, len);
  LocalRun lr = {g_main_loop_new(NULL, FALSE), 0, FALSE};
  NdOpenOptions o = {0};
  o.activation_token = env_activation_token();
  nd_dispatch_open_event_async(nd_registry_new_default(), json, NULL, file_uri, &o, NULL,
                               local_done, &lr);
  g_free(o.activation_token);
  g_main_loop_run(lr.loop);
  g_main_loop_unref(lr.loop);
  return lr.status;
}

/* Returns -1 if the service is unreachable (caller falls back in-process). */
static int bus_open_uri(const char *uri) {
  if (g_strcmp0(g_getenv("NOSTR_DISPATCHER_NO_DBUS"), "1") == 0) return -1;
  g_autoptr(GError) err = NULL;
  g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) return -1;
  GVariantBuilder pd;
  g_variant_builder_init(&pd, G_VARIANT_TYPE_VARDICT);
  g_autofree char *tok = env_activation_token();
  if (tok) g_variant_builder_add(&pd, "{sv}", "activation-token", g_variant_new_string(tok));
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
      bus, BUS_NAME, OBJ_PATH, IFACE, "Open", g_variant_new("(sa{sv})", uri, &pd),
      G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 20000, NULL, &err);
  if (r) {
    const char *id = NULL;
    g_variant_get(r, "(&s)", &id);
    g_print("%s\n", id);
    return 0;
  }
  if (g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
      g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
      g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_CHILD_EXITED) ||
      g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_FAILED) ||
      g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_SERVICE_NOT_FOUND) ||
      g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD) ||
      g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT) ||
      g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_NO_REPLY) ||
      g_error_matches(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
    g_debug("nostr-dispatcher: service unavailable (%s); dispatching in-process",
            err->message);
    return -1;
  }
  g_dbus_error_strip_remote_error(err);
  g_printerr("nostr-dispatcher: %s\n", err->message);
  return 1;
}

static gboolean looks_like_nostr_uri(const char *arg) {
  g_autofree char *l = g_ascii_strdown(arg, -1);
  return g_str_has_prefix(l, "nostr:") || g_str_has_prefix(l, "web+nostr:") ||
         g_str_has_prefix(l, "note1") || g_str_has_prefix(l, "nevent1") ||
         g_str_has_prefix(l, "naddr1") || g_str_has_prefix(l, "npub1") ||
         g_str_has_prefix(l, "nprofile1") || g_str_has_prefix(l, "nsec1");
}

static int cmd_open(const char *arg) {
  if (!looks_like_nostr_uri(arg)) {
    g_autoptr(GFile) f = g_str_has_prefix(arg, "file:") ? g_file_new_for_uri(arg)
                                                          : g_file_new_for_path(arg);
    return local_open_file(f);
  }
  int rc = bus_open_uri(arg);
  return rc >= 0 ? rc : local_open_uri(arg, FALSE);
}

static gboolean parse_query_kind(const char *arg, gint *out) {
  if (strcmp(arg, "*") == 0 || strcmp(arg, "-1") == 0) {
    *out = -1;
    return TRUE;
  }
  guint64 v = 0;
  if (!g_ascii_string_to_unsigned(arg, 10, 0, 65535, &v, NULL)) return FALSE;
  *out = (gint)v;
  return TRUE;
}

static int cmd_query_default(const char *arg) {
  gint kind = -1;
  if (!parse_query_kind(arg, &kind)) {
    g_printerr("nostr-dispatcher: invalid kind '%s' (0..65535 or *)\n", arg);
    return 2;
  }
  g_autoptr(NdRegistry) reg = nd_registry_new_default();
  NdSource src = ND_SOURCE_NONE;
  g_autofree char *id = nd_registry_choose(reg, kind, &src);
  if (!id) {
    g_printerr("nostr-dispatcher: no handler for kind %s\n", arg);
    return 1;
  }
  g_print("%s\n", id);
  g_printerr("(from %s)\n", nd_source_name(src));
  return 0;
}

static int cmd_set_default(const char *key, const char *desktop_id) {
  g_autoptr(GError) err = NULL;
  g_autofree char *path = nd_registry_user_config_file();
  if (!nd_registry_set_default(path, key, desktop_id, &err)) {
    g_printerr("nostr-dispatcher: %s\n", err->message);
    return 1;
  }
  g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new(desktop_id);
  if (!info)
    g_printerr("nostr-dispatcher: warning: %s is not installed; it will be skipped "
               "until it is\n", desktop_id);
  return 0;
}

static void usage(FILE *f) {
  fprintf(f,
          "Usage:\n"
          "  nostr-dispatcher open <nostr:URI | web+nostr:URI | FILE | file://URI>\n"
          "  nostr-dispatcher resolve <URI>\n"
          "  nostr-dispatcher query-default <KIND | *>\n"
          "  nostr-dispatcher set-default <KIND | A-B | *> <DESKTOP-ID>\n"
          "  nostr-dispatcher daemon\n"
          "\n"
          "Routes nostr: links to the application registered for the event kind.\n"
          "Registry: X-Nostr-Kinds= in .desktop files, overridden by\n"
          "$XDG_CONFIG_HOME/nostr/handlers.list. See README.md.\n");
}

int main(int argc, char **argv) {
  register_dbus_errors();
  if (argc < 2) {
    usage(stderr);
    return 2;
  }
  const char *cmd = argv[1];
  if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
    usage(stdout);
    return 0;
  }
  if (strcmp(cmd, "daemon") == 0 && argc == 2) return run_daemon(argv[0]);
  if (strcmp(cmd, "open") == 0 && argc == 3) return cmd_open(argv[2]);
  if (strcmp(cmd, "resolve") == 0 && argc == 3) return local_open_uri(argv[2], TRUE);
  if (strcmp(cmd, "query-default") == 0 && argc == 3) return cmd_query_default(argv[2]);
  if (strcmp(cmd, "set-default") == 0 && argc == 4) return cmd_set_default(argv[2], argv[3]);
  usage(stderr);
  return 2;
}
