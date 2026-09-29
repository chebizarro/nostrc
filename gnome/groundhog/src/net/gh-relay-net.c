#include "gh-relay-net.h"
#include "gh-relay-soup.h"

typedef struct {
  gint refs;
  GhNetSession *session;
  GMainContext *context;       /* owning context */
  gboolean publish_side;
  GhRelayScope *scope;         /* borrowed; the scope closes us first */
  GhRelayPublish *publish;     /* borrowed; the publish closes us first */
  const NostrFilters *filters; /* the scope's, borrowed like the scope */
  gchar *url;
  gchar *event_json;
  gpointer inner;              /* the open connection's handle, or NULL */
  gboolean inner_soup;         /* inner is a gh-relay-soup handle */
  guint64 serial;              /* the session serial inner was opened in */
  gboolean closed;             /* owning context */
} NetHandle;

static GMutex lock;            /* installed, registry */
static GhNetSession *installed;
static gulong changed_handler;
static GPtrArray *registry;    /* NetHandle, borrowed; live until closed */

static NetHandle *
net_ref(NetHandle *handle)
{
  g_atomic_int_inc(&handle->refs);
  return handle;
}

static void
net_unref(gpointer data)
{
  NetHandle *handle = data;
  if (!g_atomic_int_dec_and_test(&handle->refs))
    return;
  g_object_unref(handle->session);
  g_main_context_unref(handle->context);
  g_free(handle->url);
  g_free(handle->event_json);
  g_free(handle);
}

static gboolean
on_owner_context(NetHandle *handle)
{
  GMainContext *current = g_main_context_get_thread_default();
  return (current ? current : g_main_context_default()) == handle->context;
}

static void
on_status(gpointer data, gboolean connected)
{
  NetHandle *handle = data;
  gh_net_session_report(handle->session, connected);
}

static void
close_inner(NetHandle *handle)
{
  gpointer inner = g_steal_pointer(&handle->inner);
  if (!inner)
    return;
  if (handle->publish_side) {
    if (handle->inner_soup)
      gh_relay_soup_publish_close(inner);
    else
      gh_relay_publish_gnostr_transport.close(inner, NULL);
  } else {
    if (handle->inner_soup)
      gh_relay_soup_scope_close(inner);
    else
      gh_relay_gnostr_transport.close(inner, NULL);
  }
}

/* Opens the connection for the session's current mode, or refuses. */
static gboolean
open_inner(NetHandle *handle, GError **error)
{
  GhNetMode mode = gh_net_session_get_mode(handle->session);
  handle->serial = gh_net_session_get_serial(handle->session);
  if (!gh_net_relay_url_allowed(mode, handle->url, error))
    return FALSE;
  if (mode != GH_NET_MODE_TOR) {
    handle->inner_soup = FALSE;
    handle->inner = handle->publish_side
      ? gh_relay_publish_gnostr_transport.open(handle->publish, handle->url,
                                               handle->event_json, NULL, error)
      : gh_relay_gnostr_transport.open(handle->scope, handle->url, handle->filters, NULL,
                                       error);
    return handle->inner != NULL;
  }
  /* The isolation label never contains the URL or a key (NT-6). */
  g_autofree gchar *label =
    handle->publish_side
      ? g_strdup_printf("%" G_GUINT64_FORMAT "/publish/%s",
                        gh_relay_publish_get_generation(handle->publish),
                        gh_relay_publish_get_isolation(handle->publish))
      : g_strdup_printf("%" G_GUINT64_FORMAT "/scope/%s",
                        gh_relay_scope_get_generation(handle->scope),
                        gh_relay_scope_get_isolation(handle->scope));
  g_autoptr(GProxyResolver) resolver = gh_net_session_dup_resolver(handle->session, label,
                                                                   error);
  if (!resolver)
    return FALSE;
  handle->inner_soup = TRUE;
  handle->inner = handle->publish_side
    ? gh_relay_soup_publish_open(handle->publish, handle->url, handle->event_json, resolver,
                                 on_status, handle, error)
    : gh_relay_soup_scope_open(handle->scope, handle->url, handle->filters, resolver,
                               on_status, handle, error);
  return handle->inner != NULL;
}

/* On the owning context, after a mode change: the old connection is gone;
 * a scope reconnects in the new mode, a publish fails. */
static gboolean
rebuild(gpointer data)
{
  NetHandle *handle = data;
  if (handle->closed ||
      (handle->inner && handle->serial == gh_net_session_get_serial(handle->session)))
    return G_SOURCE_REMOVE;
  close_inner(handle);
  if (handle->publish_side) {
    handle->serial = gh_net_session_get_serial(handle->session);
    gh_relay_publish_failed(handle->publish, handle->url,
                            "The connection mode changed before the relay answered");
    return G_SOURCE_REMOVE;
  }
  gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_DISCONNECTED, NULL, FALSE,
                        NULL);
  if (handle->closed)
    return G_SOURCE_REMOVE;
  g_autoptr(GError) error = NULL;
  if (!open_inner(handle, &error))
    gh_relay_scope_notice(handle->scope, handle->url, GH_RELAY_NOTICE_ERROR, NULL, FALSE,
                          error ? error->message : "relay connection refused");
  return G_SOURCE_REMOVE;
}

static void
on_session_changed(GhNetSession *session, gpointer data)
{
  (void)data;
  g_autoptr(GPtrArray) handles = g_ptr_array_new_with_free_func(net_unref);
  g_mutex_lock(&lock);
  for (guint i = 0; registry && i < registry->len; i++) {
    NetHandle *handle = g_ptr_array_index(registry, i);
    if (handle->session == session)
      g_ptr_array_add(handles, net_ref(handle));
  }
  g_mutex_unlock(&lock);
  for (guint i = 0; i < handles->len; i++) {
    NetHandle *handle = g_ptr_array_index(handles, i);
    /* Close the old mode's socket now when we are on its context (no
     * callback into the scope or publish happens here), so it is gone
     * before anything opens in the new mode; the rest follows on its
     * context. */
    if (on_owner_context(handle) && !handle->closed)
      close_inner(handle);
    GSource *source = g_idle_source_new();
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(source, rebuild, net_ref(handle), net_unref);
    g_source_attach(source, handle->context);
    g_source_unref(source);
  }
}

void
gh_relay_net_install(GhNetSession *session)
{
  g_return_if_fail(!session || GH_IS_NET_SESSION(session));
  g_mutex_lock(&lock);
  GhNetSession *old = installed;
  gulong old_handler = changed_handler;
  installed = session ? g_object_ref(session) : NULL;
  changed_handler = session ? g_signal_connect(session, "changed",
                                               G_CALLBACK(on_session_changed), NULL)
                            : 0;
  if (!registry)
    registry = g_ptr_array_new();
  g_mutex_unlock(&lock);
  if (old) {
    g_signal_handler_disconnect(old, old_handler);
    g_object_unref(old);
  }
  gh_relay_scope_set_default_transport(session ? &gh_relay_net_transport : NULL,
                                       session ? &gh_relay_net_auth_transport : NULL, NULL);
  gh_relay_publish_set_default_transport(session ? &gh_relay_net_publish_transport : NULL,
                                         session ? &gh_relay_net_publish_auth_transport : NULL,
                                         NULL);
  gh_net_session_set_default(session);
}

static NetHandle *
handle_new(const gchar *url, GError **error)
{
  g_mutex_lock(&lock);
  GhNetSession *session = installed ? g_object_ref(installed) : NULL;
  g_mutex_unlock(&lock);
  if (!session) {
    /* Made for the dispatcher, but no network session is left: refuse
     * rather than guess a mode (P5). */
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        "Groundhog's network connection is not set up");
    return NULL;
  }
  NetHandle *handle = g_new0(NetHandle, 1);
  handle->refs = 1; /* the scope's or publish's, dropped by close */
  handle->session = session;
  handle->context = g_main_context_ref_thread_default();
  handle->url = g_strdup(url);
  return handle;
}

static gpointer
register_or_drop(NetHandle *handle, gboolean opened)
{
  if (!opened) {
    close_inner(handle);
    net_unref(handle);
    return NULL;
  }
  g_mutex_lock(&lock);
  g_ptr_array_add(registry, handle);
  g_mutex_unlock(&lock);
  return handle;
}

static void
net_close(gpointer data, gpointer transport_data)
{
  (void)transport_data;
  NetHandle *handle = data;
  handle->closed = TRUE;
  g_mutex_lock(&lock);
  g_ptr_array_remove_fast(registry, handle);
  g_mutex_unlock(&lock);
  close_inner(handle);
  net_unref(handle);
}

/* ---- scopes ---------------------------------------------------------------------- */

static gpointer
net_scope_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters,
               gpointer transport_data, GError **error)
{
  (void)transport_data;
  NetHandle *handle = handle_new(url, error);
  if (!handle)
    return NULL;
  handle->scope = scope;
  handle->filters = filters;
  return register_or_drop(handle, open_inner(handle, error));
}

static gboolean
net_scope_send_auth(gpointer data, const gchar *json, gpointer transport_data, GError **error)
{
  (void)transport_data;
  NetHandle *handle = data;
  if (!handle->inner) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "relay not connected");
    return FALSE;
  }
  return handle->inner_soup
    ? gh_relay_soup_scope_send_auth(handle->inner, json, error)
    : gh_relay_gnostr_auth_transport.send_auth(handle->inner, json, NULL, error);
}

static void
net_scope_resubscribe(gpointer data, gpointer transport_data)
{
  (void)transport_data;
  NetHandle *handle = data;
  if (!handle->inner)
    return;
  if (handle->inner_soup)
    gh_relay_soup_scope_resubscribe(handle->inner);
  else
    gh_relay_gnostr_auth_transport.resubscribe(handle->inner, NULL);
}

/* ---- publishes ------------------------------------------------------------------- */

static gpointer
net_publish_open(GhRelayPublish *publish, const gchar *url, const gchar *event_json,
                 gpointer transport_data, GError **error)
{
  (void)transport_data;
  NetHandle *handle = handle_new(url, error);
  if (!handle)
    return NULL;
  handle->publish_side = TRUE;
  handle->publish = publish;
  handle->event_json = g_strdup(event_json);
  return register_or_drop(handle, open_inner(handle, error));
}

static gboolean
net_publish_send_auth(gpointer data, const gchar *json, gpointer transport_data, GError **error)
{
  (void)transport_data;
  NetHandle *handle = data;
  if (!handle->inner) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "relay not connected");
    return FALSE;
  }
  return handle->inner_soup
    ? gh_relay_soup_publish_send_auth(handle->inner, json, error)
    : gh_relay_publish_gnostr_auth_transport.send_auth(handle->inner, json, NULL, error);
}

static gboolean
net_publish_resend(gpointer data, gpointer transport_data, GError **error)
{
  (void)transport_data;
  NetHandle *handle = data;
  if (!handle->inner) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "relay not connected");
    return FALSE;
  }
  return handle->inner_soup
    ? gh_relay_soup_publish_resend(handle->inner, error)
    : gh_relay_publish_gnostr_auth_transport.resend(handle->inner, NULL, error);
}

const GhRelayTransport gh_relay_net_transport = { net_scope_open, net_close };
const GhRelayAuthTransport gh_relay_net_auth_transport = { net_scope_send_auth,
                                                           net_scope_resubscribe };
const GhRelayPublishTransport gh_relay_net_publish_transport = { net_publish_open, net_close };
const GhRelayPublishAuthTransport gh_relay_net_publish_auth_transport = { net_publish_send_auth,
                                                                          net_publish_resend };
