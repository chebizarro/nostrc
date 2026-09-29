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
  GhNetHttp *owner;       /* a strong reference, released last: GTask drops its
                           * source object before freeing its task data */
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
  gboolean any_success;   /* G21 send: any 2xx is success, and HTTP errors are
                           * told apart (gh_net_http_send_async) */
  gboolean own_session;   /* session is the request's alone (Tor, or public only) */
} Request;

static void
request_free(gpointer data)
{
  Request *request = data;
  GhNetHttp *owner = request->owner;
  if (owner->requests)
    g_ptr_array_remove_fast(owner->requests, request);
  if (request->caller)
    g_cancellable_disconnect(request->caller, request->caller_handler);
  g_clear_object(&request->caller);
  g_clear_object(&request->cancellable);
  g_clear_object(&request->message);
  /* A Tor or public-only request's session is its own: close its connection
   * with it (disposing a session with a live connection is a libsoup
   * warning). */
  if (request->own_session && request->session)
    soup_session_abort(request->session);
  g_clear_object(&request->session);
  g_clear_object(&request->stream);
  g_free(request->buffer);
  g_free(request);
  g_object_unref(owner);
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

/* remote: every connection's target instead of the URI's host (NULL: the
 * URI's host); see public_session_new(). */
static SoupSession *
session_new(GProxyResolver *resolver, GSocketConnectable *remote)
{
  SoupSession *session = soup_session_new_with_options(
    "timeout", GH_NET_HTTP_TIMEOUT_S, "idle-timeout", GH_NET_HTTP_TIMEOUT_S,
    "proxy-resolver", resolver, "user-agent", NULL, "accept-language-auto", FALSE,
    "remote-connectable", remote, NULL);
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
    return resolver ? session_new(resolver, NULL) : NULL;
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
    self->session = session_new(resolver, NULL);
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

/* ---- public addresses (nostrc-qi5e) ---------------------------------------------- */

static gboolean
ipv4_public(const guint8 *bytes)
{
  g_autoptr(GInetAddress) v4 = g_inet_address_new_from_bytes(bytes, G_SOCKET_FAMILY_IPV4);
  return gh_net_address_is_public(v4);
}

/* 64:ff9b:1::/48 (RFC 8215) is a network's own NAT64 prefix, of any RFC 6052
 * length from /48 to /96, so where the IPv4 address sits is not known. Bits
 * 64-71 are always zero, and so are the bits after the IPv4 address: every
 * layout the address fits is decoded, and all of them must be public. */
static gboolean
nat64_local_public(const guint8 *b)
{
  static const struct {
    guint8 at[4];  /* the IPv4 address's bytes */
    guint8 suffix; /* the first byte after it */
  } layouts[] = {
    { { 6, 7, 9, 10 }, 11 },   /* /48 */
    { { 7, 9, 10, 11 }, 12 },  /* /56 */
    { { 9, 10, 11, 12 }, 13 }, /* /64 */
    { { 12, 13, 14, 15 }, 16 }, /* /96 */
  };
  if (b[8] != 0)
    return FALSE;
  gboolean any = FALSE;
  for (guint i = 0; i < G_N_ELEMENTS(layouts); i++) {
    gboolean fits = TRUE;
    for (guint j = layouts[i].suffix; j < 16; j++)
      fits &= b[j] == 0;
    if (!fits)
      continue;
    const guint8 v4[4] = { b[layouts[i].at[0]], b[layouts[i].at[1]], b[layouts[i].at[2]],
                           b[layouts[i].at[3]] };
    if (!ipv4_public(v4))
      return FALSE;
    any = TRUE;
  }
  return any;
}

gboolean
gh_net_address_is_public(GInetAddress *address)
{
  g_return_val_if_fail(G_IS_INET_ADDRESS(address), FALSE);
  if (g_inet_address_get_is_loopback(address) || g_inet_address_get_is_link_local(address) ||
      g_inet_address_get_is_site_local(address) || g_inet_address_get_is_multicast(address) ||
      g_inet_address_get_is_any(address))
    return FALSE;
  const guint8 *b = g_inet_address_to_bytes(address);
  if (g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV4)
    return !(b[0] == 0 || b[0] >= 240 ||                /* "this network", reserved */
             (b[0] == 100 && (b[1] & 0xc0) == 64) ||    /* 100.64/10 shared (CGNAT) */
             (b[0] == 192 && b[1] == 0 && b[2] == 0) || /* 192.0.0/24 IETF */
             (b[0] == 198 && (b[1] & 0xfe) == 18));     /* 198.18/15 benchmarking */
  /* IPv6 forms that carry an IPv4 address: the IPv4 address decides. */
  static const guint8 zero[12] = { 0 };
  if (memcmp(b, zero, 10) == 0 &&
      ((b[10] == 0 && b[11] == 0) ||       /* ::a.b.c.d, IPv4-compatible */
       (b[10] == 0xff && b[11] == 0xff)))  /* ::ffff:a.b.c.d, IPv4-mapped */
    return ipv4_public(b + 12);
  if (memcmp(b, zero, 8) == 0 && b[8] == 0xff && b[9] == 0xff && b[10] == 0 && b[11] == 0)
    return ipv4_public(b + 12);            /* ::ffff:0:a.b.c.d, SIIT */
  if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b) {
    if (memcmp(b + 4, zero, 8) == 0)
      return ipv4_public(b + 12);          /* 64:ff9b::/96, NAT64 */
    if (b[4] == 0 && b[5] == 1)
      return nat64_local_public(b);        /* 64:ff9b:1::/48, local NAT64 */
    return FALSE;                          /* the rest of 64:ff9b::/32 */
  }
  if (b[0] == 0x20 && b[1] == 0x02)
    return ipv4_public(b + 2);             /* 2002::/16, 6to4 */
  if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0 && b[3] == 0) {
    /* 2001::/32, Teredo: the server's address, and the client's inverted. */
    const guint8 client[4] = { b[12] ^ 0xff, b[13] ^ 0xff, b[14] ^ 0xff, b[15] ^ 0xff };
    return ipv4_public(b + 4) && ipv4_public(client);
  }
  if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8)
    return FALSE;                          /* 2001:db8::/32, documentation */
  /* Global unicast is 2000::/3. Outside it: unique local fc00::/7,
   * link-local, multicast, discard-only 100::/64 and the reserved ::/8. */
  return (b[0] & 0xe0) == 0x20;
}

/* The addresses of a host name as a connection tries them, without any that
 * gh_net_address_is_public() refuses. When every one was refused, the end is
 * G_IO_ERROR_PERMISSION_DENIED. */
#define GH_TYPE_PUBLIC_ENUMERATOR (gh_public_enumerator_get_type())
G_DECLARE_FINAL_TYPE(GhPublicEnumerator, gh_public_enumerator, GH, PUBLIC_ENUMERATOR,
                     GSocketAddressEnumerator)
struct _GhPublicEnumerator {
  GSocketAddressEnumerator parent_instance;
  GSocketAddressEnumerator *inner;
  gboolean refused;
  gboolean yielded;
};
G_DEFINE_FINAL_TYPE(GhPublicEnumerator, gh_public_enumerator, G_TYPE_SOCKET_ADDRESS_ENUMERATOR)

static gboolean
socket_address_public(GSocketAddress *address)
{
  return G_IS_INET_SOCKET_ADDRESS(address) &&
         gh_net_address_is_public(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(address)));
}

/* Takes address; NULL when it was refused. */
static GSocketAddress *
public_enumerator_filter(GhPublicEnumerator *self, GSocketAddress *address)
{
  if (socket_address_public(address)) {
    self->yielded = TRUE;
    return address;
  }
  self->refused = TRUE;
  g_object_unref(address);
  return NULL;
}

/* The end of the addresses: inner's error, if any, unless nothing was left
 * after the refusals. */
static void
public_enumerator_end(GhPublicEnumerator *self, GError *inner_error, GError **error)
{
  if (self->refused && !self->yielded) {
    g_clear_error(&inner_error);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "This address leads to your own computer or local network, so it "
                        "isn't used");
  } else if (inner_error) {
    g_propagate_error(error, inner_error);
  }
}

static GSocketAddress *
public_enumerator_next(GSocketAddressEnumerator *enumerator, GCancellable *cancellable,
                       GError **error)
{
  GhPublicEnumerator *self = GH_PUBLIC_ENUMERATOR(enumerator);
  for (;;) {
    GError *inner_error = NULL;
    GSocketAddress *address = g_socket_address_enumerator_next(self->inner, cancellable,
                                                               &inner_error);
    if (!address) {
      public_enumerator_end(self, inner_error, error);
      return NULL;
    }
    if ((address = public_enumerator_filter(self, address)))
      return address;
  }
}

static void
on_inner_next(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GTask) task = data;
  GhPublicEnumerator *self = g_task_get_source_object(task);
  GError *inner_error = NULL;
  GSocketAddress *address = g_socket_address_enumerator_next_finish(
    G_SOCKET_ADDRESS_ENUMERATOR(source), result, &inner_error);
  if (!address) {
    GError *error = NULL;
    public_enumerator_end(self, inner_error, &error);
    if (error)
      g_task_return_error(task, error);
    else
      g_task_return_pointer(task, NULL, NULL);
    return;
  }
  if ((address = public_enumerator_filter(self, address))) {
    g_task_return_pointer(task, address, g_object_unref);
    return;
  }
  /* Read the cancellable before handing the task on: in one call's argument
   * list the order is unspecified, and GCC on x86_64 evaluated
   * g_steal_pointer() first, passing NULL to g_task_get_cancellable()
   * (nostrc-qp24.91). */
  GCancellable *cancellable = g_task_get_cancellable(task);
  g_socket_address_enumerator_next_async(self->inner, cancellable, on_inner_next,
                                         g_steal_pointer(&task));
}

static void
public_enumerator_next_async(GSocketAddressEnumerator *enumerator, GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer data)
{
  GhPublicEnumerator *self = GH_PUBLIC_ENUMERATOR(enumerator);
  GTask *task = g_task_new(self, cancellable, callback, data);
  g_task_set_source_tag(task, public_enumerator_next_async);
  g_socket_address_enumerator_next_async(self->inner, cancellable, on_inner_next, task);
}

static GSocketAddress *
public_enumerator_next_finish(GSocketAddressEnumerator *enumerator, GAsyncResult *result,
                              GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, enumerator), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
gh_public_enumerator_dispose(GObject *object)
{
  g_clear_object(&GH_PUBLIC_ENUMERATOR(object)->inner);
  G_OBJECT_CLASS(gh_public_enumerator_parent_class)->dispose(object);
}

static void
gh_public_enumerator_class_init(GhPublicEnumeratorClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_public_enumerator_dispose;
  GSocketAddressEnumeratorClass *enumerator = G_SOCKET_ADDRESS_ENUMERATOR_CLASS(klass);
  enumerator->next = public_enumerator_next;
  enumerator->next_async = public_enumerator_next_async;
  enumerator->next_finish = public_enumerator_next_finish;
}

static void
gh_public_enumerator_init(GhPublicEnumerator *self)
{
  (void)self;
}

/* A host name (with port and scheme) whose direct addresses are enumerated
 * through a GhPublicEnumerator. A GNetworkAddress otherwise: libsoup takes it
 * as the TLS server identity (the name for SNI and certificate checks), and
 * its proxy enumeration asks the session's proxy resolver first, enumerating
 * this object for a direct connection only. */
#define GH_TYPE_PUBLIC_ADDRESS (gh_public_address_get_type())
G_DECLARE_FINAL_TYPE(GhPublicAddress, gh_public_address, GH, PUBLIC_ADDRESS, GNetworkAddress)
struct _GhPublicAddress {
  GNetworkAddress parent_instance;
};

static GSocketConnectableIface *public_address_parent_iface;

static GSocketAddressEnumerator *
public_address_enumerate(GSocketConnectable *connectable)
{
  GhPublicEnumerator *enumerator = g_object_new(GH_TYPE_PUBLIC_ENUMERATOR, NULL);
  enumerator->inner = public_address_parent_iface->enumerate(connectable);
  return G_SOCKET_ADDRESS_ENUMERATOR(enumerator);
}

static void
public_address_connectable_init(GSocketConnectableIface *iface)
{
  public_address_parent_iface = g_type_interface_peek_parent(iface);
  iface->enumerate = public_address_enumerate;
  iface->proxy_enumerate = public_address_parent_iface->proxy_enumerate;
  iface->to_string = public_address_parent_iface->to_string;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(GhPublicAddress, gh_public_address, G_TYPE_NETWORK_ADDRESS,
                              G_IMPLEMENT_INTERFACE(G_TYPE_SOCKET_CONNECTABLE,
                                                    public_address_connectable_init))

static void
gh_public_address_class_init(GhPublicAddressClass *klass)
{
  (void)klass;
}

static void
gh_public_address_init(GhPublicAddress *self)
{
  (void)self;
}

/* A session of the request's own whose every connection goes to parsed's
 * host through a GhPublicAddress: the addresses checked are the addresses
 * connected to, so a DNS answer cannot change between the check and the
 * connection, and no connection kept from an earlier request is reused. */
GSocketConnectable *
gh_net_public_address_new(const gchar *hostname, guint16 port, const gchar *scheme)
{
  g_return_val_if_fail(hostname != NULL && scheme != NULL, NULL);
  g_autofree gchar *lower_scheme = g_ascii_strdown(scheme, -1);
  return g_object_new(GH_TYPE_PUBLIC_ADDRESS, "hostname", hostname, "port", (guint)port, "scheme",
                      lower_scheme, NULL);
}

static SoupSession *
public_session_new(GhNetMode mode, GUri *parsed)
{
  const gchar *scheme = g_uri_get_scheme(parsed);
  gboolean https = g_ascii_strcasecmp(scheme, "https") == 0;
  gint port = g_uri_get_port(parsed);
  g_autoptr(GSocketConnectable) remote = gh_net_public_address_new(
    g_uri_get_host(parsed), (guint16)(port > 0 ? port : https ? 443 : 80), scheme);
  g_autoptr(GProxyResolver) resolver = gh_net_proxy_resolver_new(mode, NULL, NULL, NULL);
  return session_new(resolver, remote);
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

/* G21: the server's own short reason (Blossom's X-Reason, BUD-01), kept only
 * when it is plain printable ASCII, and cut. */
static gchar *
server_reason(SoupMessage *message)
{
  const gchar *reason = soup_message_headers_get_one(
    soup_message_get_response_headers(message), "X-Reason");
  if (!reason || !*reason)
    return NULL;
  GString *out = g_string_new(NULL);
  for (const gchar *p = reason; *p && out->len < 160; p++)
    if (*p >= 0x20 && *p < 0x7f)
      g_string_append_c(out, *p);
  return g_string_free(out, out->len == 0);
}

G_DEFINE_QUARK(gh-net-http-error-quark, gh_net_http_error)

/* A send's HTTP failure: GH_NET_HTTP_ERROR with the status as its code, so a
 * caller tells "the server refused" apart from Groundhog's own refusals
 * (e.g. a .onion outside Tor, G_IO_ERROR_PERMISSION_DENIED). */
static GError *
status_error(SoupMessage *message, guint status)
{
  g_autofree gchar *reason = server_reason(message);
  return reason ? g_error_new(GH_NET_HTTP_ERROR, (gint)status,
                              "The server answered HTTP %u: %s", status, reason)
                : g_error_new(GH_NET_HTTP_ERROR, (gint)status, "The server answered HTTP %u",
                              status);
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
  if (request->any_success) {
    if (!SOUP_STATUS_IS_SUCCESSFUL(status)) {
      g_task_return_error(task, status_error(request->message, status));
      return;
    }
  } else if (status != SOUP_STATUS_OK) {
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

/* One request: a GET (gh_net_http_get_accept_async) or, for G21, any
 * gh_net_http_send_async() one (send set). */
static void
request_start(GhNetHttp *self, const GhNetHttpRequest *send, const gchar *uri,
              const gchar *accept, gsize max_bytes, gboolean public_only,
              GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
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
  /* Public only: checked where the connection is made, in System and No
   * Proxy modes; through Tor (or a desktop proxy) the proxy resolves. */
  gboolean own_session = mode == GH_NET_MODE_TOR || public_only;
  g_autoptr(SoupSession) session = public_only && mode != GH_NET_MODE_TOR
                                     ? public_session_new(mode, parsed)
                                     : session_for(self, mode, &error);
  if (!session) {
    /* Fail closed (P5): an unusable Tor address never means "direct". */
    g_task_return_error(task, error);
    return;
  }
  Request *request = g_new0(Request, 1);
  request->owner = g_object_ref(self);
  request->mode = mode;
  request->max_bytes = max_bytes;
  request->session = g_object_ref(session);
  request->message = soup_message_new_from_uri(send && send->method ? send->method
                                                                   : SOUP_METHOD_GET, parsed);
  request->any_success = send != NULL;
  request->own_session = own_session;
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
  if (send && send->authorization)
    soup_message_headers_replace(headers, "Authorization", send->authorization);
  if (send && send->body)
    soup_message_set_request_body_from_bytes(request->message,
                                             send->content_type ? send->content_type
                                                                : "application/octet-stream",
                                             send->body);
  soup_session_send_async(session, request->message, G_PRIORITY_DEFAULT, request->cancellable,
                          on_sent, g_steal_pointer(&task));
}

void
gh_net_http_get_accept_async(GhNetHttp *self, const gchar *uri, const gchar *accept,
                             gsize max_bytes, GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_NET_HTTP(self));
  g_return_if_fail(uri != NULL && max_bytes > 0 && max_bytes < G_MAXSIZE);
  g_return_if_fail(!accept || (*accept && !strpbrk(accept, "\r\n")));
  request_start(self, NULL, uri, accept, max_bytes, FALSE, cancellable, callback, user_data);
}

void
gh_net_http_get_public_async(GhNetHttp *self, const gchar *uri, const gchar *accept,
                             gsize max_bytes, GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_NET_HTTP(self));
  g_return_if_fail(uri != NULL && max_bytes > 0 && max_bytes < G_MAXSIZE);
  g_return_if_fail(!accept || (*accept && !strpbrk(accept, "\r\n")));
  request_start(self, NULL, uri, accept, max_bytes, TRUE, cancellable, callback, user_data);
}

static gboolean
header_value_ok(const gchar *value)
{
  return !value || (*value && !strpbrk(value, "\r\n"));
}

void
gh_net_http_send_async(GhNetHttp *self, const GhNetHttpRequest *request,
                       GCancellable *cancellable, GAsyncReadyCallback callback,
                       gpointer user_data)
{
  g_return_if_fail(GH_IS_NET_HTTP(self));
  g_return_if_fail(request != NULL && request->uri != NULL);
  g_return_if_fail(request->max_bytes > 0 && request->max_bytes < G_MAXSIZE);
  g_return_if_fail(!request->method || g_str_equal(request->method, SOUP_METHOD_GET) ||
                   g_str_equal(request->method, SOUP_METHOD_PUT) ||
                   g_str_equal(request->method, SOUP_METHOD_HEAD));
  g_return_if_fail(header_value_ok(request->accept) && header_value_ok(request->authorization) &&
                   header_value_ok(request->content_type));
  request_start(self, request, request->uri, request->accept, request->max_bytes, FALSE,
                cancellable, callback, user_data);
}

GBytes *
gh_net_http_send_finish(GhNetHttp *self, GAsyncResult *result, GError **error)
{
  return gh_net_http_get_finish(self, result, error);
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
