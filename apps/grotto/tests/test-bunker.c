/* Real BunkerService lifecycle/custody/configuration regressions (G05).
 * Wire policy/crypto/replay/cancellation are exercised by bunker-e2e. */
#include <glib.h>
#include "bunker_service.h"
#include "bunker_test_relay.h"
#include <nostr/nip55l/signer_ops.h>
#include <nostr-utils.h>
#include <glib/gstdio.h>
#include "json.h"
#include <keys.h>
#include <nostr-event.h>
#include <nostr/nip46/nip46_bunker.h>
#include <nostr/nip46/nip46_client.h>
#include <nostr/nip46/nip46_msg.h>
#include <nostr-gobject-1.0/nostr_nip46_bunker.h>
#include <secure_buf.h>
#include <time.h>

static gboolean failure_bound(gpointer data);
static const gchar *identity =
    "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";

typedef struct {
    guint errors;
    const gchar *expected_error;
} StateEvidence;

static void state_changed(BunkerState state, const gchar *error, gpointer data)
{
    StateEvidence *evidence = data;
    g_assert_cmpint(state, !=, BUNKER_STATE_RUNNING);
    if (state == BUNKER_STATE_ERROR) {
        evidence->errors++;
        g_assert_cmpstr(error, ==, evidence->expected_error);
    }
}

static void test_no_relays(void)
{
    BunkerService *service = bunker_service_new();
    StateEvidence evidence = {0, "No bunker relays configured"};
    const gchar *empty[] = {NULL};
    bunker_service_set_state_callback(service, state_changed, &evidence);
    g_assert_false(bunker_service_start(service, NULL, identity));
    g_assert_cmpint(bunker_service_get_state(service), ==, BUNKER_STATE_ERROR);
    g_assert_false(bunker_service_start(service, empty, identity));
    g_assert_cmpint(bunker_service_get_state(service), ==, BUNKER_STATE_ERROR);
    g_assert_cmpuint(evidence.errors, ==, 2);
    bunker_service_free(service);
}

static void test_listener_failure(void)
{
    BunkerService *service = bunker_service_new();
    StateEvidence evidence = {0, "Transport key missing; reset bunker endpoint"};
    g_autofree gchar *key_path = g_build_filename(g_get_user_config_dir(), "grotto", "test-bunker-transport", NULL);
    g_unlink(key_path);
    const gchar *relays[] = {"ws://127.0.0.1:1/", NULL};
    bunker_service_set_state_callback(service, state_changed, &evidence);
    /* Missing custody never silently regenerates a new endpoint. */
    for (guint i = 0; i < 2; i++) {
        guint bound = g_timeout_add_seconds(10, failure_bound, NULL);
        g_assert_true(bunker_service_start(service, relays, identity));
        while (bunker_service_get_state(service) == BUNKER_STATE_STARTING) g_main_context_iteration(NULL, TRUE);
        g_source_remove(bound);
        g_assert_cmpint(bunker_service_get_state(service), ==, BUNKER_STATE_ERROR);
        g_assert_false(g_file_test(key_path, G_FILE_TEST_EXISTS));
    }
    g_assert_cmpuint(evidence.errors, ==, 2);
    bunker_service_free(service);
}

static gboolean failure_bound(gpointer data) { (void)data; g_error("bunker operation exceeded failure bound"); return G_SOURCE_REMOVE; }
static void provision_transport(void) {
    guint8 key[32] = {0}; key[31] = 2;
    g_assert_cmpint(nostr_nip55l_bunker_transport_store(key), ==, 0);
}
static void test_lifecycle(void) {
    provision_transport();
    BunkerService *service = bunker_service_new();
    BunkerTestRelay *relay = bunker_test_relay_new();
    const gchar *relays[] = {bunker_test_relay_url(relay), bunker_test_relay_url(relay), NULL};
    for (guint i = 0; i < 2; i++) {
        guint bound = g_timeout_add_seconds(10, failure_bound, NULL);
        g_assert_true(bunker_service_start(service, relays, identity));
        g_assert_cmpint(bunker_service_get_state(service), ==, BUNKER_STATE_STARTING);
        g_assert_null(bunker_service_get_bunker_uri(service, NULL));
        while (bunker_service_get_state(service) != BUNKER_STATE_RUNNING) g_main_context_iteration(NULL, TRUE);
        g_source_remove(bound);
        g_autofree gchar *uri = bunker_service_get_bunker_uri(service, NULL);
        g_assert_nonnull(uri);
        g_assert_nonnull(strstr(uri, bunker_service_get_transport_pubkey(service)));
        g_assert_null(strstr(uri, identity));
        bunker_service_stop(service);
        g_assert_cmpint(bunker_service_get_state(service), ==, BUNKER_STATE_STOPPED);
        g_assert_null(bunker_service_get_bunker_uri(service, NULL));
    }
    bunker_service_free(service); bunker_test_relay_free(relay);
}
static void test_invalid_config(void) {
    BunkerService *service = bunker_service_new();
    const gchar *invalid[] = {"ws://example.invalid/", "https://example.invalid/", "wss://user:password@example.invalid/", "wss://example.onion/", "wss://EXAMPLE.ONION/", "wss://example.invalid/#fragment", "not-a-url"};
    for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
        const gchar *relays[] = {invalid[i], NULL};
        g_assert_false(bunker_service_start(service, relays, identity));
        g_assert_cmpint(bunker_service_get_state(service), ==, BUNKER_STATE_ERROR);
        g_assert_null(bunker_service_get_bunker_uri(service, "not-an-admission-token"));
    }
    g_assert_false(bunker_service_handle_connect_uri(service, "nostrconnect://not-admission"));
    g_autoptr(GPtrArray) connections = bunker_service_list_connections(service);
    g_assert_cmpuint(connections->len, ==, 0);
    bunker_service_free(service);
}
typedef struct {
    BunkerTestRelay *relay;
    NostrNip46Session *sender;
    gchar *secret, *client, *transport, *response;
    guint dispatched;
    GThread *owner;
    NostrNip46BunkerRequest *pending;
} Wire;
static void wire_capture(const gchar *json, gpointer data) {
    Wire *w = data; NostrEvent *event = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize(event, json), ==, 0);
    g_assert_true(nostr_event_check_signature(event));
    g_assert_cmpstr(nostr_event_get_pubkey(event), ==, w->transport);
    gchar *plain = NULL;
    g_assert_cmpint(nostr_nip46_client_nip44_decrypt(w->sender, w->transport, nostr_event_get_content(event), &plain), ==, 0);
    g_free(w->response); w->response = plain; nostr_event_free(event);
}
static void wire_init(Wire *w, BunkerTestRelay *relay, const gchar *transport) {
    w->relay = relay; w->transport = g_strdup(transport); w->owner = g_thread_self();
    w->secret = nostr_key_generate_private(); w->client = nostr_key_get_public(w->secret);
    w->sender = nostr_nip46_client_new();
    g_assert_cmpint(nostr_nip46_client_set_secret(w->sender, w->secret), ==, 0);
    bunker_test_relay_set_event_callback(relay, wire_capture, w);
}
static void wire_inject(Wire *w, const gchar *method) {
    g_clear_pointer(&w->response, g_free);
    g_autofree gchar *plain = nostr_nip46_request_build(method, method, NULL, 0);
    gchar *cipher = NULL;
    g_assert_cmpint(nostr_nip46_client_nip44_encrypt(w->sender, w->transport, plain, &cipher), ==, 0);
    NostrEvent *event = nostr_event_new(); nostr_event_set_kind(event, NOSTR_EVENT_KIND_NIP46);
    nostr_event_set_pubkey(event, w->client); nostr_event_set_created_at(event, time(NULL));
    nostr_event_set_content(event, cipher); free(cipher);
    nostr_event_set_tags(event, nostr_tags_new(1, nostr_tag_new("p", w->transport, NULL)));
    g_assert_cmpint(nostr_event_sign(event, w->secret), ==, 0);
    g_autofree gchar *json = nostr_event_serialize(event); nostr_event_free(event);
    bunker_test_relay_inject(w->relay, json);
}
static void wire_expect(Wire *w, const gchar *error) {
    guint bound = g_timeout_add_seconds(10, failure_bound, NULL);
    while (!w->response) g_main_context_iteration(NULL, TRUE);
    g_source_remove(bound);
    NostrNip46Response response = {0};
    g_assert_cmpint(nostr_nip46_response_parse(w->response, &response), ==, 0);
    g_assert_cmpstr(response.error, ==, error);
    if (!error) g_assert_cmpstr(response.result, ==, "handled");
    nostr_nip46_response_free(&response);
}
static void wire_clear(Wire *w) {
    bunker_test_relay_set_event_callback(w->relay, NULL, NULL);
    nostr_nip46_session_free(w->sender);
    secure_wipe(w->secret, strlen(w->secret)); free(w->secret); free(w->client);
    g_free(w->transport); g_free(w->response);
}
static void deferred_request(GNostrNip46Bunker *bunker, NostrNip46BunkerRequest *request, gpointer data) {
    (void)bunker; Wire *w = data;
    g_assert_true(g_thread_self() == w->owner);
    g_assert_cmpstr(nostr_nip46_bunker_request_client(request), ==, w->client);
    g_assert_nonnull(nostr_nip46_bunker_request_event_id(request));
    w->dispatched++;
    if (!strcmp(nostr_nip46_bunker_request_rpc(request)->method, "pending")) {
        w->pending = nostr_nip46_bunker_request_ref(request); return;
    }
    g_assert_cmpint(nostr_nip46_bunker_request_complete(request, "\"handled\"", NULL), ==, 0);
    g_assert_cmpint(nostr_nip46_bunker_request_complete(request, "\"twice\"", NULL), ==, -1);
}
static void test_request_boundary(void) {
    guint8 key[32] = {0}; key[31] = 3;
    g_autoptr(GNostrNip46Bunker) bunker = gnostr_nip46_bunker_new();
    /* No account secret, sign callback or ACL is populated. Every method must
     * reach the opt-in boundary, including the legacy built-ins and unknowns. */
    g_assert_true(gnostr_nip46_bunker_set_transport_key(bunker, key, NULL));
    g_assert_true(gnostr_nip46_bunker_enable_requests(bunker));
    gchar *transport = NULL;
    g_assert_cmpint(nostr_nip46_bunker_get_transport_pubkey(gnostr_nip46_bunker_get_session(bunker), &transport), ==, 0);
    BunkerTestRelay *relay = bunker_test_relay_new(); Wire w = {0}; wire_init(&w, relay, transport); free(transport);
    g_signal_connect(bunker, "request", G_CALLBACK(deferred_request), &w);
    const gchar *relays[] = {bunker_test_relay_url(relay)};
    g_assert_true(gnostr_nip46_bunker_listen(bunker, relays, 1, NULL));
    guint bound = g_timeout_add_seconds(10, failure_bound, NULL), connections, requests, published;
    do { g_main_context_iteration(NULL, TRUE); bunker_test_relay_stats(relay, &connections, &requests, &published); } while (!requests);
    g_source_remove(bound);
    bunker_test_relay_hold_acks(relay, TRUE);
    wire_inject(&w, "get_public_key"); wire_expect(&w, NULL);
    wire_inject(&w, "pending");
    /* The second request must reach its owner while the first response's OK
     * is deliberately withheld, not after the 3s publication failure bound. */
    bound = g_timeout_add(1000, failure_bound, NULL);
    while (!w.pending) g_main_context_iteration(NULL, TRUE);
    g_source_remove(bound); bunker_test_relay_hold_acks(relay, FALSE);
    g_assert_cmpint(nostr_nip46_bunker_request_complete(w.pending, "\"handled\"", NULL), ==, 0);
    nostr_nip46_bunker_request_unref(w.pending); w.pending = NULL; wire_expect(&w, NULL);
    const gchar *methods[] = {"connect", "get_public_key", "sign_event", "nip04_encrypt", "nip04_decrypt", "nip44_encrypt", "nip44_decrypt", "ping", "get_relays", "unknown"};
    for (guint i = 0; i < G_N_ELEMENTS(methods); i++) { wire_inject(&w, methods[i]); wire_expect(&w, NULL); }
    g_assert_cmpuint(w.dispatched, ==, G_N_ELEMENTS(methods) + 2);
    wire_inject(&w, "pending"); bound = g_timeout_add_seconds(10, failure_bound, NULL);
    while (!w.pending) g_main_context_iteration(NULL, TRUE);
    g_source_remove(bound); gnostr_nip46_bunker_stop(bunker);
    g_assert_cmpint(nostr_nip46_bunker_request_complete(w.pending, "\"late\"", NULL), ==, -1);
    nostr_nip46_bunker_request_unref(w.pending);
    wire_clear(&w); bunker_test_relay_free(relay);
}
static void counted_request(GNostrNip46Bunker *bunker, NostrNip46BunkerRequest *request, gpointer data) {
    (void)bunker; Wire *w = data; g_assert_true(g_thread_self() == w->owner); w->dispatched++;
    g_assert_cmpint(nostr_nip46_bunker_request_complete(request, "\"handled\"", NULL), ==, 0);
}
static void test_owner_queue_bound(void) {
    guint8 key[32] = {0}; key[31] = 4;
    g_autoptr(GNostrNip46Bunker) bunker = gnostr_nip46_bunker_new();
    g_assert_true(gnostr_nip46_bunker_set_transport_key(bunker, key, NULL));
    g_assert_true(gnostr_nip46_bunker_enable_requests(bunker));
    gchar *transport = NULL;
    g_assert_cmpint(nostr_nip46_bunker_get_transport_pubkey(gnostr_nip46_bunker_get_session(bunker), &transport), ==, 0);
    BunkerTestRelay *relay = bunker_test_relay_new(); Wire w = {0}; wire_init(&w, relay, transport); free(transport);
    g_signal_connect(bunker, "request", G_CALLBACK(counted_request), &w);
    const gchar *relays[] = {bunker_test_relay_url(relay)};
    g_assert_true(gnostr_nip46_bunker_listen(bunker, relays, 1, NULL));
    guint bound = g_timeout_add_seconds(10, failure_bound, NULL), connections, requests, published;
    do { g_main_context_iteration(NULL, TRUE); bunker_test_relay_stats(relay, &connections, &requests, &published); } while (!requests);
    g_source_remove(bound);
    g_autofree gchar *plain = nostr_nip46_request_build("queue-bound", "ping", NULL, 0);
    gchar *cipher = NULL;
    g_assert_cmpint(nostr_nip46_client_nip44_encrypt(w.sender, w.transport, plain, &cipher), ==, 0);
    /* Trusted direct-call fixture owns the sender key. Do not dispatch the
     * owner context while flooding the producer boundary with retries. */
    for (guint i = 0; i < 300; i++) {
        gchar *reply = NULL;
        g_assert_true(gnostr_nip46_bunker_handle_cipher(bunker, w.client, cipher, &reply, NULL));
        g_assert_null(reply);
    }
    free(cipher); g_assert_cmpuint(w.dispatched, ==, 0);
    bound = g_timeout_add_seconds(10, failure_bound, NULL);
    while (w.dispatched < 256) g_main_context_iteration(NULL, TRUE);
    g_source_remove(bound); g_assert_cmpuint(w.dispatched, ==, 256);
    gnostr_nip46_bunker_stop(bunker); wire_clear(&w); bunker_test_relay_free(relay);
}
static void test_method_ceiling(void) {
    provision_transport();
    BunkerService *service = bunker_service_new(); BunkerTestRelay *relay = bunker_test_relay_new();
    const gchar *relays[] = {bunker_test_relay_url(relay), NULL};
    const gchar *none[] = {NULL}; bunker_service_set_allowed_methods(service, none);
    g_assert_true(bunker_service_start(service, relays, identity));
    guint bound = g_timeout_add_seconds(10, failure_bound, NULL);
    while (bunker_service_get_state(service) != BUNKER_STATE_RUNNING) g_main_context_iteration(NULL, TRUE);
    g_source_remove(bound); Wire w = {0}; wire_init(&w, relay, bunker_service_get_transport_pubkey(service));
    const gchar *methods[] = {"get_public_key", "ping", "get_relays", "switch_relays", "auth_url", "disconnect"};
    for (guint i = 0; i < G_N_ELEMENTS(methods); i++) { wire_inject(&w, methods[i]); wire_expect(&w, "unsupported_method"); }
    /* A rejected OK is not a successful publish or a usable subscription. */
    bunker_test_relay_reject_publishes(relay, TRUE); wire_inject(&w, "unknown"); wire_expect(&w, "unsupported_method");
    bound = g_timeout_add_seconds(10, failure_bound, NULL);
    while (bunker_service_get_state(service) != BUNKER_STATE_OFFLINE) g_main_context_iteration(NULL, TRUE);
    g_source_remove(bound);
    wire_clear(&w); bunker_service_free(service); bunker_test_relay_free(relay);
}
static void test_relay_rejections(void) {
    provision_transport();
    for (guint mode = 0; mode < 2; mode++) {
        BunkerService *service = bunker_service_new(); BunkerTestRelay *relay = bunker_test_relay_new();
        if (!mode) bunker_test_relay_reject_subscriptions(relay, TRUE);
        const gchar *relays[] = {bunker_test_relay_url(relay), NULL};
        g_assert_true(bunker_service_start(service, relays, identity));
        guint bound = g_timeout_add_seconds(10, failure_bound, NULL);
        if (mode) {
            while (bunker_service_get_state(service) != BUNKER_STATE_RUNNING) g_main_context_iteration(NULL, TRUE);
            bunker_test_relay_challenge(relay);
        }
        while (bunker_service_get_state(service) != BUNKER_STATE_OFFLINE) g_main_context_iteration(NULL, TRUE);
        g_source_remove(bound); g_assert_null(bunker_service_get_bunker_uri(service, NULL));
        bunker_service_free(service); bunker_test_relay_free(relay);
    }
}
static void test_start_cancel(void) {
    provision_transport();
    BunkerService *service = bunker_service_new(); BunkerTestRelay *relay = bunker_test_relay_new();
    const gchar *relays[] = {bunker_test_relay_url(relay), NULL};
    g_assert_true(bunker_service_start(service, relays, identity));
    bunker_service_stop(service); bunker_service_free(service);
    /* A subsequent active startup drains any obsolete worker completion on the
     * same owner context; the closed service must not resurrect a listener. */
    service = bunker_service_new(); g_assert_true(bunker_service_start(service, relays, identity));
    guint bound = g_timeout_add_seconds(10, failure_bound, NULL);
    while (bunker_service_get_state(service) != BUNKER_STATE_RUNNING) g_main_context_iteration(NULL, TRUE);
    g_source_remove(bound);
    guint connections, requests, published; bunker_test_relay_stats(relay, &connections, &requests, &published);
    g_assert_cmpuint(requests, ==, 1); bunker_service_free(service); bunker_test_relay_free(relay);
}
static void test_corrupt_replay(void) {
    g_autofree gchar *path = g_build_filename(g_get_user_config_dir(), "grotto", "bunker-seen.ini", NULL);
    g_autofree gchar *dir = g_path_get_dirname(path); g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
    g_assert_true(g_file_set_contents(path, "[meta]\nversion=9000\n", -1, NULL));
    BunkerService *service = bunker_service_new(); const gchar *relays[] = {"ws://127.0.0.1:1/", NULL};
    g_assert_false(bunker_service_replay_healthy(service));
    g_assert_false(bunker_service_start(service, relays, identity));
    g_assert_cmpint(bunker_service_get_state(service), ==, BUNKER_STATE_ERROR);
    bunker_service_free(service); g_unlink(path);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_assert_cmpstr(g_getenv("NOSTRC_TEST_ISOLATION"), ==, "1");
    g_setenv("NOSTR_SIGNER_TEST_TRANSPORT_BACKEND", "file", TRUE);
    nostr_json_init();
    g_test_add_func("/bunker-startup/no-relays", test_no_relays);
    g_test_add_func("/bunker-startup/listener-failure", test_listener_failure);
    g_test_add_func("/bunker/lifecycle", test_lifecycle);
    g_test_add_func("/bunker/invalid-config", test_invalid_config);
    g_test_add_func("/bunker/request-boundary", test_request_boundary);
    g_test_add_func("/bunker/owner-queue-bound", test_owner_queue_bound);
    g_test_add_func("/bunker/method-ceiling", test_method_ceiling);
    g_test_add_func("/bunker/relay-rejections", test_relay_rejections);
    g_test_add_func("/bunker/start-cancel", test_start_cancel);
    g_test_add_func("/bunker/corrupt-replay", test_corrupt_replay);
    int rc = g_test_run(); nostr_json_cleanup(); return rc;
}
