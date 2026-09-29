#include "gh-net-http.h"
#include "gh-net-session.h"
#include "gh-net-tls.h"

#include <libsoup/soup.h>
#include <string.h>

struct _GhNetHttp {
  GObject parent_instance;
  GSettings *settings;
  SoupSession *session;
  gchar *session_mode; /* the network mode session was made for */
  GPtrArray *requests; /* Request, in flight (not owned) */
};

G_DEFINE_FINAL_TYPE(GhNetHttp, gh_net_http, G_TYPE_OBJECT)

typedef struct {
  GhNetHttp *owner;       /* the task's source object, so alive */
  GhNetMode mode;         /* the mode the request was made in */
  SoupSession *session;
  SoupMessage *message;
  GCancellable *cancellable; /* the request's own: the caller's, or a mode change */
  GCancellable *caller;
  gulong caller_handler;
  gboolean mode_changed;  /* cancelled by a mode change */
  GInputStream *stream;
  guint8 *buffer;
  gsize max_bytes;
} Request;

static void
request_free(gpointer data)
{
  Request *request = data;
  if (request->owner->requests)
    g_ptr_array_remove_fast(request->owner->requests, request);
  if (request->caller)
    g_cancellable_disconnect(request->caller, request->caller_handler);
  g_clear_object(&request->caller);
  g_clear_object(&request->cancellable);
  g_clear_object(&request->message);
  /* A Tor request's session is its own: close its connection with it
   * (disposing a session with a live connection is a libsoup warning). */
  if (request->mode == GH_NET_MODE_TOR && request->session)
    soup_session_abort(request->session);
  g_clear_object(&request->session);
  g_clear_object(&request->stream);
  g_free(request->buffer);
  g_free(request);
}

static void
on_caller_cancelled(GCancellable *caller, gpointer data)
{
  (void)caller;
  g_cancellable_cancel(G_CANCELLABLE(data));
}

/* A request cancelled by a mode change fails with this, not
 * G_IO_ERROR_CANCELLED, which callers take for their own cancellation. */
static GError *
request_error(Request *request, GError *error)
{
  if (request->mode_changed && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
      !g_cancellable_is_cancelled(request->caller)) {
    g_error_free(error);
    return g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED,
                               "The network setting changed before the server answered");
  }
  return error;
}

static gchar *
setting(GhNetHttp *self, const gchar *key, const gchar *fallback)
{
  if (!self->settings)
    return g_strdup(fallback);
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(self->settings, "settings-schema", &schema, NULL);
  if (!schema || !g_settings_schema_has_key(schema, key))
    return g_strdup(fallback);
  return g_settings_get_string(self->settings, key);
}

static SoupSession *
session_new(GProxyResolver *resolver)
{
  SoupSession *session = soup_session_new_with_options(
    "timeout", GH_NET_HTTP_TIMEOUT_S, "idle-timeout", GH_NET_HTTP_TIMEOUT_S,
    "proxy-resolver", resolver, "user-agent", NULL, "accept-language-auto", FALSE, NULL);
  /* No cookie jar, cache, HSTS or auth store is added; drop any content
   * sniffer a default session may carry (nothing here renders content). */
  soup_session_remove_feature_by_type(session, SOUP_TYPE_CONTENT_SNIFFER);
  return session;
}

/* A new reference to the session for mode. System and No Proxy sessions are
 * kept (and made again when the mode changed); in Tor mode every request gets
 * its own session and SOCKS credentials, so its own circuit (charter §4.3:
 * media and lookups are "random per fetch"). NULL with an error when the Tor
 * address is unusable. */
static SoupSession *
session_for(GhNetHttp *self, GhNetMode mode, GError **error)
{
  if (mode == GH_NET_MODE_TOR) {
    g_autofree gchar *address = setting(self, "tor-socks-address", NULL);
    g_autoptr(GProxyResolver) resolver = gh_net_proxy_resolver_new(mode, address, NULL, error);
    return resolver ? session_new(resolver) : NULL;
  }
  const gchar *name = mode == GH_NET_MODE_SYSTEM ? "system" : "none";
  if (!self->session || g_strcmp0(self->session_mode, name) != 0) {
    if (self->session)
      soup_session_abort(self->session);
    g_clear_object(&self->session);
    g_free(self->session_mode);
    /* SYSTEM follows the desktop settings (a NULL resolver would disable
     * proxies); NONE has no proxy. */
    g_autoptr(GProxyResolver) resolver = gh_net_proxy_resolver_new(mode, NULL, NULL, NULL);
    self->session = session_new(resolver);
    self->session_mode = g_strdup(name);
  }
  return g_object_ref(self->session);
}

static void
report(Request *request, gboolean connected)
{
  if (request->mode != GH_NET_MODE_TOR)
    return;
  g_autoptr(GhNetSession) session = gh_net_session_dup_default();
  if (session)
    gh_net_session_report(session, connected);
}

static gboolean
loopback_literal(const gchar *host)
{
  g_autoptr(GInetAddress) address = g_inet_address_new_from_string(host);
  return address && g_inet_address_get_is_loopback(address);
}

/* https anywhere; http only to a loopback address (the fixtures) or, in Tor
 * mode, a .onion host (the onion address authenticates the service). A
 * .onion host only in Tor mode: anywhere else it would reach the local DNS. */
static gboolean
uri_allowed(GUri *parsed, GhNetMode mode, GError **error)
{
  const gchar *scheme = parsed ? g_uri_get_scheme(parsed) : NULL;
  const gchar *host = parsed ? g_uri_get_host(parsed) : NULL;
  if (!scheme || !host || !*host || g_uri_get_userinfo(parsed) ||
      (g_ascii_strcasecmp(scheme, "https") != 0 && g_ascii_strcasecmp(scheme, "http") != 0)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Only https addresses are fetched");
    return FALSE;
  }
  gboolean onion = gh_net_host_is_onion(host);
  if (onion && mode != GH_NET_MODE_TOR) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        ".onion addresses can only be reached through Tor");
    return FALSE;
  }
  if (g_ascii_strcasecmp(scheme, "http") == 0 && !onion && !loopback_literal(host)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Only https addresses are fetched");
    return FALSE;
  }
  return TRUE;
}

static void
on_read(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GTask) task = data;
  Request *request = g_task_get_task_data(task);
  gsize read = 0;
  GError *error = NULL;
  if (!g_input_stream_read_all_finish(G_INPUT_STREAM(source), result, &read, &error)) {
    g_task_return_error(task, request_error(request, error));
    return;
  }
  (void)g_input_stream_close(request->stream, NULL, NULL);
  if (read > request->max_bytes) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                            "The server's answer is larger than %" G_GSIZE_FORMAT " bytes",
                            request->max_bytes);
    return;
  }
  g_task_return_pointer(task, g_bytes_new(request->buffer, read),
                        (GDestroyNotify)g_bytes_unref);
}

static void
on_sent(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GTask) task = data;
  Request *request = g_task_get_task_data(task);
  GError *error = NULL;
  request->stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);
  if (!request->stream) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      report(request, FALSE);
    g_task_return_error(task, request_error(request, error));
    return;
  }
  report(request, TRUE);
  guint status = soup_message_get_status(request->message);
  if (SOUP_STATUS_IS_REDIRECTION(status)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "The server redirected the request (HTTP %u), which is not followed",
                            status);
    return;
  }
  if (status != SOUP_STATUS_OK) {
    g_task_return_new_error(task, G_IO_ERROR,
                            status == SOUP_STATUS_NOT_FOUND ? G_IO_ERROR_NOT_FOUND
                                                            : G_IO_ERROR_FAILED,
                            "The server answered HTTP %u", status);
    return;
  }
  goffset length = soup_message_headers_get_content_length(
    soup_message_get_response_headers(request->message));
  if (length > 0 && (guint64)length > request->max_bytes) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                            "The server's answer is larger than %" G_GSIZE_FORMAT " bytes",
                            request->max_bytes);
    return;
  }
  /* One byte more than allowed tells an oversized body apart. */
  request->buffer = g_malloc(request->max_bytes + 1);
  g_input_stream_read_all_async(request->stream, request->buffer, request->max_bytes + 1,
                                G_PRIORITY_DEFAULT, request->cancellable, on_read,
                                g_object_ref(task));
}

void
gh_net_http_get_async(GhNetHttp *self, const gchar *uri, gsize max_bytes,
                      GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  gh_net_http_get_accept_async(self, uri, NULL, max_bytes, cancellable, callback, user_data);
}

void
gh_net_http_get_accept_async(GhNetHttp *self, const gchar *uri, const gchar *accept,
                             gsize max_bytes, GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_NET_HTTP(self));
  g_return_if_fail(uri != NULL && max_bytes > 0 && max_bytes < G_MAXSIZE);
  g_return_if_fail(!accept || (*accept && !strpbrk(accept, "\r\n")));
  g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_net_http_get_async);
  g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_ENCODED, NULL);
  g_autofree gchar *mode_name = setting(self, "network-mode", "system");
  GhNetMode mode = gh_net_mode_from_string(mode_name);
  GError *error = NULL;
  if (!uri_allowed(parsed, mode, &error)) {
    g_task_return_error(task, error);
    return;
  }
  g_autoptr(SoupSession) session = session_for(self, mode, &error);
  if (!session) {
    /* Fail closed (P5): an unusable Tor address never means "direct". */
    g_task_return_error(task, error);
    return;
  }
  Request *request = g_new0(Request, 1);
  request->owner = self;
  request->mode = mode;
  request->max_bytes = max_bytes;
  request->session = g_object_ref(session);
  request->message = soup_message_new_from_uri(SOUP_METHOD_GET, parsed);
  request->cancellable = g_cancellable_new();
  g_task_set_task_data(task, request, request_free);
  g_ptr_array_add(self->requests, request);
  if (cancellable) {
    request->caller = g_object_ref(cancellable);
    request->caller_handler = g_cancellable_connect(cancellable, G_CALLBACK(on_caller_cancelled),
                                                    g_object_ref(request->cancellable),
                                                    g_object_unref);
  }
  gh_net_tls_no_resumption(request->message); /* PD-6: gh-net-tls.h */
  soup_message_add_flags(request->message, SOUP_MESSAGE_NO_REDIRECT);
  SoupMessageHeaders *headers = soup_message_get_request_headers(request->message);
  soup_message_headers_replace(headers, "Accept", accept ? accept : "application/json");
  soup_session_send_async(session, request->message, G_PRIORITY_DEFAULT, request->cancellable,
                          on_sent, g_steal_pointer(&task));
}

GBytes *
gh_net_http_get_finish(GhNetHttp *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
transport_get_async(gpointer data, const gchar *uri, gsize max_bytes, GCancellable *cancellable,
                    GAsyncReadyCallback callback, gpointer user_data)
{
  gh_net_http_get_async(GH_NET_HTTP(data), uri, max_bytes, cancellable, callback, user_data);
}

static GBytes *
transport_get_finish(gpointer data, GAsyncResult *result, GError **error)
{
  return gh_net_http_get_finish(GH_NET_HTTP(data), result, error);
}

const GhHttpTransport *
gh_net_http_transport(void)
{
  static const GhHttpTransport transport = { transport_get_async, transport_get_finish };
  return &transport;
}

/* A mode change ends every request made in another mode (as the relay
 * dispatcher closes every old connection): e.g. a System-mode request
 * still waiting for its server when the user chooses Tor completes no more.
 * The kept session of the old mode goes too. */
static void
on_mode_changed(GSettings *settings, const gchar *key, gpointer data)
{
  (void)settings;
  (void)key;
  GhNetHttp *self = data;
  g_autofree gchar *name = setting(self, "network-mode", "system");
  GhNetMode mode = gh_net_mode_from_string(name);
  /* Mark first: cancelling may finish (and free) a request at once, so hold
   * the cancellables, not the requests. */
  g_autoptr(GPtrArray) cancel = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; i < self->requests->len; i++) {
    Request *request = g_ptr_array_index(self->requests, i);
    if (request->mode != mode) {
      request->mode_changed = TRUE;
      g_ptr_array_add(cancel, g_object_ref(request->cancellable));
    }
  }
  if (self->session && g_strcmp0(self->session_mode, name) != 0) {
    soup_session_abort(self->session);
    g_clear_object(&self->session);
    g_clear_pointer(&self->session_mode, g_free);
  }
  for (guint i = 0; i < cancel->len; i++)
    g_cancellable_cancel(g_ptr_array_index(cancel, i));
}

GhNetHttp *
gh_net_http_new(GSettings *settings)
{
  g_return_val_if_fail(!settings || G_IS_SETTINGS(settings), NULL);
  GhNetHttp *self = g_object_new(GH_TYPE_NET_HTTP, NULL);
  self->settings = settings ? g_object_ref(settings) : NULL;
  if (settings) {
    g_signal_connect(settings, "changed::network-mode", G_CALLBACK(on_mode_changed), self);
    /* GSettings reports a change only of a key read since connecting. */
    g_free(setting(self, "network-mode", NULL));
  }
  return self;
}

static void
gh_net_http_dispose(GObject *object)
{
  GhNetHttp *self = GH_NET_HTTP(object);
  if (self->settings)
    g_signal_handlers_disconnect_by_data(self->settings, self);
  if (self->session)
    soup_session_abort(self->session);
  g_clear_object(&self->session);
  g_clear_object(&self->settings);
  G_OBJECT_CLASS(gh_net_http_parent_class)->dispose(object);
}

static void
gh_net_http_finalize(GObject *object)
{
  GhNetHttp *self = GH_NET_HTTP(object);
  g_free(self->session_mode);
  g_clear_pointer(&self->requests, g_ptr_array_unref);
  G_OBJECT_CLASS(gh_net_http_parent_class)->finalize(object);
}

static void
gh_net_http_class_init(GhNetHttpClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_net_http_dispose;
  G_OBJECT_CLASS(klass)->finalize = gh_net_http_finalize;
}

static void
gh_net_http_init(GhNetHttp *self)
{
  self->requests = g_ptr_array_new();
}
