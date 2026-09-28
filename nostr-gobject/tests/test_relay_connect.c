/*
 * test_relay_connect — GNostrRelay connect/teardown against real sockets.
 *
 *  - nostrc-oz77: connecting to a loopback port nobody listens on must fail
 *    (quickly), not report success and leave publish waiting for an OK.
 *  - nostrc-jc2o: disconnect + unref must not close fd 0 behind our back,
 *    and a fresh connect afterwards must work.
 *  - nostrc-qp24.4.5: every core-driven state transition reaches
 *    "state-changed" on the main context, in order, including a lone
 *    CONNECTED -> DISCONNECTED when the relay goes away.
 *
 * SPDX-License-Identifier: MIT
 */
#include <nostr-gobject-1.0/nostr_relay.h>
#include <nostr-relay.h>
#include "nostr/testing/mock_relay_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

/* Generous for a loaded CI box; a refused loopback connect takes
 * milliseconds. Before the fix this waited forever. */
#define CONNECT_BOUND_US (10 * G_USEC_PER_SEC)

typedef struct {
  GMainLoop *loop;
  gboolean done, ok;
  GError *error;
} Wait;

static void
on_connected(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Wait *w = user_data;
  w->ok = gnostr_relay_connect_finish(GNOSTR_RELAY(source), res, &w->error);
  w->done = TRUE;
  g_main_loop_quit(w->loop);
}

static gboolean
on_timeout(gpointer user_data)
{
  g_main_loop_quit(((Wait *)user_data)->loop);
  return G_SOURCE_REMOVE;
}

/* Connect and wait at most CONNECT_BOUND_US. */
static gboolean
connect_bounded(GNostrRelay *relay, GError **error, gint64 *elapsed_us)
{
  Wait w = { .loop = g_main_loop_new(NULL, FALSE) };
  gint64 start = g_get_monotonic_time();
  guint t = g_timeout_add(CONNECT_BOUND_US / 1000, on_timeout, &w);
  gnostr_relay_connect_async(relay, NULL, on_connected, &w);
  g_main_loop_run(w.loop);
  if (w.done)
    g_source_remove(t);
  *elapsed_us = g_get_monotonic_time() - start;
  g_main_loop_unref(w.loop);
  g_assert_true(w.done);  /* the connect finished within the bound */
  if (w.error)
    g_propagate_error(error, w.error);
  return w.ok;
}

/* A loopback port with nothing listening: bind an ephemeral port, then
 * close it. */
static guint16
closed_port(void)
{
  int s = socket(AF_INET, SOCK_STREAM, 0);
  g_assert_cmpint(s, >=, 0);
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
  g_assert_cmpint(bind(s, (struct sockaddr *)&a, sizeof a), ==, 0);
  socklen_t len = sizeof a;
  g_assert_cmpint(getsockname(s, (struct sockaddr *)&a, &len), ==, 0);
  close(s);
  return ntohs(a.sin_port);
}

static void
spin(guint ms)
{
  gint64 until = g_get_monotonic_time() + (gint64)ms * 1000;
  while (g_get_monotonic_time() < until) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(10 * 1000);
  }
}

static void
test_refused_port_fails(void)
{
  g_autofree gchar *url = g_strdup_printf("ws://127.0.0.1:%u", closed_port());
  g_autoptr(GNostrRelay) relay = gnostr_relay_new(url);
  g_autoptr(GError) error = NULL;
  gint64 elapsed = 0;

  g_assert_false(connect_bounded(relay, &error, &elapsed));
  g_assert_error(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED);
  /* State changes reach the object on the main loop; the relay must stop
   * claiming to be connected (its reconnect loop then backs off). */
  gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (gnostr_relay_get_connected(relay) && g_get_monotonic_time() < until) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(10 * 1000);
  }
  g_assert_false(gnostr_relay_get_connected(relay));
  g_test_message("refused connect failed after %" G_GINT64_FORMAT " ms", elapsed / 1000);
}

typedef struct {
  GArray *pairs;          /* old, new, old, new, ... */
  GThread *main_thread;
  guint off_thread;
  guint connected_notifies;
  gboolean timed_out;
} StateLog;

static void
on_state_changed(GNostrRelay *relay G_GNUC_UNUSED, GNostrRelayState old_state,
                 GNostrRelayState new_state, gpointer user_data)
{
  StateLog *log = user_data;
  if (g_thread_self() != log->main_thread)
    log->off_thread++;
  gint pair[2] = { old_state, new_state };
  g_array_append_vals(log->pairs, pair, 2);
}

static void
on_connected_notify(GObject *object G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED,
                    gpointer user_data)
{
  ((StateLog *)user_data)->connected_notifies++;
}

static gboolean
state_log_expire(gpointer user_data)
{
  ((StateLog *)user_data)->timed_out = TRUE;
  return G_SOURCE_REMOVE;
}

/* Index of the first emission at or after @from whose new state is @a or
 * @b, or -1. */
static gint
find_emission(StateLog *log, guint from, GNostrRelayState a, GNostrRelayState b)
{
  for (guint i = from; i < log->pairs->len / 2; i++) {
    gint new_state = g_array_index(log->pairs, gint, 2 * i + 1);
    if (new_state == (gint)a || new_state == (gint)b)
      return (gint)i;
  }
  return -1;
}

/* Blocks on the main context until an emission appears; the timeout is a
 * failure bound only, progress is driven by the relay's signals. */
static gint
wait_for_emission(StateLog *log, guint from, GNostrRelayState a, GNostrRelayState b)
{
  log->timed_out = FALSE;
  guint bound = g_timeout_add_seconds(10, state_log_expire, log);
  gint index;
  while ((index = find_emission(log, from, a, b)) < 0 && !log->timed_out)
    g_main_context_iteration(NULL, TRUE);
  if (!log->timed_out)
    g_source_remove(bound);
  return index;
}

static void
test_state_changed_every_transition(void)
{
  NostrMockRelayServerConfig cfg = nostr_mock_server_config_default();
  NostrMockRelayServer *server = nostr_mock_server_new(&cfg);
  g_assert_nonnull(server);
  g_assert_cmpint(nostr_mock_server_start(server), ==, 0);

  /* A private relay (not the URL registry) with a single connection, so the
   * lost connection is one CONNECTED -> DISCONNECTED transition that the
   * former implementation swallowed. */
  GNostrRelay *relay = g_object_new(GNOSTR_TYPE_RELAY, "url",
                                    nostr_mock_server_get_url(server), NULL);
  nostr_relay_set_auto_reconnect(gnostr_relay_get_core_relay(relay), false);
  StateLog log = { .pairs = g_array_new(FALSE, FALSE, sizeof(gint)),
                   .main_thread = g_thread_self() };
  g_signal_connect(relay, "state-changed", G_CALLBACK(on_state_changed), &log);
  g_signal_connect(relay, "notify::connected", G_CALLBACK(on_connected_notify), &log);

  g_autoptr(GError) error = NULL;
  gint64 elapsed = 0;
  g_assert_true(connect_bounded(relay, &error, &elapsed));
  g_assert_no_error(error);
  gint up = wait_for_emission(&log, 0, GNOSTR_RELAY_STATE_CONNECTED,
                              GNOSTR_RELAY_STATE_CONNECTED);
  g_assert_cmpint(up, >=, 0);

  nostr_mock_server_stop(server); /* drops the client's socket */
  gint down = wait_for_emission(&log, (guint)up + 1, GNOSTR_RELAY_STATE_DISCONNECTED,
                                GNOSTR_RELAY_STATE_ERROR);
  g_assert_cmpint(down, >, up);
  g_assert_false(gnostr_relay_get_connected(relay));

  /* One chain: starts from DISCONNECTED, each old state is the previous
   * emission's new state, no no-op emissions, all on this thread. */
  guint n = log.pairs->len / 2;
  g_assert_cmpuint(n, >=, 2);
  g_assert_cmpint(g_array_index(log.pairs, gint, 0), ==, GNOSTR_RELAY_STATE_DISCONNECTED);
  for (guint i = 0; i < n; i++) {
    gint old_state = g_array_index(log.pairs, gint, 2 * i);
    gint new_state = g_array_index(log.pairs, gint, 2 * i + 1);
    g_assert_cmpint(old_state, !=, new_state);
    if (i > 0)
      g_assert_cmpint(old_state, ==, g_array_index(log.pairs, gint, 2 * i - 1));
  }
  g_assert_cmpuint(log.off_thread, ==, 0);
  g_assert_cmpuint(log.connected_notifies, >=, 2); /* up, then down */

  g_signal_handlers_disconnect_by_data(relay, &log);
  g_object_unref(relay);
  g_array_unref(log.pairs);
  nostr_mock_server_free(server);
}

static void
test_teardown_keeps_fd0(void)
{
  g_assert_cmpint(fcntl(0, F_GETFD), !=, -1);  /* the harness gives us a stdin */

  NostrMockRelayServerConfig cfg = nostr_mock_server_config_default();
  NostrMockRelayServer *server = nostr_mock_server_new(&cfg);
  g_assert_nonnull(server);
  g_assert_cmpint(nostr_mock_server_start(server), ==, 0);
  const char *url = nostr_mock_server_get_url(server);

  for (int round = 0; round < 3; round++) {
    GNostrRelay *relay = gnostr_relay_new(url);
    g_autoptr(GError) error = NULL;
    gint64 elapsed = 0;
    g_assert_true(connect_bounded(relay, &error, &elapsed));
    g_assert_no_error(error);

    gnostr_relay_disconnect(relay);
    g_object_unref(relay);           /* last ref: finalize frees in a thread */
    spin(500);                        /* let the background teardown finish */
    g_assert_cmpint(fcntl(0, F_GETFD), !=, -1);
  }

  nostr_mock_server_stop(server);
  nostr_mock_server_free(server);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  /* ctest runs us with stdin on /dev/null or closed: make sure fd 0 is ours
   * so "fd 0 was closed" is observable. */
  if (fcntl(0, F_GETFD) == -1) {
    int fd = open("/dev/null", O_RDONLY);
    g_assert_cmpint(fd, ==, 0);
  }
  g_test_add_func("/relay/connect/refused-port-fails", test_refused_port_fails);
  g_test_add_func("/relay/connect/teardown-keeps-fd0", test_teardown_keeps_fd0);
  g_test_add_func("/relay/state/every-transition-emitted",
                  test_state_changed_every_transition);
  return g_test_run();
}
