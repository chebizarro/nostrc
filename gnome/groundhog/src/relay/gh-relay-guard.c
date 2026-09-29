#include "gh-relay-guard.h"

#include <string.h>

typedef struct {
  gint refs;
  GMainContext *context;       /* owning context */
  gboolean publish_side;
  GhRelayScope *scope;         /* borrowed; the scope closes us first */
  GhRelayPublish *publish;     /* borrowed; the publish closes us first */
  const NostrFilters *filters; /* the scope's, borrowed like the scope */
  gchar *url;
  gchar *event_json;
  const GhRelayTransport *direct;                /* the scope side's */
  const GhRelayAuthTransport *direct_auth;
  const GhRelayPublishTransport *direct_publish; /* the publish side's */
  const GhRelayPublishAuthTransport *direct_publish_auth;
  gpointer inner;              /* the direct connection's handle, or NULL */
  gboolean reported;           /* the scope has been told about this refusal */
  gboolean closed;             /* owning context */
} GuardHandle;

static GMutex lock;          /* installed, changed_handler, registry, direct_* */
static GSettings *installed;
static gulong changed_handler;
static GPtrArray *registry;  /* GuardHandle, borrowed; live until closed */
static gint refusing = 1;    /* atomic; nothing installed refuses */

static const GhRelayTransport *direct_scope = &gh_relay_gnostr_transport;
static const GhRelayAuthTransport *direct_scope_auth = &gh_relay_gnostr_auth_transport;
static const GhRelayPublishTransport *direct_publish = &gh_relay_publish_gnostr_transport;
static const GhRelayPublishAuthTransport *direct_publish_auth =
  &gh_relay_publish_gnostr_auth_transport;

/* ---- the decision ---------------------------------------------------------------- */

gboolean
gh_relay_guard_mode_allowed(const gchar *mode)
{
  return g_strcmp0(mode, "system") == 0 || g_strcmp0(mode, "none") == 0;
}

static gboolean
url_is_onion(const gchar *url)
{
  g_autoptr(GUri) uri = url ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
  const gchar *host = uri ? g_uri_get_host(uri) : NULL;
  gsize length = host ? strlen(host) : 0;
  /* A trailing dot is the same name. */
  if (length > 0 && host[length - 1] == '.')
    length--;
  return length > strlen(".onion") &&
         g_ascii_strncasecmp(host + length - strlen(".onion"), ".onion", strlen(".onion")) == 0;
}

static void
set_onion_error(GError **error)
{
  /* Never resolved or dialled directly: that would ask the local DNS. */
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                      ".onion relays can only be reached through Tor, which isn't available "
                      "in this build");
}

static void
set_refused_error(GError **error)
{
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                      GH_RELAY_GUARD_REFUSED_MESSAGE);
}

gboolean
gh_relay_guard_url_allowed(const gchar *mode, const gchar *url, GError **error)
{
  if (url_is_onion(url)) {
    set_onion_error(error);
    return FALSE;
  }
  if (!gh_relay_guard_mode_allowed(mode)) {
    set_refused_error(error);
    return FALSE;
  }
  return TRUE;
}

/* ---- handles --------------------------------------------------------------------- */

static GuardHandle *
guard_ref(GuardHandle *handle)
{
  g_atomic_int_inc(&handle->refs);
  return handle;
}

static void
guard_unref(gpointer data)
{
  GuardHandle *handle = data;
  if (!g_atomic_int_dec_and_test(&handle->refs))
    return;
  g_main_context_unref(handle->context);
  g_free(handle->url);
  g_free(handle->event_json);
  g_free(handle);
}

static void
close_inner(GuardHandle *handle)
{
  gpointer inner = g_steal_pointer(&handle->inner);
  if (!inner)
    return;
  if (handle->publish_side)
    handle->direct_publish->close(inner, NULL);
  else
    handle->direct->close(inner, NULL);
}

/* Refuses a .onion relay, and anything once no guard is installed. */
static GuardHandle *
handle_new(const gchar *url, GError **error)
{
  if (url_is_onion(url)) {
    set_onion_error(error);
    return NULL;
  }
  g_mutex_lock(&lock);
  gboolean ready = installed != NULL;
  GuardHandle *handle = NULL;
  if (ready) {
    handle = g_new0(GuardHandle, 1);
    handle->refs = 1; /* the scope's or publish's, dropped by close */
    handle->context = g_main_context_ref_thread_default();
    handle->url = g_strdup(url);
    handle->direct = direct_scope;
    handle->direct_auth = direct_scope_auth;
    handle->direct_publish = direct_publish;
    handle->direct_publish_auth = direct_publish_auth;
    /* Registered before the mode is read, so a change after that read
     * always reaches it. */
    g_ptr_array_add(registry, handle);
  }
  g_mutex_unlock(&lock);
  if (!ready)
    /* Made for the guard, but it is gone: refuse rather than guess (P5). */
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        "Groundhog's network connection is not set up");
  return handle;
}

static void
unregister(GuardHandle *handle)
{
  g_mutex_lock(&lock);
  g_ptr_array_remove_fast(registry, handle);
  g_mutex_unlock(&lock);
}

/* On the owning context: bring the handle in line with the current mode. */
static gboolean
rebuild(gpointer data)
{
  GuardHandle *handle = data;
  if (handle->closed)
    return G_SOURCE_REMOVE;
  if (g_atomic_int_get(&refusing)) {
    if (handle->publish_side) {
      /* Only a publish opened in an allowed mode is registered. */
      if (handle->inner) {
        close_inner(handle);
        gh_relay_publish_failed(handle->publish, handle->url, GH_RELAY_GUARD_REFUSED_MESSAGE);
      }
      return G_SOURCE_REMOVE;
    }
    if (handle->inner) {
      close_inner(handle);
      gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_DISCONNECTED, NULL,
                            FALSE, NULL);
      if (handle->closed)
        return G_SOURCE_REMOVE;
    }
    if (!handle->reported) {
      handle->reported = TRUE;
      gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_ERROR, NULL, FALSE,
                            GH_RELAY_GUARD_REFUSED_MESSAGE);
    }
    return G_SOURCE_REMOVE;
  }
  /* Allowed: a refused scope URL connects now; a live one stays. */
  if (handle->publish_side || handle->inner)
    return G_SOURCE_REMOVE;
  handle->reported = FALSE;
  g_autoptr(GError) error = NULL;
  handle->inner = handle->direct->open(handle->scope, handle->url, handle->filters, NULL, &error);
  if (!handle->inner)
    gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_ERROR, NULL, FALSE,
                          error ? error->message : "relay open failed");
  return G_SOURCE_REMOVE;
}

static void
queue_rebuild(GuardHandle *handle)
{
  GSource *source = g_idle_source_new();
  g_source_set_priority(source, G_PRIORITY_DEFAULT);
  g_source_set_callback(source, rebuild, guard_ref(handle), guard_unref);
  g_source_attach(source, handle->context);
  g_source_unref(source);
}

static void
on_mode_changed(GSettings *settings, const gchar *key, gpointer data)
{
  (void)key;
  (void)data;
  g_autofree gchar *mode = g_settings_get_string(settings, "network-mode");
  gint now = !gh_relay_guard_mode_allowed(mode);
  if (g_atomic_int_get(&refusing) == now)
    return;
  g_atomic_int_set(&refusing, now);
  g_autoptr(GPtrArray) handles = g_ptr_array_new_with_free_func(guard_unref);
  g_mutex_lock(&lock);
  for (guint i = 0; registry && i < registry->len; i++)
    g_ptr_array_add(handles, guard_ref(g_ptr_array_index(registry, i)));
  g_mutex_unlock(&lock);
  for (guint i = 0; i < handles->len; i++)
    queue_rebuild(g_ptr_array_index(handles, i));
}

void
gh_relay_guard_install(GSettings *settings)
{
  g_return_if_fail(!settings || G_IS_SETTINGS(settings));
  g_mutex_lock(&lock);
  GSettings *old = installed;
  gulong old_handler = changed_handler;
  installed = settings ? g_object_ref(settings) : NULL;
  changed_handler = settings ? g_signal_connect(settings, "changed::network-mode",
                                                G_CALLBACK(on_mode_changed), NULL)
                             : 0;
  if (!registry)
    registry = g_ptr_array_new();
  g_mutex_unlock(&lock);
  if (old) {
    g_signal_handler_disconnect(old, old_handler);
    g_object_unref(old);
  }
  if (settings)
    on_mode_changed(settings, "network-mode", NULL); /* reads it: notifications follow */
  else
    g_atomic_int_set(&refusing, 1);
  gh_relay_scope_set_default_transport(settings ? &gh_relay_guard_transport : NULL,
                                       settings ? &gh_relay_guard_auth_transport : NULL, NULL);
  gh_relay_publish_set_default_transport(settings ? &gh_relay_guard_publish_transport : NULL,
                                         settings ? &gh_relay_guard_publish_auth_transport : NULL,
                                         NULL);
}

void
gh_relay_guard_set_direct_transports(const GhRelayTransport *scope,
                                     const GhRelayAuthTransport *scope_auth,
                                     const GhRelayPublishTransport *publish,
                                     const GhRelayPublishAuthTransport *publish_auth)
{
  g_mutex_lock(&lock);
  direct_scope = scope ? scope : &gh_relay_gnostr_transport;
  direct_scope_auth = scope_auth ? scope_auth : &gh_relay_gnostr_auth_transport;
  direct_publish = publish ? publish : &gh_relay_publish_gnostr_transport;
  direct_publish_auth = publish_auth ? publish_auth : &gh_relay_publish_gnostr_auth_transport;
  g_mutex_unlock(&lock);
}

static void
guard_close(gpointer data, gpointer transport_data)
{
  (void)transport_data;
  GuardHandle *handle = data;
  handle->closed = TRUE;
  unregister(handle);
  close_inner(handle);
  guard_unref(handle);
}

/* ---- scopes ---------------------------------------------------------------------- */

static gpointer
guard_scope_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters,
                 gpointer transport_data, GError **error)
{
  (void)transport_data;
  GuardHandle *handle = handle_new(url, error);
  if (!handle)
    return NULL;
  handle->scope = scope;
  handle->filters = filters;
  if (g_atomic_int_get(&refusing)) {
    /* Kept, so the URL connects if the user picks another mode; the
     * refusal is reported on the owning context, after open returns. */
    queue_rebuild(handle);
    return handle;
  }
  handle->inner = handle->direct->open(scope, url, filters, NULL, error);
  if (!handle->inner) {
    unregister(handle);
    guard_unref(handle);
    return NULL;
  }
  return handle;
}

static gboolean
guard_scope_send_auth(gpointer data, const gchar *json, gpointer transport_data,
                      GError **error)
{
  (void)transport_data;
  GuardHandle *handle = data;
  if (!handle->inner || !handle->direct_auth || !handle->direct_auth->send_auth) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "relay not connected");
    return FALSE;
  }
  return handle->direct_auth->send_auth(handle->inner, json, NULL, error);
}

static void
guard_scope_resubscribe(gpointer data, gpointer transport_data)
{
  (void)transport_data;
  GuardHandle *handle = data;
  if (handle->inner && handle->direct_auth && handle->direct_auth->resubscribe)
    handle->direct_auth->resubscribe(handle->inner, NULL);
}

/* ---- publishes ------------------------------------------------------------------- */

static gpointer
guard_publish_open(GhRelayPublish *publish, const gchar *url, const gchar *event_json,
                   gpointer transport_data, GError **error)
{
  (void)transport_data;
  GuardHandle *handle = handle_new(url, error);
  if (!handle)
    return NULL;
  handle->publish_side = TRUE;
  handle->publish = publish;
  handle->event_json = g_strdup(event_json);
  if (g_atomic_int_get(&refusing)) {
    set_refused_error(error);
    unregister(handle);
    guard_unref(handle);
    return NULL;
  }
  handle->inner = handle->direct_publish->open(publish, url, event_json, NULL, error);
  if (!handle->inner) {
    unregister(handle);
    guard_unref(handle);
    return NULL;
  }
  return handle;
}

static gboolean
guard_publish_send_auth(gpointer data, const gchar *json, gpointer transport_data,
                        GError **error)
{
  (void)transport_data;
  GuardHandle *handle = data;
  if (!handle->inner || !handle->direct_publish_auth ||
      !handle->direct_publish_auth->send_auth) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "relay not connected");
    return FALSE;
  }
  return handle->direct_publish_auth->send_auth(handle->inner, json, NULL, error);
}

static gboolean
guard_publish_resend(gpointer data, gpointer transport_data, GError **error)
{
  (void)transport_data;
  GuardHandle *handle = data;
  if (!handle->inner || !handle->direct_publish_auth || !handle->direct_publish_auth->resend) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "relay not connected");
    return FALSE;
  }
  return handle->direct_publish_auth->resend(handle->inner, NULL, error);
}

const GhRelayTransport gh_relay_guard_transport = { guard_scope_open, guard_close };
const GhRelayAuthTransport gh_relay_guard_auth_transport = { guard_scope_send_auth,
                                                             guard_scope_resubscribe };
const GhRelayPublishTransport gh_relay_guard_publish_transport = { guard_publish_open,
                                                                   guard_close };
const GhRelayPublishAuthTransport gh_relay_guard_publish_auth_transport = {
  guard_publish_send_auth, guard_publish_resend
};
