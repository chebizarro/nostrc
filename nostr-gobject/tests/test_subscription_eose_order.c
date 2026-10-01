/*
 * test_subscription_eose_order — GNostrSubscription emits every stored EVENT
 * before EOSE (nostrc-qp24.10.6).
 *
 * A local relay answers each REQ with N signed stored events immediately
 * followed by EOSE, all in one burst on the socket. libnostr enqueues them in
 * wire order but on separate channels; the wrapper used to pick among ready
 * channels at random and dispatch EOSE from a higher-priority idle than the
 * event queue, so "eose" could reach the main loop before events received
 * ahead of it. Every round must see all N "event" emissions before "eose".
 * Many rounds, each a fresh REQ, since the old failure was a race.
 *
 * The relay is a minimal libsoup WebSocket server on its own thread and
 * main context, like a remote relay: it never waits for the client's main
 * loop. (The jansson-based nostr_testing mock relay cannot serve REQs in a
 * binary that also loads json-glib: its json_object_foreach binds to
 * json-glib's json_object_iter_next.) Waits iterate the default main
 * context; their deadlines are failure bounds only.
 *
 * nostrc-dha5: a backfill larger than the 200-event bound. In lossless mode
 * (the default) all 1,000 stored events and then EOSE reach the handlers, in
 * order, although the main loop is not run until all of them are queued; in
 * bounded mode (Gnostr) the newest 200 do, still in order, then EOSE.
 *
 * nostrc-5rfp: lossless mode's hard ceiling. A backfill exactly at the
 * event ceiling still arrives complete; a flood past the event ceiling, or
 * past the byte ceiling, ends the subscription: the events queued before it
 * are emitted in order, then "closed" with the overflow reason (no EOSE),
 * and the relay receives a CLOSE.
 *
 * SPDX-License-Identifier: MIT
 */
#include <nostr-gobject-1.0/nostr_relay.h>
#include <nostr-gobject-1.0/nostr_subscription.h>
#include <nostr-event.h>
#include <nostr-filter.h>
#include <nostr-keys.h>
#include <nostr-relay.h>
#include <json.h>
#include "nostr_subscription_test_hooks.h"

#include <gio/gio.h>
#include <glib.h>
#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>

#define N_STORED 24
#define N_BACKFILL 1000
#define BOUNDED_CAPACITY 200 /* EVENT_QUEUE_CAPACITY */
#define N_ROUNDS 200
#define WAIT_BOUND_S 10

typedef struct {
  guint events;
  guint events_at_eose;
  gboolean eose;
  gboolean closed;
  gboolean timed_out;
} Round;

static void
on_event(GNostrSubscription *sub G_GNUC_UNUSED, const gchar *json G_GNUC_UNUSED,
         gpointer data)
{
  ((Round *)data)->events++;
}

static void
on_eose(GNostrSubscription *sub G_GNUC_UNUSED, gpointer data)
{
  Round *round = data;
  if (!round->eose)
    round->events_at_eose = round->events;
  round->eose = TRUE;
}

static void
on_closed(GNostrSubscription *sub G_GNUC_UNUSED, const gchar *reason G_GNUC_UNUSED,
          gpointer data)
{
  ((Round *)data)->closed = TRUE;
}

static gboolean
on_bound(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

typedef struct {
  GMainLoop *loop;
  gboolean ok;
  GError *error;
} ConnectWait;

static void
on_connected(GObject *source, GAsyncResult *res, gpointer data)
{
  ConnectWait *w = data;
  w->ok = gnostr_relay_connect_finish(GNOSTR_RELAY(source), res, &w->error);
  g_main_loop_quit(w->loop);
}

static NostrFilters *
kind1_filters(void)
{
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  const int kinds[] = { 1 };
  nostr_filter_set_kinds(filter, kinds, G_N_ELEMENTS(kinds));
  g_assert_true(nostr_filters_add(filters, filter));
  nostr_filter_free(filter); /* contents moved into the vector */
  return filters;
}

/* ---- tolerated warnings --------------------------------------------------- */

/* The flood's overflow warning, counted by a log handler rather than
 * expected with g_test_expect_message(): GLib reads that function's list in
 * every g_log() call, on any thread, without a lock, and the relay thread logs
 * (libsoup, per frame) while a libnostr thread consumes the expectation. That
 * read of a freed node crashed the relay thread in g_logv (nostrc-79mi). */
static gint overflow_warnings; /* atomic */
static gint overflow_expected; /* atomic: a flood case counts it */

static gboolean
is_overflow_warning(const gchar *domain, GLogLevelFlags level, const gchar *message)
{
  return g_strcmp0(domain, "gnostr-subscription") == 0 && (level & G_LOG_LEVEL_WARNING) &&
         g_pattern_match_simple("*over its limit*", message);
}

static void
on_subscription_warning(const gchar *domain, GLogLevelFlags level, const gchar *message,
                        gpointer data)
{
  if (is_overflow_warning(domain, level, message))
    g_atomic_int_inc(&overflow_warnings);
  else
    g_log_default_handler(domain, level, message, data);
}

/* The burst relay may accept a connection the relay client has already reset
 * (its NIP-11 fetch, cancelled at disconnect). libsoup then cannot read the
 * peer address (Linux: ENOTCONN, macOS: EINVAL) and warns from
 * soup-server-connection.c, which G_DEBUG=fatal-warnings made a SIGTRAP in the
 * Linux gate (nostrc-79mi). That is this harness's accept race, as in
 * gnome/groundhog/tests/nip29/nip29-relay.h, so exactly that warning is not
 * fatal. */
static gboolean
is_accept_race(const gchar *domain, const gchar *message)
{
  return g_strcmp0(domain, "libsoup") == 0 && message &&
         strstr(message, "could not get remote address") != NULL;
}

static GLogFunc previous_default_handler;

/* A forgiven warning goes out as an ordinary one, not "Bail out!". */
static void
log_forgiven_as_warning(const gchar *domain, GLogLevelFlags level, const gchar *message,
                        gpointer data)
{
  if (is_accept_race(domain, message))
    level &= ~G_LOG_FLAG_FATAL;
  previous_default_handler(domain, level, message, data);
}

/* GTest clears the fatal handler before each case: relay_start() arms it. */
static gboolean
fatal_unless_tolerated(const gchar *domain, GLogLevelFlags level, const gchar *message,
                       gpointer data G_GNUC_UNUSED)
{
  return !is_accept_race(domain, message) &&
         !(g_atomic_int_get(&overflow_expected) && is_overflow_warning(domain, level, message));
}

/* ---- burst relay ---------------------------------------------------------- */

typedef struct {
  GThread *thread;
  GMainContext *context;
  GMainLoop *loop;
  SoupServer *server;
  GPtrArray *connections; /* SoupWebsocketConnection, relay thread only */
  GPtrArray *stored;      /* signed event JSON, immutable once started */
  gchar *url;
  GMutex lock;
  GCond ready;
  guint reqs;             /* atomic */
  guint closes;           /* atomic: CLOSE frames received */
} BurstRelay;

/* The subscription id of a ["REQ","<id>",...] frame, or NULL. */
static gchar *
req_sub_id(const gchar *text)
{
  static const gchar prefix[] = "[\"REQ\",\"";
  if (!g_str_has_prefix(text, prefix))
    return NULL;
  const gchar *start = text + strlen(prefix);
  const gchar *end = strchr(start, '"');
  return end ? g_strndup(start, end - start) : NULL;
}

/* Every stored event, then EOSE, back to back with no main-loop turn in
 * between: libsoup writes them out as one burst. */
static void
on_relay_message(SoupWebsocketConnection *connection, SoupWebsocketDataType type,
                 GBytes *message, gpointer data)
{
  BurstRelay *relay = data;
  if (type != SOUP_WEBSOCKET_DATA_TEXT)
    return;
  gsize len = 0;
  const gchar *raw = g_bytes_get_data(message, &len);
  g_autofree gchar *text = g_strndup(raw, len);
  g_autofree gchar *sub_id = req_sub_id(text);
  if (g_str_has_prefix(text, "[\"CLOSE\""))
    g_atomic_int_inc(&relay->closes);
  if (!sub_id)
    return; /* CLOSE and anything else */
  g_atomic_int_inc(&relay->reqs);
  for (guint i = 0; i < relay->stored->len; i++) {
    g_autofree gchar *frame = g_strdup_printf("[\"EVENT\",\"%s\",%s]", sub_id,
                                              (const gchar *)g_ptr_array_index(relay->stored, i));
    soup_websocket_connection_send_text(connection, frame);
  }
  g_autofree gchar *eose = g_strdup_printf("[\"EOSE\",\"%s\"]", sub_id);
  soup_websocket_connection_send_text(connection, eose);
}

static void
on_relay_socket(SoupServer *server G_GNUC_UNUSED, SoupServerMessage *msg G_GNUC_UNUSED,
                const char *path G_GNUC_UNUSED, SoupWebsocketConnection *connection,
                gpointer data)
{
  BurstRelay *relay = data;
  g_ptr_array_add(relay->connections, g_object_ref(connection));
  g_signal_connect(connection, "message", G_CALLBACK(on_relay_message), relay);
}

static gpointer
relay_thread(gpointer data)
{
  BurstRelay *relay = data;
  g_main_context_push_thread_default(relay->context);
  relay->server = soup_server_new(NULL, NULL);
  soup_server_add_websocket_handler(relay->server, "/", NULL, NULL, on_relay_socket,
                                    relay, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true(soup_server_listen_local(relay->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY,
                                         &error));
  g_assert_no_error(error);
  GSList *uris = soup_server_get_uris(relay->server);
  g_assert_nonnull(uris);
  g_mutex_lock(&relay->lock);
  relay->url = g_strdup_printf("ws://127.0.0.1:%d/", g_uri_get_port(uris->data));
  g_cond_signal(&relay->ready);
  g_mutex_unlock(&relay->lock);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);

  g_main_loop_run(relay->loop);

  for (guint i = 0; i < relay->connections->len; i++) {
    SoupWebsocketConnection *c = g_ptr_array_index(relay->connections, i);
    g_signal_handlers_disconnect_by_data(c, relay);
  }
  g_ptr_array_set_size(relay->connections, 0);
  soup_server_disconnect(relay->server);
  g_clear_object(&relay->server);
  while (g_main_context_iteration(relay->context, FALSE))
    ;
  g_main_context_pop_thread_default(relay->context);
  return NULL;
}

static void
relay_start(BurstRelay *relay)
{
  g_test_log_set_fatal_handler(fatal_unless_tolerated, NULL);
  g_mutex_init(&relay->lock);
  g_cond_init(&relay->ready);
  relay->context = g_main_context_new();
  relay->loop = g_main_loop_new(relay->context, FALSE);
  relay->connections = g_ptr_array_new_with_free_func(g_object_unref);
  relay->thread = g_thread_new("burst-relay", relay_thread, relay);
  g_mutex_lock(&relay->lock);
  while (!relay->url)
    g_cond_wait(&relay->ready, &relay->lock);
  g_mutex_unlock(&relay->lock);
}

static gboolean
relay_quit(gpointer data)
{
  g_main_loop_quit(((BurstRelay *)data)->loop);
  return G_SOURCE_REMOVE;
}

static void
relay_stop(BurstRelay *relay)
{
  g_main_context_invoke(relay->context, relay_quit, relay);
  g_thread_join(relay->thread);
  g_ptr_array_unref(relay->connections);
  g_main_loop_unref(relay->loop);
  g_main_context_unref(relay->context);
  g_free(relay->url);
  g_mutex_clear(&relay->lock);
  g_cond_clear(&relay->ready);
}

/* @n signed kind-1 events, content "stored <i>" in order. */
static GPtrArray *
stored_events_n(guint n)
{
  GPtrArray *stored = g_ptr_array_new_with_free_func(g_free);
  char *sk = nostr_key_generate_private();
  g_assert_nonnull(sk);
  for (guint i = 0; i < n; i++) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);
    nostr_event_set_created_at(ev, 1700000000 + i);
    g_autofree gchar *content = g_strdup_printf("stored %u", i);
    nostr_event_set_content(ev, content);
    g_assert_cmpint(nostr_event_sign(ev, sk), ==, 0);
    char *json = nostr_event_serialize(ev);
    g_assert_nonnull(json);
    g_ptr_array_add(stored, g_strdup(json));
    free(json);
    nostr_event_free(ev);
  }
  free(sk);
  return stored;
}

static GPtrArray *
stored_events(void)
{
  return stored_events_n(N_STORED);
}

static void
test_stored_events_precede_eose(void)
{
  BurstRelay server = { .stored = stored_events() };
  relay_start(&server);

  GNostrRelay *relay = g_object_new(GNOSTR_TYPE_RELAY, "url", server.url, NULL);
  nostr_relay_set_auto_reconnect(gnostr_relay_get_core_relay(relay), false);
  ConnectWait cw = { .loop = g_main_loop_new(NULL, FALSE) };
  gnostr_relay_connect_async(relay, NULL, on_connected, &cw);
  g_main_loop_run(cw.loop);
  g_main_loop_unref(cw.loop);
  g_assert_no_error(cw.error);
  g_assert_true(cw.ok);

  guint early_eose = 0;
  for (guint r = 0; r < N_ROUNDS; r++) {
    Round round = { 0 };
    GNostrSubscription *sub = gnostr_subscription_new(relay, kind1_filters());
    g_assert_nonnull(sub);
    g_signal_connect(sub, "event", G_CALLBACK(on_event), &round);
    g_signal_connect(sub, "eose", G_CALLBACK(on_eose), &round);
    g_signal_connect(sub, "closed", G_CALLBACK(on_closed), &round);

    g_autoptr(GError) error = NULL;
    g_assert_true(gnostr_subscription_fire(sub, &error));
    g_assert_no_error(error);

    /* Until EOSE and every stored event have arrived (the latter only so a
     * failure below is about order, not loss). */
    guint bound = g_timeout_add_seconds(WAIT_BOUND_S, on_bound, &round.timed_out);
    while (!(round.eose && round.events >= N_STORED) && !round.closed && !round.timed_out)
      g_main_context_iteration(NULL, TRUE);
    if (!round.timed_out)
      g_source_remove(bound);
    g_assert_false(round.closed);
    g_assert_false(round.timed_out);
    g_assert_cmpuint(round.events, ==, N_STORED);
    g_assert_cmpint(gnostr_subscription_get_state(sub), ==,
                    GNOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED);
    if (round.events_at_eose != N_STORED) {
      early_eose++;
      g_test_message("round %u: eose after %u of %u stored events", r,
                     round.events_at_eose, N_STORED);
    }

    g_signal_handlers_disconnect_by_data(sub, &round);
    gnostr_subscription_close(sub);
    g_object_unref(sub);
  }
  g_test_message("%u/%u rounds emitted EOSE before all stored events", early_eose,
                 N_ROUNDS);
  g_assert_cmpuint(early_eose, ==, 0);
  g_assert_cmpuint(g_atomic_int_get(&server.reqs), ==, N_ROUNDS);

  gnostr_relay_disconnect(relay);
  g_object_unref(relay);
  relay_stop(&server);
  g_ptr_array_unref(server.stored);
}

/* ---- backfill larger than the bounded queue (nostrc-dha5) -------------- */

typedef struct {
  GArray *indices;   /* "stored <i>" of each emitted event, in order */
  gboolean eose;
  guint events_at_eose;
  gboolean closed;
  gchar *closed_reason;
  guint events_at_close;
} Backfill;

static void
on_backfill_event(GNostrSubscription *sub G_GNUC_UNUSED, const gchar *json, gpointer data)
{
  Backfill *b = data;
  const gchar *content = strstr(json, "\"stored ");
  g_assert_nonnull(content);
  guint index = (guint)g_ascii_strtoull(content + strlen("\"stored "), NULL, 10);
  g_array_append_val(b->indices, index);
}

static void
on_backfill_eose(GNostrSubscription *sub G_GNUC_UNUSED, gpointer data)
{
  Backfill *b = data;
  if (!b->eose)
    b->events_at_eose = b->indices->len;
  b->eose = TRUE;
}

static void
on_backfill_closed(GNostrSubscription *sub G_GNUC_UNUSED, const gchar *reason,
                   gpointer data)
{
  Backfill *b = data;
  if (!b->closed) {
    b->closed_reason = g_strdup(reason);
    b->events_at_close = b->indices->len;
  }
  b->closed = TRUE;
}

static GNostrRelay *
connect_relay(BurstRelay *server)
{
  GNostrRelay *relay = g_object_new(GNOSTR_TYPE_RELAY, "url", server->url, NULL);
  nostr_relay_set_auto_reconnect(gnostr_relay_get_core_relay(relay), false);
  ConnectWait cw = { .loop = g_main_loop_new(NULL, FALSE) };
  gnostr_relay_connect_async(relay, NULL, on_connected, &cw);
  g_main_loop_run(cw.loop);
  g_main_loop_unref(cw.loop);
  g_assert_no_error(cw.error);
  g_assert_true(cw.ok);
  return relay;
}

/* Fires a REQ answered by N_BACKFILL stored events and EOSE, keeps the main
 * loop still until the monitor thread has queued all of it (so the queue in
 * front of the main loop is far over the bound), then runs the main loop
 * until EOSE. Returns the emitted indices. */
static GArray *
run_backfill(gboolean lossless, guint max_events, guint expect_queued)
{
  BurstRelay server = { .stored = stored_events_n(N_BACKFILL) };
  relay_start(&server);
  GNostrRelay *relay = connect_relay(&server);

  Backfill b = { .indices = g_array_new(FALSE, FALSE, sizeof(guint)) };
  GNostrSubscription *sub = gnostr_subscription_new(relay, kind1_filters());
  g_assert_nonnull(sub);
  g_assert_true(gnostr_subscription_get_lossless(sub)); /* the default */
  gnostr_subscription_set_lossless(sub, lossless);
  if (max_events)
    gnostr_subscription_set_backlog_limit(sub, max_events, 0);
  g_signal_connect(sub, "event", G_CALLBACK(on_backfill_event), &b);
  g_signal_connect(sub, "eose", G_CALLBACK(on_backfill_eose), &b);
  g_signal_connect(sub, "closed", G_CALLBACK(on_backfill_closed), &b);
  g_autoptr(GError) error = NULL;
  g_assert_true(gnostr_subscription_fire(sub, &error));
  g_assert_no_error(error);

  /* The main loop does not run here: nothing is emitted, everything the
   * relay sent piles up in front of it. */
  gint64 deadline = g_get_monotonic_time() + WAIT_BOUND_S * G_USEC_PER_SEC;
  gboolean eose_queued = FALSE;
  guint queued = 0;
  while (g_get_monotonic_time() < deadline) {
    queued = gnostr_subscription_test_queued(sub, &eose_queued);
    if (eose_queued)
      break;
    g_usleep(1000);
  }
  g_assert_true(eose_queued);
  g_assert_cmpuint(queued, ==, expect_queued);
  g_assert_cmpuint(b.indices->len, ==, 0);

  gboolean timed_out = FALSE;
  guint bound = g_timeout_add_seconds(WAIT_BOUND_S, on_bound, &timed_out);
  while (!b.eose && !b.closed && !timed_out)
    g_main_context_iteration(NULL, TRUE);
  if (!timed_out)
    g_source_remove(bound);
  g_assert_false(timed_out);
  g_assert_false(b.closed);
  g_assert_false(gnostr_subscription_get_overflowed(sub));
  g_assert_cmpuint(b.events_at_eose, ==, b.indices->len); /* nothing after EOSE */

  g_signal_handlers_disconnect_by_data(sub, &b);
  gnostr_subscription_close(sub);
  g_object_unref(sub);
  gnostr_relay_disconnect(relay);
  g_object_unref(relay);
  relay_stop(&server);
  g_ptr_array_unref(server.stored);
  return b.indices;
}

/* Groundhog's case: every stored event reaches the handler, in order. */
static void
test_lossless_backfill_complete(void)
{
  guint events = 0;
  guint64 bytes = 0;
  GNostrSubscription *probe = g_object_new(GNOSTR_TYPE_SUBSCRIPTION, NULL);
  gnostr_subscription_get_backlog_limit(probe, &events, &bytes);
  g_assert_cmpuint(events, ==, GNOSTR_SUBSCRIPTION_DEFAULT_MAX_BACKLOG_EVENTS);
  g_assert_cmpuint(bytes, ==, GNOSTR_SUBSCRIPTION_DEFAULT_MAX_BACKLOG_BYTES);
  g_object_unref(probe);

  GArray *indices = run_backfill(TRUE, 0, N_BACKFILL);
  g_assert_cmpuint(indices->len, ==, N_BACKFILL);
  for (guint i = 0; i < indices->len; i++)
    g_assert_cmpuint(g_array_index(indices, guint, i), ==, i);
  g_array_unref(indices);
}

/* nostrc-5rfp: a legitimate backfill that fills the backlog exactly up to
 * the event ceiling is not cut: all 1,000 events, then EOSE. */
static void
test_lossless_backfill_at_ceiling(void)
{
  GArray *indices = run_backfill(TRUE, N_BACKFILL, N_BACKFILL);
  g_assert_cmpuint(indices->len, ==, N_BACKFILL);
  for (guint i = 0; i < indices->len; i++)
    g_assert_cmpuint(g_array_index(indices, guint, i), ==, i);
  g_array_unref(indices);
}

/* nostrc-5rfp: a flood past the ceiling while the main loop does not run.
 * Returns how many events were emitted before the explicit close. */
static guint
run_flood(guint max_events, guint64 max_bytes)
{
  BurstRelay server = { .stored = stored_events_n(N_BACKFILL) };
  relay_start(&server);
  GNostrRelay *relay = connect_relay(&server);

  Backfill b = { .indices = g_array_new(FALSE, FALSE, sizeof(guint)) };
  GNostrSubscription *sub = gnostr_subscription_new(relay, kind1_filters());
  g_assert_nonnull(sub);
  gnostr_subscription_set_backlog_limit(sub, max_events, max_bytes);
  g_signal_connect(sub, "event", G_CALLBACK(on_backfill_event), &b);
  g_signal_connect(sub, "eose", G_CALLBACK(on_backfill_eose), &b);
  g_signal_connect(sub, "closed", G_CALLBACK(on_backfill_closed), &b);
  g_atomic_int_set(&overflow_warnings, 0);
  g_atomic_int_set(&overflow_expected, 1);
  guint warning_handler = g_log_set_handler("gnostr-subscription",
                                            G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL |
                                              G_LOG_FLAG_RECURSION,
                                            on_subscription_warning, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true(gnostr_subscription_fire(sub, &error));
  g_assert_no_error(error);

  /* The main loop does not run: the backlog grows until the ceiling. */
  gint64 deadline = g_get_monotonic_time() + WAIT_BOUND_S * G_USEC_PER_SEC;
  while (!gnostr_subscription_get_overflowed(sub) && g_get_monotonic_time() < deadline)
    g_usleep(1000);
  g_assert_true(gnostr_subscription_get_overflowed(sub));
  gboolean eose_queued = TRUE;
  guint queued = gnostr_subscription_test_queued(sub, &eose_queued);
  g_assert_false(eose_queued);   /* the flood never reached its EOSE */
  g_assert_cmpuint(queued, >, 0);
  g_assert_cmpuint(queued, <, N_BACKFILL);
  if (!max_bytes)   /* the event ceiling alone */
    g_assert_cmpuint(queued, ==, max_events);

  /* The relay is asked to stop sending, before the main loop runs. */
  while (g_atomic_int_get(&server.closes) == 0 && g_get_monotonic_time() < deadline)
    g_usleep(1000);
  g_assert_cmpuint(g_atomic_int_get(&server.closes), ==, 1);

  gboolean timed_out = FALSE;
  guint bound = g_timeout_add_seconds(WAIT_BOUND_S, on_bound, &timed_out);
  while (!b.closed && !timed_out)
    g_main_context_iteration(NULL, TRUE);
  if (!timed_out)
    g_source_remove(bound);
  g_assert_false(timed_out);
  g_assert_cmpint(g_atomic_int_get(&overflow_warnings), ==, 1);

  /* Every queued event, in order, then the explicit close; no EOSE. */
  g_assert_cmpstr(b.closed_reason, ==, GNOSTR_SUBSCRIPTION_OVERFLOW_REASON);
  g_assert_true(g_str_has_prefix(b.closed_reason, GNOSTR_SUBSCRIPTION_OVERFLOW_PREFIX));
  g_assert_false(b.eose);
  g_assert_cmpuint(b.events_at_close, ==, queued);
  g_assert_cmpuint(b.indices->len, ==, queued);
  for (guint i = 0; i < b.indices->len; i++)
    g_assert_cmpuint(g_array_index(b.indices, guint, i), ==, i);
  g_assert_cmpint(gnostr_subscription_get_state(sub), ==, GNOSTR_SUBSCRIPTION_STATE_CLOSED);

  /* Nothing follows the close, even with the main loop running a while. */
  gboolean settled = FALSE;
  bound = g_timeout_add(200, on_bound, &settled);
  while (!settled)
    g_main_context_iteration(NULL, TRUE);
  g_assert_cmpuint(b.indices->len, ==, queued);
  g_assert_false(b.eose);

  /* Closing again sends no second CLOSE. */
  gnostr_subscription_close(sub);
  g_assert_cmpuint(g_atomic_int_get(&server.closes), ==, 1);

  g_signal_handlers_disconnect_by_data(sub, &b);
  g_object_unref(sub);
  gnostr_relay_disconnect(relay);
  g_object_unref(relay);
  relay_stop(&server);
  g_log_remove_handler("gnostr-subscription", warning_handler);
  g_atomic_int_set(&overflow_expected, 0);
  g_ptr_array_unref(server.stored);
  g_array_unref(b.indices);
  g_free(b.closed_reason);
  return queued;
}

static void
test_lossless_flood_event_ceiling(void)
{
  g_assert_cmpuint(run_flood(300, 0), ==, 300);
}

/* The byte ceiling alone: about 100 events' worth of JSON. */
static void
test_lossless_flood_byte_ceiling(void)
{
  g_autoptr(GPtrArray) sample = stored_events_n(1);
  guint64 one = strlen(g_ptr_array_index(sample, 0));
  guint emitted = run_flood(G_MAXUINT, 100 * one);
  g_assert_cmpuint(emitted, >=, 90);
  g_assert_cmpuint(emitted, <=, 110);
}

/* Gnostr's case: the queue stays bounded, keeps the newest events, in order. */
static void
test_bounded_backfill_keeps_newest(void)
{
  GArray *indices = run_backfill(FALSE, 0, BOUNDED_CAPACITY);
  g_assert_cmpuint(indices->len, ==, BOUNDED_CAPACITY);
  for (guint i = 0; i < indices->len; i++)
    g_assert_cmpuint(g_array_index(indices, guint, i), ==, N_BACKFILL - BOUNDED_CAPACITY + i);
  g_array_unref(indices);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  /* Before any thread logs. */
  previous_default_handler = g_log_set_default_handler(log_forgiven_as_warning, NULL);
  g_test_add_func("/nostr-gobject/subscription/stored-events-precede-eose",
                  test_stored_events_precede_eose);
  g_test_add_func("/nostr-gobject/subscription/lossless-backfill-complete",
                  test_lossless_backfill_complete);
  g_test_add_func("/nostr-gobject/subscription/lossless-backfill-at-ceiling",
                  test_lossless_backfill_at_ceiling);
  g_test_add_func("/nostr-gobject/subscription/lossless-flood-event-ceiling",
                  test_lossless_flood_event_ceiling);
  g_test_add_func("/nostr-gobject/subscription/lossless-flood-byte-ceiling",
                  test_lossless_flood_byte_ceiling);
  g_test_add_func("/nostr-gobject/subscription/bounded-backfill-keeps-newest",
                  test_bounded_backfill_keeps_newest);
  return g_test_run();
}
