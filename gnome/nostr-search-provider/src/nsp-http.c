/* nsp-http.c — see nsp-http.h. */
#include "nsp-http.h"

#include <string.h>

gboolean nsp_inet_address_is_public(GInetAddress *addr) {
  if (!addr) return FALSE;
  if (g_inet_address_get_is_any(addr) || g_inet_address_get_is_loopback(addr) ||
      g_inet_address_get_is_link_local(addr) || g_inet_address_get_is_site_local(addr) ||
      g_inet_address_get_is_multicast(addr))
    return FALSE;
  const guint8 *b = g_inet_address_to_bytes(addr);
  if (g_inet_address_get_family(addr) == G_SOCKET_FAMILY_IPV4) {
    if (b[0] == 0 || b[0] == 127 || b[0] >= 240) return FALSE; /* this-net, loop, reserved */
    if (b[0] == 100 && (b[1] & 0xC0) == 64) return FALSE;       /* 100.64/10 CGNAT */
    if (b[0] == 192 && b[1] == 0 && (b[2] == 0 || b[2] == 2)) return FALSE; /* IETF, TEST-NET-1 */
    if (b[0] == 198 && (b[1] & 0xFE) == 18) return FALSE;       /* 198.18/15 benchmarking */
    if (b[0] == 198 && b[1] == 51 && b[2] == 100) return FALSE; /* TEST-NET-2 */
    if (b[0] == 203 && b[1] == 0 && b[2] == 113) return FALSE;  /* TEST-NET-3 */
    return TRUE;
  }
  if ((b[0] & 0xFE) == 0xFC) return FALSE; /* fc00::/7 ULA */
  if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return FALSE; /* doc */
  if (b[0] == 0x00 || b[0] == 0xFF) {
    /* ::/8 covers ::, ::1, IPv4-mapped/compatible (::ffff:a.b.c.d) and
     * 0064:ff9b::/96 NAT64 — refuse them all rather than re-derive the
     * embedded v4 policy. */
    return FALSE;
  }
  if (b[0] == 0x20 && b[1] == 0x02) return FALSE; /* 6to4 can embed private v4 */
  return TRUE;
}

SoupSession *nsp_http_session_new(guint timeout_s) {
  return soup_session_new_with_options("timeout", timeout_s, "idle-timeout", 30,
                                       "max-conns-per-host", 2,
                                       "user-agent", "nostr-search-provider/1.0 ", NULL);
}

typedef struct {
  SoupMessage *msg;
  GInputStream *in;
  GByteArray *buf;
  gsize max;
  gboolean follow;
  gboolean refused; /* SSRF / scheme guard fired */
  char *content_type;
  GCancellable *cancellable; /* private: guard trips cancel it */
  GCancellable *outer;
  gulong outer_id;
} GetData;

static void get_data_free(gpointer p) {
  GetData *d = p;
  if (d->outer_id) g_cancellable_disconnect(d->outer, d->outer_id);
  g_clear_object(&d->outer);
  g_clear_object(&d->msg);
  g_clear_object(&d->in);
  if (d->buf) g_byte_array_unref(d->buf);
  g_free(d->content_type);
  g_clear_object(&d->cancellable);
  g_free(d);
}

static void on_network_event(SoupMessage *msg, GSocketClientEvent ev, GIOStream *conn,
                             gpointer user_data) {
  GetData *d = user_data;
  if (ev != G_SOCKET_CLIENT_CONNECTED || !G_IS_SOCKET_CONNECTION(conn)) return;
  g_autoptr(GSocketAddress) sa =
      g_socket_connection_get_remote_address(G_SOCKET_CONNECTION(conn), NULL);
  GInetAddress *ia = G_IS_INET_SOCKET_ADDRESS(sa)
                         ? g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(sa))
                         : NULL;
  if (!nsp_inet_address_is_public(ia)) {
    d->refused = TRUE;
    g_cancellable_cancel(d->cancellable);
  }
}

static void on_restarted(SoupMessage *msg, gpointer user_data) {
  GetData *d = user_data;
  GUri *u = soup_message_get_uri(msg);
  if (!d->follow || g_strcmp0(g_uri_get_scheme(u), "https") != 0) {
    d->refused = TRUE;
    g_cancellable_cancel(d->cancellable);
  }
}

static void fail(GTask *task, GError *err) {
  GetData *d = g_task_get_task_data(task);
  if (d->refused) {
    g_clear_error(&err);
    err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                              "refused: non-public peer address or non-https redirect");
  }
  g_task_return_error(task, err);
  g_object_unref(task);
}

static void read_more(GTask *task);

static void on_read(GObject *src, GAsyncResult *res, gpointer user_data) {
  GTask *task = user_data;
  GetData *d = g_task_get_task_data(task);
  GError *err = NULL;
  g_autoptr(GBytes) chunk = g_input_stream_read_bytes_finish(G_INPUT_STREAM(src), res, &err);
  if (!chunk) {
    fail(task, err);
    return;
  }
  gsize n = g_bytes_get_size(chunk);
  if (n == 0) {
    g_input_stream_close_async(d->in, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
    GBytes *body = g_byte_array_free_to_bytes(g_steal_pointer(&d->buf));
    g_task_return_pointer(task, body, (GDestroyNotify)g_bytes_unref);
    g_object_unref(task);
    return;
  }
  if (d->buf->len + n > d->max) {
    g_cancellable_cancel(d->cancellable);
    fail(task, g_error_new(G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                           "response larger than %" G_GSIZE_FORMAT " bytes", d->max));
    return;
  }
  g_byte_array_append(d->buf, g_bytes_get_data(chunk, NULL), (guint)n);
  read_more(task);
}

static void read_more(GTask *task) {
  GetData *d = g_task_get_task_data(task);
  g_input_stream_read_bytes_async(d->in, 16384, G_PRIORITY_DEFAULT, d->cancellable, on_read,
                                  task);
}

static void on_sent(GObject *src, GAsyncResult *res, gpointer user_data) {
  GTask *task = user_data;
  GetData *d = g_task_get_task_data(task);
  GError *err = NULL;
  d->in = soup_session_send_finish(SOUP_SESSION(src), res, &err);
  if (!d->in) {
    fail(task, err);
    return;
  }
  guint status = soup_message_get_status(d->msg);
  if (status != SOUP_STATUS_OK) {
    g_cancellable_cancel(d->cancellable);
    fail(task, g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "HTTP status %u", status));
    return;
  }
  SoupMessageHeaders *h = soup_message_get_response_headers(d->msg);
  goffset cl = soup_message_headers_get_content_length(h);
  if (cl > 0 && (guint64)cl > d->max) {
    g_cancellable_cancel(d->cancellable);
    fail(task, g_error_new(G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE, "Content-Length too large"));
    return;
  }
  const char *ct = soup_message_headers_get_content_type(h, NULL);
  d->content_type = g_strdup(ct);
  d->buf = g_byte_array_new();
  read_more(task);
}

static void on_outer_cancelled(GCancellable *c, gpointer user_data) {
  g_cancellable_cancel(G_CANCELLABLE(user_data));
}

void nsp_http_get_async(SoupSession *session, const char *url, const char *accept,
                        gsize max_bytes, gboolean follow_redirects, GCancellable *cancellable,
                        GAsyncReadyCallback callback, gpointer user_data) {
  GTask *task = g_task_new(NULL, NULL, callback, user_data);
  g_task_set_source_tag(task, nsp_http_get_async);
  GetData *d = g_new0(GetData, 1);
  d->max = max_bytes;
  d->follow = follow_redirects;
  d->cancellable = g_cancellable_new();
  g_task_set_task_data(task, d, get_data_free);

  if (!url || !g_str_has_prefix(url, "https://") ||
      !(d->msg = soup_message_new("GET", url))) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "only https:// URLs are fetched");
    g_object_unref(task);
    return;
  }
  if (cancellable) {
    d->outer = g_object_ref(cancellable);
    d->outer_id = g_cancellable_connect(cancellable, G_CALLBACK(on_outer_cancelled),
                                        d->cancellable, NULL);
  }
  if (!follow_redirects) soup_message_add_flags(d->msg, SOUP_MESSAGE_NO_REDIRECT);
  if (accept) soup_message_headers_replace(soup_message_get_request_headers(d->msg), "Accept", accept);
  g_signal_connect(d->msg, "network-event", G_CALLBACK(on_network_event), d);
  g_signal_connect(d->msg, "restarted", G_CALLBACK(on_restarted), d);
  soup_session_send_async(session, d->msg, G_PRIORITY_DEFAULT, d->cancellable, on_sent, task);
}

GBytes *nsp_http_get_finish(GAsyncResult *res, char **content_type_out, GError **error) {
  GTask *task = G_TASK(res);
  GBytes *b = g_task_propagate_pointer(task, error);
  if (content_type_out) {
    GetData *d = g_task_get_task_data(task);
    *content_type_out = b ? g_strdup(d->content_type) : NULL;
  }
  return b;
}
