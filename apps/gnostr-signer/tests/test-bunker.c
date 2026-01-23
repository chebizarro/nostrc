/* test-bunker.c - Unit tests for gnostr-signer NIP-46 bunker service
 *
 * Tests NIP-46 remote signing functionality including:
 * - Service lifecycle (start/stop)
 * - Connection management
 * - Authorization and permissions
 * - URI generation and parsing
 * - Request handling
 *
 * Issue: nostrc-ddh
 */
#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ===========================================================================
 * Mock Bunker Service for Testing
 *
 * This mock replicates the core logic of BunkerService for unit testing
 * without requiring actual relay connections or crypto backends.
 * =========================================================================== */

typedef enum {
    MOCK_BUNKER_STATE_STOPPED,
    MOCK_BUNKER_STATE_STARTING,
    MOCK_BUNKER_STATE_RUNNING,
    MOCK_BUNKER_STATE_ERROR
} MockBunkerState;

typedef struct {
    gchar *client_pubkey;
    gchar *app_name;
    gchar **permissions;
    gint64 connected_at;
    gint64 last_request;
    guint request_count;
} MockBunkerConnection;

typedef struct {
    gchar *request_id;
    gchar *client_pubkey;
    gchar *method;
    gchar *event_json;
    gint event_kind;
    gchar *preview;
} MockBunkerSignRequest;

typedef void (*MockBunkerStateChangedCb)(MockBunkerState state, const gchar *error, gpointer user_data);
typedef void (*MockBunkerConnectionCb)(const MockBunkerConnection *conn, gpointer user_data);
typedef gboolean (*MockBunkerAuthorizeCb)(const MockBunkerSignRequest *req, gpointer user_data);

typedef struct {
    MockBunkerState state;
    gchar *error_message;
    gchar *identity_npub;
    gchar *identity_pubkey_hex;
    gchar **relays;
    gsize n_relays;
    gchar **allowed_methods;
    gchar **allowed_pubkeys;
    gchar **auto_approve_kinds;
    GHashTable *connections;
    GHashTable *pending_requests;
    MockBunkerStateChangedCb state_cb;
    gpointer state_cb_ud;
    MockBunkerConnectionCb conn_cb;
    gpointer conn_cb_ud;
    MockBunkerAuthorizeCb auth_cb;
    gpointer auth_cb_ud;
} MockBunkerService;

static void
mock_bunker_connection_free(MockBunkerConnection *conn)
{
    if (!conn) return;
    g_free(conn->client_pubkey);
    g_free(conn->app_name);
    g_strfreev(conn->permissions);
    g_free(conn);
}

static void
mock_bunker_sign_request_free(MockBunkerSignRequest *req)
{
    if (!req) return;
    g_free(req->request_id);
    g_free(req->client_pubkey);
    g_free(req->method);
    g_free(req->event_json);
    g_free(req->preview);
    g_free(req);
}

static void
mock_set_state(MockBunkerService *bs, MockBunkerState state, const gchar *error)
{
    if (!bs) return;
    bs->state = state;
    g_free(bs->error_message);
    bs->error_message = error ? g_strdup(error) : NULL;

    if (bs->state_cb) {
        bs->state_cb(state, error, bs->state_cb_ud);
    }
}

static MockBunkerService *
mock_bunker_service_new(void)
{
    MockBunkerService *bs = g_new0(MockBunkerService, 1);
    bs->state = MOCK_BUNKER_STATE_STOPPED;
    bs->connections = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            (GDestroyNotify)mock_bunker_connection_free);
    bs->pending_requests = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                 (GDestroyNotify)mock_bunker_sign_request_free);
    return bs;
}

static void
mock_bunker_service_free(MockBunkerService *bs)
{
    if (!bs) return;
    g_free(bs->error_message);
    g_free(bs->identity_npub);
    g_free(bs->identity_pubkey_hex);
    g_strfreev(bs->relays);
    g_strfreev(bs->allowed_methods);
    g_strfreev(bs->allowed_pubkeys);
    g_strfreev(bs->auto_approve_kinds);
    g_hash_table_destroy(bs->connections);
    g_hash_table_destroy(bs->pending_requests);
    g_free(bs);
}

static gboolean
mock_bunker_service_start(MockBunkerService *bs,
                          const gchar *const *relays,
                          const gchar *identity)
{
    if (!bs || !identity) return FALSE;

    if (bs->state == MOCK_BUNKER_STATE_RUNNING) {
        return TRUE;
    }

    mock_set_state(bs, MOCK_BUNKER_STATE_STARTING, NULL);

    g_free(bs->identity_npub);
    bs->identity_npub = g_strdup(identity);

    /* Simulate hex conversion for npub */
    if (g_str_has_prefix(identity, "npub1")) {
        /* Fake pubkey hex for testing */
        g_free(bs->identity_pubkey_hex);
        bs->identity_pubkey_hex = g_strdup("abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234");
    } else {
        g_free(bs->identity_pubkey_hex);
        bs->identity_pubkey_hex = g_strdup(identity);
    }

    g_strfreev(bs->relays);
    if (relays) {
        gsize n = 0;
        while (relays[n]) n++;
        bs->n_relays = n;
        bs->relays = g_new0(gchar*, n + 1);
        for (gsize i = 0; i < n; i++) {
            bs->relays[i] = g_strdup(relays[i]);
        }
    }

    mock_set_state(bs, MOCK_BUNKER_STATE_RUNNING, NULL);
    return TRUE;
}

static void
mock_bunker_service_stop(MockBunkerService *bs)
{
    if (!bs) return;

    g_hash_table_remove_all(bs->connections);
    g_hash_table_remove_all(bs->pending_requests);

    mock_set_state(bs, MOCK_BUNKER_STATE_STOPPED, NULL);
}

static MockBunkerState
mock_bunker_service_get_state(MockBunkerService *bs)
{
    return bs ? bs->state : MOCK_BUNKER_STATE_STOPPED;
}

static gchar *
mock_bunker_service_get_bunker_uri(MockBunkerService *bs, const gchar *secret)
{
    if (!bs || !bs->identity_pubkey_hex) return NULL;

    GString *s = g_string_new("bunker://");
    g_string_append(s, bs->identity_pubkey_hex);

    gboolean first = TRUE;
    if (bs->relays) {
        for (gsize i = 0; bs->relays[i]; i++) {
            g_string_append_printf(s, "%srelay=%s", first ? "?" : "&", bs->relays[i]);
            first = FALSE;
        }
    }
    if (secret && *secret) {
        g_string_append_printf(s, "%ssecret=%s", first ? "?" : "&", secret);
    }

    return g_string_free(s, FALSE);
}

static gboolean
mock_bunker_service_handle_connect_uri(MockBunkerService *bs, const gchar *uri)
{
    if (!bs || !uri) return FALSE;

    if (!g_str_has_prefix(uri, "nostrconnect://")) {
        return FALSE;
    }

    /* Parse pubkey from URI */
    const gchar *p = uri + strlen("nostrconnect://");
    const gchar *q = strchr(p, '?');
    gsize len = q ? (gsize)(q - p) : strlen(p);

    gchar *client_pubkey = g_strndup(p, len);

    MockBunkerConnection *conn = g_new0(MockBunkerConnection, 1);
    conn->client_pubkey = client_pubkey;
    conn->connected_at = (gint64)time(NULL);

    g_hash_table_replace(bs->connections, g_strdup(client_pubkey), conn);

    if (bs->conn_cb) {
        bs->conn_cb(conn, bs->conn_cb_ud);
    }

    return TRUE;
}

static guint
mock_bunker_service_connection_count(MockBunkerService *bs)
{
    return bs ? g_hash_table_size(bs->connections) : 0;
}

static gboolean
mock_bunker_service_disconnect_client(MockBunkerService *bs, const gchar *client_pubkey)
{
    if (!bs || !client_pubkey) return FALSE;
    return g_hash_table_remove(bs->connections, client_pubkey);
}

static void
mock_bunker_service_set_allowed_methods(MockBunkerService *bs, const gchar *const *methods)
{
    if (!bs) return;
    g_strfreev(bs->allowed_methods);
    bs->allowed_methods = g_strdupv((gchar**)methods);
}

static void
mock_bunker_service_set_allowed_pubkeys(MockBunkerService *bs, const gchar *const *pubkeys)
{
    if (!bs) return;
    g_strfreev(bs->allowed_pubkeys);
    bs->allowed_pubkeys = g_strdupv((gchar**)pubkeys);
}

static void
mock_bunker_service_set_auto_approve_kinds(MockBunkerService *bs, const gchar *const *kinds)
{
    if (!bs) return;
    g_strfreev(bs->auto_approve_kinds);
    bs->auto_approve_kinds = g_strdupv((gchar**)kinds);
}

static void
mock_bunker_service_set_state_callback(MockBunkerService *bs,
                                       MockBunkerStateChangedCb cb,
                                       gpointer user_data)
{
    if (!bs) return;
    bs->state_cb = cb;
    bs->state_cb_ud = user_data;
}

static void
mock_bunker_service_set_connection_callback(MockBunkerService *bs,
                                            MockBunkerConnectionCb cb,
                                            gpointer user_data)
{
    if (!bs) return;
    bs->conn_cb = cb;
    bs->conn_cb_ud = user_data;
}

static void
mock_bunker_service_set_authorize_callback(MockBunkerService *bs,
                                           MockBunkerAuthorizeCb cb,
                                           gpointer user_data)
{
    if (!bs) return;
    bs->auth_cb = cb;
    bs->auth_cb_ud = user_data;
}

/* Check if pubkey is in allowed list */
static gboolean
mock_bunker_check_allowed_pubkey(MockBunkerService *bs, const gchar *pubkey)
{
    if (!bs || !pubkey) return FALSE;

    /* If no allowed list, allow all */
    if (!bs->allowed_pubkeys || !bs->allowed_pubkeys[0]) {
        return TRUE;
    }

    for (gint i = 0; bs->allowed_pubkeys[i]; i++) {
        if (g_strcmp0(bs->allowed_pubkeys[i], pubkey) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Check if event kind is auto-approved */
static gboolean
mock_bunker_check_auto_approve_kind(MockBunkerService *bs, gint kind)
{
    if (!bs || !bs->auto_approve_kinds) return FALSE;

    gchar kind_str[16];
    g_snprintf(kind_str, sizeof(kind_str), "%d", kind);

    for (gint i = 0; bs->auto_approve_kinds[i]; i++) {
        if (g_strcmp0(bs->auto_approve_kinds[i], kind_str) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Create a pending sign request */
static MockBunkerSignRequest *
mock_bunker_create_sign_request(MockBunkerService *bs,
                                const gchar *client_pubkey,
                                const gchar *event_json,
                                gint kind)
{
    if (!bs) return NULL;

    MockBunkerSignRequest *req = g_new0(MockBunkerSignRequest, 1);
    req->request_id = g_strdup_printf("mock_%ld_%d", (long)time(NULL), g_random_int_range(0, 10000));
    req->client_pubkey = g_strdup(client_pubkey);
    req->method = g_strdup("sign_event");
    req->event_json = g_strdup(event_json);
    req->event_kind = kind;
    req->preview = g_strdup_printf("Event kind %d", kind);

    g_hash_table_insert(bs->pending_requests, g_strdup(req->request_id), req);

    return req;
}

static guint
mock_bunker_pending_request_count(MockBunkerService *bs)
{
    return bs ? g_hash_table_size(bs->pending_requests) : 0;
}

static void
mock_bunker_authorize_response(MockBunkerService *bs,
                               const gchar *request_id,
                               gboolean approved)
{
    if (!bs || !request_id) return;
    (void)approved;
    g_hash_table_remove(bs->pending_requests, request_id);
}

/* ===========================================================================
 * Test Fixtures
 * =========================================================================== */

typedef struct {
    MockBunkerService *bunker;
    gint state_change_count;
    MockBunkerState last_state;
    gint connection_count;
    gchar *last_connected_pubkey;
} BunkerFixture;

static void
test_state_callback(MockBunkerState state, const gchar *error, gpointer user_data)
{
    BunkerFixture *f = (BunkerFixture*)user_data;
    (void)error;
    f->state_change_count++;
    f->last_state = state;
}

static void
test_connection_callback(const MockBunkerConnection *conn, gpointer user_data)
{
    BunkerFixture *f = (BunkerFixture*)user_data;
    f->connection_count++;
    g_free(f->last_connected_pubkey);
    f->last_connected_pubkey = g_strdup(conn->client_pubkey);
}

static void
bunker_fixture_setup(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;
    fixture->bunker = mock_bunker_service_new();
    fixture->state_change_count = 0;
    fixture->last_state = MOCK_BUNKER_STATE_STOPPED;
    fixture->connection_count = 0;
    fixture->last_connected_pubkey = NULL;
}

static void
bunker_fixture_teardown(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;
    mock_bunker_service_free(fixture->bunker);
    g_free(fixture->last_connected_pubkey);
}

/* ===========================================================================
 * Service Lifecycle Tests
 * =========================================================================== */

static void
test_bunker_create_starts_stopped(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    g_assert_cmpint(mock_bunker_service_get_state(fixture->bunker), ==, MOCK_BUNKER_STATE_STOPPED);
}

static void
test_bunker_start_basic(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *relays[] = { "wss://relay.example.com", NULL };
    const gchar *identity = "npub1test1234567890abcdef1234567890abcdef1234567890abcdef12345678";

    gboolean result = mock_bunker_service_start(fixture->bunker, relays, identity);

    g_assert_true(result);
    g_assert_cmpint(mock_bunker_service_get_state(fixture->bunker), ==, MOCK_BUNKER_STATE_RUNNING);
}

static void
test_bunker_start_null_identity(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *relays[] = { "wss://relay.example.com", NULL };

    gboolean result = mock_bunker_service_start(fixture->bunker, relays, NULL);

    g_assert_false(result);
    g_assert_cmpint(mock_bunker_service_get_state(fixture->bunker), ==, MOCK_BUNKER_STATE_STOPPED);
}

static void
test_bunker_start_no_relays(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *identity = "npub1test";

    gboolean result = mock_bunker_service_start(fixture->bunker, NULL, identity);

    g_assert_true(result);
    g_assert_cmpint(mock_bunker_service_get_state(fixture->bunker), ==, MOCK_BUNKER_STATE_RUNNING);
}

static void
test_bunker_start_already_running(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *relays[] = { "wss://relay.example.com", NULL };

    mock_bunker_service_start(fixture->bunker, relays, "npub1test");
    g_assert_cmpint(mock_bunker_service_get_state(fixture->bunker), ==, MOCK_BUNKER_STATE_RUNNING);

    /* Starting again should succeed (no-op) */
    gboolean result = mock_bunker_service_start(fixture->bunker, relays, "npub1other");

    g_assert_true(result);
    g_assert_cmpint(mock_bunker_service_get_state(fixture->bunker), ==, MOCK_BUNKER_STATE_RUNNING);
}

static void
test_bunker_stop(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *relays[] = { "wss://relay.example.com", NULL };
    mock_bunker_service_start(fixture->bunker, relays, "npub1test");

    mock_bunker_service_stop(fixture->bunker);

    g_assert_cmpint(mock_bunker_service_get_state(fixture->bunker), ==, MOCK_BUNKER_STATE_STOPPED);
}

static void
test_bunker_stop_clears_connections(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *relays[] = { "wss://relay.example.com", NULL };
    mock_bunker_service_start(fixture->bunker, relays, "npub1test");

    /* Add a connection */
    mock_bunker_service_handle_connect_uri(fixture->bunker, "nostrconnect://client_pk123");
    g_assert_cmpuint(mock_bunker_service_connection_count(fixture->bunker), ==, 1);

    /* Stop should clear connections */
    mock_bunker_service_stop(fixture->bunker);
    g_assert_cmpuint(mock_bunker_service_connection_count(fixture->bunker), ==, 0);
}

static void
test_bunker_state_callback(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    mock_bunker_service_set_state_callback(fixture->bunker, test_state_callback, fixture);

    const gchar *relays[] = { "wss://relay.example.com", NULL };
    mock_bunker_service_start(fixture->bunker, relays, "npub1test");

    /* Should have received state changes: STARTING, RUNNING */
    g_assert_cmpint(fixture->state_change_count, ==, 2);
    g_assert_cmpint(fixture->last_state, ==, MOCK_BUNKER_STATE_RUNNING);
}

/* ===========================================================================
 * URI Generation Tests
 * =========================================================================== */

static void
test_bunker_uri_basic(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *relays[] = { "wss://relay.example.com", NULL };
    mock_bunker_service_start(fixture->bunker, relays, "npub1test");

    gchar *uri = mock_bunker_service_get_bunker_uri(fixture->bunker, NULL);

    g_assert_nonnull(uri);
    g_assert_true(g_str_has_prefix(uri, "bunker://"));
    g_assert_true(strstr(uri, "relay=wss://relay.example.com") != NULL);

    g_free(uri);
}

static void
test_bunker_uri_with_secret(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *relays[] = { "wss://relay.example.com", NULL };
    mock_bunker_service_start(fixture->bunker, relays, "npub1test");

    gchar *uri = mock_bunker_service_get_bunker_uri(fixture->bunker, "mysecret123");

    g_assert_nonnull(uri);
    g_assert_true(strstr(uri, "secret=mysecret123") != NULL);

    g_free(uri);
}

static void
test_bunker_uri_multiple_relays(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *relays[] = { "wss://relay1.example.com", "wss://relay2.example.com", NULL };
    mock_bunker_service_start(fixture->bunker, relays, "npub1test");

    gchar *uri = mock_bunker_service_get_bunker_uri(fixture->bunker, NULL);

    g_assert_nonnull(uri);
    g_assert_true(strstr(uri, "relay=wss://relay1.example.com") != NULL);
    g_assert_true(strstr(uri, "relay=wss://relay2.example.com") != NULL);

    g_free(uri);
}

static void
test_bunker_uri_not_started(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    gchar *uri = mock_bunker_service_get_bunker_uri(fixture->bunker, NULL);

    g_assert_null(uri);
}

/* ===========================================================================
 * Connection Handling Tests
 * =========================================================================== */

static void
test_bunker_handle_connect_uri(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *relays[] = { "wss://relay.example.com", NULL };
    mock_bunker_service_start(fixture->bunker, relays, "npub1test");

    gboolean result = mock_bunker_service_handle_connect_uri(fixture->bunker,
        "nostrconnect://clientpubkey123?relay=wss://relay.example.com");

    g_assert_true(result);
    g_assert_cmpuint(mock_bunker_service_connection_count(fixture->bunker), ==, 1);
}

static void
test_bunker_handle_connect_uri_invalid(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *relays[] = { "wss://relay.example.com", NULL };
    mock_bunker_service_start(fixture->bunker, relays, "npub1test");

    /* Wrong scheme */
    gboolean result = mock_bunker_service_handle_connect_uri(fixture->bunker,
        "bunker://clientpubkey123");

    g_assert_false(result);
    g_assert_cmpuint(mock_bunker_service_connection_count(fixture->bunker), ==, 0);
}

static void
test_bunker_handle_connect_uri_null(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *relays[] = { "wss://relay.example.com", NULL };
    mock_bunker_service_start(fixture->bunker, relays, "npub1test");

    gboolean result = mock_bunker_service_handle_connect_uri(fixture->bunker, NULL);

    g_assert_false(result);
}

static void
test_bunker_connection_callback(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    mock_bunker_service_set_connection_callback(fixture->bunker, test_connection_callback, fixture);

    const gchar *relays[] = { "wss://relay.example.com", NULL };
    mock_bunker_service_start(fixture->bunker, relays, "npub1test");

    mock_bunker_service_handle_connect_uri(fixture->bunker, "nostrconnect://testclient123");

    g_assert_cmpint(fixture->connection_count, ==, 1);
    g_assert_cmpstr(fixture->last_connected_pubkey, ==, "testclient123");
}

static void
test_bunker_disconnect_client(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *relays[] = { "wss://relay.example.com", NULL };
    mock_bunker_service_start(fixture->bunker, relays, "npub1test");

    mock_bunker_service_handle_connect_uri(fixture->bunker, "nostrconnect://client1");
    mock_bunker_service_handle_connect_uri(fixture->bunker, "nostrconnect://client2");

    g_assert_cmpuint(mock_bunker_service_connection_count(fixture->bunker), ==, 2);

    gboolean result = mock_bunker_service_disconnect_client(fixture->bunker, "client1");
    g_assert_true(result);
    g_assert_cmpuint(mock_bunker_service_connection_count(fixture->bunker), ==, 1);
}

static void
test_bunker_disconnect_nonexistent(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *relays[] = { "wss://relay.example.com", NULL };
    mock_bunker_service_start(fixture->bunker, relays, "npub1test");

    gboolean result = mock_bunker_service_disconnect_client(fixture->bunker, "nonexistent");
    g_assert_false(result);
}

/* ===========================================================================
 * Authorization Tests
 * =========================================================================== */

static void
test_bunker_allowed_pubkeys_empty(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    /* Empty allowed list should allow all */
    mock_bunker_service_set_allowed_pubkeys(fixture->bunker, NULL);

    g_assert_true(mock_bunker_check_allowed_pubkey(fixture->bunker, "anypubkey"));
}

static void
test_bunker_allowed_pubkeys_match(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *allowed[] = { "pubkey1", "pubkey2", NULL };
    mock_bunker_service_set_allowed_pubkeys(fixture->bunker, allowed);

    g_assert_true(mock_bunker_check_allowed_pubkey(fixture->bunker, "pubkey1"));
    g_assert_true(mock_bunker_check_allowed_pubkey(fixture->bunker, "pubkey2"));
    g_assert_false(mock_bunker_check_allowed_pubkey(fixture->bunker, "pubkey3"));
}

static void
test_bunker_auto_approve_kinds(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *kinds[] = { "1", "4", "30023", NULL };
    mock_bunker_service_set_auto_approve_kinds(fixture->bunker, kinds);

    g_assert_true(mock_bunker_check_auto_approve_kind(fixture->bunker, 1));
    g_assert_true(mock_bunker_check_auto_approve_kind(fixture->bunker, 4));
    g_assert_true(mock_bunker_check_auto_approve_kind(fixture->bunker, 30023));
    g_assert_false(mock_bunker_check_auto_approve_kind(fixture->bunker, 0));
    g_assert_false(mock_bunker_check_auto_approve_kind(fixture->bunker, 3));
}

static void
test_bunker_auto_approve_empty(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    /* Empty auto-approve list should not auto-approve anything */
    mock_bunker_service_set_auto_approve_kinds(fixture->bunker, NULL);

    g_assert_false(mock_bunker_check_auto_approve_kind(fixture->bunker, 1));
}

/* ===========================================================================
 * Pending Request Tests
 * =========================================================================== */

static void
test_bunker_create_sign_request(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    const gchar *event_json = "{\"kind\":1,\"content\":\"test\"}";

    MockBunkerSignRequest *req = mock_bunker_create_sign_request(
        fixture->bunker, "client123", event_json, 1);

    g_assert_nonnull(req);
    g_assert_nonnull(req->request_id);
    g_assert_cmpstr(req->method, ==, "sign_event");
    g_assert_cmpstr(req->event_json, ==, event_json);
    g_assert_cmpint(req->event_kind, ==, 1);
    g_assert_cmpuint(mock_bunker_pending_request_count(fixture->bunker), ==, 1);
}

static void
test_bunker_authorize_response_approved(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    MockBunkerSignRequest *req = mock_bunker_create_sign_request(
        fixture->bunker, "client123", "{\"kind\":1}", 1);

    gchar *request_id = g_strdup(req->request_id);

    mock_bunker_authorize_response(fixture->bunker, request_id, TRUE);

    g_assert_cmpuint(mock_bunker_pending_request_count(fixture->bunker), ==, 0);

    g_free(request_id);
}

static void
test_bunker_authorize_response_denied(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    MockBunkerSignRequest *req = mock_bunker_create_sign_request(
        fixture->bunker, "client123", "{\"kind\":1}", 1);

    gchar *request_id = g_strdup(req->request_id);

    mock_bunker_authorize_response(fixture->bunker, request_id, FALSE);

    g_assert_cmpuint(mock_bunker_pending_request_count(fixture->bunker), ==, 0);

    g_free(request_id);
}

static void
test_bunker_multiple_pending_requests(BunkerFixture *fixture, gconstpointer data)
{
    (void)data;

    mock_bunker_create_sign_request(fixture->bunker, "client1", "{\"kind\":1}", 1);
    mock_bunker_create_sign_request(fixture->bunker, "client2", "{\"kind\":4}", 4);
    mock_bunker_create_sign_request(fixture->bunker, "client3", "{\"kind\":0}", 0);

    g_assert_cmpuint(mock_bunker_pending_request_count(fixture->bunker), ==, 3);
}

/* ===========================================================================
 * Test Runner
 * =========================================================================== */

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* Service lifecycle tests */
    g_test_add("/signer/bunker/lifecycle/create_starts_stopped", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_create_starts_stopped, bunker_fixture_teardown);
    g_test_add("/signer/bunker/lifecycle/start_basic", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_start_basic, bunker_fixture_teardown);
    g_test_add("/signer/bunker/lifecycle/start_null_identity", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_start_null_identity, bunker_fixture_teardown);
    g_test_add("/signer/bunker/lifecycle/start_no_relays", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_start_no_relays, bunker_fixture_teardown);
    g_test_add("/signer/bunker/lifecycle/start_already_running", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_start_already_running, bunker_fixture_teardown);
    g_test_add("/signer/bunker/lifecycle/stop", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_stop, bunker_fixture_teardown);
    g_test_add("/signer/bunker/lifecycle/stop_clears_connections", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_stop_clears_connections, bunker_fixture_teardown);
    g_test_add("/signer/bunker/lifecycle/state_callback", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_state_callback, bunker_fixture_teardown);

    /* URI generation tests */
    g_test_add("/signer/bunker/uri/basic", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_uri_basic, bunker_fixture_teardown);
    g_test_add("/signer/bunker/uri/with_secret", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_uri_with_secret, bunker_fixture_teardown);
    g_test_add("/signer/bunker/uri/multiple_relays", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_uri_multiple_relays, bunker_fixture_teardown);
    g_test_add("/signer/bunker/uri/not_started", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_uri_not_started, bunker_fixture_teardown);

    /* Connection handling tests */
    g_test_add("/signer/bunker/connect/handle_uri", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_handle_connect_uri, bunker_fixture_teardown);
    g_test_add("/signer/bunker/connect/handle_uri_invalid", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_handle_connect_uri_invalid, bunker_fixture_teardown);
    g_test_add("/signer/bunker/connect/handle_uri_null", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_handle_connect_uri_null, bunker_fixture_teardown);
    g_test_add("/signer/bunker/connect/callback", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_connection_callback, bunker_fixture_teardown);
    g_test_add("/signer/bunker/connect/disconnect", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_disconnect_client, bunker_fixture_teardown);
    g_test_add("/signer/bunker/connect/disconnect_nonexistent", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_disconnect_nonexistent, bunker_fixture_teardown);

    /* Authorization tests */
    g_test_add("/signer/bunker/auth/allowed_pubkeys_empty", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_allowed_pubkeys_empty, bunker_fixture_teardown);
    g_test_add("/signer/bunker/auth/allowed_pubkeys_match", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_allowed_pubkeys_match, bunker_fixture_teardown);
    g_test_add("/signer/bunker/auth/auto_approve_kinds", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_auto_approve_kinds, bunker_fixture_teardown);
    g_test_add("/signer/bunker/auth/auto_approve_empty", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_auto_approve_empty, bunker_fixture_teardown);

    /* Pending request tests */
    g_test_add("/signer/bunker/request/create", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_create_sign_request, bunker_fixture_teardown);
    g_test_add("/signer/bunker/request/authorize_approved", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_authorize_response_approved, bunker_fixture_teardown);
    g_test_add("/signer/bunker/request/authorize_denied", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_authorize_response_denied, bunker_fixture_teardown);
    g_test_add("/signer/bunker/request/multiple_pending", BunkerFixture, NULL,
               bunker_fixture_setup, test_bunker_multiple_pending_requests, bunker_fixture_teardown);

    return g_test_run();
}
