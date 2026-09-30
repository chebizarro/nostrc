/*
 * test_relay_teardown_race — GNostrRelay / GNostrPool teardown while another
 * thread is still using callback data (nostrc-flp7).
 *
 * The W20 crash: libnostr's message_loop was in relay_set_state ->
 * on_core_state_changed -> g_weak_ref_get when the main thread finalized the
 * GNostrRelay. Finalize freed the callback data before it detached the core
 * callbacks, and libnostr calls a callback after releasing the lock it copied
 * it under, so the worker read freed memory.
 *
 * The relay cases park a real libnostr worker at the first statement of a
 * GNostrRelay core callback (test seam, nostr_relay_test_hooks.h), finalize
 * the relay on this thread, then let the worker go on. Before the fix the
 * worker's g_weak_ref_get() then reads the freed callback data. That read is
 * inside GLib, which ASan does not instrument, so under ASan freed memory is
 * filled (see __asan_default_options below) and the read faults the way the
 * W20 crash did: SEGV in g_weak_ref_get <- on_core_state_changed <-
 * relay_set_state <- message_loop. Without ASan the test still fails,
 * because it checks that the callback data was alive while the worker was
 * parked. The pool cases do the same with the pool's cache-query and
 * auth-handler user data.
 *
 * Every wait has a bound; reaching it fails the test.
 *
 * SPDX-License-Identifier: MIT
 */
#include <nostr-gobject-1.0/nostr_pool.h>
#include <nostr-gobject-1.0/nostr_relay.h>
#include <nostr-event.h>
#include <nostr-filter.h>
#include <nostr-keys.h>
#include <nostr-relay.h>
#include "nostr/testing/mock_relay_server.h"
#include "nostr_relay_test_hooks.h"

#include <gio/gio.h>
#include <glib.h>
#include <stdlib.h>

#define BOUND_US (10 * G_USEC_PER_SEC)
#define PAYLOAD_MAGIC 0x666c7037 /* "flp7" */

#if defined(__SANITIZE_ADDRESS__)
#define TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_ASAN 1
#endif
#endif

#ifdef TEST_ASAN
/* Fill freed memory with 0xbe, so a pointer read from freed callback data is
 * wild and faults, rather than a stale but intact one that happens to work.
 * Bit 0 must be clear: GLib bit-locks the GWeakRef's pointer word and would
 * wait forever on a set bit. ASAN_OPTIONS from the environment still apply;
 * this only supplies defaults. */
const char *__asan_default_options(void);
const char *
__asan_default_options(void)
{
  return "max_free_fill_size=4096:free_fill_byte=190";
}
#endif

/* --- Parking a thread at a test point ------------------------------------ */

typedef struct {
  GMutex mu;
  GCond cv;
  GNostrRelayTestPoint point;
  gboolean armed;      /* park the next thread that reaches `point` */
  gboolean parked;
  gboolean release;
} Park;

static void
park_init(Park *p, GNostrRelayTestPoint point)
{
  g_mutex_init(&p->mu);
  g_cond_init(&p->cv);
  p->point = point;
  p->armed = TRUE;
  p->parked = p->release = FALSE;
}

static void
park_clear(Park *p)
{
  g_mutex_clear(&p->mu);
  g_cond_clear(&p->cv);
}

static void
park_hook(GNostrRelayTestPoint point, gpointer data)
{
  Park *p = data;
  g_mutex_lock(&p->mu);
  if (p->armed && point == p->point) {
    p->armed = FALSE;
    p->parked = TRUE;
    g_cond_broadcast(&p->cv);
    gint64 deadline = g_get_monotonic_time() + 2 * BOUND_US;
    while (!p->release && g_cond_wait_until(&p->cv, &p->mu, deadline))
      ;
  }
  g_mutex_unlock(&p->mu);
}

static gboolean
park_wait_parked(Park *p)
{
  gint64 deadline = g_get_monotonic_time() + BOUND_US;
  g_mutex_lock(&p->mu);
  while (!p->parked && g_cond_wait_until(&p->cv, &p->mu, deadline))
    ;
  gboolean parked = p->parked;
  g_mutex_unlock(&p->mu);
  return parked;
}

static void
park_release(Park *p)
{
  g_mutex_lock(&p->mu);
  p->release = TRUE;
  g_cond_broadcast(&p->cv);
  g_mutex_unlock(&p->mu);
}

/* --- Main-context helpers ------------------------------------------------ */

static gboolean
tick(gpointer user_data G_GNUC_UNUSED)
{
  return G_SOURCE_CONTINUE;
}

typedef gboolean (*Condition)(gpointer data);

/* Runs the default main context until @cond holds; FALSE at the bound. A
 * 10 ms tick wakes the loop for conditions other threads make true. */
static gboolean
spin_until(Condition cond, gpointer data)
{
  guint ticker = g_timeout_add(10, tick, NULL);
  gint64 deadline = g_get_monotonic_time() + BOUND_US;
  gboolean ok;
  while (!(ok = cond(data)) && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, TRUE);
  g_source_remove(ticker);
  return ok;
}

static gboolean
only_our_ref(gpointer relay)
{
  return g_atomic_int_get((gint *)&G_OBJECT(relay)->ref_count) == 1;
}

/* The core relay's count is written under its mutex; a racy read is fine
 * for a condition that, once true, stays true. */
static gboolean
only_our_core_ref(gpointer core)
{
  return __atomic_load_n(&((NostrRelay *)core)->refcount, __ATOMIC_SEQ_CST) == 1;
}

/* --- A connected private relay ------------------------------------------- */

typedef struct {
  gboolean done, ok, connected_emitted;
} ConnectWait;

static void
on_connected(GObject *source, GAsyncResult *res, gpointer user_data)
{
  ConnectWait *w = user_data;
  w->ok = gnostr_relay_connect_finish(GNOSTR_RELAY(source), res, NULL);
  w->done = TRUE;
}

static void
on_state_changed(GNostrRelay *relay G_GNUC_UNUSED, GNostrRelayState old_state G_GNUC_UNUSED,
                 GNostrRelayState new_state, gpointer user_data)
{
  if (new_state == GNOSTR_RELAY_STATE_CONNECTED)
    ((ConnectWait *)user_data)->connected_emitted = TRUE;
}

static gboolean
connect_done(gpointer data)
{
  ConnectWait *w = data;
  return w->done && (!w->ok || w->connected_emitted);
}

/* A connected relay outside the URL registry whose only reference is ours,
 * with nothing queued on the main context: its callback data is held by the
 * core callback registrations alone. */
static GNostrRelay *
connected_private_relay(NostrMockRelayServer *server)
{
  GNostrRelay *relay = g_object_new(GNOSTR_TYPE_RELAY, "url",
                                    nostr_mock_server_get_url(server), NULL);
  ConnectWait w = { FALSE, FALSE, FALSE };
  gulong handler = g_signal_connect(relay, "state-changed",
                                    G_CALLBACK(on_state_changed), &w);
  gnostr_relay_connect_async(relay, NULL, on_connected, &w);
  g_assert_true(spin_until(connect_done, &w));
  g_assert_true(w.ok);
  g_signal_handler_disconnect(relay, handler);
  /* Announcing CONNECTED started a NIP-11 fetch, which holds a reference
   * until it is done. */
  g_assert_true(spin_until(only_our_ref, relay));
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_true(gnostr_relay_get_connected(relay));
  return relay;
}

/* Parks the libnostr worker at @point once @trigger has made it invoke that
 * callback, finalizes @relay here, and lets the worker go on. Returns whether
 * the callback data was still alive while the worker was parked. */
static gboolean
finalize_while_parked(GNostrRelay *relay, GNostrRelayTestPoint point,
                      void (*trigger)(gpointer), gpointer trigger_data)
{
  gint live = gnostr_relay_test_live_callback_data();
  g_assert_cmpint(live, >=, 1);

  /* Hold the core relay, as a NostrSubscription still being freed does in
   * the app: finalize then only drops its own reference, and freeing ours
   * below joins the core relay's worker threads. That join is how we know
   * the parked worker has finished the callback. */
  NostrRelay *core = nostr_relay_ref(gnostr_relay_get_core_relay(relay));

  Park park;
  park_init(&park, point);
  gnostr_relay_test_set_hook(park_hook, &park);

  trigger(trigger_data);
  g_assert_true(park_wait_parked(&park));

  /* The last reference: finalize runs here, while the worker is parked at
   * the top of the callback, before it has looked at its data. It must not
   * wait for that worker either. */
  g_assert_true(only_our_ref(relay));
  g_object_unref(relay);
  gboolean alive_while_parked = gnostr_relay_test_live_callback_data() == live;

  park_release(&park);

  /* Finalize handed its core reference to a background thread; once that is
   * dropped, ours is the last, and freeing it joins the core relay's workers:
   * it returns only after the parked worker has left the callback. Before
   * the fix that worker reads the freed callback data in there. */
  g_assert_true(spin_until(only_our_core_ref, core));
  nostr_relay_free(core);
  gnostr_relay_test_set_hook(NULL, NULL);
  park_clear(&park);

  /* The call was the last holder of the callback data. */
  g_assert_cmpint(gnostr_relay_test_live_callback_data(), ==, live - 1);
  return alive_while_parked;
}

static void
stop_server(gpointer server)
{
  nostr_mock_server_stop(server); /* drops the client's socket */
}

/* The W20 crash path: message_loop sees the connection drop and reports it
 * through relay_set_state -> on_core_state_changed. */
static void
test_finalize_during_state_callback(void)
{
  NostrMockRelayServerConfig cfg = nostr_mock_server_config_default();
  NostrMockRelayServer *server = nostr_mock_server_new(&cfg);
  g_assert_nonnull(server);
  g_assert_cmpint(nostr_mock_server_start(server), ==, 0);

  GNostrRelay *relay = connected_private_relay(server);
  g_assert_true(finalize_while_parked(relay, GNOSTR_RELAY_TEST_POINT_CORE_STATE,
                                      stop_server, server));
  nostr_mock_server_free(server);
}

static void
publish_signed_note(gpointer relay)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 1);
  nostr_event_set_created_at(event, (int64_t)g_get_real_time() / G_USEC_PER_SEC);
  nostr_event_set_content(event, "nostrc-flp7");
  char *sk = nostr_key_generate_private();
  g_assert_cmpint(nostr_event_sign(event, sk), ==, 0);
  free(sk);
  g_autoptr(GError) error = NULL;
  g_assert_true(gnostr_relay_publish(relay, event, &error));
  g_assert_no_error(error);
  nostr_event_free(event);
}

/* The OK for a publish reaches on_core_ok_response on message_loop. */
static void
test_finalize_during_ok_callback(void)
{
  NostrMockRelayServerConfig cfg = nostr_mock_server_config_default();
  NostrMockRelayServer *server = nostr_mock_server_new(&cfg);
  g_assert_nonnull(server);
  g_assert_cmpint(nostr_mock_server_start(server), ==, 0);

  GNostrRelay *relay = connected_private_relay(server);
  g_assert_true(finalize_while_parked(relay, GNOSTR_RELAY_TEST_POINT_CORE_OK,
                                      publish_signed_note, relay));
  nostr_mock_server_stop(server);
  nostr_mock_server_free(server);
}

/* --- The URL registry ---------------------------------------------------- */

static gpointer
unref_thread(gpointer relay)
{
  g_object_unref(relay);
  return NULL;
}

/* A lookup racing the last unref must not return the relay being finalized
 * (the registry used to hold the bare pointer and g_object_ref() it at
 * refcount zero), and the dying relay must not drop its successor's entry. */
static void
test_registry_lookup_during_finalize(void)
{
  const gchar *url = "ws://127.0.0.1:9/nostrc-flp7-registry";
  GNostrRelay *dying = gnostr_relay_new(url);
  g_assert_true(only_our_ref(dying));

  Park park;
  park_init(&park, GNOSTR_RELAY_TEST_POINT_FINALIZE);
  gnostr_relay_test_set_hook(park_hook, &park);
  GThread *thread = g_thread_new("flp7-unref", unref_thread, dying);
  g_assert_true(park_wait_parked(&park));

  GNostrRelay *successor = gnostr_relay_new(url);
  g_assert_true(successor != dying);  /* still allocated: finalize is parked */

  park_release(&park);
  g_thread_join(thread);
  gnostr_relay_test_set_hook(NULL, NULL);
  park_clear(&park);

  GNostrRelay *again = gnostr_relay_new(url);
  g_assert_true(again == successor);
  g_object_unref(again);
  g_object_unref(successor);
}

/* --- GNostrPool hook user data ------------------------------------------- */

typedef struct {
  GMutex mu;
  GCond cv;
  gboolean entered, release;
  gint destroyed;
} Probe;

typedef struct {
  gint magic;
  Probe *probe;
} Payload;

static Payload *
payload_new(Probe *probe)
{
  Payload *p = g_new0(Payload, 1);
  p->magic = PAYLOAD_MAGIC;
  p->probe = probe;
  return p;
}

static void
payload_destroy(gpointer data)
{
  Payload *p = data;
  g_assert_cmpint(p->magic, ==, PAYLOAD_MAGIC);
  g_mutex_lock(&p->probe->mu);
  p->probe->destroyed++;
  g_mutex_unlock(&p->probe->mu);
  p->magic = 0;
  g_free(p);
}

static gint
probe_destroyed(Probe *probe)
{
  g_mutex_lock(&probe->mu);
  gint n = probe->destroyed;
  g_mutex_unlock(&probe->mu);
  return n;
}

/* Cache query (a pool worker thread): parks until released, then uses its
 * user data and reports a cache hit so the query needs no relay. */
static GPtrArray *
parking_cache_query(NostrFilters *filters G_GNUC_UNUSED, gpointer user_data)
{
  Payload *p = user_data;
  Probe *probe = p->probe;
  g_mutex_lock(&probe->mu);
  probe->entered = TRUE;
  g_cond_broadcast(&probe->cv);
  gint64 deadline = g_get_monotonic_time() + 2 * BOUND_US;
  while (!probe->release && g_cond_wait_until(&probe->cv, &probe->mu, deadline))
    ;
  g_mutex_unlock(&probe->mu);

  g_assert_cmpint(p->magic, ==, PAYLOAD_MAGIC);
  GPtrArray *hits = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(hits, g_strdup("{}"));
  return hits;
}

typedef struct { gboolean done; guint results; } QueryWait;

static void
on_query_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  QueryWait *w = user_data;
  GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, NULL);
  w->results = results ? results->len : 0;
  if (results)
    g_ptr_array_unref(results);
  w->done = TRUE;
}

static gboolean
query_done(gpointer data)
{
  return ((QueryWait *)data)->done;
}

static gboolean
probe_is_destroyed(gpointer probe)
{
  return probe_destroyed(probe) == 1;
}

/* Clearing the cache query while a query's worker is inside it: the old
 * user data used to be destroyed at once. */
static void
test_pool_cache_query_cleared_mid_call(void)
{
  Probe probe = { 0 };
  g_mutex_init(&probe.mu);
  g_cond_init(&probe.cv);

  GNostrPool *pool = gnostr_pool_new();
  gnostr_pool_set_cache_query(pool, parking_cache_query, payload_new(&probe),
                              payload_destroy);

  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  int kinds[] = { 1 };
  nostr_filter_set_kinds(filter, kinds, 1);
  g_assert_true(nostr_filters_add(filters, filter));
  nostr_filter_free(filter);

  QueryWait w = { FALSE, 0 };
  gnostr_pool_query_async(pool, filters, NULL, on_query_done, &w);

  gint64 deadline = g_get_monotonic_time() + BOUND_US;
  g_mutex_lock(&probe.mu);
  while (!probe.entered && g_cond_wait_until(&probe.cv, &probe.mu, deadline))
    ;
  g_assert_true(probe.entered);
  g_mutex_unlock(&probe.mu);

  gnostr_pool_set_cache_query(pool, NULL, NULL, NULL);
  gboolean alive_while_parked = probe_destroyed(&probe) == 0;

  g_mutex_lock(&probe.mu);
  probe.release = TRUE;
  g_cond_broadcast(&probe.cv);
  g_mutex_unlock(&probe.mu);

  g_assert_true(spin_until(query_done, &w));
  g_assert_cmpuint(w.results, ==, 1);
  g_assert_true(spin_until(probe_is_destroyed, &probe));
  g_assert_true(alive_while_parked);

  g_object_unref(pool);
  g_assert_cmpint(probe_destroyed(&probe), ==, 1);
  g_mutex_clear(&probe.mu);
  g_cond_clear(&probe.cv);
}

static void
noop_sign(NostrEvent *event G_GNUC_UNUSED, GError **error G_GNUC_UNUSED,
          gpointer user_data)
{
  g_assert_cmpint(((Payload *)user_data)->magic, ==, PAYLOAD_MAGIC);
}

/* Relays keep the pool's auth handler after the pool is gone; its user data
 * must live until they let go of it (it used to be destroyed with the pool,
 * leaving every relay with a dangling pointer). */
static void
test_pool_auth_handler_outlives_pool(void)
{
  Probe probe = { 0 };
  g_mutex_init(&probe.mu);
  g_cond_init(&probe.cv);

  GNostrPool *pool = gnostr_pool_new();
  GNostrRelay *relay = gnostr_pool_add_relay(pool, "ws://127.0.0.1:9/nostrc-flp7-auth");
  g_assert_nonnull(relay);
  g_object_ref(relay);
  gnostr_pool_set_auth_handler(pool, noop_sign, payload_new(&probe), payload_destroy);

  g_object_unref(pool);
  g_assert_cmpint(probe_destroyed(&probe), ==, 0);

  g_object_unref(relay);
  g_assert_cmpint(probe_destroyed(&probe), ==, 1);
  g_mutex_clear(&probe.mu);
  g_cond_clear(&probe.cv);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/relay/teardown/finalize-during-state-callback",
                  test_finalize_during_state_callback);
  g_test_add_func("/relay/teardown/finalize-during-ok-callback",
                  test_finalize_during_ok_callback);
  g_test_add_func("/relay/registry/lookup-during-finalize",
                  test_registry_lookup_during_finalize);
  g_test_add_func("/pool/cache-query/cleared-mid-call",
                  test_pool_cache_query_cleared_mid_call);
  g_test_add_func("/pool/auth-handler/outlives-pool",
                  test_pool_auth_handler_outlives_pool);
  return g_test_run();
}
