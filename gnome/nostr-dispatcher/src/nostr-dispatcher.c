/*
 * nostr-dispatcher — sole owner of x-scheme-handler/nostr and web+nostr.
 *
 *   nostr-dispatcher open <nostr:uri | web+nostr:uri | file | file://uri>
 *   nostr-dispatcher resolve <uri>          (dry run: kind + handler)
 *   nostr-dispatcher query-default <kind|*>
 *   nostr-dispatcher set-default <kind|A-B|*> <desktop-id>
 *   nostr-dispatcher discover <kind> [uri]  (NIP-89 suggestions; never opens)
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
#include "nd-nip89.h"
#include "nd-registry.h"

/* The GApplication id matches org.nostr.Dispatcher.desktop so GNOME Shell
 * accepts its notifications (a GNotification whose app id has no desktop
 * file is dropped) and routes their buttons back here; the API name
 * org.nostr.Dispatcher1 is owned on the same connection (nostrc-prqu.1). */
#define APP_ID "org.nostr.Dispatcher"
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

/* ---- NIP-89 suggestions (nostrc-prqu.1) ---------------------------- */

typedef struct {
  guint32 kind;
  NdTarget *target;        /* nullable */
  gboolean use_network;
  GPtrArray *handlers;     /* result */
} Nip89Job;

static void nip89_job_free(gpointer p) {
  Nip89Job *j = p;
  nd_target_free(j->target);
  if (j->handlers) g_ptr_array_unref(j->handlers);
  g_free(j);
}

static void nip89_thread(GTask *task, gpointer src, gpointer data, GCancellable *c) {
  Nip89Job *j = data;
  NdNip89Options o = {0};
  o.use_network = j->use_network;
  j->handlers = nd_nip89_discover_sync(j->kind, &o, c);
  g_task_return_boolean(task, TRUE);
}

/* Best offer: the top-ranked handler with a usable web URL for @t, else
 * the top one with a desktop app hint. */
static const NdNip89Handler *pick_offer(GPtrArray *handlers, const NdTarget *t, char **out_url) {
  *out_url = NULL;
  for (guint i = 0; handlers && i < handlers->len; i++) {
    const NdNip89Handler *h = g_ptr_array_index(handlers, i);
    char *url = nd_nip89_web_url(h, t);
    if (url) {
      *out_url = url;
      return h;
    }
  }
  for (guint i = 0; handlers && i < handlers->len; i++) {
    const NdNip89Handler *h = g_ptr_array_index(handlers, i);
    if (h->app_id) return h;
  }
  return NULL;
}

static void nip89_done(GObject *src, GAsyncResult *res, gpointer user_data) {
  GApplication *app = G_APPLICATION(user_data);
  Nip89Job *j = g_task_get_task_data(G_TASK(res));
  g_autofree char *url = NULL;
  const NdNip89Handler *h = pick_offer(j->handlers, j->target, &url);
  g_autofree char *id = g_strdup_printf("no-handler-%u", j->kind);
  g_autoptr(GNotification) n = g_notification_new("No app installed for this Nostr link");
  if (!h) {
    g_autofree char *body = g_strdup_printf(
        "No installed application handles Nostr events of kind %u, and no "
        "recommended handler (NIP-89) was found.", j->kind);
    g_notification_set_body(n, body);
    g_notification_set_priority(n, G_NOTIFICATION_PRIORITY_LOW);
    g_message("nostr-dispatcher: NIP-89: kind %u: nothing to offer", j->kind);
  } else {
    g_autofree char *body = nd_nip89_offer_body(h, j->kind, url);
    g_notification_set_body(n, body);
    /* Buttons only — no default action, so clicking the banner itself
     * never opens anything. Each button carries only the random token of
     * this presented offer (nd_nip89_offer_store). */
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    g_autofree char *uri = j->target ? nd_target_to_uri(j->target) : NULL;
    g_autofree char *label = url ? nd_nip89_offer_button(h, url) : NULL;
    g_autoptr(GError) terr = NULL;
    g_autofree char *token = nd_nip89_offer_store(NULL, j->kind, h->address,
                                                  url && label ? uri : NULL, h->app_id, now,
                                                  &terr);
    if (!token)
      g_warning("nostr-dispatcher: NIP-89: offer not actionable: %s", terr->message);
    if (token && url && label && uri)
      g_notification_add_button_with_target_value(n, label, "app.nip89-open",
                                                  g_variant_new_string(token));
    if (token && h->app_id)
      g_notification_add_button_with_target_value(n, "Show in Software", "app.nip89-install",
                                                  g_variant_new_string(token));
    g_autofree char *host = nd_nip89_url_host(url);
    g_message("nostr-dispatcher: NIP-89: kind %u: offering \"%s\" (%s%s%s), "
              "recommended by %u; not opened",
              j->kind, h->name ? h->name : "unnamed", host ? "web: " : "no web template",
              host ? host : "", h->app_id ? ", desktop app hint" : "", h->recommended_by);
  }
  g_application_send_notification(app, id, n);
  g_application_release(app);
}

/* After a NO_HANDLER failure for a known kind: look for NIP-89 handlers
 * off the main loop, then offer the best one. */
static gboolean start_nip89(GApplication *app, GAsyncResult *res) {
  gint kind = nd_dispatch_open_failed_kind(res);
  if (kind < 0) return FALSE;
  g_autoptr(NdRegistry) reg = nd_registry_new_default();
  if (!nd_registry_nip89_discovery(reg)) return FALSE;
  Nip89Job *j = g_new0(Nip89Job, 1);
  j->kind = (guint32)kind;
  j->target = nd_dispatch_open_failed_target(res);
  j->use_network = nd_registry_fetch_relay_hints(reg);
  g_application_hold(app);
  GTask *task = g_task_new(NULL, NULL, nip89_done, app);
  g_task_set_task_data(task, j, nip89_job_free);
  g_task_run_in_thread(task, nip89_thread);
  g_object_unref(task);
  return TRUE;
}

/* app.nip89-open(token) / app.nip89-install(token): the token names an
 * offer this service actually presented (random, single use, 24 h), so a
 * session-bus peer calling ActivateAction cannot open anything else. The
 * URL is then re-derived from the cached, signature-checked 31990 rather
 * than stored. */
static void action_nip89_open(GSimpleAction *a, GVariant *param, gpointer user_data) {
  guint32 kind = 0;
  g_autofree char *address = NULL, *uri = NULL, *app_id = NULL;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  if (!nd_nip89_offer_take(NULL, g_variant_get_string(param, NULL), now, &kind, &address,
                           &uri, &app_id) ||
      !address || !uri) {
    g_warning("nostr-dispatcher: NIP-89: unknown or expired offer");
    return;
  }
  g_autoptr(NdTarget) t = nd_target_parse_uri(uri, NULL);
  if (!t || (t->kind >= 0 && (guint32)t->kind != kind)) {
    g_warning("nostr-dispatcher: NIP-89: refusing an inconsistent offer");
    return;
  }
  g_autoptr(GPtrArray) cached = nd_nip89_cache_load(NULL, kind, now, ND_NIP89_OFFER_TTL_S + 3600,
                                                    NULL);
  for (guint i = 0; cached && i < cached->len; i++) {
    const NdNip89Handler *h = g_ptr_array_index(cached, i);
    if (g_strcmp0(h->address, address) != 0) continue;
    g_autofree char *url = nd_nip89_web_url(h, t);
    if (!url) break;
    g_autoptr(GError) err = NULL;
    if (!g_app_info_launch_default_for_uri(url, NULL, &err))
      g_warning("nostr-dispatcher: NIP-89: cannot open %s: %s", url, err->message);
    else
      g_message("nostr-dispatcher: NIP-89: user opened kind %u in %s", kind, url);
    return;
  }
  g_warning("nostr-dispatcher: NIP-89: offered handler for kind %u is no longer cached", kind);
}

static void action_nip89_install(GSimpleAction *a, GVariant *param, gpointer user_data) {
  guint32 kind = 0;
  g_autofree char *address = NULL, *uri = NULL, *app_id = NULL;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  if (!nd_nip89_offer_take(NULL, g_variant_get_string(param, NULL), now, &kind, &address,
                           &uri, &app_id) ||
      !app_id || !g_application_id_is_valid(app_id) || !strchr(app_id, '.')) {
    g_warning("nostr-dispatcher: NIP-89: unknown or expired offer");
    return;
  }
  g_autofree char *store_uri = g_strconcat("appstream://", app_id, NULL);
  g_autoptr(GError) err = NULL;
  if (!g_app_info_launch_default_for_uri(store_uri, NULL, &err))
    g_message("nostr-dispatcher: NIP-89: no app store handles %s: %s", store_uri, err->message);
}

static void call_done(GObject *src, GAsyncResult *res, gpointer user_data) {
  CallCtx *c = user_data;
  GError *err = NULL;
  NdOpenResult *r = nd_dispatch_open_finish(res, &err);
  if (!r) {
    g_message("nostr-dispatcher: %s", err->message);
    if (!c->want_kind &&
        !(g_error_matches(err, ND_ERROR, ND_ERROR_NO_HANDLER) && start_nip89(c->app, res)))
      notify_no_handler(c->app, err);
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
  if (!self->reg_id) return FALSE;
  /* The API name, taken after the object is exported (an activating call
   * must never reach an unexported object) and before GApplication
   * requests APP_ID. */
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
      conn, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "RequestName", g_variant_new("(su)", BUS_NAME, 0x4 /* DO_NOT_QUEUE */),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  guint32 reply = 0;
  if (r) g_variant_get(r, "(u)", &reply);
  if (reply != 1 /* PRIMARY_OWNER */ && reply != 4 /* ALREADY_OWNER */) {
    if (r)
      g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_ADDRESS_IN_USE,
                  "%s is already owned by another process", BUS_NAME);
    return FALSE;
  }
  return TRUE;
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

static void nd_app_startup(GApplication *app) {
  G_APPLICATION_CLASS(nd_app_parent_class)->startup(app);
  static const GActionEntry actions[] = {
      {"nip89-open", action_nip89_open, "s", NULL, NULL, {0}},
      {"nip89-install", action_nip89_install, "s", NULL, NULL, {0}},
  };
  g_action_map_add_action_entries(G_ACTION_MAP(app), actions, G_N_ELEMENTS(actions), app);
}

static void nd_app_class_init(NdAppClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = nd_app_finalize;
  G_APPLICATION_CLASS(klass)->dbus_register = nd_app_dbus_register;
  G_APPLICATION_CLASS(klass)->dbus_unregister = nd_app_dbus_unregister;
  G_APPLICATION_CLASS(klass)->activate = nd_app_activate;
  G_APPLICATION_CLASS(klass)->startup = nd_app_startup;
}

static void nd_app_init(NdApp *self) {}

static int run_daemon(const char *argv0) {
  g_autoptr(GApplication) app = g_object_new(ND_TYPE_APP, "application-id", APP_ID,
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

/* Print NIP-89 suggestions for @kind to stderr (CLI; never opens). */
static void print_suggestions(guint32 kind, const NdTarget *t, gboolean use_network) {
  NdNip89Options o = {0};
  o.use_network = use_network;
  g_autoptr(GPtrArray) hs = nd_nip89_discover_sync(kind, &o, NULL);
  if (!hs->len) {
    g_printerr("nostr-dispatcher: no recommended handler (NIP-89) found for kind %u\n", kind);
    return;
  }
  for (guint i = 0; i < hs->len && i < 5; i++) {
    const NdNip89Handler *h = g_ptr_array_index(hs, i);
    g_autofree char *url = nd_nip89_web_url(h, t);
    g_autofree char *body = nd_nip89_offer_body(h, kind, url);
    g_printerr("nostr-dispatcher: NIP-89 suggestion %u: %s\n", i + 1, body);
    if (url) g_printerr("    open it yourself: %s\n", url);
  }
}

static void local_done(GObject *src, GAsyncResult *res, gpointer user_data) {
  LocalRun *lr = user_data;
  g_autoptr(GError) err = NULL;
  g_autoptr(NdOpenResult) r = nd_dispatch_open_finish(res, &err);
  if (!r) {
    g_printerr("nostr-dispatcher: %s\n", err->message);
    lr->status = 1;
    gint kind = nd_dispatch_open_failed_kind(res);
    if (!lr->resolve && kind >= 0 && g_error_matches(err, ND_ERROR, ND_ERROR_NO_HANDLER)) {
      g_autoptr(NdRegistry) reg = nd_registry_new_default();
      if (nd_registry_nip89_discovery(reg)) {
        g_autoptr(NdTarget) t = nd_dispatch_open_failed_target(res);
        print_suggestions((guint32)kind, t, nd_registry_fetch_relay_hints(reg));
      }
    }
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

static int cmd_discover(const char *kind_arg, const char *uri) {
  gint kind = -1;
  if (!parse_query_kind(kind_arg, &kind) || kind < 0) {
    g_printerr("nostr-dispatcher: invalid kind '%s' (0..65535)\n", kind_arg);
    return 2;
  }
  g_autoptr(NdTarget) t = NULL;
  if (uri) {
    g_autoptr(GError) err = NULL;
    t = nd_target_parse_uri(uri, &err);
    if (!t) {
      g_printerr("nostr-dispatcher: %s\n", err->message);
      return 1;
    }
  }
  g_autoptr(NdRegistry) reg = nd_registry_new_default();
  if (!nd_registry_nip89_discovery(reg)) {
    g_printerr("nostr-dispatcher: NIP-89 discovery is disabled ([Dispatcher] nip89-discovery=false)\n");
    return 1;
  }
  NdNip89Options o = {0};
  o.use_network = nd_registry_fetch_relay_hints(reg);
  g_autoptr(GPtrArray) hs = nd_nip89_discover_sync((guint32)kind, &o, NULL);
  for (guint i = 0; i < hs->len; i++) {
    const NdNip89Handler *h = g_ptr_array_index(hs, i);
    g_autofree char *url = t ? nd_nip89_web_url(h, t) : NULL;
    g_print("%u\t%s\t%s\t%u\t%s\t%s\n", i + 1, h->name ? h->name : "-", h->address,
            h->recommended_by, url ? url : (h->web->len ? "(web)" : "-"),
            h->app_id ? h->app_id : "-");
  }
  return hs->len ? 0 : 1;
}

static void usage(FILE *f) {
  fprintf(f,
          "Usage:\n"
          "  nostr-dispatcher open <nostr:URI | web+nostr:URI | FILE | file://URI>\n"
          "  nostr-dispatcher resolve <URI>\n"
          "  nostr-dispatcher query-default <KIND | *>\n"
          "  nostr-dispatcher set-default <KIND | A-B | *> <DESKTOP-ID>\n"
          "  nostr-dispatcher discover <KIND> [URI]   (NIP-89 suggestions; opens nothing)\n"
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
  if (strcmp(cmd, "discover") == 0 && (argc == 3 || argc == 4))
    return cmd_discover(argv[2], argc == 4 ? argv[3] : NULL);
  usage(stderr);
  return 2;
}
