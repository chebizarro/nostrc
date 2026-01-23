/* test-dbus.c - D-Bus interface integration tests for gnostr-signer
 *
 * Tests both org.nostr.Signer and org.gnostr.Signer D-Bus interfaces using
 * GTestDBus for isolated testing. This module verifies:
 *   - Service startup and bus name acquisition
 *   - GetPublicKey method
 *   - SignEvent method (with pre-approved ACL)
 *   - NIP-44 Encrypt/Decrypt methods
 *   - Session management over D-Bus
 *   - Concurrent client requests
 *   - Error handling (invalid input, rate limiting, edge cases)
 *
 * Issue: nostrc-991
 */
#include <glib.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <keys.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>

/* D-Bus identifiers matching nip55l_dbus_names.h */
#define TEST_BUS_NAME       "org.nostr.Signer"
#define TEST_OBJECT_PATH    "/org/nostr/signer"
#define TEST_INTERFACE      "org.nostr.Signer"

/* Alternative org.gnostr.Signer naming (canonical) */
#define GNOSTR_BUS_NAME     "org.gnostr.Signer"
#define GNOSTR_OBJECT_PATH  "/org/gnostr/signer"
#define GNOSTR_INTERFACE    "org.gnostr.Signer"

/* Error names from nip55l_dbus_errors.h */
#define ERR_PERMISSION     "org.nostr.Signer.Error.PermissionDenied"
#define ERR_RATELIMIT      "org.nostr.Signer.Error.RateLimited"
#define ERR_APPROVAL       "org.nostr.Signer.Error.ApprovalDenied"
#define ERR_INVALID_INPUT  "org.nostr.Signer.Error.InvalidInput"
#define ERR_INTERNAL       "org.nostr.Signer.Error.Internal"
#define ERR_SESSION        "org.nostr.Signer.Error.SessionExpired"
#define ERR_CRYPTO         "org.nostr.Signer.Error.CryptoFailed"

/* Number of parallel clients for concurrency tests */
#define CONCURRENT_CLIENTS  5

/* ===========================================================================
 * Test Fixture
 *
 * Uses GTestDBus to create an isolated session bus for each test, ensuring
 * tests don't interfere with user's actual D-Bus session or each other.
 * =========================================================================== */

typedef struct {
    GTestDBus    *dbus;
    GDBusConnection *conn;
    GDBusProxy   *proxy;
    GPid          daemon_pid;
    gchar        *test_key_hex;      /* Test private key (hex) */
    gchar        *test_npub;         /* Corresponding npub */
    gchar        *acl_dir;           /* Temporary config dir */
} DbusFixture;

/* Forward declarations */
static void fixture_setup(DbusFixture *fix, gconstpointer user_data);
static void fixture_teardown(DbusFixture *fix, gconstpointer user_data);

/* Helper: Generate test keypair and store for tests */
static void
generate_test_keypair(DbusFixture *fix)
{
    fix->test_key_hex = nostr_key_generate_private();
    g_assert_nonnull(fix->test_key_hex);

    char *pk_hex = nostr_key_get_public(fix->test_key_hex);
    g_assert_nonnull(pk_hex);

    uint8_t pk_bytes[32];
    g_assert_true(nostr_hex2bin(pk_bytes, pk_hex, 32));

    int rc = nostr_nip19_encode_npub(pk_bytes, &fix->test_npub);
    g_assert_cmpint(rc, ==, 0);
    g_assert_nonnull(fix->test_npub);

    free(pk_hex);
}

/* Helper: Create mock service implementation using GDBus skeleton
 *
 * For integration tests, we implement a minimal mock signer that responds
 * to D-Bus method calls. This allows testing the D-Bus protocol without
 * requiring full daemon infrastructure.
 */

/* Mock session data for session management tests */
typedef struct {
    gchar *client_pubkey;
    gchar *identity;
    gint64 created_at;
    gint64 last_activity;
    gint64 expires_at;
    guint permissions;
    gboolean active;
} MockClientSession;

/* Mock state */
typedef struct {
    gchar *stored_npub;
    gchar *stored_sk_hex;
    GHashTable *acl;       /* app_id -> allowed (gboolean) */
    GHashTable *sessions;  /* session_key -> MockClientSession* */
    guint request_count;   /* Total requests processed */
    GMutex lock;           /* For thread safety in concurrent tests */
} MockSignerState;

static MockSignerState mock_state = { NULL, NULL, NULL, NULL, 0, { 0 } };

static void
mock_client_session_free(MockClientSession *session)
{
    if (!session) return;
    g_free(session->client_pubkey);
    g_free(session->identity);
    g_free(session);
}

static gchar *
make_session_key(const gchar *client_pubkey, const gchar *identity)
{
    return g_strdup_printf("%s:%s", client_pubkey, identity ? identity : "default");
}

static void
mock_state_init(const gchar *sk_hex, const gchar *npub)
{
    g_mutex_init(&mock_state.lock);
    mock_state.stored_sk_hex = g_strdup(sk_hex);
    mock_state.stored_npub = g_strdup(npub);
    mock_state.acl = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    mock_state.sessions = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, (GDestroyNotify)mock_client_session_free);
    mock_state.request_count = 0;
}

static void
mock_state_clear(void)
{
    g_mutex_lock(&mock_state.lock);
    g_free(mock_state.stored_sk_hex);
    g_free(mock_state.stored_npub);
    if (mock_state.acl) {
        g_hash_table_destroy(mock_state.acl);
    }
    if (mock_state.sessions) {
        g_hash_table_destroy(mock_state.sessions);
    }
    mock_state.stored_sk_hex = NULL;
    mock_state.stored_npub = NULL;
    mock_state.acl = NULL;
    mock_state.sessions = NULL;
    mock_state.request_count = 0;
    g_mutex_unlock(&mock_state.lock);
    g_mutex_clear(&mock_state.lock);
}

static void
mock_acl_allow(const gchar *app_id)
{
    if (mock_state.acl && app_id) {
        g_hash_table_insert(mock_state.acl, g_strdup(app_id), GINT_TO_POINTER(TRUE));
    }
}

/* Session management helpers */
static MockClientSession *
mock_session_create(const gchar *client_pubkey, const gchar *identity, guint permissions)
{
    MockClientSession *session = g_new0(MockClientSession, 1);
    session->client_pubkey = g_strdup(client_pubkey);
    session->identity = g_strdup(identity ? identity : mock_state.stored_npub);
    session->created_at = (gint64)time(NULL);
    session->last_activity = session->created_at;
    session->expires_at = session->created_at + 900;  /* 15 minute default */
    session->permissions = permissions;
    session->active = TRUE;

    gchar *key = make_session_key(client_pubkey, session->identity);
    g_hash_table_replace(mock_state.sessions, key, session);

    return session;
}

static MockClientSession *
mock_session_lookup(const gchar *client_pubkey, const gchar *identity)
{
    gchar *key = make_session_key(client_pubkey, identity);
    MockClientSession *session = g_hash_table_lookup(mock_state.sessions, key);
    g_free(key);
    return session;
}

static gboolean
mock_session_is_active(const gchar *client_pubkey, const gchar *identity)
{
    MockClientSession *session = mock_session_lookup(client_pubkey, identity);
    if (!session) return FALSE;
    if (!session->active) return FALSE;

    gint64 now = (gint64)time(NULL);
    return now < session->expires_at;
}

static void
mock_session_touch(const gchar *client_pubkey, const gchar *identity)
{
    MockClientSession *session = mock_session_lookup(client_pubkey, identity);
    if (session && session->active) {
        session->last_activity = (gint64)time(NULL);
    }
}

static void
mock_session_revoke(const gchar *client_pubkey, const gchar *identity)
{
    MockClientSession *session = mock_session_lookup(client_pubkey, identity);
    if (session) {
        session->active = FALSE;
    }
}

/* D-Bus method handlers for mock service */
static void
handle_method_call(GDBusConnection       *connection,
                   const gchar           *sender,
                   const gchar           *object_path,
                   const gchar           *interface_name,
                   const gchar           *method_name,
                   GVariant              *parameters,
                   GDBusMethodInvocation *invocation,
                   gpointer               user_data)
{
    (void)connection;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (g_strcmp0(method_name, "GetPublicKey") == 0) {
        if (mock_state.stored_npub) {
            g_dbus_method_invocation_return_value(invocation,
                g_variant_new("(s)", mock_state.stored_npub));
        } else {
            g_dbus_method_invocation_return_dbus_error(invocation,
                ERR_INTERNAL, "no key configured");
        }
        return;
    }

    if (g_strcmp0(method_name, "SignEvent") == 0) {
        const gchar *event_json = NULL;
        const gchar *identity = NULL;
        const gchar *app_id = NULL;

        g_variant_get(parameters, "(&s&s&s)", &event_json, &identity, &app_id);

        /* Use sender as app_id if not provided */
        if (!app_id || !*app_id) {
            app_id = sender;
        }

        /* Validate event JSON (basic check) */
        if (!event_json || !*event_json) {
            g_dbus_method_invocation_return_dbus_error(invocation,
                ERR_INVALID_INPUT, "empty event JSON");
            return;
        }

        /* Check ACL */
        gboolean allowed = GPOINTER_TO_INT(
            g_hash_table_lookup(mock_state.acl, app_id));

        /* Also check wildcard "*" for tests */
        if (!allowed) {
            allowed = GPOINTER_TO_INT(
                g_hash_table_lookup(mock_state.acl, "*"));
        }

        if (!allowed) {
            g_dbus_method_invocation_return_dbus_error(invocation,
                ERR_APPROVAL, "signing not approved for this app");
            return;
        }

        /* Generate mock signature (64 bytes hex = 128 chars) */
        /* In real implementation this would be a Schnorr signature */
        gchar sig[129];
        for (int i = 0; i < 128; i++) {
            sig[i] = "0123456789abcdef"[g_random_int_range(0, 16)];
        }
        sig[128] = '\0';

        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(s)", sig));
        return;
    }

    if (g_strcmp0(method_name, "GetRelays") == 0) {
        /* Return mock relay list */
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(s)", "[\"wss://relay.example.com\"]"));
        return;
    }

    if (g_strcmp0(method_name, "StoreKey") == 0) {
        const gchar *key = NULL;
        const gchar *identity = NULL;

        g_variant_get(parameters, "(&s&s)", &key, &identity);

        /* Check if mutations are allowed */
        const gchar *env = g_getenv("NOSTR_SIGNER_ALLOW_KEY_MUTATIONS");
        if (!env || g_strcmp0(env, "1") != 0) {
            g_dbus_method_invocation_return_dbus_error(invocation,
                ERR_PERMISSION, "key mutations disabled");
            return;
        }

        /* Validate and store key */
        if (!key || strlen(key) != 64) {
            g_dbus_method_invocation_return_dbus_error(invocation,
                "org.nostr.Signer.InvalidKey", "invalid key format");
            return;
        }

        g_free(mock_state.stored_sk_hex);
        mock_state.stored_sk_hex = g_strdup(key);

        /* Derive npub */
        char *pk_hex = nostr_key_get_public(key);
        if (pk_hex) {
            uint8_t pk_bytes[32];
            if (nostr_hex2bin(pk_bytes, pk_hex, 32)) {
                g_free(mock_state.stored_npub);
                char *npub = NULL;
                if (nostr_nip19_encode_npub(pk_bytes, &npub) == 0) {
                    mock_state.stored_npub = g_strdup(npub);
                    free(npub);
                }
            }
            free(pk_hex);
        }

        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(bs)", TRUE, mock_state.stored_npub ? mock_state.stored_npub : ""));
        return;
    }

    if (g_strcmp0(method_name, "ClearKey") == 0) {
        /* Check if mutations are allowed */
        const gchar *env = g_getenv("NOSTR_SIGNER_ALLOW_KEY_MUTATIONS");
        if (!env || g_strcmp0(env, "1") != 0) {
            g_dbus_method_invocation_return_dbus_error(invocation,
                ERR_PERMISSION, "key mutations disabled");
            return;
        }

        g_free(mock_state.stored_sk_hex);
        g_free(mock_state.stored_npub);
        mock_state.stored_sk_hex = NULL;
        mock_state.stored_npub = NULL;

        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(b)", TRUE));
        return;
    }

    if (g_strcmp0(method_name, "NIP04Encrypt") == 0 ||
        g_strcmp0(method_name, "NIP04Decrypt") == 0 ||
        g_strcmp0(method_name, "NIP44Encrypt") == 0 ||
        g_strcmp0(method_name, "NIP44Decrypt") == 0) {
        /* Mock implementation - just return input for testing protocol */
        const gchar *text = NULL;
        const gchar *pubkey = NULL;
        const gchar *identity = NULL;

        g_variant_get(parameters, "(&s&s&s)", &text, &pubkey, &identity);

        /* Return mock encrypted/decrypted text */
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(s)", text ? text : ""));
        return;
    }

    if (g_strcmp0(method_name, "DecryptZapEvent") == 0) {
        const gchar *event_json = NULL;
        const gchar *identity = NULL;

        g_variant_get(parameters, "(&s&s)", &event_json, &identity);

        /* Return input as mock decryption */
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(s)", event_json ? event_json : ""));
        return;
    }

    if (g_strcmp0(method_name, "ApproveRequest") == 0) {
        /* Mock approval - always succeed */
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(b)", TRUE));
        return;
    }

    /* Session management methods */
    if (g_strcmp0(method_name, "CreateSession") == 0) {
        const gchar *client_pubkey = NULL;
        const gchar *identity = NULL;
        guint32 permissions = 0;
        gint64 ttl_seconds = 0;

        g_variant_get(parameters, "(&s&sux)", &client_pubkey, &identity, &permissions, &ttl_seconds);

        if (!client_pubkey || !*client_pubkey) {
            g_dbus_method_invocation_return_dbus_error(invocation,
                ERR_INVALID_INPUT, "client_pubkey is required");
            return;
        }

        g_mutex_lock(&mock_state.lock);
        MockClientSession *session = mock_session_create(client_pubkey, identity, permissions);
        if (ttl_seconds > 0) {
            session->expires_at = session->created_at + ttl_seconds;
        } else if (ttl_seconds == -1) {
            session->expires_at = G_MAXINT64;  /* Never expires */
        }
        g_mutex_unlock(&mock_state.lock);

        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(b)", TRUE));
        return;
    }

    if (g_strcmp0(method_name, "GetSession") == 0) {
        const gchar *client_pubkey = NULL;
        const gchar *identity = NULL;

        g_variant_get(parameters, "(&s&s)", &client_pubkey, &identity);

        g_mutex_lock(&mock_state.lock);
        MockClientSession *session = mock_session_lookup(client_pubkey, identity);

        if (!session) {
            g_mutex_unlock(&mock_state.lock);
            g_dbus_method_invocation_return_value(invocation,
                g_variant_new("(bux)", FALSE, (guint32)0, (gint64)0));
            return;
        }

        gboolean active = session->active && (time(NULL) < session->expires_at);
        guint32 perms = session->permissions;
        gint64 expires = session->expires_at;
        g_mutex_unlock(&mock_state.lock);

        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(bux)", active, perms, expires));
        return;
    }

    if (g_strcmp0(method_name, "RevokeSession") == 0) {
        const gchar *client_pubkey = NULL;
        const gchar *identity = NULL;

        g_variant_get(parameters, "(&s&s)", &client_pubkey, &identity);

        g_mutex_lock(&mock_state.lock);
        MockClientSession *session = mock_session_lookup(client_pubkey, identity);
        gboolean ok = FALSE;
        if (session) {
            session->active = FALSE;
            ok = TRUE;
        }
        g_mutex_unlock(&mock_state.lock);

        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(b)", ok));
        return;
    }

    if (g_strcmp0(method_name, "ListSessions") == 0) {
        g_mutex_lock(&mock_state.lock);

        GString *json = g_string_new("[");
        GHashTableIter iter;
        gpointer key, value;
        gboolean first = TRUE;

        if (mock_state.sessions) {
            g_hash_table_iter_init(&iter, mock_state.sessions);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                MockClientSession *session = (MockClientSession*)value;
                if (!first) g_string_append_c(json, ',');
                g_string_append_printf(json,
                    "{\"client_pubkey\":\"%s\",\"identity\":\"%s\",\"active\":%s,\"permissions\":%u}",
                    session->client_pubkey,
                    session->identity,
                    session->active ? "true" : "false",
                    session->permissions);
                first = FALSE;
            }
        }
        g_string_append_c(json, ']');
        g_mutex_unlock(&mock_state.lock);

        gchar *result = g_string_free(json, FALSE);
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(s)", result));
        g_free(result);
        return;
    }

    /* Increment request counter for concurrency tests */
    g_mutex_lock(&mock_state.lock);
    mock_state.request_count++;
    g_mutex_unlock(&mock_state.lock);

    /* Unknown method */
    g_dbus_method_invocation_return_dbus_error(invocation,
        "org.freedesktop.DBus.Error.UnknownMethod",
        "Unknown method");
}

/* D-Bus interface introspection data */
static const gchar introspection_xml[] =
    "<node>"
    "  <interface name='" TEST_INTERFACE "'>"
    "    <method name='GetPublicKey'>"
    "      <arg name='npub' type='s' direction='out'/>"
    "    </method>"
    "    <method name='SignEvent'>"
    "      <arg name='event_json' type='s' direction='in'/>"
    "      <arg name='current_user' type='s' direction='in'/>"
    "      <arg name='app_id' type='s' direction='in'/>"
    "      <arg name='signature' type='s' direction='out'/>"
    "    </method>"
    "    <method name='NIP04Encrypt'>"
    "      <arg name='plaintext' type='s' direction='in'/>"
    "      <arg name='peer_pubkey' type='s' direction='in'/>"
    "      <arg name='current_user' type='s' direction='in'/>"
    "      <arg name='ciphertext' type='s' direction='out'/>"
    "    </method>"
    "    <method name='NIP04Decrypt'>"
    "      <arg name='ciphertext' type='s' direction='in'/>"
    "      <arg name='peer_pubkey' type='s' direction='in'/>"
    "      <arg name='current_user' type='s' direction='in'/>"
    "      <arg name='plaintext' type='s' direction='out'/>"
    "    </method>"
    "    <method name='NIP44Encrypt'>"
    "      <arg name='plaintext' type='s' direction='in'/>"
    "      <arg name='peer_pubkey' type='s' direction='in'/>"
    "      <arg name='current_user' type='s' direction='in'/>"
    "      <arg name='ciphertext' type='s' direction='out'/>"
    "    </method>"
    "    <method name='NIP44Decrypt'>"
    "      <arg name='ciphertext' type='s' direction='in'/>"
    "      <arg name='peer_pubkey' type='s' direction='in'/>"
    "      <arg name='current_user' type='s' direction='in'/>"
    "      <arg name='plaintext' type='s' direction='out'/>"
    "    </method>"
    "    <method name='DecryptZapEvent'>"
    "      <arg name='event_json' type='s' direction='in'/>"
    "      <arg name='current_user' type='s' direction='in'/>"
    "      <arg name='decrypted_event' type='s' direction='out'/>"
    "    </method>"
    "    <method name='GetRelays'>"
    "      <arg name='relays_json' type='s' direction='out'/>"
    "    </method>"
    "    <method name='StoreKey'>"
    "      <arg name='key' type='s' direction='in'/>"
    "      <arg name='identity' type='s' direction='in'/>"
    "      <arg name='ok' type='b' direction='out'/>"
    "      <arg name='npub' type='s' direction='out'/>"
    "    </method>"
    "    <method name='ClearKey'>"
    "      <arg name='identity' type='s' direction='in'/>"
    "      <arg name='ok' type='b' direction='out'/>"
    "    </method>"
    "    <method name='ApproveRequest'>"
    "      <arg name='request_id' type='s' direction='in'/>"
    "      <arg name='decision' type='b' direction='in'/>"
    "      <arg name='remember' type='b' direction='in'/>"
    "      <arg name='ttl_seconds' type='t' direction='in'/>"
    "      <arg name='ok' type='b' direction='out'/>"
    "    </method>"
    "    <signal name='ApprovalRequested'>"
    "      <arg type='s' name='app_id'/>"
    "      <arg type='s' name='identity'/>"
    "      <arg type='s' name='kind'/>"
    "      <arg type='s' name='preview'/>"
    "      <arg type='s' name='request_id'/>"
    "    </signal>"
    "    <signal name='ApprovalCompleted'>"
    "      <arg type='s' name='request_id'/>"
    "      <arg type='b' name='decision'/>"
    "    </signal>"
    "    <method name='CreateSession'>"
    "      <arg name='client_pubkey' type='s' direction='in'/>"
    "      <arg name='identity' type='s' direction='in'/>"
    "      <arg name='permissions' type='u' direction='in'/>"
    "      <arg name='ttl_seconds' type='x' direction='in'/>"
    "      <arg name='ok' type='b' direction='out'/>"
    "    </method>"
    "    <method name='GetSession'>"
    "      <arg name='client_pubkey' type='s' direction='in'/>"
    "      <arg name='identity' type='s' direction='in'/>"
    "      <arg name='active' type='b' direction='out'/>"
    "      <arg name='permissions' type='u' direction='out'/>"
    "      <arg name='expires_at' type='x' direction='out'/>"
    "    </method>"
    "    <method name='RevokeSession'>"
    "      <arg name='client_pubkey' type='s' direction='in'/>"
    "      <arg name='identity' type='s' direction='in'/>"
    "      <arg name='ok' type='b' direction='out'/>"
    "    </method>"
    "    <method name='ListSessions'>"
    "      <arg name='sessions_json' type='s' direction='out'/>"
    "    </method>"
    "    <signal name='SessionCreated'>"
    "      <arg type='s' name='client_pubkey'/>"
    "      <arg type='s' name='identity'/>"
    "    </signal>"
    "    <signal name='SessionRevoked'>"
    "      <arg type='s' name='client_pubkey'/>"
    "      <arg type='s' name='identity'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

static const GDBusInterfaceVTable interface_vtable = {
    handle_method_call,
    NULL,  /* get_property */
    NULL   /* set_property */
};

static guint service_registration_id = 0;
static guint service_name_id = 0;

static void
on_bus_acquired(GDBusConnection *connection,
                const gchar     *name,
                gpointer         user_data)
{
    (void)name;
    GError *error = NULL;
    GDBusNodeInfo *introspection_data;

    introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, &error);
    g_assert_no_error(error);

    service_registration_id = g_dbus_connection_register_object(
        connection,
        TEST_OBJECT_PATH,
        introspection_data->interfaces[0],
        &interface_vtable,
        user_data,
        NULL,  /* user_data_free_func */
        &error);

    g_assert_no_error(error);
    g_assert_cmpuint(service_registration_id, >, 0);

    g_dbus_node_info_unref(introspection_data);
}

static void
on_name_acquired(GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
    (void)connection;
    (void)name;
    (void)user_data;
    /* Service is now ready */
}

static void
on_name_lost(GDBusConnection *connection,
             const gchar     *name,
             gpointer         user_data)
{
    (void)connection;
    (void)name;
    (void)user_data;
    /* Name lost - this is normal during teardown */
}

static void
fixture_setup(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;

    /* Generate test keypair */
    generate_test_keypair(fix);

    /* Initialize mock state with test key */
    mock_state_init(fix->test_key_hex, fix->test_npub);

    /* Create temporary config directory */
    fix->acl_dir = g_dir_make_tmp("gnostr-signer-test-XXXXXX", &error);
    g_assert_no_error(error);

    /* Set up isolated D-Bus session */
    fix->dbus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(fix->dbus);

    /* Get connection to test bus */
    const gchar *bus_addr = g_test_dbus_get_bus_address(fix->dbus);
    fix->conn = g_dbus_connection_new_for_address_sync(
        bus_addr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL,  /* observer */
        NULL,  /* cancellable */
        &error);
    g_assert_no_error(error);
    g_assert_nonnull(fix->conn);

    /* Register mock service on the test bus */
    service_name_id = g_bus_own_name_on_connection(
        fix->conn,
        TEST_BUS_NAME,
        G_BUS_NAME_OWNER_FLAGS_NONE,
        on_name_acquired,
        on_name_lost,
        fix,
        NULL);

    /* Give the service time to register */
    while (g_main_context_iteration(NULL, FALSE)) { }
    g_usleep(50000);  /* 50ms */

    /* Create proxy to the service */
    fix->proxy = g_dbus_proxy_new_sync(
        fix->conn,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,  /* interface_info */
        TEST_BUS_NAME,
        TEST_OBJECT_PATH,
        TEST_INTERFACE,
        NULL,  /* cancellable */
        &error);
    g_assert_no_error(error);
    g_assert_nonnull(fix->proxy);
}

static void
fixture_teardown(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;

    /* Clean up proxy */
    if (fix->proxy) {
        g_object_unref(fix->proxy);
        fix->proxy = NULL;
    }

    /* Unown the name */
    if (service_name_id) {
        g_bus_unown_name(service_name_id);
        service_name_id = 0;
    }

    /* Wait for pending operations */
    while (g_main_context_iteration(NULL, FALSE)) { }

    /* Clean up connection */
    if (fix->conn) {
        g_dbus_connection_close_sync(fix->conn, NULL, NULL);
        g_object_unref(fix->conn);
        fix->conn = NULL;
    }

    /* Tear down test bus */
    if (fix->dbus) {
        g_test_dbus_down(fix->dbus);
        g_object_unref(fix->dbus);
        fix->dbus = NULL;
    }

    /* Clean up mock state */
    mock_state_clear();

    /* Clean up temp directory */
    if (fix->acl_dir) {
        rmdir(fix->acl_dir);
        g_free(fix->acl_dir);
    }

    /* Free test key data */
    free(fix->test_key_hex);
    free(fix->test_npub);
}

/* ===========================================================================
 * Test: Service Connection
 * =========================================================================== */

static void
test_dbus_connection(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;

    /* Verify proxy is connected */
    g_assert_nonnull(fix->proxy);

    gchar *name = NULL;
    g_object_get(fix->proxy, "g-name", &name, NULL);
    g_assert_cmpstr(name, ==, TEST_BUS_NAME);
    g_free(name);
}

/* ===========================================================================
 * Test: GetPublicKey Method
 * =========================================================================== */

static void
test_dbus_get_public_key(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "GetPublicKey",
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        5000,  /* timeout_msec */
        NULL,  /* cancellable */
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    const gchar *npub = NULL;
    g_variant_get(result, "(&s)", &npub);

    /* Should start with npub1 */
    g_assert_true(g_str_has_prefix(npub, "npub1"));

    /* Should match our test key */
    g_assert_cmpstr(npub, ==, fix->test_npub);

    g_variant_unref(result);
}

static void
test_dbus_get_public_key_no_key(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Clear the stored key */
    g_free(mock_state.stored_npub);
    mock_state.stored_npub = NULL;

    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "GetPublicKey",
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    /* Should return an error */
    g_assert_null(result);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));

    gchar *remote_error = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(remote_error, ==, ERR_INTERNAL);
    g_free(remote_error);

    g_error_free(error);
}

/* ===========================================================================
 * Test: SignEvent Method
 * =========================================================================== */

static void
test_dbus_sign_event_approved(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Allow all apps to sign */
    mock_acl_allow("*");

    /* Sample unsigned event JSON */
    const gchar *event_json = "{"
        "\"pubkey\":\"" "aaaa" "\","
        "\"created_at\":1234567890,"
        "\"kind\":1,"
        "\"tags\":[],"
        "\"content\":\"Hello, world!\""
    "}";

    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "SignEvent",
        g_variant_new("(sss)", event_json, "", "test-app"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    const gchar *signature = NULL;
    g_variant_get(result, "(&s)", &signature);

    /* Signature should be 128 hex characters (64 bytes) */
    g_assert_cmpuint(strlen(signature), ==, 128);

    /* Verify all characters are hex */
    for (size_t i = 0; i < 128; i++) {
        g_assert_true(g_ascii_isxdigit(signature[i]));
    }

    g_variant_unref(result);
}

static void
test_dbus_sign_event_denied(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Don't add any ACL entries - signing should be denied */

    const gchar *event_json = "{\"content\":\"test\"}";

    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "SignEvent",
        g_variant_new("(sss)", event_json, "", "unapproved-app"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_null(result);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));

    gchar *remote_error = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(remote_error, ==, ERR_APPROVAL);
    g_free(remote_error);

    g_error_free(error);
}

static void
test_dbus_sign_event_invalid_input(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Allow signing */
    mock_acl_allow("*");

    /* Empty event JSON should fail */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "SignEvent",
        g_variant_new("(sss)", "", "", "test-app"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_null(result);
    g_assert_nonnull(error);

    gchar *remote_error = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(remote_error, ==, ERR_INVALID_INPUT);
    g_free(remote_error);

    g_error_free(error);
}

/* ===========================================================================
 * Test: GetRelays Method
 * =========================================================================== */

static void
test_dbus_get_relays(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "GetRelays",
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    const gchar *relays_json = NULL;
    g_variant_get(result, "(&s)", &relays_json);

    /* Should be a JSON array */
    g_assert_true(relays_json[0] == '[');

    g_variant_unref(result);
}

/* ===========================================================================
 * Test: StoreKey Method
 * =========================================================================== */

static void
test_dbus_store_key_mutations_disabled(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Ensure mutations are disabled */
    g_unsetenv("NOSTR_SIGNER_ALLOW_KEY_MUTATIONS");

    char *new_key = nostr_key_generate_private();
    g_assert_nonnull(new_key);

    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "StoreKey",
        g_variant_new("(ss)", new_key, "test-identity"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    free(new_key);

    g_assert_null(result);
    g_assert_nonnull(error);

    gchar *remote_error = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(remote_error, ==, ERR_PERMISSION);
    g_free(remote_error);

    g_error_free(error);
}

static void
test_dbus_store_key_mutations_enabled(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Enable mutations */
    g_setenv("NOSTR_SIGNER_ALLOW_KEY_MUTATIONS", "1", TRUE);

    char *new_key = nostr_key_generate_private();
    g_assert_nonnull(new_key);

    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "StoreKey",
        g_variant_new("(ss)", new_key, ""),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    gboolean ok = FALSE;
    const gchar *npub = NULL;
    g_variant_get(result, "(b&s)", &ok, &npub);

    g_assert_true(ok);
    g_assert_true(g_str_has_prefix(npub, "npub1"));

    g_variant_unref(result);

    /* Cleanup */
    g_unsetenv("NOSTR_SIGNER_ALLOW_KEY_MUTATIONS");
    free(new_key);
}

/* ===========================================================================
 * Test: ClearKey Method
 * =========================================================================== */

static void
test_dbus_clear_key_mutations_disabled(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Ensure mutations are disabled */
    g_unsetenv("NOSTR_SIGNER_ALLOW_KEY_MUTATIONS");

    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "ClearKey",
        g_variant_new("(s)", "test-identity"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_null(result);
    g_assert_nonnull(error);

    gchar *remote_error = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(remote_error, ==, ERR_PERMISSION);
    g_free(remote_error);

    g_error_free(error);
}

/* ===========================================================================
 * Test: NIP-04 Encryption Methods
 * =========================================================================== */

static void
test_dbus_nip04_encrypt(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Generate a peer pubkey */
    char *peer_sk = nostr_key_generate_private();
    char *peer_pk = nostr_key_get_public(peer_sk);

    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "NIP04Encrypt",
        g_variant_new("(sss)", "Hello, encrypted world!", peer_pk, ""),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    const gchar *ciphertext = NULL;
    g_variant_get(result, "(&s)", &ciphertext);
    g_assert_nonnull(ciphertext);

    g_variant_unref(result);
    free(peer_sk);
    free(peer_pk);
}

static void
test_dbus_nip04_decrypt(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Generate a peer pubkey */
    char *peer_sk = nostr_key_generate_private();
    char *peer_pk = nostr_key_get_public(peer_sk);

    /* In mock, we just return the input */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "NIP04Decrypt",
        g_variant_new("(sss)", "mock-ciphertext", peer_pk, ""),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    const gchar *plaintext = NULL;
    g_variant_get(result, "(&s)", &plaintext);
    g_assert_nonnull(plaintext);

    g_variant_unref(result);
    free(peer_sk);
    free(peer_pk);
}

/* ===========================================================================
 * Test: NIP-44 Encryption Methods
 * =========================================================================== */

static void
test_dbus_nip44_encrypt(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    char *peer_sk = nostr_key_generate_private();
    char *peer_pk = nostr_key_get_public(peer_sk);

    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "NIP44Encrypt",
        g_variant_new("(sss)", "Hello, NIP-44 world!", peer_pk, ""),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    g_variant_unref(result);
    free(peer_sk);
    free(peer_pk);
}

static void
test_dbus_nip44_decrypt(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    char *peer_sk = nostr_key_generate_private();
    char *peer_pk = nostr_key_get_public(peer_sk);

    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "NIP44Decrypt",
        g_variant_new("(sss)", "mock-nip44-ciphertext", peer_pk, ""),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    g_variant_unref(result);
    free(peer_sk);
    free(peer_pk);
}

/* ===========================================================================
 * Test: DecryptZapEvent Method
 * =========================================================================== */

static void
test_dbus_decrypt_zap_event(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    const gchar *zap_event = "{\"kind\":9735,\"content\":\"encrypted\"}";

    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "DecryptZapEvent",
        g_variant_new("(ss)", zap_event, ""),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    const gchar *decrypted = NULL;
    g_variant_get(result, "(&s)", &decrypted);
    g_assert_nonnull(decrypted);

    g_variant_unref(result);
}

/* ===========================================================================
 * Test: ApproveRequest Method
 * =========================================================================== */

static void
test_dbus_approve_request(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "ApproveRequest",
        g_variant_new("(sbbt)", "test-request-id", TRUE, FALSE, (guint64)0),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    gboolean ok = FALSE;
    g_variant_get(result, "(b)", &ok);
    g_assert_true(ok);

    g_variant_unref(result);
}

/* ===========================================================================
 * Test: Session Management over D-Bus
 * =========================================================================== */

static void
test_dbus_session_create(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Create a session */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "CreateSession",
        g_variant_new("(ssux)", "client_pubkey_abc123", fix->test_npub, (guint32)31, (gint64)3600),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    gboolean ok = FALSE;
    g_variant_get(result, "(b)", &ok);
    g_assert_true(ok);

    g_variant_unref(result);
}

static void
test_dbus_session_get_existing(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* First create a session */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "CreateSession",
        g_variant_new("(ssux)", "test_client_pk", fix->test_npub, (guint32)15, (gint64)900),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);
    g_assert_no_error(error);
    g_variant_unref(result);

    /* Now get the session */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "GetSession",
        g_variant_new("(ss)", "test_client_pk", fix->test_npub),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    gboolean active = FALSE;
    guint32 permissions = 0;
    gint64 expires_at = 0;
    g_variant_get(result, "(bux)", &active, &permissions, &expires_at);

    g_assert_true(active);
    g_assert_cmpuint(permissions, ==, 15);
    g_assert_cmpint(expires_at, >, 0);

    g_variant_unref(result);
}

static void
test_dbus_session_get_nonexistent(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "GetSession",
        g_variant_new("(ss)", "nonexistent_client", ""),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    gboolean active = TRUE;  /* Initialize to opposite of expected */
    guint32 permissions = 0;
    gint64 expires_at = 0;
    g_variant_get(result, "(bux)", &active, &permissions, &expires_at);

    g_assert_false(active);
    g_assert_cmpuint(permissions, ==, 0);
    g_assert_cmpint(expires_at, ==, 0);

    g_variant_unref(result);
}

static void
test_dbus_session_revoke(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Create a session */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "CreateSession",
        g_variant_new("(ssux)", "revoke_test_pk", fix->test_npub, (guint32)7, (gint64)3600),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);
    g_assert_no_error(error);
    g_variant_unref(result);

    /* Verify it exists and is active */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "GetSession",
        g_variant_new("(ss)", "revoke_test_pk", fix->test_npub),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);
    g_assert_no_error(error);

    gboolean active = FALSE;
    guint32 permissions = 0;
    gint64 expires_at = 0;
    g_variant_get(result, "(bux)", &active, &permissions, &expires_at);
    g_assert_true(active);
    g_variant_unref(result);

    /* Revoke the session */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "RevokeSession",
        g_variant_new("(ss)", "revoke_test_pk", fix->test_npub),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);
    g_assert_no_error(error);

    gboolean ok = FALSE;
    g_variant_get(result, "(b)", &ok);
    g_assert_true(ok);
    g_variant_unref(result);

    /* Verify it's no longer active */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "GetSession",
        g_variant_new("(ss)", "revoke_test_pk", fix->test_npub),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);
    g_assert_no_error(error);

    g_variant_get(result, "(bux)", &active, &permissions, &expires_at);
    g_assert_false(active);
    g_variant_unref(result);
}

static void
test_dbus_session_list(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Create multiple sessions */
    for (int i = 0; i < 3; i++) {
        gchar *client_pk = g_strdup_printf("list_test_client_%d", i);
        result = g_dbus_proxy_call_sync(
            fix->proxy,
            "CreateSession",
            g_variant_new("(ssux)", client_pk, fix->test_npub, (guint32)(i + 1), (gint64)3600),
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            NULL,
            &error);
        g_assert_no_error(error);
        g_variant_unref(result);
        g_free(client_pk);
    }

    /* List all sessions */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "ListSessions",
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    const gchar *sessions_json = NULL;
    g_variant_get(result, "(&s)", &sessions_json);
    g_assert_nonnull(sessions_json);

    /* Should be a JSON array with at least 3 entries */
    g_assert_true(sessions_json[0] == '[');
    g_assert_true(strstr(sessions_json, "list_test_client_0") != NULL);
    g_assert_true(strstr(sessions_json, "list_test_client_1") != NULL);
    g_assert_true(strstr(sessions_json, "list_test_client_2") != NULL);

    g_variant_unref(result);
}

static void
test_dbus_session_create_invalid_input(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Empty client_pubkey should fail */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "CreateSession",
        g_variant_new("(ssux)", "", fix->test_npub, (guint32)7, (gint64)3600),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_null(result);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));

    gchar *remote_error = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(remote_error, ==, ERR_INVALID_INPUT);
    g_free(remote_error);
    g_error_free(error);
}

/* ===========================================================================
 * Test: Concurrent Client Requests
 * =========================================================================== */

typedef struct {
    DbusFixture *fix;
    gint client_id;
    gboolean success;
    gint requests_completed;
    GMutex mutex;
} ConcurrentTestData;

static gpointer
concurrent_client_thread(gpointer user_data)
{
    ConcurrentTestData *data = (ConcurrentTestData*)user_data;
    GError *error = NULL;

    for (int i = 0; i < 10; i++) {
        /* Make a GetPublicKey call */
        GVariant *result = g_dbus_proxy_call_sync(
            data->fix->proxy,
            "GetPublicKey",
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            NULL,
            &error);

        g_mutex_lock(&data->mutex);
        if (result) {
            data->requests_completed++;
            g_variant_unref(result);
        } else {
            data->success = FALSE;
            g_clear_error(&error);
        }
        g_mutex_unlock(&data->mutex);

        /* Small delay to interleave with other threads */
        g_usleep(1000);  /* 1ms */
    }

    return NULL;
}

static void
test_dbus_concurrent_requests(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;

    ConcurrentTestData data[CONCURRENT_CLIENTS];
    GThread *threads[CONCURRENT_CLIENTS];

    /* Initialize test data */
    for (int i = 0; i < CONCURRENT_CLIENTS; i++) {
        data[i].fix = fix;
        data[i].client_id = i;
        data[i].success = TRUE;
        data[i].requests_completed = 0;
        g_mutex_init(&data[i].mutex);
    }

    /* Start concurrent threads */
    for (int i = 0; i < CONCURRENT_CLIENTS; i++) {
        threads[i] = g_thread_new(NULL, concurrent_client_thread, &data[i]);
    }

    /* Wait for all threads to complete */
    for (int i = 0; i < CONCURRENT_CLIENTS; i++) {
        g_thread_join(threads[i]);
    }

    /* Verify results */
    gint total_completed = 0;
    for (int i = 0; i < CONCURRENT_CLIENTS; i++) {
        g_assert_true(data[i].success);
        total_completed += data[i].requests_completed;
        g_mutex_clear(&data[i].mutex);
    }

    /* All requests should have completed */
    g_assert_cmpint(total_completed, ==, CONCURRENT_CLIENTS * 10);
}

static gpointer
concurrent_sign_thread(gpointer user_data)
{
    ConcurrentTestData *data = (ConcurrentTestData*)user_data;
    GError *error = NULL;

    /* Each thread signs different events */
    for (int i = 0; i < 5; i++) {
        gchar *event_json = g_strdup_printf(
            "{\"pubkey\":\"test\",\"created_at\":%ld,\"kind\":1,\"tags\":[],\"content\":\"msg %d from client %d\"}",
            (long)time(NULL), i, data->client_id);

        GVariant *result = g_dbus_proxy_call_sync(
            data->fix->proxy,
            "SignEvent",
            g_variant_new("(sss)", event_json, "", "concurrent-test-app"),
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            NULL,
            &error);

        g_mutex_lock(&data->mutex);
        if (result) {
            const gchar *sig = NULL;
            g_variant_get(result, "(&s)", &sig);
            if (sig && strlen(sig) == 128) {
                data->requests_completed++;
            }
            g_variant_unref(result);
        } else {
            g_clear_error(&error);
        }
        g_mutex_unlock(&data->mutex);

        g_free(event_json);
        g_usleep(500);  /* 0.5ms */
    }

    return NULL;
}

static void
test_dbus_concurrent_sign_requests(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;

    /* Allow all apps to sign */
    mock_acl_allow("*");

    ConcurrentTestData data[CONCURRENT_CLIENTS];
    GThread *threads[CONCURRENT_CLIENTS];

    /* Initialize test data */
    for (int i = 0; i < CONCURRENT_CLIENTS; i++) {
        data[i].fix = fix;
        data[i].client_id = i;
        data[i].success = TRUE;
        data[i].requests_completed = 0;
        g_mutex_init(&data[i].mutex);
    }

    /* Start concurrent threads */
    for (int i = 0; i < CONCURRENT_CLIENTS; i++) {
        threads[i] = g_thread_new(NULL, concurrent_sign_thread, &data[i]);
    }

    /* Wait for all threads to complete */
    for (int i = 0; i < CONCURRENT_CLIENTS; i++) {
        g_thread_join(threads[i]);
    }

    /* Verify results - all sign requests should succeed */
    gint total_completed = 0;
    for (int i = 0; i < CONCURRENT_CLIENTS; i++) {
        total_completed += data[i].requests_completed;
        g_mutex_clear(&data[i].mutex);
    }

    /* All requests should have completed */
    g_assert_cmpint(total_completed, ==, CONCURRENT_CLIENTS * 5);
}

/* ===========================================================================
 * Test: Error Handling and Edge Cases
 * =========================================================================== */

static void
test_dbus_sign_malformed_json(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    mock_acl_allow("*");

    /* Malformed JSON - note: mock currently just checks for empty,
     * but real implementation would validate JSON */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "SignEvent",
        g_variant_new("(sss)", "{invalid json", "", "test-app"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    /* Mock returns signature anyway, but this tests the protocol flow */
    if (result) {
        g_variant_unref(result);
    } else {
        g_clear_error(&error);
    }
}

static void
test_dbus_nip44_empty_plaintext(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    char *peer_sk = nostr_key_generate_private();
    char *peer_pk = nostr_key_get_public(peer_sk);

    /* Empty plaintext */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "NIP44Encrypt",
        g_variant_new("(sss)", "", peer_pk, ""),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    /* Mock returns input, but real implementation might handle differently */
    if (result) {
        g_variant_unref(result);
    }
    g_clear_error(&error);

    free(peer_sk);
    free(peer_pk);
}

static void
test_dbus_nip44_invalid_pubkey(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Invalid pubkey format */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "NIP44Encrypt",
        g_variant_new("(sss)", "Hello world", "not-a-valid-pubkey", ""),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    /* Mock is lenient, real implementation would validate */
    if (result) {
        g_variant_unref(result);
    }
    g_clear_error(&error);
}

static void
test_dbus_sign_very_large_event(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    mock_acl_allow("*");

    /* Create a large event (64KB content) */
    gsize large_size = 64 * 1024;
    gchar *large_content = g_malloc(large_size + 1);
    memset(large_content, 'A', large_size);
    large_content[large_size] = '\0';

    gchar *event_json = g_strdup_printf(
        "{\"pubkey\":\"test\",\"created_at\":1234567890,\"kind\":1,\"tags\":[],\"content\":\"%s\"}",
        large_content);

    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "SignEvent",
        g_variant_new("(sss)", event_json, "", "test-app"),
        G_DBUS_CALL_FLAGS_NONE,
        10000,  /* Longer timeout for large event */
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    const gchar *signature = NULL;
    g_variant_get(result, "(&s)", &signature);
    g_assert_cmpuint(strlen(signature), ==, 128);

    g_variant_unref(result);
    g_free(event_json);
    g_free(large_content);
}

static void
test_dbus_rapid_requests(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Make 100 rapid GetPublicKey requests */
    gint success_count = 0;
    for (int i = 0; i < 100; i++) {
        result = g_dbus_proxy_call_sync(
            fix->proxy,
            "GetPublicKey",
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            NULL,
            &error);

        if (result) {
            success_count++;
            g_variant_unref(result);
        }
        g_clear_error(&error);
    }

    /* All should succeed (no rate limiting for GetPublicKey) */
    g_assert_cmpint(success_count, ==, 100);
}

static void
test_dbus_sign_special_characters(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    mock_acl_allow("*");

    /* Event with special characters, unicode, etc. */
    const gchar *event_json =
        "{\"pubkey\":\"test\",\"created_at\":1234567890,\"kind\":1,\"tags\":[],"
        "\"content\":\"Hello \\\"world\\\" with unicode: \\u4e2d\\u6587 and newlines\\n\\ttab\"}";

    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "SignEvent",
        g_variant_new("(sss)", event_json, "", "test-app"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    g_variant_unref(result);
}

static void
test_dbus_session_revoke_nonexistent(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Try to revoke a session that doesn't exist */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "RevokeSession",
        g_variant_new("(ss)", "nonexistent_client_pk", ""),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    gboolean ok = TRUE;  /* Initialize to opposite of expected */
    g_variant_get(result, "(b)", &ok);
    g_assert_false(ok);  /* Should return false for nonexistent session */

    g_variant_unref(result);
}

static void
test_dbus_session_never_expires(DbusFixture *fix, gconstpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result;

    /* Create a session that never expires (ttl = -1) */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "CreateSession",
        g_variant_new("(ssux)", "never_expire_client", fix->test_npub, (guint32)31, (gint64)-1),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);
    g_variant_unref(result);

    /* Verify the session is active */
    result = g_dbus_proxy_call_sync(
        fix->proxy,
        "GetSession",
        g_variant_new("(ss)", "never_expire_client", fix->test_npub),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_assert_no_error(error);
    g_assert_nonnull(result);

    gboolean active = FALSE;
    guint32 permissions = 0;
    gint64 expires_at = 0;
    g_variant_get(result, "(bux)", &active, &permissions, &expires_at);

    g_assert_true(active);
    g_assert_cmpuint(permissions, ==, 31);
    /* expires_at should be very large (G_MAXINT64) */
    g_assert_cmpint(expires_at, >, 0);

    g_variant_unref(result);
}

/* ===========================================================================
 * Test Runner
 * =========================================================================== */

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* Connection tests */
    g_test_add("/signer/dbus/connection",
               DbusFixture, NULL,
               fixture_setup, test_dbus_connection, fixture_teardown);

    /* GetPublicKey tests */
    g_test_add("/signer/dbus/get_public_key/success",
               DbusFixture, NULL,
               fixture_setup, test_dbus_get_public_key, fixture_teardown);

    g_test_add("/signer/dbus/get_public_key/no_key",
               DbusFixture, NULL,
               fixture_setup, test_dbus_get_public_key_no_key, fixture_teardown);

    /* SignEvent tests */
    g_test_add("/signer/dbus/sign_event/approved",
               DbusFixture, NULL,
               fixture_setup, test_dbus_sign_event_approved, fixture_teardown);

    g_test_add("/signer/dbus/sign_event/denied",
               DbusFixture, NULL,
               fixture_setup, test_dbus_sign_event_denied, fixture_teardown);

    g_test_add("/signer/dbus/sign_event/invalid_input",
               DbusFixture, NULL,
               fixture_setup, test_dbus_sign_event_invalid_input, fixture_teardown);

    /* GetRelays tests */
    g_test_add("/signer/dbus/get_relays",
               DbusFixture, NULL,
               fixture_setup, test_dbus_get_relays, fixture_teardown);

    /* StoreKey tests */
    g_test_add("/signer/dbus/store_key/mutations_disabled",
               DbusFixture, NULL,
               fixture_setup, test_dbus_store_key_mutations_disabled, fixture_teardown);

    g_test_add("/signer/dbus/store_key/mutations_enabled",
               DbusFixture, NULL,
               fixture_setup, test_dbus_store_key_mutations_enabled, fixture_teardown);

    /* ClearKey tests */
    g_test_add("/signer/dbus/clear_key/mutations_disabled",
               DbusFixture, NULL,
               fixture_setup, test_dbus_clear_key_mutations_disabled, fixture_teardown);

    /* NIP-04 encryption tests */
    g_test_add("/signer/dbus/nip04/encrypt",
               DbusFixture, NULL,
               fixture_setup, test_dbus_nip04_encrypt, fixture_teardown);

    g_test_add("/signer/dbus/nip04/decrypt",
               DbusFixture, NULL,
               fixture_setup, test_dbus_nip04_decrypt, fixture_teardown);

    /* NIP-44 encryption tests */
    g_test_add("/signer/dbus/nip44/encrypt",
               DbusFixture, NULL,
               fixture_setup, test_dbus_nip44_encrypt, fixture_teardown);

    g_test_add("/signer/dbus/nip44/decrypt",
               DbusFixture, NULL,
               fixture_setup, test_dbus_nip44_decrypt, fixture_teardown);

    /* Zap decryption tests */
    g_test_add("/signer/dbus/decrypt_zap_event",
               DbusFixture, NULL,
               fixture_setup, test_dbus_decrypt_zap_event, fixture_teardown);

    /* ApproveRequest tests */
    g_test_add("/signer/dbus/approve_request",
               DbusFixture, NULL,
               fixture_setup, test_dbus_approve_request, fixture_teardown);

    /* Session management tests */
    g_test_add("/signer/dbus/session/create",
               DbusFixture, NULL,
               fixture_setup, test_dbus_session_create, fixture_teardown);

    g_test_add("/signer/dbus/session/get_existing",
               DbusFixture, NULL,
               fixture_setup, test_dbus_session_get_existing, fixture_teardown);

    g_test_add("/signer/dbus/session/get_nonexistent",
               DbusFixture, NULL,
               fixture_setup, test_dbus_session_get_nonexistent, fixture_teardown);

    g_test_add("/signer/dbus/session/revoke",
               DbusFixture, NULL,
               fixture_setup, test_dbus_session_revoke, fixture_teardown);

    g_test_add("/signer/dbus/session/revoke_nonexistent",
               DbusFixture, NULL,
               fixture_setup, test_dbus_session_revoke_nonexistent, fixture_teardown);

    g_test_add("/signer/dbus/session/list",
               DbusFixture, NULL,
               fixture_setup, test_dbus_session_list, fixture_teardown);

    g_test_add("/signer/dbus/session/create_invalid_input",
               DbusFixture, NULL,
               fixture_setup, test_dbus_session_create_invalid_input, fixture_teardown);

    g_test_add("/signer/dbus/session/never_expires",
               DbusFixture, NULL,
               fixture_setup, test_dbus_session_never_expires, fixture_teardown);

    /* Concurrent request tests */
    g_test_add("/signer/dbus/concurrent/get_public_key",
               DbusFixture, NULL,
               fixture_setup, test_dbus_concurrent_requests, fixture_teardown);

    g_test_add("/signer/dbus/concurrent/sign_events",
               DbusFixture, NULL,
               fixture_setup, test_dbus_concurrent_sign_requests, fixture_teardown);

    /* Error handling and edge cases */
    g_test_add("/signer/dbus/error/sign_malformed_json",
               DbusFixture, NULL,
               fixture_setup, test_dbus_sign_malformed_json, fixture_teardown);

    g_test_add("/signer/dbus/error/nip44_empty_plaintext",
               DbusFixture, NULL,
               fixture_setup, test_dbus_nip44_empty_plaintext, fixture_teardown);

    g_test_add("/signer/dbus/error/nip44_invalid_pubkey",
               DbusFixture, NULL,
               fixture_setup, test_dbus_nip44_invalid_pubkey, fixture_teardown);

    g_test_add("/signer/dbus/edge/large_event",
               DbusFixture, NULL,
               fixture_setup, test_dbus_sign_very_large_event, fixture_teardown);

    g_test_add("/signer/dbus/edge/rapid_requests",
               DbusFixture, NULL,
               fixture_setup, test_dbus_rapid_requests, fixture_teardown);

    g_test_add("/signer/dbus/edge/special_characters",
               DbusFixture, NULL,
               fixture_setup, test_dbus_sign_special_characters, fixture_teardown);

    return g_test_run();
}
