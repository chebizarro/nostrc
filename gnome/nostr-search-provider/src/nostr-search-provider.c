/*
 * nostr-search-provider — GNOME Shell search provider for Nostr
 * (org.gnome.Shell.SearchProvider2 on org.nostr.SearchProvider at
 * /org/nostr/SearchProvider; bead nostrc-hwwn).
 *
 *   nostr-search-provider daemon          D-Bus service (activated on demand)
 *   nostr-search-provider search TERMS…   run one search in-process and print
 *                                         the result ids and metas (debugging)
 *   nostr-search-provider launch          open the default Nostr application
 *                                         (the .desktop entry's Exec)
 *
 * Results come from the per-user session relay only. Activating a result
 * hands its nostr: URI to org.nostr.Dispatcher1.Open, so the kind→app
 * registry decides which application opens it.
 *
 * Configuration: $XDG_CONFIG_HOME/nostr/search-provider.conf
 *   [Network]
 *   resolve-nip05=true   # HTTPS .well-known look-ups for typed NIP-05 ids
 *   fetch-avatars=true   # download kind-0 pictures into the icon cache
 * Environment (tests / debugging): NOSTR_SEARCH_PROVIDER_SOCKET,
 * NOSTR_SEARCH_PROVIDER_DEADLINE_MS, NOSTR_SEARCH_PROVIDER_NO_NETWORK=1.
 */
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

#include "nd-uri.h"
#include "nsp-avatar.h"
#include "nsp-engine.h"
#include "nsp-introspection.h"

#ifdef NSP_HAVE_GDESKTOPAPPINFO
#include <gio/gdesktopappinfo.h>
#endif

#define BUS_NAME "org.nostr.SearchProvider"
#define OBJ_PATH "/org/nostr/SearchProvider"
#define IFACE "org.gnome.Shell.SearchProvider2"
#define INACTIVITY_TIMEOUT_MS 60000

#define DISPATCHER_NAME "org.nostr.Dispatcher1"
#define DISPATCHER_PATH "/org/nostr/Dispatcher1"

typedef struct {
  char *socket_path;
  guint deadline_ms;
  gboolean resolve_nip05;
  gboolean fetch_avatars;
} Config;

static void config_load(Config *c) {
  memset(c, 0, sizeof *c);
  c->resolve_nip05 = TRUE;
  c->fetch_avatars = TRUE;
  g_autofree char *path =
      g_build_filename(g_get_user_config_dir(), "nostr", "search-provider.conf", NULL);
  g_autoptr(GKeyFile) kf = g_key_file_new();
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    g_autoptr(GError) err = NULL;
    gboolean v = g_key_file_get_boolean(kf, "Network", "resolve-nip05", &err);
    if (!err) c->resolve_nip05 = v;
    g_clear_error(&err);
    v = g_key_file_get_boolean(kf, "Network", "fetch-avatars", &err);
    if (!err) c->fetch_avatars = v;
  }
  const char *s = g_getenv("NOSTR_SEARCH_PROVIDER_SOCKET");
  if (s && *s) c->socket_path = g_strdup(s);
  s = g_getenv("NOSTR_SEARCH_PROVIDER_DEADLINE_MS");
  guint64 ms = 0;
  if (s && g_ascii_string_to_unsigned(s, 10, 50, 10000, &ms, NULL)) c->deadline_ms = (guint)ms;
  if (g_strcmp0(g_getenv("NOSTR_SEARCH_PROVIDER_NO_NETWORK"), "1") == 0)
    c->resolve_nip05 = c->fetch_avatars = FALSE;
}

static NspEngine *engine_new(const Config *c, GApplication *app, NspAvatars **avatars_out) {
  g_autofree char *dir = g_build_filename(g_get_user_cache_dir(), "nostr-search", "avatars", NULL);
  *avatars_out = nsp_avatars_new(dir, c->fetch_avatars, app);
  NspEngineOptions o = {
      .socket_path = c->socket_path,
      .deadline_ms = c->deadline_ms,
      .resolve_nip05 = c->resolve_nip05,
      .avatars = *avatars_out,
  };
  return nsp_engine_new(&o);
}

/* ================================================================== */
/* Activation through nostr-dispatcher                                 */
/* ================================================================== */

/* The id is our own canonical URI, but it arrives over D-Bus: re-parse it
 * (this also refuses nsec/ncryptsec) before forwarding anything. */
static char *checked_uri(const char *id) {
  if (!id || !g_str_has_prefix(id, "nostr:") || strlen(id) > ND_URI_MAX_LEN) return NULL;
  g_autoptr(NdTarget) t = nd_target_parse_uri(id, NULL);
  return t ? nd_target_to_uri(t) : NULL;
}

typedef struct {
  GApplication *app;
  char *uri;
} OpenCtx;

static void fallback_open(const char *uri) {
  g_autoptr(GError) err = NULL;
  if (!g_app_info_launch_default_for_uri(uri, NULL, &err))
    g_warning("nostr-search-provider: cannot open %s: %s", uri, err->message);
}

static void on_dispatcher_open(GObject *src, GAsyncResult *res, gpointer user_data) {
  OpenCtx *c = user_data;
  g_autoptr(GError) err = NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  if (r) {
    const char *desktop_id = NULL;
    g_variant_get(r, "(&s)", &desktop_id);
    g_message("nostr-search-provider: opened %s with %s", c->uri, desktop_id);
  } else if (g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
             g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
             g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_SERVICE_NOT_FOUND)) {
    /* nostr-dispatcher not installed: the nostr: scheme default. */
    fallback_open(c->uri);
  } else {
    g_dbus_error_strip_remote_error(err);
    g_warning("nostr-search-provider: nostr-dispatcher could not open %s: %s", c->uri,
              err->message);
  }
  g_application_release(c->app);
  g_free(c->uri);
  g_free(c);
}

static void activate_result(GApplication *app, GDBusConnection *conn, const char *id) {
  g_autofree char *uri = checked_uri(id);
  if (!uri) {
    g_warning("nostr-search-provider: refusing to activate an invalid result id");
    return;
  }
  OpenCtx *c = g_new0(OpenCtx, 1);
  c->app = app;
  c->uri = g_steal_pointer(&uri);
  g_application_hold(app);
  GVariantBuilder pd;
  g_variant_builder_init(&pd, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_call(conn, DISPATCHER_NAME, DISPATCHER_PATH, DISPATCHER_NAME, "Open",
                         g_variant_new("(sa{sv})", c->uri, &pd), G_VARIANT_TYPE("(s)"),
                         G_DBUS_CALL_FLAGS_NONE, 20000, NULL, on_dispatcher_open, c);
}

/* ================================================================== */
/* Service                                                             */
/* ================================================================== */

#define NSP_TYPE_APP (nsp_app_get_type())
G_DECLARE_FINAL_TYPE(NspApp, nsp_app, NSP, APP, GApplication)

struct _NspApp {
  GApplication parent;
  GDBusNodeInfo *node;
  guint reg_id;
  NspEngine *engine;
  NspAvatars *avatars;
};

G_DEFINE_FINAL_TYPE(NspApp, nsp_app, G_TYPE_APPLICATION)

typedef struct {
  GApplication *app;
  GDBusMethodInvocation *inv;
} CallCtx;

static void search_done(GObject *src, GAsyncResult *res, gpointer user_data) {
  CallCtx *c = user_data;
  NspSearchStats st = {0};
  g_auto(GStrv) ids = nsp_engine_search_finish(res, &st);
  g_dbus_method_invocation_return_value(
      c->inv, g_variant_new("(@as)", g_variant_new_strv((const char *const *)ids, -1)));
  g_application_release(c->app);
  g_free(c);
}

static void method_call(GDBusConnection *conn, const char *sender, const char *path,
                        const char *iface, const char *method, GVariant *params,
                        GDBusMethodInvocation *inv, gpointer user_data) {
  NspApp *self = NSP_APP(user_data);
  GApplication *app = G_APPLICATION(self);

  if (strcmp(method, "GetInitialResultSet") == 0 ||
      strcmp(method, "GetSubsearchResultSet") == 0) {
    g_autofree const char **terms = NULL, **previous = NULL;
    if (method[3] == 'I')
      g_variant_get(params, "(^a&s)", &terms);
    else
      g_variant_get(params, "(^a&s^a&s)", &previous, &terms);
    CallCtx *c = g_new0(CallCtx, 1);
    c->app = app;
    c->inv = inv;
    g_application_hold(app); /* no idle exit mid-search */
    /* The engine copies what it needs before returning. */
    nsp_engine_search_async(self->engine, terms, previous ? previous : NULL, search_done, c);
    return;
  }
  if (strcmp(method, "GetResultMetas") == 0) {
    g_autofree const char **ids = NULL;
    g_variant_get(params, "(^a&s)", &ids);
    GVariant *metas = nsp_engine_result_metas(self->engine, ids);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@aa{sv})", metas));
    return;
  }
  if (strcmp(method, "ActivateResult") == 0) {
    const char *id = NULL;
    g_variant_get(params, "(&s^a&su)", &id, NULL, NULL);
    activate_result(app, conn, id);
    g_dbus_method_invocation_return_value(inv, NULL);
    return;
  }
  if (strcmp(method, "LaunchSearch") == 0) {
    /* No Nostr application declares a search entry point yet (GNostr has
     * no search action; see README), so clicking the provider icon does
     * nothing rather than open an app that would drop the terms. */
    g_debug("nostr-search-provider: LaunchSearch: no application declares a search action");
    g_dbus_method_invocation_return_value(inv, NULL);
    return;
  }
  if (strcmp(method, "XUbuntuCancel") == 0) {
    nsp_engine_cancel_all(self->engine);
    g_dbus_method_invocation_return_value(inv, NULL);
    return;
  }
  g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                        "unknown method %s", method);
}

static const GDBusInterfaceVTable vtable = {method_call, NULL, NULL, {0}};

/* Export BEFORE the bus name is owned (GApplication calls dbus_register
 * first) so an activating call never reaches an unexported object. */
static gboolean nsp_app_dbus_register(GApplication *gapp, GDBusConnection *conn,
                                      const char *object_path, GError **error) {
  NspApp *self = NSP_APP(gapp);
  if (!G_APPLICATION_CLASS(nsp_app_parent_class)->dbus_register(gapp, conn, object_path, error))
    return FALSE;
  self->node = g_dbus_node_info_new_for_xml(NSP_SEARCH_PROVIDER2_XML, error);
  if (!self->node) return FALSE;
  self->reg_id = g_dbus_connection_register_object(
      conn, OBJ_PATH, g_dbus_node_info_lookup_interface(self->node, IFACE), &vtable, self, NULL,
      error);
  return self->reg_id != 0;
}

static void nsp_app_dbus_unregister(GApplication *gapp, GDBusConnection *conn,
                                    const char *object_path) {
  NspApp *self = NSP_APP(gapp);
  if (self->reg_id) g_dbus_connection_unregister_object(conn, self->reg_id);
  self->reg_id = 0;
  G_APPLICATION_CLASS(nsp_app_parent_class)->dbus_unregister(gapp, conn, object_path);
}

static void nsp_app_startup(GApplication *gapp) {
  NspApp *self = NSP_APP(gapp);
  G_APPLICATION_CLASS(nsp_app_parent_class)->startup(gapp);
  Config c;
  config_load(&c);
  self->engine = engine_new(&c, gapp, &self->avatars);
  g_free(c.socket_path);
}

static void nsp_app_shutdown(GApplication *gapp) {
  NspApp *self = NSP_APP(gapp);
  g_clear_pointer(&self->engine, nsp_engine_free);
  g_clear_pointer(&self->avatars, nsp_avatars_free);
  G_APPLICATION_CLASS(nsp_app_parent_class)->shutdown(gapp);
}

static void nsp_app_finalize(GObject *obj) {
  NspApp *self = NSP_APP(obj);
  g_clear_pointer(&self->node, g_dbus_node_info_unref);
  G_OBJECT_CLASS(nsp_app_parent_class)->finalize(obj);
}

static void nsp_app_activate(GApplication *app) { /* service: nothing to show */ }

static void nsp_app_class_init(NspAppClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = nsp_app_finalize;
  GApplicationClass *ac = G_APPLICATION_CLASS(klass);
  ac->dbus_register = nsp_app_dbus_register;
  ac->dbus_unregister = nsp_app_dbus_unregister;
  ac->startup = nsp_app_startup;
  ac->shutdown = nsp_app_shutdown;
  ac->activate = nsp_app_activate;
}

static void nsp_app_init(NspApp *self) {}

static int run_daemon(const char *argv0) {
  g_autoptr(GApplication) app = g_object_new(NSP_TYPE_APP, "application-id", BUS_NAME, "flags",
                                             G_APPLICATION_IS_SERVICE, NULL);
  g_application_set_inactivity_timeout(app, INACTIVITY_TIMEOUT_MS);
  char *argv[] = {(char *)argv0, NULL};
  return g_application_run(app, 1, argv);
}

/* ================================================================== */
/* CLI                                                                 */
/* ================================================================== */

typedef struct {
  GMainLoop *loop;
  char **ids;
  NspSearchStats st;
} CliRun;

static void cli_done(GObject *src, GAsyncResult *res, gpointer user_data) {
  CliRun *r = user_data;
  r->ids = nsp_engine_search_finish(res, &r->st);
  g_main_loop_quit(r->loop);
}

static int cmd_search(int n, char **terms) {
  Config c;
  config_load(&c);
  c.fetch_avatars = FALSE; /* CLI: report the cache state, never download */
  NspAvatars *avatars = NULL;
  NspEngine *e = engine_new(&c, NULL, &avatars);
  g_autofree char **t = g_new0(char *, n + 1);
  memcpy(t, terms, sizeof(char *) * (gsize)n);
  CliRun r = {g_main_loop_new(NULL, FALSE), NULL, {0}};
  nsp_engine_search_async(e, (const char *const *)t, NULL, cli_done, &r);
  g_main_loop_run(r.loop);
  g_print("# %u result(s) in %" G_GINT64_FORMAT " ms; relay: %s, %u REQ(s)%s%s%s\n",
          g_strv_length(r.ids), r.st.elapsed_us / 1000, nsp_relay_status_name(r.st.primary),
          r.st.requests, r.st.deadline_hit ? ", deadline hit" : "",
          r.st.fallback_scan ? ", NIP-50 unsupported: local scan" : "",
          r.st.circuit_open ? ", relay skipped (unresponsive)" : "");
  g_autoptr(GVariant) metas = g_variant_ref_sink(nsp_engine_result_metas(e, (const char *const *)r.ids));
  for (gsize i = 0; i < g_variant_n_children(metas); i++) {
    g_autoptr(GVariant) m = g_variant_get_child_value(metas, i);
    const char *id = NULL, *name = NULL, *desc = NULL;
    g_variant_lookup(m, "id", "&s", &id);
    g_variant_lookup(m, "name", "&s", &name);
    g_variant_lookup(m, "description", "&s", &desc);
    g_print("%s\n  %s\n  %s\n", id, name, desc);
  }
  g_strfreev(r.ids);
  g_main_loop_unref(r.loop);
  nsp_engine_free(e);
  nsp_avatars_free(avatars);
  g_free(c.socket_path);
  return 0;
}

/* Desktop entry Exec: open the application nostr-dispatcher uses for
 * links of unknown kind (the user's general Nostr client). */
static int cmd_launch(void) {
  g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  g_autoptr(GAppInfo) info = NULL;
#ifdef NSP_HAVE_GDESKTOPAPPINFO
  if (bus) {
    g_autoptr(GVariant) r = g_dbus_connection_call_sync(
        bus, DISPATCHER_NAME, DISPATCHER_PATH, DISPATCHER_NAME, "QueryDefault",
        g_variant_new("(i)", -1), G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
    const char *id = NULL;
    if (r) g_variant_get(r, "(&s)", &id);
    if (id) info = G_APP_INFO(g_desktop_app_info_new(id));
  }
#endif
  if (!info) info = g_app_info_get_default_for_uri_scheme("nostr");
  if (!info || g_str_has_prefix(g_app_info_get_id(info) ? g_app_info_get_id(info) : "",
                                "org.nostr.Dispatcher")) {
    g_printerr("nostr-search-provider: no Nostr application is installed\n");
    return 1;
  }
  g_autoptr(GError) err = NULL;
  if (!g_app_info_launch(info, NULL, NULL, &err)) {
    g_printerr("nostr-search-provider: %s\n", err->message);
    return 1;
  }
  return 0;
}

static void usage(FILE *f) {
  fprintf(f, "Usage:\n"
             "  nostr-search-provider daemon          D-Bus service (org.nostr.SearchProvider)\n"
             "  nostr-search-provider search TERMS…   one search against the session relay\n"
             "  nostr-search-provider launch          open the default Nostr application\n");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(stderr);
    return 2;
  }
  const char *cmd = argv[1];
  if (!strcmp(cmd, "--help") || !strcmp(cmd, "-h") || !strcmp(cmd, "help")) {
    usage(stdout);
    return 0;
  }
  if (!strcmp(cmd, "daemon") && argc == 2) return run_daemon(argv[0]);
  if (!strcmp(cmd, "search") && argc >= 3) return cmd_search(argc - 2, argv + 2);
  if (!strcmp(cmd, "launch") && argc == 2) return cmd_launch();
  usage(stderr);
  return 2;
}
