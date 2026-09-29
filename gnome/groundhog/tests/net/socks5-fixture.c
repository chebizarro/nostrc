#include "socks5-fixture.h"

#include <string.h>

struct _Socks5Fixture {
  GSocketService *service;
  gchar *address;
  gboolean refuse;
  guint16 domain_port;
  GPtrArray *requests;
  guint greetings;
  guint connects;
  GPtrArray *streams;      /* every open stream, closed on free */
  GCancellable *cancellable;
};

typedef enum {
  STEP_GREETING,
  STEP_METHODS,
  STEP_AUTH_HEAD,
  STEP_AUTH_USER,
  STEP_AUTH_PLEN,
  STEP_AUTH_PASS,
  STEP_REQUEST,
  STEP_ADDR_LEN,
  STEP_ADDR
} Step;

typedef struct {
  Socks5Fixture *fixture;
  GIOStream *client;
  Step step;
  guint8 buffer[300];
  gsize want;
  Socks5Request *request;  /* being read */
} Conn;

static void
request_free(gpointer data)
{
  Socks5Request *request = data;
  g_free(request->host);
  g_free(request->username);
  g_free(request->password);
  g_free(request);
}

static void
conn_free(Conn *conn)
{
  if (conn->request)
    request_free(conn->request);
  g_clear_object(&conn->client);
  g_free(conn);
}

static void
conn_close(Conn *conn)
{
  (void)g_io_stream_close(conn->client, NULL, NULL);
  conn_free(conn);
}

static void read_next(Conn *conn, Step step, gsize want);

static void
reply(Conn *conn, const guint8 *bytes, gsize length)
{
  GOutputStream *output = g_io_stream_get_output_stream(conn->client);
  /* Small replies on a fresh socket: a blocking write cannot stall. */
  (void)g_output_stream_write_all(output, bytes, length, NULL, NULL, NULL);
}

static void
reply_status(Conn *conn, guint8 status)
{
  const guint8 bytes[] = { 0x05, status, 0x00, 0x01, 127, 0, 0, 1, 0, 0 };
  reply(conn, bytes, sizeof bytes);
}

static void
on_spliced(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  (void)data;
  (void)g_io_stream_splice_finish(result, NULL);
}

static void
on_upstream(GObject *source, GAsyncResult *result, gpointer data)
{
  Conn *conn = data;
  g_autoptr(GError) error = NULL;
  GSocketConnection *upstream =
    g_socket_client_connect_finish(G_SOCKET_CLIENT(source), result, &error);
  if (!upstream) {
    /* Cancelled: the fixture may be gone already. */
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      reply_status(conn, 0x05); /* connection refused */
    conn_close(conn);
    return;
  }
  reply_status(conn, 0x00);
  g_ptr_array_add(conn->fixture->streams, g_object_ref(upstream));
  g_io_stream_splice_async(conn->client, G_IO_STREAM(upstream),
                           G_IO_STREAM_SPLICE_CLOSE_STREAM1 | G_IO_STREAM_SPLICE_CLOSE_STREAM2,
                           G_PRIORITY_DEFAULT, conn->fixture->cancellable, on_spliced, NULL);
  g_object_unref(upstream);
  conn_free(conn);
}

static void
connect_request(Conn *conn)
{
  Socks5Fixture *fixture = conn->fixture;
  Socks5Request *request = g_steal_pointer(&conn->request);
  g_ptr_array_add(fixture->requests, request);
  fixture->connects++;
  if (fixture->refuse) {
    reply_status(conn, 0x05);
    conn_close(conn);
    return;
  }
  /* The fixture, not the client, reaches the destination, and always
   * directly: never through a proxy of its own. */
  g_autoptr(GSocketClient) client = g_socket_client_new();
  g_socket_client_set_enable_proxy(client, FALSE);
  if (request->atyp == 0x03) {
    guint16 port = fixture->domain_port ? fixture->domain_port : request->port;
    g_socket_client_connect_to_host_async(client, "127.0.0.1", port, fixture->cancellable,
                                          on_upstream, conn);
  } else {
    g_autoptr(GSocketAddress) address = g_inet_socket_address_new_from_string(request->host,
                                                                              request->port);
    g_socket_client_connect_async(client, G_SOCKET_CONNECTABLE(address), fixture->cancellable,
                                  on_upstream, conn);
  }
}

static void
on_read(GObject *source, GAsyncResult *result, gpointer data)
{
  Conn *conn = data;
  gsize read = 0;
  if (!g_input_stream_read_all_finish(G_INPUT_STREAM(source), result, &read, NULL) ||
      read != conn->want) {
    conn_close(conn);
    return;
  }
  guint8 *b = conn->buffer;
  switch (conn->step) {
  case STEP_GREETING:
    if (b[0] != 0x05 || b[1] == 0) {
      conn_close(conn);
      return;
    }
    conn->fixture->greetings++;
    read_next(conn, STEP_METHODS, b[1]);
    return;
  case STEP_METHODS: {
    gboolean none = FALSE, password = FALSE;
    for (gsize i = 0; i < conn->want; i++) {
      none |= b[i] == 0x00;
      password |= b[i] == 0x02;
    }
    guint8 method = password ? 0x02 : none ? 0x00 : 0xff;
    const guint8 answer[] = { 0x05, method };
    reply(conn, answer, sizeof answer);
    if (method == 0xff) {
      conn_close(conn);
      return;
    }
    conn->request = g_new0(Socks5Request, 1);
    conn->request->method = method;
    if (method == 0x02)
      read_next(conn, STEP_AUTH_HEAD, 2);
    else
      read_next(conn, STEP_REQUEST, 4);
    return;
  }
  case STEP_AUTH_HEAD:
    if (b[0] != 0x01 || b[1] == 0) {
      conn_close(conn);
      return;
    }
    read_next(conn, STEP_AUTH_USER, b[1]);
    return;
  case STEP_AUTH_USER:
    conn->request->username = g_strndup((const gchar *)b, conn->want);
    read_next(conn, STEP_AUTH_PLEN, 1);
    return;
  case STEP_AUTH_PLEN:
    if (b[0] == 0) {
      conn->request->password = g_strdup("");
      const guint8 ok[] = { 0x01, 0x00 };
      reply(conn, ok, sizeof ok);
      read_next(conn, STEP_REQUEST, 4);
    } else {
      read_next(conn, STEP_AUTH_PASS, b[0]);
    }
    return;
  case STEP_AUTH_PASS: {
    conn->request->password = g_strndup((const gchar *)b, conn->want);
    const guint8 ok[] = { 0x01, 0x00 };
    reply(conn, ok, sizeof ok);
    read_next(conn, STEP_REQUEST, 4);
    return;
  }
  case STEP_REQUEST:
    if (b[0] != 0x05 || b[1] != 0x01) {
      reply_status(conn, 0x07); /* only CONNECT */
      conn_close(conn);
      return;
    }
    conn->request->atyp = b[3];
    if (b[3] == 0x01)
      read_next(conn, STEP_ADDR, 4 + 2);
    else if (b[3] == 0x04)
      read_next(conn, STEP_ADDR, 16 + 2);
    else if (b[3] == 0x03)
      read_next(conn, STEP_ADDR_LEN, 1);
    else
      conn_close(conn);
    return;
  case STEP_ADDR_LEN:
    read_next(conn, STEP_ADDR, b[0] + 2);
    return;
  case STEP_ADDR: {
    gsize host_length = conn->want - 2;
    if (conn->request->atyp == 0x03) {
      conn->request->host = g_strndup((const gchar *)b, host_length);
    } else {
      g_autoptr(GInetAddress) address =
        g_inet_address_new_from_bytes(b, conn->request->atyp == 0x01 ? G_SOCKET_FAMILY_IPV4
                                                                     : G_SOCKET_FAMILY_IPV6);
      conn->request->host = g_inet_address_to_string(address);
    }
    conn->request->port = (guint16)((b[host_length] << 8) | b[host_length + 1]);
    connect_request(conn);
    return;
  }
  }
}

static void
read_next(Conn *conn, Step step, gsize want)
{
  g_assert(want <= sizeof conn->buffer);
  conn->step = step;
  conn->want = want;
  GInputStream *input = g_io_stream_get_input_stream(conn->client);
  g_input_stream_read_all_async(input, conn->buffer, want, G_PRIORITY_DEFAULT,
                                conn->fixture->cancellable, on_read, conn);
}

static gboolean
on_incoming(GSocketService *service, GSocketConnection *connection, GObject *source,
            gpointer data)
{
  (void)service;
  (void)source;
  Socks5Fixture *fixture = data;
  Conn *conn = g_new0(Conn, 1);
  conn->fixture = fixture;
  conn->client = G_IO_STREAM(g_object_ref(connection));
  g_ptr_array_add(fixture->streams, g_object_ref(connection));
  read_next(conn, STEP_GREETING, 2);
  return TRUE;
}

Socks5Fixture *
socks5_fixture_new(void)
{
  Socks5Fixture *fixture = g_new0(Socks5Fixture, 1);
  fixture->requests = g_ptr_array_new_with_free_func(request_free);
  fixture->streams = g_ptr_array_new_with_free_func(g_object_unref);
  fixture->cancellable = g_cancellable_new();
  fixture->service = g_socket_service_new();
  g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
  g_autoptr(GSocketAddress) any = g_inet_socket_address_new(loopback, 0);
  g_autoptr(GSocketAddress) bound = NULL;
  g_autoptr(GError) error = NULL;
  g_assert_true(g_socket_listener_add_address(G_SOCKET_LISTENER(fixture->service), any,
                                              G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL,
                                              &bound, &error));
  g_assert_no_error(error);
  fixture->address = g_strdup_printf(
    "127.0.0.1:%u", g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound)));
  g_signal_connect(fixture->service, "incoming", G_CALLBACK(on_incoming), fixture);
  g_socket_service_start(fixture->service);
  return fixture;
}

void
socks5_fixture_free(Socks5Fixture *fixture)
{
  if (!fixture)
    return;
  /* Stop accepting, cancel reads, connects and splices, and let all of them
   * finish (a splice closes both its streams) before any socket, the
   * listener included, is closed under a pending poll. */
  g_signal_handlers_disconnect_by_data(fixture->service, fixture);
  g_socket_service_stop(fixture->service);
  g_cancellable_cancel(fixture->cancellable);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_socket_listener_close(G_SOCKET_LISTENER(fixture->service));
  for (guint i = 0; i < fixture->streams->len; i++)
    (void)g_io_stream_close(g_ptr_array_index(fixture->streams, i), NULL, NULL);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_object_unref(fixture->service);
  g_ptr_array_unref(fixture->streams);
  g_ptr_array_unref(fixture->requests);
  g_object_unref(fixture->cancellable);
  g_free(fixture->address);
  g_free(fixture);
}

const gchar *
socks5_fixture_address(Socks5Fixture *fixture)
{
  return fixture->address;
}

void
socks5_fixture_set_refuse(Socks5Fixture *fixture, gboolean refuse)
{
  fixture->refuse = refuse;
}

void
socks5_fixture_set_domain_port(Socks5Fixture *fixture, guint16 port)
{
  fixture->domain_port = port;
}

GPtrArray *
socks5_fixture_requests(Socks5Fixture *fixture)
{
  return fixture->requests;
}

guint
socks5_fixture_greetings(Socks5Fixture *fixture)
{
  return fixture->greetings;
}

const guint *
socks5_fixture_connect_count(Socks5Fixture *fixture)
{
  return &fixture->connects;
}
