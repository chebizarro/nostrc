/* Ordered GIO teardown and fatal-warning negative controls.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "nostrc-test-bus.h"

#define EBADF_MESSAGE "poll(2) failed due to: Bad file descriptor."

static void
assert_child_aborts(void)
{
  g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_failed();
  g_test_trap_assert_stderr("*Bad file descriptor*");
}

static void
test_warning_fatal(void)
{
  if (g_test_subprocess()) {
    g_log("GLib", G_LOG_LEVEL_WARNING, "%s", EBADF_MESSAGE);
    return;
  }
  assert_child_aborts();
}

static void
test_warning_after_up(void)
{
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NOT_SESSION);
  nostrc_test_bus_up(bus);
  test_warning_fatal();
  nostrc_test_bus_down(bus);
}

static void
locally_closed(GDBusConnection *connection, gboolean remote, GError *error, gpointer data)
{
  g_assert_true(g_dbus_connection_is_closed(connection));
  g_assert_false(remote); /* Daemon-first teardown fails this assertion. */
  g_assert_no_error(error);
  (*(guint *)data)++;
}

static void
test_local_close(void)
{
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NOT_SESSION);
  nostrc_test_bus_up(bus);
  guint closed = 0;
  for (guint i = 0; i < 2; i++) {
    GDBusConnection *connection = nostrc_test_bus_connect(bus);
    g_signal_connect(connection, "closed", G_CALLBACK(locally_closed), &closed);
  }
  nostrc_test_bus_down(bus);
  g_assert_cmpuint(closed, ==, 2);
}

static void
test_thread_default(void)
{
  g_autoptr(GMainContext) context = g_main_context_new();
  g_main_context_push_thread_default(context);
  test_local_close();
  g_main_context_pop_thread_default(context);
}

typedef struct {
  NostrcTestBus *bus;
  GMainContext *context;
  GMainLoop *loop;
  GMutex lock;
  GCond ready;
  gboolean started;
  gboolean running;
  guint closed;
} Worker;

static gboolean
worker_ready(gpointer data)
{
  Worker *worker = data;
  g_mutex_lock(&worker->lock);
  worker->started = TRUE;
  g_cond_signal(&worker->ready);
  g_mutex_unlock(&worker->lock);
  return G_SOURCE_REMOVE;
}

static gpointer
worker_run(gpointer data)
{
  Worker *worker = data;
  g_main_context_push_thread_default(worker->context);
  GDBusConnection *connection = nostrc_test_bus_connect(worker->bus);
  g_signal_connect(connection, "closed", G_CALLBACK(locally_closed), &worker->closed);
  if (worker->running) {
    GSource *source = g_idle_source_new();
    g_source_set_callback(source, worker_ready, worker, NULL);
    g_source_attach(source, worker->context);
    g_source_unref(source);
    g_main_loop_run(worker->loop);
  } else {
    worker_ready(worker);
  }
  g_main_context_pop_thread_default(worker->context);
  return NULL;
}

static void
test_worker(gconstpointer data)
{
  Worker worker = { .running = GPOINTER_TO_INT(data) };
  g_mutex_init(&worker.lock);
  g_cond_init(&worker.ready);
  worker.context = g_main_context_new();
  worker.loop = g_main_loop_new(worker.context, FALSE);
  worker.bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NOT_SESSION);
  nostrc_test_bus_up(worker.bus);
  GThread *thread = g_thread_new("bus-owner", worker_run, &worker);
  g_mutex_lock(&worker.lock);
  while (!worker.started)
    g_cond_wait(&worker.ready, &worker.lock);
  g_mutex_unlock(&worker.lock);
  if (!worker.running)
    g_thread_join(thread);
  nostrc_test_bus_down(worker.bus);
  if (worker.running) {
    g_main_loop_quit(worker.loop);
    g_thread_join(thread);
  }
  g_assert_cmpuint(worker.closed, ==, 1);
  g_main_loop_unref(worker.loop);
  g_main_context_unref(worker.context);
  g_cond_clear(&worker.ready);
  g_mutex_clear(&worker.lock);
}

static void
test_session_restart(void)
{
  for (guint i = 0; i < 2; i++) {
    NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
    nostrc_test_bus_up(bus);
    nostrc_test_bus_down(bus);
  }
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  if (!nostrc_test_bus_available()) {
    g_printerr("nostrc-test-bus-selftest needs dbus-daemon\n");
    return 1;
  }
  nostrc_test_bus_add_func("/nostrc-test-bus/ebadf/first-case-fatal", test_warning_fatal);
  nostrc_test_bus_add_func("/nostrc-test-bus/ebadf/second-case-fatal", test_warning_fatal);
  g_test_add_func("/nostrc-test-bus/ebadf/after-up-fatal", test_warning_after_up);
  g_test_add_func("/nostrc-test-bus/close/before-daemon", test_local_close);
  g_test_add_func("/nostrc-test-bus/close/thread-default", test_thread_default);
  g_test_add_data_func("/nostrc-test-bus/close/running-worker", GINT_TO_POINTER(TRUE), test_worker);
  g_test_add_data_func("/nostrc-test-bus/close/stopped-worker", GINT_TO_POINTER(FALSE), test_worker);
  g_test_add_func("/nostrc-test-bus/close/session-restart", test_session_restart);
  return g_test_run();
}
