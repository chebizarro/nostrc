/* nd-dispatch.c — see nd-dispatch.h. */
#include "nd-dispatch.h"
#include "nd-error.h"
#include "nd-event.h"
#include "nd-fetch.h"
#include "nd-uri.h"

#include <gio/gdesktopappinfo.h>
#include <string.h>

#define HANDLER1_IFACE "org.nostr.Handler1"
#define HANDOFF_TIMEOUT_MS 2000
#define EVENT_MIME "application/vnd.nostr.event+json"

void nd_open_result_free(NdOpenResult *r) {
  if (!r) return;
  g_free(r->desktop_id);
  g_free(r->launched_uri);
  g_free(r);
}

char *nd_handler_bus_name(const char *desktop_id) {
  if (!desktop_id || !g_str_has_suffix(desktop_id, ".desktop")) return NULL;
  char *name = g_strndup(desktop_id, strlen(desktop_id) - strlen(".desktop"));
  if (!g_dbus_is_name(name) || g_dbus_is_unique_name(name)) {
    g_free(name);
    return NULL;
  }
  return name;
}

char *nd_handler_object_path(const char *bus_name) {
  if (!bus_name) return NULL;
  char *path = g_strconcat("/", bus_name, NULL);
  for (char *p = path; *p; p++) {
    if (*p == '.') *p = '/';
    else if (*p == '-') *p = '_';
  }
  return path;
}

/* ------------------------------------------------------------------ */

typedef struct {
  NdRegistry *reg;
  NdTarget *target;      /* NULL for event input without an id */
  NdEvent *event;        /* validated or local event, if any */
  char *file_uri;        /* event input from a file */
  char **relays;         /* hints for Handler1 */
  NdOpenOptions opts;
  NdOpenResult *result;
} OpenData;

static void open_data_free(gpointer p) {
  OpenData *d = p;
  nd_registry_free(d->reg);
  nd_target_free(d->target);
  nd_event_free(d->event);
  g_free(d->file_uri);
  g_strfreev(d->relays);
  g_free(d->opts.activation_token);
  nd_open_result_free(d->result);
  g_free(d);
}

static void return_result(GTask *task) {
  OpenData *d = g_task_get_task_data(task);
  g_task_return_pointer(task, g_steal_pointer(&d->result),
                        (GDestroyNotify)nd_open_result_free);
  g_object_unref(task);
}

static gboolean handler_is_running(const char *desktop_id) {
  g_autofree char *name = nd_handler_bus_name(desktop_id);
  if (!name) return FALSE;
  g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  if (!bus) return FALSE;
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
      bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "NameHasOwner", g_variant_new("(s)", name), G_VARIANT_TYPE("(b)"),
      G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
  gboolean owned = FALSE;
  if (r) g_variant_get(r, "(b)", &owned);
  return owned;
}

static gboolean app_accepts_event_files(GDesktopAppInfo *info) {
  const char *const *types = g_app_info_get_supported_types(G_APP_INFO(info));
  for (guint i = 0; types && types[i]; i++)
    if (strcmp(types[i], EVENT_MIME) == 0) return TRUE;
  return FALSE;
}

/* URI we hand to the app: canonical nostr: URI, or the source file. */
static char *launch_uri_for(OpenData *d, GDesktopAppInfo *info) {
  if (d->file_uri && (!d->target || app_accepts_event_files(info)))
    return g_strdup(d->file_uri);
  if (!d->target) return NULL;
  /* Carry the resolved kind into the nevent so the handler need not refetch
   * just to learn it. */
  if (d->target->entity == ND_ENTITY_EVENT && d->target->kind < 0 && d->result->kind >= 0)
    d->target->kind = d->result->kind;
  return nd_target_to_uri(d->target);
}

static void launch_uri(GTask *task) {
  OpenData *d = g_task_get_task_data(task);
  g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new(d->result->desktop_id);
  if (!info) {
    g_task_return_new_error(task, ND_ERROR, ND_ERROR_LAUNCH_FAILED,
                            "handler %s is no longer installed", d->result->desktop_id);
    g_object_unref(task);
    return;
  }
  g_autofree char *uri = launch_uri_for(d, info);
  if (!uri) {
    g_task_return_new_error(task, ND_ERROR, ND_ERROR_LAUNCH_FAILED,
                            "cannot build a URI for this event");
    g_object_unref(task);
    return;
  }
  g_autoptr(GAppLaunchContext) ctx = g_app_launch_context_new();
  if (d->opts.activation_token) {
    g_app_launch_context_setenv(ctx, "XDG_ACTIVATION_TOKEN", d->opts.activation_token);
    g_app_launch_context_setenv(ctx, "DESKTOP_STARTUP_ID", d->opts.activation_token);
  }
  GList *uris = g_list_prepend(NULL, uri);
  g_autoptr(GError) err = NULL;
  gboolean ok = g_app_info_launch_uris(G_APP_INFO(info), uris, ctx, &err);
  g_list_free(uris);
  if (!ok) {
    g_task_return_new_error(task, ND_ERROR, ND_ERROR_LAUNCH_FAILED,
                            "launching %s failed: %s", d->result->desktop_id, err->message);
    g_object_unref(task);
    return;
  }
  d->result->launched_uri = g_steal_pointer(&uri);
  return_result(task);
}

static void handoff_done(GObject *src, GAsyncResult *res, gpointer user_data) {
  GTask *task = user_data;
  OpenData *d = g_task_get_task_data(task);
  g_autoptr(GError) err = NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  if (r) {
    d->result->handed_off = TRUE;
    return_result(task);
    return;
  }
  /* Any failure (no owner any more, UnknownObject/Interface/Method,
   * timeout, handler error) degrades to the plain URI launch. */
  g_debug("nostr-dispatcher: Handler1 handoff to %s failed (%s); launching URI",
          d->result->desktop_id, err->message);
  launch_uri(task);
}

/* Deliver via org.nostr.Handler1.OpenEvent if possible, else launch. The
 * Handler1 contract: event_json is ALWAYS id- and signature-validated. */
static void deliver(GTask *task) {
  OpenData *d = g_task_get_task_data(task);
  if (d->opts.dry_run) {
    return_result(task);
    return;
  }
  g_autofree char *name = nd_handler_bus_name(d->result->desktop_id);
  g_autoptr(GDBusConnection) bus =
      name ? g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL) : NULL;
  if (d->opts.no_handoff || !d->event || !d->event->validated || !name || !bus) {
    launch_uri(task);
    return;
  }
  g_autofree char *path = nd_handler_object_path(name);
  const char *const empty[] = {NULL};
  const char *const *relays = d->relays ? (const char *const *)d->relays : empty;
  g_dbus_connection_call(bus, name, path, HANDLER1_IFACE, "OpenEvent",
                         g_variant_new("(us^as)", (guint32)d->event->kind,
                                       d->event->json, relays),
                         NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, HANDOFF_TIMEOUT_MS,
                         g_task_get_cancellable(task), handoff_done, task);
}

static gboolean choose(GTask *task, gint kind) {
  OpenData *d = g_task_get_task_data(task);
  d->result->kind = kind;
  d->result->desktop_id = nd_registry_choose(d->reg, kind, &d->result->source);
  if (d->result->desktop_id) return TRUE;
  if (kind >= 0)
    g_task_return_new_error(task, ND_ERROR, ND_ERROR_NO_HANDLER,
                            "no installed application handles Nostr events of kind %d",
                            kind);
  else
    g_task_return_new_error(task, ND_ERROR, ND_ERROR_NO_HANDLER,
                            "event kind could not be resolved and no fallback "
                            "(X-Nostr-Kinds=*) application is installed");
  g_object_unref(task);
  return FALSE;
}

static NdFetchOptions fetch_options(OpenData *d) {
  NdFetchOptions o = {0};
  o.use_local = TRUE;
  o.use_hints = nd_registry_fetch_relay_hints(d->reg);
  return o;
}

static void fetched_for_handoff(GObject *src, GAsyncResult *res, gpointer user_data) {
  GTask *task = user_data;
  OpenData *d = g_task_get_task_data(task);
  g_autoptr(GError) err = NULL;
  d->event = nd_fetch_event_finish(res, &err);
  if (!d->event)
    g_debug("nostr-dispatcher: no event for handoff (%s)", err->message);
  deliver(task);
}

static void fetched_for_kind(GObject *src, GAsyncResult *res, gpointer user_data) {
  GTask *task = user_data;
  OpenData *d = g_task_get_task_data(task);
  g_autoptr(GError) err = NULL;
  d->event = nd_fetch_event_finish(res, &err);
  gint kind = d->event ? d->event->kind : -1;
  if (!d->event)
    g_message("nostr-dispatcher: could not resolve the event kind (%s); "
              "using the fallback handler", err->message);
  if (!choose(task, kind)) return;
  deliver(task);
}

static GTask *open_task_new(NdRegistry *reg, const NdOpenOptions *opts,
                            GCancellable *cancellable, GAsyncReadyCallback cb,
                            gpointer ud) {
  GTask *task = g_task_new(NULL, cancellable, cb, ud);
  OpenData *d = g_new0(OpenData, 1);
  d->reg = reg;
  if (opts) {
    d->opts.dry_run = opts->dry_run;
    d->opts.no_handoff = opts->no_handoff;
    d->opts.activation_token = g_strdup(opts->activation_token);
  }
  d->result = g_new0(NdOpenResult, 1);
  d->result->kind = -1;
  g_task_set_task_data(task, d, open_data_free);
  return task;
}

void nd_dispatch_open_uri_async(NdRegistry *reg, const char *uri,
                                const NdOpenOptions *opts, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data) {
  GTask *task = open_task_new(reg, opts, cancellable, callback, user_data);
  g_task_set_source_tag(task, nd_dispatch_open_uri_async);
  OpenData *d = g_task_get_task_data(task);

  GError *err = NULL;
  d->target = nd_target_parse_uri(uri, &err);
  if (!d->target) {
    g_task_return_error(task, err);
    g_object_unref(task);
    return;
  }
  d->relays = g_strdupv(d->target->relays);

  if (d->target->kind >= 0) {
    if (!choose(task, d->target->kind)) return;
    /* Only spend a fetch (latency + privacy) when a running handler could
     * take the event directly over Handler1. */
    if (!d->opts.dry_run && !d->opts.no_handoff &&
        handler_is_running(d->result->desktop_id)) {
      NdFetchOptions fo = fetch_options(d);
      nd_fetch_event_async(d->target, &fo, cancellable, fetched_for_handoff, task);
      return;
    }
    deliver(task);
    return;
  }

  NdFetchOptions fo = fetch_options(d);
  nd_fetch_event_async(d->target, &fo, cancellable, fetched_for_kind, task);
}

void nd_dispatch_open_event_async(NdRegistry *reg, const char *event_json,
                                  const char *const *relays, const char *file_uri,
                                  const NdOpenOptions *opts, GCancellable *cancellable,
                                  GAsyncReadyCallback callback, gpointer user_data) {
  GTask *task = open_task_new(reg, opts, cancellable, callback, user_data);
  g_task_set_source_tag(task, nd_dispatch_open_event_async);
  OpenData *d = g_task_get_task_data(task);

  GError *err = NULL;
  d->event = nd_event_parse(event_json, -1, &err);
  if (!d->event) {
    g_task_return_error(task, err);
    g_object_unref(task);
    return;
  }
  d->relays = nd_relays_filter(relays, relays ? g_strv_length((char **)relays) : 0);
  d->file_uri = g_strdup(file_uri);

  /* A pointer for URI launches: only for validated events with an id. */
  if (d->event->validated && d->event->id_hex) {
    d->target = g_new0(NdTarget, 1);
    d->target->entity = ND_ENTITY_EVENT;
    d->target->id_hex = g_strdup(d->event->id_hex);
    d->target->pubkey_hex = g_strdup(d->event->pubkey_hex);
    d->target->kind = d->event->kind;
    d->target->relays = g_strdupv(d->relays);
  } else if (!d->file_uri) {
    g_task_return_new_error(task, ND_ERROR, ND_ERROR_INVALID_EVENT,
                            "event is not signed/valid and has no source file to open");
    g_object_unref(task);
    return;
  }

  if (!choose(task, d->event->kind)) return;
  deliver(task);
}

NdOpenResult *nd_dispatch_open_finish(GAsyncResult *res, GError **error) {
  return g_task_propagate_pointer(G_TASK(res), error);
}
