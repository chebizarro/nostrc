/* W16 review B1 (privacy charter PD-6, R1): no Groundhog TLS connection
 * offers to resume a TLS session, so a server cannot link two of Groundhog's
 * connections to it (two Tor circuits, two accounts, or a Tor connection and
 * an earlier direct one) through glib-networking's process-wide session
 * cache (src/net/gh-net-tls.h).
 *
 * A helper program, not a GTest one: glib-networking turns resumption off in
 * any process that called g_test_init() (session-resumption-enabled defaults
 * to !g_test_initialized()), so no GTest binary can see what the app does.
 * It aborts on a failed check, exits 77 without a TLS backend, else 0.
 *
 * The TLS server is OpenSSL's (glib-networking's server issues no session
 * tickets, so a client could never resume with it), one thread per
 * connection, with the certificate below, which this process installs as
 * GLib's default TLS database. Its ClientHello callback records whether each
 * ClientHello offers a session: a TLS 1.3 pre_shared_key (extension 41) or a
 * non-empty TLS 1.2 SessionTicket (35); SSL_session_reused() says whether
 * the handshake resumed one. It answers HTTP GETs with "{}" and accepts
 * WebSocket upgrades.
 *  1. Control: two plain SoupSessions to control.test. The second MUST offer
 *     the first's session; otherwise this harness could not see what it
 *     tests.
 *  2. GhNetHttp: a No Proxy fetch, then two Tor-mode fetches through the
 *     SOCKS5 fixture (H3), all to resumption.test.
 *  3. The relay transport: two Tor-mode scopes (distinct SOCKS credentials)
 *     through the dispatcher, to resumption.test.
 * No connection in 2 or 3 may offer or resume a session. Host names resolve
 * to 127.0.0.1 through a resolver installed here; waits iterate the main
 * context, and their deadlines are failure bounds only. */
#include "gh-net-http.h"
#include "gh-net-session.h"
#include "gh-relay-net.h"
#include "gh-relay-scope.h"
#include "socks5-fixture.h"

#include <arpa/inet.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <netinet/in.h>
#include <nostr-filter.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define HOST "resumption.test"
#define CONTROL_HOST "control.test"

/* A self-signed certificate for resumption.test and control.test (valid
 * until 2126) and its key, made for this loopback fixture only. */
static const gchar CERTIFICATE[] =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIB3TCCAYKgAwIBAgIUHevpLEgo+K/trobOeR73QyPScwAwCgYIKoZIzj0EAwIw\n"
  "GjEYMBYGA1UEAwwPcmVzdW1wdGlvbi50ZXN0MCAXDTI2MDkyOTA5MjQzNFoYDzIx\n"
  "MjYwOTA1MDkyNDM0WjAaMRgwFgYDVQQDDA9yZXN1bXB0aW9uLnRlc3QwWTATBgcq\n"
  "hkjOPQIBBggqhkjOPQMBBwNCAAT6zFlagNdq0aQQy59hcbdINTyftq/DgKoZ0s7s\n"
  "CLYEv2pKKash5FSfszaUUoJPyoMy6Vb01vbXlzk/uov42WU6o4GjMIGgMB0GA1Ud\n"
  "DgQWBBSnKpEr53uYXpmuvaN2Qpt7MPU7JjAfBgNVHSMEGDAWgBSnKpEr53uYXpmu\n"
  "vaN2Qpt7MPU7JjAoBgNVHREEITAfgg9yZXN1bXB0aW9uLnRlc3SCDGNvbnRyb2wu\n"
  "dGVzdDAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIChDATBgNVHSUEDDAK\n"
  "BggrBgEFBQcDATAKBggqhkjOPQQDAgNJADBGAiEAhKFs0pCXAg1Yd5WO6uJ2PUNE\n"
  "Z4oj6tHCMSHwUF2ObRUCIQCIkhxY/AP2kJ3h3oLOj+FHa0VEvVUtJDwcIIDQJoB+\n"
  "Mg==\n"
  "-----END CERTIFICATE-----\n";
static const gchar KEY[] =
  "-----BEGIN PRIVATE KEY-----\n"
  "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgPrVUrEAMQlSkxbMc\n"
  "amj7T45p1Fp2wV/P6P7Wbx4AE6ShRANCAAT6zFlagNdq0aQQy59hcbdINTyftq/D\n"
  "gKoZ0s7sCLYEv2pKKash5FSfszaUUoJPyoMy6Vb01vbXlzk/uov42WU6\n"
  "-----END PRIVATE KEY-----\n";

/* ---- waits ------------------------------------------------------------------------- */

typedef gboolean (*Condition)(gpointer data);

static gboolean
expire(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static void
spin_until(Condition condition, gpointer data)
{
  gboolean timed_out = FALSE;
  guint bound = g_timeout_add_seconds(20, expire, &timed_out);
  while (!condition(data) && !timed_out)
    g_main_context_iteration(NULL, TRUE);
  if (!timed_out)
    g_source_remove(bound);
  g_assert_true(condition(data));
}

/* Counters the server threads bump (and then wake the main context). */
static gboolean
count_at_least(gpointer data)
{
  gint *const *counter = data;
  return g_atomic_int_get(counter[0]) >= GPOINTER_TO_INT(counter[1]);
}

static void
wait_at_least(gint *counter, gint count)
{
  gint *pair[] = { counter, GINT_TO_POINTER(count) };
  spin_until(count_at_least, pair);
}

/* ---- every host name is 127.0.0.1 -------------------------------------------------- */

#define LOOP_TYPE_RESOLVER (loop_resolver_get_type())
G_DECLARE_FINAL_TYPE(LoopResolver, loop_resolver, LOOP, RESOLVER, GResolver)
struct _LoopResolver {
  GResolver parent_instance;
};
G_DEFINE_FINAL_TYPE(LoopResolver, loop_resolver, G_TYPE_RESOLVER)

static GList *
loop_lookup_flags(GResolver *resolver, const gchar *name, GResolverNameLookupFlags flags,
                  GCancellable *cancellable, GError **error)
{
  (void)resolver;
  (void)cancellable;
  if (flags & G_RESOLVER_NAME_LOOKUP_FLAGS_IPV6_ONLY) {
    g_set_error(error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND, "no IPv6 for %s", name);
    return NULL;
  }
  return g_list_append(NULL, g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4));
}

static GList *
loop_lookup(GResolver *resolver, const gchar *name, GCancellable *cancellable, GError **error)
{
  return loop_lookup_flags(resolver, name, G_RESOLVER_NAME_LOOKUP_FLAGS_DEFAULT, cancellable,
                           error);
}

static void
loop_lookup_flags_async(GResolver *resolver, const gchar *name, GResolverNameLookupFlags flags,
                        GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  g_autoptr(GTask) task = g_task_new(resolver, cancellable, callback, data);
  GError *error = NULL;
  GList *addresses = loop_lookup_flags(resolver, name, flags, cancellable, &error);
  if (addresses)
    g_task_return_pointer(task, addresses, (GDestroyNotify)g_resolver_free_addresses);
  else
    g_task_return_error(task, error);
}

static void
loop_lookup_async(GResolver *resolver, const gchar *name, GCancellable *cancellable,
                  GAsyncReadyCallback callback, gpointer data)
{
  loop_lookup_flags_async(resolver, name, G_RESOLVER_NAME_LOOKUP_FLAGS_DEFAULT, cancellable,
                          callback, data);
}

static GList *
loop_lookup_finish(GResolver *resolver, GAsyncResult *result, GError **error)
{
  (void)resolver;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
loop_resolver_class_init(LoopResolverClass *klass)
{
  GResolverClass *resolver = G_RESOLVER_CLASS(klass);
  resolver->lookup_by_name = loop_lookup;
  resolver->lookup_by_name_async = loop_lookup_async;
  resolver->lookup_by_name_finish = loop_lookup_finish;
  resolver->lookup_by_name_with_flags = loop_lookup_flags;
  resolver->lookup_by_name_with_flags_async = loop_lookup_flags_async;
  resolver->lookup_by_name_with_flags_finish = loop_lookup_finish;
}

static void
loop_resolver_init(LoopResolver *self)
{
  (void)self;
}

/* ---- the TLS server (OpenSSL, a thread per connection) ------------------------------ */

typedef struct {
  gchar *sni;       /* server_name, NULL without one */
  gboolean psk;     /* pre_shared_key (41) */
  gssize ticket;    /* SessionTicket (35) payload length; -1 without the extension */
  gboolean resumed; /* SSL_session_reused() */
  gboolean failed;  /* the handshake failed */
} Hello;

typedef struct {
  SSL_CTX *ctx;
  gint listener;
  guint16 port;
  gint stop;
  GThread *acceptor;
  GMutex lock;
  GPtrArray *workers;  /* Worker, joined at the end */
  GPtrArray *hellos;   /* Hello, in the order the handshakes ended */
  gint handshakes;     /* handshakes that ended */
  gint websockets;     /* upgrades answered */
  gint hits;           /* GETs answered */
} TlsServer;

typedef struct {
  TlsServer *server;
  gint fd;
  GThread *thread;
} Worker;

static void
hello_free(gpointer data)
{
  Hello *hello = data;
  g_free(hello->sni);
  g_free(hello);
}

/* Called for each ClientHello (twice after a HelloRetryRequest). */
static int
on_client_hello(SSL *ssl, int *alert, void *data)
{
  (void)alert;
  (void)data;
  Hello *hello = SSL_get_app_data(ssl);
  const unsigned char *ext = NULL;
  size_t length = 0;
  if (SSL_client_hello_get0_ext(ssl, 41, &ext, &length))
    hello->psk = TRUE;
  if (SSL_client_hello_get0_ext(ssl, 35, &ext, &length))
    hello->ticket = MAX(hello->ticket, (gssize)length);
  /* server_name: list length, name type 0 (host_name), name length, name. */
  if (!hello->sni && SSL_client_hello_get0_ext(ssl, 0, &ext, &length) && length >= 5 &&
      ext[2] == 0 && (size_t)(5 + (ext[3] << 8 | ext[4])) <= length)
    hello->sni = g_strndup((const gchar *)ext + 5, ext[3] << 8 | ext[4]);
  return SSL_CLIENT_HELLO_SUCCESS;
}

static void
bump(gint *counter)
{
  g_atomic_int_inc(counter);
  g_main_context_wakeup(NULL);
}

/* Reads a request head (to its blank line) into buffer; FALSE at EOF. */
static gboolean
read_head(SSL *ssl, gchar *buffer, gsize size)
{
  gsize used = 0;
  while (used + 1 < size) {
    int got = SSL_read(ssl, buffer + used, (int)(size - 1 - used));
    if (got <= 0)
      return FALSE;
    used += (gsize)got;
    buffer[used] = '\0';
    if (strstr(buffer, "\r\n\r\n"))
      return TRUE;
  }
  return FALSE;
}

static gchar *
websocket_accept(const gchar *head)
{
  const gchar *key = g_strstr_len(head, -1, "Sec-WebSocket-Key:");
  if (!key)
    return NULL;
  key += strlen("Sec-WebSocket-Key:");
  while (*key == ' ')
    key++;
  g_autofree gchar *value = g_strndup(key, strcspn(key, "\r\n"));
  g_autofree gchar *input = g_strconcat(value, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", NULL);
  guint8 digest[20];
  gsize digest_length = sizeof digest;
  g_autoptr(GChecksum) sha1 = g_checksum_new(G_CHECKSUM_SHA1);
  g_checksum_update(sha1, (const guchar *)input, -1);
  g_checksum_get_digest(sha1, digest, &digest_length);
  return g_base64_encode(digest, digest_length);
}

static void
write_all(SSL *ssl, const gchar *text)
{
  (void)SSL_write(ssl, text, (int)strlen(text));
}

static gpointer
serve(gpointer data)
{
  Worker *worker = data;
  TlsServer *server = worker->server;
  Hello *hello = g_new0(Hello, 1);
  hello->ticket = -1;
  SSL *ssl = SSL_new(server->ctx);
  SSL_set_app_data(ssl, hello);
  SSL_set_fd(ssl, worker->fd);
  int accepted = SSL_accept(ssl);
  hello->failed = accepted != 1;
  hello->resumed = accepted == 1 && SSL_session_reused(ssl);
  if (hello->failed) {
    g_printerr("server handshake failed:\n");
    ERR_print_errors_fp(stderr);
  }
  g_mutex_lock(&server->lock);
  g_ptr_array_add(server->hellos, hello);
  g_mutex_unlock(&server->lock);
  bump(&server->handshakes);
  gchar head[8192];
  if (!hello->failed && read_head(ssl, head, sizeof head)) {
    g_autofree gchar *accept = websocket_accept(head);
    if (accept) {
      g_autofree gchar *reply = g_strdup_printf("HTTP/1.1 101 Switching Protocols\r\n"
                                                "Upgrade: websocket\r\n"
                                                "Connection: Upgrade\r\n"
                                                "Sec-WebSocket-Accept: %s\r\n\r\n",
                                                accept);
      write_all(ssl, reply);
      bump(&server->websockets);
      /* Kept open (frames ignored) until the client or the end closes it. */
      while (SSL_read(ssl, head, sizeof head) > 0)
        ;
    } else {
      write_all(ssl, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                     "Content-Length: 2\r\nConnection: close\r\n\r\n{}");
      bump(&server->hits);
      (void)SSL_shutdown(ssl);
    }
  }
  SSL_free(ssl);
  return NULL;
}

static gpointer
accept_loop(gpointer data)
{
  TlsServer *server = data;
  while (!g_atomic_int_get(&server->stop)) {
    struct pollfd listener = { server->listener, POLLIN, 0 };
    if (poll(&listener, 1, 50) <= 0)
      continue;
    gint fd = accept(server->listener, NULL, NULL);
    if (fd < 0)
      continue;
    Worker *worker = g_new0(Worker, 1);
    worker->server = server;
    worker->fd = fd;
    g_mutex_lock(&server->lock);
    g_ptr_array_add(server->workers, worker);
    g_mutex_unlock(&server->lock);
    worker->thread = g_thread_new("tls-worker", serve, worker);
  }
  return NULL;
}

static void
server_init(TlsServer *server)
{
  server->ctx = SSL_CTX_new(TLS_server_method());
  g_assert_nonnull(server->ctx);
  BIO *certificate = BIO_new_mem_buf(CERTIFICATE, -1);
  BIO *key = BIO_new_mem_buf(KEY, -1);
  X509 *x509 = PEM_read_bio_X509(certificate, NULL, NULL, NULL);
  EVP_PKEY *pkey = PEM_read_bio_PrivateKey(key, NULL, NULL, NULL);
  g_assert_true(x509 && pkey);
  g_assert_cmpint(SSL_CTX_use_certificate(server->ctx, x509), ==, 1);
  g_assert_cmpint(SSL_CTX_use_PrivateKey(server->ctx, pkey), ==, 1);
  X509_free(x509);
  EVP_PKEY_free(pkey);
  BIO_free(certificate);
  BIO_free(key);
  /* Session tickets are OpenSSL's default (two per TLS 1.3 handshake). */
  SSL_CTX_set_client_hello_cb(server->ctx, on_client_hello, NULL);
  g_mutex_init(&server->lock);
  server->workers = g_ptr_array_new();
  server->hellos = g_ptr_array_new_with_free_func(hello_free);
  server->listener = socket(AF_INET, SOCK_STREAM, 0);
  g_assert_cmpint(server->listener, >=, 0);
  struct sockaddr_in address = { 0 };
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  g_assert_cmpint(bind(server->listener, (struct sockaddr *)&address, sizeof address), ==, 0);
  g_assert_cmpint(listen(server->listener, 16), ==, 0);
  socklen_t length = sizeof address;
  g_assert_cmpint(getsockname(server->listener, (struct sockaddr *)&address, &length), ==, 0);
  server->port = ntohs(address.sin_port);
  server->acceptor = g_thread_new("tls-accept", accept_loop, server);
}

static void
server_clear(TlsServer *server)
{
  g_atomic_int_set(&server->stop, 1);
  g_thread_join(server->acceptor);
  close(server->listener);
  /* Wakes every worker blocked in SSL_read, then waits for it. */
  for (guint i = 0; i < server->workers->len; i++)
    shutdown(((Worker *)g_ptr_array_index(server->workers, i))->fd, SHUT_RDWR);
  for (guint i = 0; i < server->workers->len; i++) {
    Worker *worker = g_ptr_array_index(server->workers, i);
    g_thread_join(worker->thread);
    close(worker->fd);
    g_free(worker);
  }
  g_ptr_array_unref(server->workers);
  g_ptr_array_unref(server->hellos);
  g_mutex_clear(&server->lock);
  SSL_CTX_free(server->ctx);
}

/* Prints hellos[first..] with labels and returns how many offered or
 * resumed a session. */
static guint
report(TlsServer *server, guint first, const gchar *const *labels)
{
  guint offered = 0;
  g_mutex_lock(&server->lock);
  for (guint i = first; i < server->hellos->len; i++) {
    Hello *hello = g_ptr_array_index(server->hellos, i);
    gboolean offers = hello->psk || hello->ticket > 0 || hello->resumed;
    offered += offers;
    g_assert_false(hello->failed);
    printf("%-20s sni=%-16s pre_shared_key=%-3s session_ticket=%-3" G_GSSIZE_FORMAT
           " resumed=%s%s\n",
           labels[i - first], hello->sni ? hello->sni : "-", hello->psk ? "yes" : "no",
           hello->ticket, hello->resumed ? "yes" : "no", offers ? "  <- offers a session" : "");
  }
  g_mutex_unlock(&server->lock);
  fflush(stdout);
  return offered;
}

static Hello *
hello_at(TlsServer *server, guint index)
{
  g_mutex_lock(&server->lock);
  Hello *hello = g_ptr_array_index(server->hellos, index);
  g_mutex_unlock(&server->lock);
  return hello;
}

/* ---- clients ----------------------------------------------------------------------- */

typedef struct {
  gboolean done;
  gboolean ok;
  gchar *error;
} Done;

static gboolean
is_done(gpointer data)
{
  return ((Done *)data)->done;
}

static void
on_raw_fetched(GObject *source, GAsyncResult *result, gpointer data)
{
  Done *done = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
                                                             &error);
  done->ok = body != NULL;
  done->error = error ? g_strdup(error->message) : NULL;
  done->done = TRUE;
}

/* A plain SoupSession of its own, direct, resumption at glib-networking's
 * default: what Groundhog must not be. */
static void
raw_fetch(const gchar *uri)
{
  g_autoptr(GProxyResolver) direct = g_simple_proxy_resolver_new(NULL, NULL);
  g_autoptr(SoupSession) session = soup_session_new_with_options("proxy-resolver", direct, NULL);
  g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_GET, uri);
  Done done = { 0 };
  soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, on_raw_fetched,
                                   &done);
  spin_until(is_done, &done);
  if (!done.ok)
    g_error("control fetch failed: %s", done.error);
  soup_session_abort(session);
}

static void
on_fetched(GObject *source, GAsyncResult *result, gpointer data)
{
  Done *done = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) body = gh_net_http_get_finish(GH_NET_HTTP(source), result, &error);
  done->ok = body != NULL;
  done->error = error ? g_strdup(error->message) : NULL;
  done->done = TRUE;
}

static void
fetch(GhNetHttp *http, const gchar *uri)
{
  Done done = { 0 };
  gh_net_http_get_async(http, uri, 1024, NULL, on_fetched, &done);
  spin_until(is_done, &done);
  if (!done.ok)
    g_error("GhNetHttp fetch failed: %s", done.error);
}

static void
on_scope(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  (void)scope;
  (void)update;
  (void)data;
}

static GhRelayScope *
open_scope(const gchar *url)
{
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  g_assert_true(nostr_filters_add(filters, filter));
  nostr_filter_free(filter);
  GhRelayScope *scope = gh_relay_scope_new(1, filters, on_scope, NULL);
  g_assert_true(gh_relay_scope_add_url(scope, url, NULL));
  gh_relay_scope_start(scope);
  return scope;
}

/* ---- the checks -------------------------------------------------------------------- */

int
main(void)
{
  /* The point of this program: glib-networking resumes sessions here. */
  g_assert_false(g_test_initialized());
  signal(SIGPIPE, SIG_IGN);
  GTlsBackend *backend = g_tls_backend_get_default();
  if (!g_tls_backend_supports_tls(backend)) {
    printf("SKIP: no TLS backend\n");
    return 77;
  }
  g_autoptr(GError) error = NULL;
  g_autofree gchar *ca_path = NULL;
  gint fd = g_file_open_tmp("groundhog-tls-XXXXXX.pem", &ca_path, &error);
  g_assert_no_error(error);
  close(fd);
  g_assert_true(g_file_set_contents(ca_path, CERTIFICATE, -1, &error));
  g_autoptr(GTlsDatabase) trust = g_tls_file_database_new(ca_path, &error);
  g_assert_no_error(error);
  g_tls_backend_set_default_database(backend, trust); /* reads ca_path when first used */
  g_autoptr(GResolver) resolver = g_object_new(LOOP_TYPE_RESOLVER, NULL);
  g_resolver_set_default(resolver);

  TlsServer server = { 0 };
  server_init(&server);
  Socks5Fixture *socks = socks5_fixture_new();
  socks5_fixture_set_domain_port(socks, server.port);

  /* 1. The control, on a host of its own. */
  g_autofree gchar *control_uri = g_strdup_printf("https://" CONTROL_HOST ":%u/", server.port);
  raw_fetch(control_uri);
  raw_fetch(control_uri);
  wait_at_least(&server.handshakes, 2);
  static const gchar *const control_labels[] = { "control 1", "control 2" };
  printf("-- control: a plain SoupSession, which must offer a session the second time\n");
  report(&server, 0, control_labels);
  Hello *control = hello_at(&server, 1);
  if (!(control->psk || control->ticket > 0))
    g_error("the control's second connection offered no session: this harness cannot see "
            "TLS session resumption, so it proves nothing");

  /* 2. GhNetHttp: direct, then two Tor circuits, one host. */
  const guint first = 2;
  g_autofree gchar *uri = g_strdup_printf("https://" HOST ":%u/", server.port);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "network-mode", "none");
  g_autoptr(GhNetHttp) http = gh_net_http_new(settings);
  fetch(http, uri);
  g_settings_set_string(settings, "tor-socks-address", socks5_fixture_address(socks));
  g_settings_set_string(settings, "network-mode", "tor");
  fetch(http, uri);
  fetch(http, uri);
  wait_at_least(&server.handshakes, first + 3);
  g_assert_cmpint(g_atomic_int_get(&server.hits), ==, 5);

  /* 3. The relay transport in Tor mode: two scopes, two circuits. */
  GhNetSession *session = gh_net_session_new(NULL);
  gh_net_session_set_mode(session, GH_NET_MODE_TOR, socks5_fixture_address(socks));
  gh_relay_net_install(session);
  g_autofree gchar *relay = g_strdup_printf("wss://" HOST ":%u/", server.port);
  GhRelayScope *scope1 = open_scope(relay);
  wait_at_least(&server.websockets, 1);
  GhRelayScope *scope2 = open_scope(relay);
  wait_at_least(&server.websockets, 2);
  wait_at_least(&server.handshakes, first + 5);

  static const gchar *const labels[] = { "GhNetHttp no proxy", "GhNetHttp tor 1",
                                         "GhNetHttp tor 2", "relay scope tor 1",
                                         "relay scope tor 2" };
  printf("-- Groundhog: no connection may offer or resume a session\n");
  guint offered = report(&server, first, labels);
  g_assert_cmpuint(server.hellos->len, ==, first + 5);
  for (guint i = first; i < first + 5; i++)
    g_assert_cmpstr(hello_at(&server, i)->sni, ==, HOST);
  /* The four Tor connections had four sets of SOCKS credentials. */
  GPtrArray *requests = socks5_fixture_requests(socks);
  g_assert_cmpuint(requests->len, ==, 4);
  for (guint i = 0; i < requests->len; i++)
    for (guint j = i + 1; j < requests->len; j++)
      g_assert_cmpstr(((Socks5Request *)g_ptr_array_index(requests, i))->username, !=,
                      ((Socks5Request *)g_ptr_array_index(requests, j))->username);
  g_assert_cmpuint(offered, ==, 0);

  gh_relay_scope_cancel(scope1);
  gh_relay_scope_unref(scope1);
  gh_relay_scope_cancel(scope2);
  gh_relay_scope_unref(scope2);
  gh_relay_net_install(NULL);
  g_object_unref(session);
  g_clear_object(&http);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  socks5_fixture_free(socks);
  server_clear(&server);
  g_unlink(ca_path);
  printf("PASS: no Groundhog TLS connection offered or resumed a session\n");
  return 0;
}
