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
 * SPDX-License-Identifier: MIT
 */
#include <nostr-gobject-1.0/nostr_relay.h>
#include <nostr-gobject-1.0/nostr_subscription.h>
#include <nostr-event.h>
#include <nostr-filter.h>
#include <nostr-keys.h>
#include <nostr-relay.h>
#include <json.h>

#include <gio/gio.h>
#include <glib.h>
#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>

#define N_STORED 24
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

/* N_STORED signed kind-1 events. */
static GPtrArray *
stored_events(void)
{
  GPtrArray *stored = g_ptr_array_new_with_free_func(g_free);
  char *sk = nostr_key_generate_private();
  g_assert_nonnull(sk);
  for (guint i = 0; i < N_STORED; i++) {
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

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-gobject/subscription/stored-events-precede-eose",
                  test_stored_events_precede_eose);
  return g_test_run();
}
