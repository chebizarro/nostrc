/* Ports for tests whose dial must fail (nostrc-gem9, nostrc-f56o). Never
 * reserve a port by binding a listener and closing it: under a parallel
 * ctest the kernel can hand that port to another process in between (a
 * listener, or an outgoing connection's local port), and the dial then
 * reaches a stranger (reset, served) instead of failing.
 *
 * gh_test_refused_port(): nothing ever answers; a dial is refused at once.
 * Loopback port 1 lies below every ephemeral range (macOS 49152-65535, Linux
 * 32768-60999 by default), so the kernel never hands it out for a bind to
 * port 0 or an outgoing connection, and nothing in this suite binds it. A
 * host with a listener there fails the test by name, not as a flake, and so
 * does one whose firewall drops (rather than refuses) loopback traffic to
 * it: the probe gives up after GH_TEST_REFUSED_PORT_PROBE_SECONDS instead
 * of waiting out the kernel's SYN retries past the ctest timeout.
 *
 * GhTestHeldPort: the port of a server that is down now and comes up later
 * on the same URL. A loopback listener, held for the whole test, that closes
 * every connection unanswered (a dial fails) until gh_test_held_port_serve()
 * hands its connections to the server, so the port is never free. (A bound
 * socket that does not listen refuses dials on Linux, but on macOS drops the
 * SYN until the dial times out, and another process can still bind the
 * port's wildcard address. Handing the listening socket itself to libsoup
 * fails on macOS: soup_server_listen_socket() asks for SO_ACCEPTCONN, which
 * macOS does not answer.)
 *
 * gh_test_listen_loopback(): a test's own listener, for dials to 127.0.0.1.
 *
 * Header-only; the path is relative, for every test directory. */
#ifndef GH_TEST_PORT_H
#define GH_TEST_PORT_H

#include <gio/gio.h>
#include <errno.h>
#include <sys/socket.h>

#ifndef GH_TEST_REFUSED_PORT /* overridable only to exercise the probe */
#define GH_TEST_REFUSED_PORT 1
#endif
#define GH_TEST_REFUSED_PORT_PROBE_SECONDS 5

static G_GNUC_UNUSED guint16
gh_test_refused_port(void)
{
  static gsize checked = 0;
  if (g_once_init_enter(&checked)) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocket) socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
                                             G_SOCKET_PROTOCOL_DEFAULT, &error);
    g_assert_no_error(error);
    g_socket_set_timeout(socket, GH_TEST_REFUSED_PORT_PROBE_SECONDS);
    g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    g_autoptr(GSocketAddress) address = g_inet_socket_address_new(loopback,
                                                                  GH_TEST_REFUSED_PORT);
    if (g_socket_connect(socket, address, NULL, &error))
      g_error("something listens on 127.0.0.1:%u; the tests need that port closed",
              GH_TEST_REFUSED_PORT);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
      g_error("a dial to 127.0.0.1:%u got no answer within %u s (a firewall dropping "
              "loopback traffic?); the tests need it refused at once", GH_TEST_REFUSED_PORT,
              GH_TEST_REFUSED_PORT_PROBE_SECONDS);
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED))
      g_error("a dial to 127.0.0.1:%u was not refused: %s", GH_TEST_REFUSED_PORT,
              error->message);
    g_once_init_leave(&checked, 1);
  }
  return GH_TEST_REFUSED_PORT;
}

/* The accept queue of a test's listening sockets (nostrc-pk7zx): as deep as
 * the host allows, as a real relay's or HTTP server's is. GLib's (and so
 * libsoup's) default is 10. A test server accepts on the test's own main
 * thread, so a burst of dials (a publish per event of a message burst opens
 * its own connection) queues up while that thread runs other work, and under
 * load it runs late. On a full queue macOS resets the dial (ECONNRESET, or a
 * queued connection dropped before the WebSocket upgrade) and Linux drops
 * the SYN, so the client retries after 1, 3, 7 s...: either way the code
 * under test sees a relay that refuses or stalls, which these servers are
 * not modelling. */
#define GH_TEST_LISTEN_BACKLOG SOMAXCONN

/* Deepens the queue of sockets already listening (e.g. a SoupServer's, from
 * soup_server_get_listeners()) to GH_TEST_LISTEN_BACKLOG; frees the list.
 * listen(2) again on a listening socket only sets its backlog (Linux, BSD);
 * GSocket has no call for that once it listens. */
static G_GNUC_UNUSED void
gh_test_deepen_listen_backlog(GSList *sockets)
{
  g_assert_nonnull(sockets);
  for (GSList *l = sockets; l; l = l->next)
    if (listen(g_socket_get_fd(l->data), GH_TEST_LISTEN_BACKLOG) != 0)
      g_error("listen(%d) on a test server socket: %s", GH_TEST_LISTEN_BACKLOG,
              g_strerror(errno));
  g_slist_free(sockets);
}

/* Adds a 127.0.0.1 socket on a port of the kernel's choosing to listener and
 * returns the port (nostrc-vzls). Never the wildcard: on macOS the port
 * picked for a dual-stack [::] socket bound with SO_REUSEADDR (what
 * g_socket_listener_add_any_inet_port() makes) is not checked against IPv4
 * sockets bound to one address, so it can be one another process already
 * listens on at 127.0.0.1 (bd's `dolt sql-server -H 127.0.0.1 -P <port>`, one
 * per beads workspace, did). Every IPv4 dial to 127.0.0.1 on that port then
 * reaches that listener, the more specific one, and never the test's. The
 * port of a 127.0.0.1 bind is checked against every 127.0.0.1 socket, and no
 * listener is more specific for a 127.0.0.1 dial. */
static G_GNUC_UNUSED guint16
gh_test_listen_loopback(GSocketListener *listener)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
  g_autoptr(GSocketAddress) any_port = g_inet_socket_address_new(loopback, 0);
  g_autoptr(GSocketAddress) bound = NULL;
  g_socket_listener_set_backlog(listener, GH_TEST_LISTEN_BACKLOG);
  g_assert_true(g_socket_listener_add_address(listener, any_port, G_SOCKET_TYPE_STREAM,
                                              G_SOCKET_PROTOCOL_TCP, NULL, &bound, &error));
  g_assert_no_error(error);
  guint16 port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
  g_assert_cmpuint(port, >, 0);
  return port;
}

/* Takes a connection over (the handler's reference is dropped after). */
typedef void (*GhTestHeldPortServe)(GSocketConnection *connection, gpointer data);

typedef struct {
  GSocketService *service;
  guint16 port;
  GhTestHeldPortServe serve; /* NULL while down */
  gpointer serve_data;
} GhTestHeldPort;

static G_GNUC_UNUSED gboolean
gh_test_held_port_incoming(GSocketService *service, GSocketConnection *connection,
                           GObject *source, gpointer data)
{
  (void)service;
  (void)source;
  GhTestHeldPort *held = data;
  if (held->serve)
    held->serve(connection, held->serve_data);
  else
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
  return TRUE;
}

/* held must stay where it is until gh_test_held_port_clear(). */
static G_GNUC_UNUSED void
gh_test_held_port_init(GhTestHeldPort *held)
{
  *held = (GhTestHeldPort){ 0 };
  held->service = g_socket_service_new();
  held->port = gh_test_listen_loopback(G_SOCKET_LISTENER(held->service));
  g_signal_connect(held->service, "incoming", G_CALLBACK(gh_test_held_port_incoming), held);
  g_socket_service_start(held->service);
}

/* From now on every connection goes to serve; dials queued meanwhile too. */
static G_GNUC_UNUSED void
gh_test_held_port_serve(GhTestHeldPort *held, GhTestHeldPortServe serve, gpointer data)
{
  g_assert_nonnull(held->service);
  held->serve = serve;
  held->serve_data = data;
}

static G_GNUC_UNUSED void
gh_test_held_port_clear(GhTestHeldPort *held)
{
  if (!held->service)
    return;
  g_signal_handlers_disconnect_by_data(held->service, held);
  g_socket_service_stop(held->service);
  /* Stopping cancels the pending accept, which completes on a later turn;
   * closing the sockets before that leaves its source polling a closed fd. */
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_socket_listener_close(G_SOCKET_LISTENER(held->service));
  g_clear_object(&held->service);
  held->serve = NULL;
}

#endif
