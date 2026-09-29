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
 * Header-only; the path is relative, for every test directory. */
#ifndef GH_TEST_PORT_H
#define GH_TEST_PORT_H

#include <gio/gio.h>

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
  g_autoptr(GError) error = NULL;
  *held = (GhTestHeldPort){ 0 };
  held->service = g_socket_service_new();
  g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
  g_autoptr(GSocketAddress) any_port = g_inet_socket_address_new(loopback, 0);
  g_autoptr(GSocketAddress) bound = NULL;
  g_assert_true(g_socket_listener_add_address(G_SOCKET_LISTENER(held->service), any_port,
                                              G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP,
                                              NULL, &bound, &error));
  g_assert_no_error(error);
  held->port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
  g_assert_cmpuint(held->port, >, 0);
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
