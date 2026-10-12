/* G00: exercise the shipped daemon, not MockBunkerService or a core stand-in. */
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>
#include "nostrc-test-bus.h"
#include "bunker_test_relay.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-kinds.h"
#include "json.h"
#include <json-glib/json-glib.h>

typedef struct {
    NostrcTestBus *bus;
    BunkerTestRelay *relay;
    NostrNip46Session *client;
    GMainLoop *loop;
    GThread *worker;
    gchar *log_path;
    gchar *account_sk, *account_pk, *client_sk, *client_pk, *peer_sk, *peer_pk;
    gboolean ready, passed, timed_out;
} Fixture;

#define REQUIRE(condition, message) do { \
    if (!(condition)) { g_printerr("ASSERTION FAILED: %s\n", message); return FALSE; } \
} while (0)

static gboolean run_rpc_flow(Fixture *f)
{
    g_autofree gchar *encoded = g_uri_escape_string(bunker_test_relay_url(f->relay), NULL, FALSE);
    g_autofree gchar *uri = g_strdup_printf("bunker://%s?relay=%s", f->account_pk, encoded);
    REQUIRE(nostr_nip46_client_connect(f->client, uri, NULL) == 0, "parse real daemon bunker endpoint");
    REQUIRE(nostr_nip46_client_set_secret(f->client, f->client_sk) == 0, "install disposable client key");
    nostr_nip46_client_set_timeout(f->client, 4000); /* failure bound, not readiness */

    g_autofree gchar *ack = NULL;
    int rc = nostr_nip46_client_connect_rpc(f->client, NULL,
        "get_public_key,sign_event,nip44_encrypt,nip44_decrypt", &ack);
    g_print("client connect rc=%d reply=%s\n", rc, ack ? ack : "(none)");
    REQUIRE(rc == 0 && g_strcmp0(ack, "ack") == 0,
        "real grotto-daemon --bunker must answer encrypted connect; no listener/reply is NOT RUNNING");

    g_autofree gchar *pubkey = NULL;
    REQUIRE(nostr_nip46_client_get_public_key_rpc(f->client, &pubkey) == 0, "get_public_key RPC reply");
    REQUIRE(g_strcmp0(pubkey, f->account_pk) == 0, "get_public_key must return the disposable account key");
    g_print("get_public_key = configured account key\n");

    g_autofree gchar *unsigned_json = g_strdup_printf(
        "{\"kind\":%d,\"created_at\":%" G_GINT64_FORMAT ",\"tags\":[],\"content\":\"G00 bunker proof\"}",
        NOSTR_KIND_TEXT_NOTE, g_get_real_time() / G_USEC_PER_SEC);
    g_autofree gchar *signed_json = NULL;
    REQUIRE(nostr_nip46_client_sign_event(f->client, unsigned_json, &signed_json) == 0,
        "auto-approved sign_event RPC reply");
    NostrEvent *event = nostr_event_new();
    gboolean valid = event && nostr_event_deserialize(event, signed_json) == 0 &&
        g_strcmp0(nostr_event_get_pubkey(event), f->account_pk) == 0 &&
        nostr_event_get_kind(event) == NOSTR_KIND_TEXT_NOTE &&
        g_strcmp0(nostr_event_get_content(event), "G00 bunker proof") == 0 &&
        nostr_event_check_signature(event);
    nostr_event_free(event);
    REQUIRE(valid, "signed event must have the account pubkey, requested content/kind and a valid signature");

    /* Independently decrypt/encrypt as the peer: two matching RPC bugs must not
     * turn a self-consistent round-trip into false proof of account-key use. */
    NostrNip46Session *peer = nostr_nip46_client_new();
    REQUIRE(peer != NULL, "create disposable NIP-44 peer");
    nostr_nip46_client_set_secret(peer, f->peer_sk);
    gchar *cipher = NULL, *plain = NULL, *incoming = NULL, *decrypted = NULL;
    valid = nostr_nip46_client_nip44_encrypt_rpc(f->client, f->peer_pk, "outgoing G00", &cipher) == 0 &&
        nostr_nip46_client_nip44_decrypt(peer, f->account_pk, cipher, &plain) == 0 &&
        g_strcmp0(plain, "outgoing G00") == 0 &&
        nostr_nip46_client_nip44_encrypt(peer, f->account_pk, "incoming G00", &incoming) == 0 &&
        nostr_nip46_client_nip44_decrypt_rpc(f->client, f->peer_pk, incoming, &decrypted) == 0 &&
        g_strcmp0(decrypted, "incoming G00") == 0;
    free(cipher); free(plain); free(incoming); free(decrypted);
    nostr_nip46_session_free(peer);
    REQUIRE(valid, "NIP-44 RPCs must interoperate with an independent peer under the account key");
    return TRUE;
}

static gboolean flow_done(gpointer data)
{
    Fixture *f = data;
    g_main_loop_quit(f->loop);
    return G_SOURCE_REMOVE;
}

static gpointer rpc_worker(gpointer data)
{
    Fixture *f = data;
    f->passed = run_rpc_flow(f);
    g_idle_add(flow_done, f);
    return NULL;
}

static void check_daemon_log(Fixture *f)
{
    if (f->ready) return;
    g_autofree gchar *log = NULL;
    if (!g_file_get_contents(f->log_path, &log, NULL, NULL)) return;
    if (strstr(log, "NIP-46 bunker running for")) {
        f->ready = TRUE;
        guint connections, requests, published;
        bunker_test_relay_stats(f->relay, &connections, &requests, &published);
        g_print("daemon claimed RUNNING; before client: connections=%u REQ=%u\n",
            connections, requests);
        f->worker = g_thread_new("bunker-client", rpc_worker, f);
    } else if (strstr(log, "BUNKER_START_FAILED") || strstr(log, "BUNKER_NO_") ||
               strstr(log, "BUNKER_DISABLED") || strstr(log, "BUNKER_OPEN_ACL")) {
        g_printerr("ASSERTION FAILED: isolated daemon configuration/startup rejected\n");
        g_main_loop_quit(f->loop);
    }
}

static void log_changed(GFileMonitor *monitor, GFile *file, GFile *other,
                        GFileMonitorEvent event, gpointer data)
{
    (void)monitor; (void)file; (void)other; (void)event;
    check_daemon_log(data);
}

static gboolean deadline(gpointer data)
{
    Fixture *f = data;
    f->timed_out = TRUE;
    g_printerr("ASSERTION FAILED: daemon/client exceeded the 40s failure bound\n");
    nostr_nip46_client_cancel_all(f->client);
    g_main_loop_quit(f->loop);
    return G_SOURCE_REMOVE;
}

/* Observe encrypted traffic only. This session never listens or answers RPCs. */
static gboolean audit_frames(Fixture *f)
{
    JsonArray *events = bunker_test_relay_events(f->relay);
    REQUIRE(events != NULL, "relay capture is an array");
    g_autoptr(GHashTable) requests = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_autoptr(GHashTable) replies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    NostrNip46Session *observer = nostr_nip46_client_new();
    REQUIRE(observer != NULL, "create transport observation session");
    nostr_nip46_client_set_secret(observer, f->account_sk);
    gboolean valid = TRUE;
    for (guint i = 0; i < json_array_get_length(events); i++) {
        JsonNode *node = json_array_get_element(events, i);
        g_autofree gchar *event_json = json_to_string(node, FALSE);
        NostrEvent *event = nostr_event_new();
        gchar *plain = NULL;
        valid = event && nostr_event_deserialize(event, event_json) == 0 &&
            nostr_event_get_kind(event) == NOSTR_EVENT_KIND_NIP46 && nostr_event_check_signature(event);
        if (!valid) { nostr_event_free(event); break; }
        gboolean request = g_strcmp0(nostr_event_get_pubkey(event), f->client_pk) == 0;
        const gchar *author = request ? f->client_pk : f->account_pk;
        const gchar *target = request ? f->account_pk : f->client_pk;
        JsonArray *tags = json_object_get_array_member(json_node_get_object(node), "tags");
        gboolean addressed = FALSE;
        for (guint j = 0; tags && j < json_array_get_length(tags); j++) {
            JsonArray *tag = json_array_get_array_element(tags, j);
            if (tag && json_array_get_length(tag) >= 2 &&
                g_strcmp0(json_array_get_string_element(tag, 0), "p") == 0 &&
                g_strcmp0(json_array_get_string_element(tag, 1), target) == 0) addressed = TRUE;
        }
        valid = addressed && g_strcmp0(nostr_event_get_pubkey(event), author) == 0 &&
            nostr_nip46_client_nip44_decrypt(request ? observer : f->client, author,
                nostr_event_get_content(event), &plain) == 0;
        nostr_event_free(event);
        if (!valid) { free(plain); break; }
        if (request) {
            NostrNip46Request req = {0};
            valid = nostr_nip46_request_parse(plain, &req) == 0 && req.id && req.method &&
                !g_hash_table_contains(requests, req.id);
            if (valid) {
                g_hash_table_insert(requests, g_strdup(req.id), g_strdup(req.method));
                g_print("relay request method=%s id=%s signature=valid target=account\n", req.method, req.id);
            }
            nostr_nip46_request_free(&req);
        } else {
            NostrNip46Response resp = {0};
            valid = nostr_nip46_response_parse(plain, &resp) == 0 && resp.id && resp.result && !resp.error &&
                g_hash_table_contains(requests, resp.id) && !g_hash_table_contains(replies, resp.id);
            if (valid) {
                g_hash_table_add(replies, g_strdup(resp.id));
                g_print("relay response id=%s matched decrypted request\n", resp.id);
            }
            nostr_nip46_response_free(&resp);
        }
        free(plain);
        if (!valid) break;
    }
    nostr_nip46_session_free(observer);
    g_print("relay decrypted requests=%u matched replies=%u\n",
        g_hash_table_size(requests), g_hash_table_size(replies));
    REQUIRE(valid, "signed NIP-44 envelopes, transport targets and unique matching response ids");
    if (f->passed) {
        REQUIRE(g_hash_table_size(requests) == 5 && g_hash_table_size(replies) == 5,
            "connect/get_public_key/sign_event/nip44_encrypt/nip44_decrypt each have one matching response");
        const gchar *methods[] = {"connect", "get_public_key", "sign_event", "nip44_encrypt", "nip44_decrypt"};
        for (guint i = 0; i < G_N_ELEMENTS(methods); i++) {
            gboolean found = FALSE;
            GHashTableIter iter; gpointer value;
            g_hash_table_iter_init(&iter, requests);
            while (g_hash_table_iter_next(&iter, NULL, &value))
                if (g_strcmp0(value, methods[i]) == 0) found = TRUE;
            REQUIRE(found, "all five NIP-46 methods observed at the relay");
        }
    }
    return TRUE;
}

static gboolean configure_schema(Fixture *f, const gchar *schema_source, const gchar *compiler)
{
    g_autofree gchar *dir = g_build_filename(nostrc_test_bus_get_dir(f->bus), "schemas", NULL);
    REQUIRE(g_mkdir_with_parents(dir, 0700) == 0, "private schema directory");
    g_autofree gchar *schema = NULL;
    g_autofree gchar *path = g_build_filename(dir, "org.nostr.Grotto.gschema.xml", NULL);
    REQUIRE(g_file_get_contents(schema_source, &schema, NULL, NULL) &&
        g_file_set_contents(path, schema, -1, NULL), "copy shipped Grotto schema");
    g_autofree gchar *defaults = g_strdup_printf(
        "[org.nostr.Grotto]\nbunker-enabled=true\nbunker-relays=['%s']\n"
        "bunker-allowed-pubkeys=['%s']\nbunker-auto-approve-kinds=['%d']\n"
        "bunker-allowed-methods=['connect','get_public_key','sign_event','nip44_encrypt','nip44_decrypt']\n"
        "default-identity='%s'\n", bunker_test_relay_url(f->relay), f->client_pk,
        NOSTR_KIND_TEXT_NOTE, f->account_pk);
    g_autofree gchar *override = g_build_filename(dir, "g00.gschema.override", NULL);
    REQUIRE(g_file_set_contents(override, defaults, -1, NULL), "write private memory GSettings defaults");
    const gchar *argv[] = {compiler, "--strict", dir, NULL};
    gint status = -1;
    REQUIRE(g_spawn_sync(NULL, (gchar **)argv, NULL, 0, NULL, NULL, NULL, NULL, &status, NULL) &&
        g_spawn_check_wait_status(status, NULL), "compile isolated GSettings schema");
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    g_setenv("GSETTINGS_SCHEMA_DIR", dir, TRUE);
    g_setenv("NOSTR_SIGNER_SECKEY_HEX", f->account_sk, TRUE);
    g_unsetenv("NOSTR_SIGNER_NSEC");
    g_unsetenv("NOSTR_SIGNER_ENDPOINT");
    g_setenv("NOSTR_DEBUG", "1", TRUE);
    return TRUE;
}

int main(int argc, char **argv)
{
    if (argc != 4 || g_strcmp0(g_getenv("NOSTRC_TEST_ISOLATION"), "1") != 0) {
        g_printerr("Run through CTest: private HOME/XDG and NOSTRC_TEST_ISOLATION are required\n");
        return 1;
    }
    if (!nostrc_test_bus_available()) return 77;
    nostr_json_init();
    Fixture f = {0};
    f.account_sk = nostr_key_generate_private(); f.account_pk = nostr_key_get_public(f.account_sk);
    f.client_sk = nostr_key_generate_private(); f.client_pk = nostr_key_get_public(f.client_sk);
    f.peer_sk = nostr_key_generate_private(); f.peer_pk = nostr_key_get_public(f.peer_sk);
    g_assert_nonnull(f.account_pk); g_assert_nonnull(f.client_pk); g_assert_nonnull(f.peer_pk);
    f.client = nostr_nip46_client_new();
    g_assert_nonnull(f.client);
    f.bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
    nostrc_test_bus_up(f.bus);
    f.relay = bunker_test_relay_new();
    f.loop = g_main_loop_new(NULL, FALSE);
    f.log_path = g_build_filename(nostrc_test_bus_get_dir(f.bus), "grotto-daemon.log", NULL);
    gboolean configured = configure_schema(&f, argv[2], argv[3]);
    g_autoptr(GFileMonitor) monitor = NULL;
    guint timeout_id = 0;
    if (configured) {
        g_autoptr(GFile) log = g_file_new_for_path(f.log_path);
        monitor = g_file_monitor_file(log, G_FILE_MONITOR_NONE, NULL, NULL);
        g_assert_nonnull(monitor);
        g_signal_connect(monitor, "changed", G_CALLBACK(log_changed), &f);
        /* Optional debugger wrapper observes the same binary, without changing
         * arguments or keys. Normal CTest never needs a debugger. */
        const gchar *wrapper = g_getenv("NOSTRC_BUNKER_DEBUGGER_WRAPPER");
        const gchar *command[] = {wrapper ? wrapper : argv[1], wrapper ? argv[1] : "--bunker",
            wrapper ? "--bunker" : NULL, NULL};
        nostrc_test_bus_spawn_supervised(f.bus, "grotto-daemon.log", NULL, command);
        timeout_id = g_timeout_add_seconds(40, deadline, &f);
        check_daemon_log(&f);
        g_main_loop_run(f.loop);
    }
    if (timeout_id && !f.timed_out) g_source_remove(timeout_id);
    if (monitor) g_file_monitor_cancel(monitor);
    if (f.worker) g_thread_join(f.worker);
    guint connections, requests, published;
    bunker_test_relay_stats(f.relay, &connections, &requests, &published);
    g_print("relay evidence: connections=%u REQ=%u published=%u\n",
        connections, requests, published);
    gboolean audited = audit_frames(&f);
    nostrc_test_bus_dump_log(f.bus, "grotto-daemon.log");
    nostr_nip46_session_free(f.client);
    bunker_test_relay_free(f.relay);
    nostrc_test_bus_down(f.bus);
    g_main_loop_unref(f.loop);
    g_free(f.log_path);
    memset(f.account_sk, 0, strlen(f.account_sk)); free(f.account_sk); free(f.account_pk);
    memset(f.client_sk, 0, strlen(f.client_sk)); free(f.client_sk); free(f.client_pk);
    memset(f.peer_sk, 0, strlen(f.peer_sk)); free(f.peer_sk); free(f.peer_pk);
    g_unsetenv("NOSTR_SIGNER_SECKEY_HEX");
    nostr_json_cleanup();
    return configured && f.ready && f.passed && audited && !f.timed_out ? 0 : 1;
}
