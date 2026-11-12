/* nostrc-0cfsg.36.33: teardown during real stalled TLS/HTTP and WS upgrades.
 * No sleeps, public relays, suppression, test-mode transport, or crypto cleanup
 * calls. Socket reads establish the in-flight boundary; deadlines only fail a
 * stuck test. Normal process exit exercises the production terminal drain. */
#include <gio/gio.h>
#include <string.h>
#include <nostr-gobject-1.0/nostr_relay.h>

#define WATCHDOG_SECONDS 4

typedef struct {
    GSocketService *service;
    GCancellable *cancel;
    GPtrArray *peers;
    guint requests;
    guint ws_requests;
    guint closed;
    gboolean done;
    gboolean expired;
    GMainLoop *loop;
    GNostrRelay *expected;
    guint callbacks;
    guint info_signals;
} Fixture;

typedef struct {
    Fixture *fixture;
    GSocketConnection *connection;
    GByteArray *request;
    gboolean started;
} Peer;

static void peer_free(gpointer data)
{
    Peer *peer = data;
    g_io_stream_close(G_IO_STREAM(peer->connection), NULL, NULL);
    g_object_unref(peer->connection);
    g_byte_array_unref(peer->request);
    g_free(peer);
}

static void read_peer(GObject *source, GAsyncResult *result, gpointer data)
{
    Peer *peer = data;
    Fixture *f = peer->fixture;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);
    if (error || g_bytes_get_size(bytes) == 0) {
        f->closed++;
        g_main_loop_quit(f->loop);
        return;
    }
    if (!peer->started) {
        gsize length = 0;
        const guint8 *data_bytes = g_bytes_get_data(bytes, &length);
        g_byte_array_append(peer->request, data_bytes, length);
        g_assert_cmpuint(peer->request->len, <, 65536);
        /* TLS ClientHello is enough for the fetch boundary. For HTTP, assemble
         * the complete headers so a metadata GET cannot masquerade as the WS
         * upgrade and TCP segmentation cannot make the test timing-dependent. */
        g_autofree gchar *headers = g_strndup((const gchar *)peer->request->data, peer->request->len);
        if (peer->request->data[0] == 0x16 || strstr(headers, "\r\n\r\n")) {
            peer->started = TRUE;
            f->requests++;
            g_autofree gchar *lower = g_ascii_strdown(headers, -1);
            if (strstr(lower, "upgrade: websocket")) f->ws_requests++;
        }
        g_main_loop_quit(f->loop);
    }
    /* Never answer ClientHello, HTTP headers or the WebSocket upgrade. */
    g_input_stream_read_bytes_async(G_INPUT_STREAM(source), 4096, G_PRIORITY_DEFAULT,
                                    f->cancel, read_peer, peer);
}

static gboolean incoming(GSocketService *service, GSocketConnection *connection,
                          GObject *source, gpointer data)
{
    (void)service; (void)source;
    Fixture *f = data;
    Peer *peer = g_new0(Peer, 1);
    peer->fixture = f;
    peer->connection = g_object_ref(connection);
    peer->request = g_byte_array_new();
    g_ptr_array_add(f->peers, peer);
    g_input_stream_read_bytes_async(g_io_stream_get_input_stream(G_IO_STREAM(connection)),
                                    4096, G_PRIORITY_DEFAULT, f->cancel, read_peer, peer);
    return TRUE;
}

static gboolean watchdog(gpointer data)
{
    Fixture *f = data;
    f->expired = TRUE;
    g_main_loop_quit(f->loop);
    return G_SOURCE_REMOVE;
}

static guint16 fixture_init(Fixture *f)
{
    g_autoptr(GError) error = NULL;
    f->loop = g_main_loop_new(NULL, FALSE);
    f->cancel = g_cancellable_new();
    f->peers = g_ptr_array_new_with_free_func(peer_free);
    f->service = g_socket_service_new();
    g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    g_autoptr(GSocketAddress) address = g_inet_socket_address_new(loopback, 0);
    g_autoptr(GSocketAddress) bound = NULL;
    g_assert_true(g_socket_listener_add_address(G_SOCKET_LISTENER(f->service), address,
        G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL, &bound, &error));
    g_assert_no_error(error);
    g_signal_connect(f->service, "incoming", G_CALLBACK(incoming), f);
    g_socket_service_start(f->service);
    return g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
}

static void wait_for(Fixture *f, gboolean (*ready)(Fixture *))
{
    guint timeout = g_timeout_add_seconds(WATCHDOG_SECONDS, watchdog, f);
    while (!ready(f) && !f->expired)
        g_main_loop_run(f->loop);
    if (!f->expired) g_source_remove(timeout);
    g_assert_false(f->expired);
}

static gboolean request_seen(Fixture *f) { return f->requests > 0; }
static gboolean ws_request_seen(Fixture *f) { return f->ws_requests > 0; }
static gboolean peer_closed(Fixture *f) { return f->closed > 0; }
static gboolean callback_seen(Fixture *f) { return f->callbacks > 0; }
static gboolean all_closed(Fixture *f) { return f->closed == f->peers->len; }

static void fixture_clear(Fixture *f)
{
    g_socket_service_stop(f->service);
    g_socket_listener_close(G_SOCKET_LISTENER(f->service));
    g_cancellable_cancel(f->cancel);
    wait_for(f, all_closed); /* async reads no longer borrow Fixture/Peer */
    g_object_unref(f->service);
    g_ptr_array_unref(f->peers);
    g_object_unref(f->cancel);
    g_main_loop_unref(f->loop);
}

static void info_received(GNostrRelay *relay, gpointer data)
{
    (void)relay;
    ((Fixture *)data)->info_signals++;
}

static void test_fetch_dispose(void)
{
    Fixture f = {0};
    guint16 port = fixture_init(&f);
    g_autofree gchar *url = g_strdup_printf("wss://127.0.0.1:%u", port);
    GNostrRelay *relay = g_object_new(GNOSTR_TYPE_RELAY, "url", url, NULL);
    GWeakRef weak;
    g_weak_ref_init(&weak, relay);
    g_signal_connect(relay, "nip11-info-fetched", G_CALLBACK(info_received), &f);
    gnostr_relay_fetch_nip11_async(relay);
    wait_for(&f, request_seen); /* curl is inside a stalled TLS handshake */
    gint64 before = g_get_monotonic_time();
    g_object_run_dispose(G_OBJECT(relay));
    g_object_unref(relay);
    g_assert_cmpint(g_get_monotonic_time() - before, <, 250000);
    wait_for(&f, peer_closed); /* the worker has really cleaned up its socket */
    g_autoptr(GObject) remaining = g_weak_ref_get(&weak);
    g_assert_null(remaining);
    g_assert_cmpuint(f.info_signals, ==, 0);
    g_weak_ref_clear(&weak);
    fixture_clear(&f);
}

static void connected(GObject *source, GAsyncResult *result, gpointer data)
{
    Fixture *f = data;
    g_autoptr(GError) error = NULL;
    g_assert_true(source == G_OBJECT(f->expected));
    g_assert_false(gnostr_relay_connect_finish(GNOSTR_RELAY(source), result, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    f->callbacks++;
    g_main_loop_quit(f->loop);
}

static void test_connect_cancel(void)
{
    Fixture f = {0};
    guint16 port = fixture_init(&f);
    g_autofree gchar *url = g_strdup_printf("ws://127.0.0.1:%u", port);
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    GNostrRelay *relay = g_object_new(GNOSTR_TYPE_RELAY, "url", url, NULL);
    f.expected = relay;
    GWeakRef weak;
    g_weak_ref_init(&weak, relay);
    gnostr_relay_connect_async(relay, cancel, connected, &f);
    wait_for(&f, ws_request_seen); /* LWS has sent the upgrade; server never answers */
    gint64 before = g_get_monotonic_time();
    g_cancellable_cancel(cancel);
    g_object_run_dispose(G_OBJECT(relay));
    g_object_unref(relay);
    g_assert_cmpint(g_get_monotonic_time() - before, <, 250000);
    wait_for(&f, callback_seen);
    wait_for(&f, peer_closed);
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_autoptr(GObject) remaining = g_weak_ref_get(&weak);
    g_assert_null(remaining);
    g_assert_cmpuint(f.callbacks, ==, 1);
    g_weak_ref_clear(&weak);
    fixture_clear(&f);
}

static void test_terminal_drain(void)
{
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 15000000, G_TEST_SUBPROCESS_DEFAULT);
        g_test_trap_assert_passed();
        return;
    }
    Fixture fetch = {0}, connect = {0};
    guint16 http_port = fixture_init(&fetch);
    guint16 ws_port = fixture_init(&connect);
    g_autofree gchar *http_url = g_strdup_printf("wss://127.0.0.1:%u", http_port);
    g_autofree gchar *ws_url = g_strdup_printf("ws://127.0.0.1:%u", ws_port);
    GNostrRelay *http = g_object_new(GNOSTR_TYPE_RELAY, "url", http_url, NULL);
    GNostrRelay *ws = g_object_new(GNOSTR_TYPE_RELAY, "url", ws_url, NULL);
    connect.expected = ws;
    GWeakRef weak_http, weak_ws;
    g_weak_ref_init(&weak_http, http);
    g_weak_ref_init(&weak_ws, ws);
    gnostr_relay_fetch_nip11_async(http);
    gnostr_relay_connect_async(ws, NULL, connected, &connect);
    wait_for(&fetch, request_seen);
    wait_for(&connect, ws_request_seen);
    g_object_unref(http);
    g_object_unref(ws);
    /* Unlike the per-operation tests, neither cancellation nor the connect
     * callback has happened. The production drain must do BOTH, not merely
     * join workers and strand the task on a stopped application context. */
    g_assert_cmpuint(connect.callbacks, ==, 0);
    gnostr_relay_workers_shutdown();
    /* Late application callbacks must be rejected without reopening transport
     * or aborting a normal shutdown. */
    g_autoptr(GNostrRelay) rejected = g_object_new(GNOSTR_TYPE_RELAY, "url", ws_url, NULL);
    g_assert_null(gnostr_relay_get_core_relay(rejected));
    g_assert_cmpuint(connect.callbacks, ==, 1);
    g_autoptr(GObject) remaining_http = g_weak_ref_get(&weak_http);
    g_autoptr(GObject) remaining_ws = g_weak_ref_get(&weak_ws);
    g_assert_null(remaining_http);
    g_assert_null(remaining_ws);
    g_weak_ref_clear(&weak_http);
    g_weak_ref_clear(&weak_ws);
    fixture_clear(&fetch);
    fixture_clear(&connect);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/relay-worker/stalled-nip11-dispose", test_fetch_dispose);
    g_test_add_func("/relay-worker/stalled-connect-cancel-dispose", test_connect_cancel);
    g_test_add_func("/relay-worker/terminal-drains-pending-callback", test_terminal_drain);
    return g_test_run();
}
