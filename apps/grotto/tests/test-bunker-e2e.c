/* G00's shipped-daemon proof, repaired and extended for G05. No mock service
 * or replacement signer stands in for grotto-daemon. */
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
#include <nostr-utils.h>
#include <nostr-tag.h>
#include <nostr/nip55l/signer_ops.h>
#include <secure_buf.h>
#include "grotto_clients.h"
#include "signer_permissions.h"
#include "nip55l_dbus_errors.h"
#include <nostr/nip19/nip19.h>
#include "json.h"
#include <json-glib/json-glib.h>

typedef struct {
    NostrcTestBus *bus;
    BunkerTestRelay *relay;
    NostrNip46Session *client;
    GMainLoop *loop;
    GThread *worker;
    gchar *log_path;
    gchar *account_sk, *account_pk, *client_sk, *client_pk, *peer_sk, *peer_pk, *transport_sk, *transport_pk;
    GDBusConnection *approver;
    guint approver_name, approval_subscription, approvals;
    GMutex capture_mutex; GCond capture_cond; GHashTable *captured;
    NostrNip46Session *second;
    gchar *second_sk, *second_pk;
    gint defer_sign, cancel_mode;
    gboolean headless;
    const gchar *daemon_binary;
    GSubprocess *daemon;
    GFileMonitor *restart_monitor;
    gboolean restarted, restart_ok;
    GPtrArray *deferred_ids;
    gboolean ready, passed, timed_out;
} Fixture;

#define REQUIRE(condition, message) do { \
    if (!(condition)) { g_printerr("ASSERTION FAILED: %s\n", message); return FALSE; } \
} while (0)

typedef struct { Fixture *fixture; gchar *event; } Injection;
static gboolean inject_on_main(gpointer data) {
    Injection *job = data; bunker_test_relay_inject(job->fixture->relay, job->event);
    g_free(job->event); g_free(job); return G_SOURCE_REMOVE;
}
static void capture_response(const gchar *event_json, gpointer data) {
    Fixture *f = data; NostrEvent *event = nostr_event_new();
    if (nostr_event_deserialize(event, event_json) || g_strcmp0(nostr_event_get_pubkey(event), f->transport_pk)) {
        nostr_event_free(event); return;
    }
    char *plain = NULL;
    if (nostr_nip46_client_nip44_decrypt(f->client, f->transport_pk, nostr_event_get_content(event), &plain)) {
        /* A response to the unpaired independent peer is still observable. */
        NostrNip46Session *peer = nostr_nip46_client_new(); nostr_nip46_client_set_secret(peer, f->peer_sk);
        nostr_nip46_client_nip44_decrypt(peer, f->transport_pk, nostr_event_get_content(event), &plain);
        nostr_nip46_session_free(peer);
    }
    nostr_event_free(event);
    NostrNip46Response reply = {0};
    if (plain && !nostr_nip46_response_parse(plain, &reply) && reply.id) {
        g_mutex_lock(&f->capture_mutex);
        g_hash_table_replace(f->captured, g_strdup(reply.id), g_strdup(plain));
        g_cond_broadcast(&f->capture_cond); g_mutex_unlock(&f->capture_mutex);
    }
    nostr_nip46_response_free(&reply); free(plain);
}
static gchar *request_event(Fixture *f, const gchar *id, const gchar *method,
    const gchar *const *params, gsize count, gint64 created, gboolean wrong_client) {
    NostrNip46Session *sender = nostr_nip46_client_new();
    const gchar *sk = wrong_client ? f->peer_sk : f->client_sk;
    nostr_nip46_client_set_secret(sender, sk);
    g_autofree gchar *plain = nostr_nip46_request_build(id, method, params, count);
    gchar *cipher = NULL; g_assert_cmpint(nostr_nip46_client_nip44_encrypt(sender, f->transport_pk, plain, &cipher), ==, 0);
    nostr_nip46_session_free(sender);
    NostrEvent *event = nostr_event_new(); nostr_event_set_kind(event, NOSTR_EVENT_KIND_NIP46);
    nostr_event_set_pubkey(event, wrong_client ? f->peer_pk : f->client_pk);
    nostr_event_set_created_at(event, created); nostr_event_set_content(event, cipher); free(cipher);
    nostr_event_set_tags(event, nostr_tags_new(1, nostr_tag_new("p", f->transport_pk, NULL)));
    g_assert_cmpint(nostr_event_sign(event, sk), ==, 0);
    gchar *serialized = nostr_event_serialize(event); nostr_event_free(event); return serialized;
}
static gchar *send_raw(Fixture *f, const gchar *id, const gchar *event) {
    g_mutex_lock(&f->capture_mutex); g_hash_table_remove(f->captured, id);
    Injection *job = g_new0(Injection, 1); job->fixture = f; job->event = g_strdup(event);
    g_idle_add(inject_on_main, job);
    gint64 bound = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (!g_hash_table_contains(f->captured, id)) {
        if (!g_cond_wait_until(&f->capture_cond, &f->capture_mutex, bound)) {
            g_mutex_unlock(&f->capture_mutex); g_printerr("raw RPC %s had no response\n", id); return NULL;
        }
    }
    gchar *reply = g_strdup(g_hash_table_lookup(f->captured, id)); g_mutex_unlock(&f->capture_mutex); return reply;
}
static gboolean raw_expect(Fixture *f, const gchar *id, const gchar *event, const gchar *error) {
    g_autofree gchar *text = send_raw(f, id, event); NostrNip46Response response = {0};
    REQUIRE(text && !nostr_nip46_response_parse(text, &response), "raw request receives a parseable response");
    g_autoptr(JsonParser) parser = json_parser_new();
    REQUIRE(json_parser_load_from_data(parser, text, -1, NULL), "response is JSON");
    JsonObject *object = json_node_get_object(json_parser_get_root(parser));
    JsonNode *value = json_object_get_member(object, error ? "error" : "result");
    REQUIRE(value && JSON_NODE_HOLDS_VALUE(value) && json_node_get_value_type(value) == G_TYPE_STRING,
        "NIP-46 result/error is a string, not a permissively parsed JSON object");
    gboolean valid = !g_strcmp0(response.id, id) && (!error ? response.result && !response.error : !g_strcmp0(response.error, error));
    if (!valid) g_printerr("raw RPC %s expected %s received %s\n", id, error ? error : "success", response.error ? response.error : response.result);
    nostr_nip46_response_free(&response); return valid;
}
static gboolean control(Fixture *f, const gchar *method, GVariant *args) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer/grotto",
        "org.nostr.Grotto.Control1", method, args, NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);
    if (error) g_printerr("%s: %s\n", method, error->message);
    return reply != NULL;
}
static gboolean grant_scoped_account_ops(Fixture *f) {
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer/grotto",
        "org.nostr.Grotto.Control1", "GetStatus", NULL, G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
    REQUIRE(reply != NULL, "read permission revision before explicit scoped provisioning");
    g_autoptr(GVariant) status = g_variant_get_child_value(reply, 0); guint64 revision = 0;
    REQUIRE(g_variant_lookup(status, "permissions_revision", "t", &revision), "permission revision is present");
    guint8 pubkey[32]; gchar *identity = NULL;
    REQUIRE(nostr_hex2bin(pubkey, f->account_pk, 32) && !nostr_nip19_encode_npub(pubkey, &identity), "canonical account identity");
    g_autofree gchar *principal = g_strdup_printf("nip46:%s", f->client_pk);
    const gchar *operations[] = {"connect", "get_public_key", "event", "nip04_encrypt", "nip04_decrypt", "nip44_encrypt", "nip44_decrypt"};
    GVariantBuilder rows; g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
    for (guint i = 0; i < G_N_ELEMENTS(operations); i++) {
        GVariantBuilder row; g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&row, "{sv}", "principal", g_variant_new_string(principal));
        g_variant_builder_add(&row, "{sv}", "identity", g_variant_new_string(identity));
        g_variant_builder_add(&row, "{sv}", "operation", g_variant_new_string(operations[i]));
        g_variant_builder_add(&row, "{sv}", "decision", g_variant_new_string("allow"));
        g_variant_builder_add(&row, "{sv}", "event-kind", g_variant_new_int32(i == 2 ? NOSTR_KIND_TEXT_NOTE : -1));
        g_variant_builder_add(&row, "{sv}", "expires-at", g_variant_new_uint64(time(NULL) + 600));
        g_variant_builder_add_value(&rows, g_variant_builder_end(&row));
    }
    gboolean ok = control(f, "SetPermissions", g_variant_new("(ss@aa{sv}t)", principal, identity, g_variant_builder_end(&rows), revision));
    free(identity); return ok;
}
typedef struct { NostrNip46Session *client; const gchar *content; gchar *result; gint rc; } DualSign;
static gpointer sign_worker(gpointer data) {
    DualSign *job = data;
    g_autofree gchar *event = g_strdup_printf("{\"kind\":%d,\"created_at\":%ld,\"tags\":[],\"content\":\"%s\"}",
        NOSTR_KIND_FOLLOW_LIST, (long)time(NULL), job->content);
    job->rc = nostr_nip46_client_sign_event(job->client, event, &job->result); return NULL;
}
static gboolean signed_as(DualSign *job, const gchar *pubkey) {
    NostrEvent *event = nostr_event_new();
    gboolean valid = job->rc == 0 && job->result && !nostr_event_deserialize(event, job->result) &&
        nostr_event_check_signature(event) && !g_strcmp0(nostr_event_get_pubkey(event), pubkey) &&
        !g_strcmp0(nostr_event_get_content(event), job->content);
    nostr_event_free(event); free(job->result); return valid;
}
static void restart_log_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer data) {
    (void)monitor; (void)file; (void)other; (void)event;
    Fixture *f = data;
    g_autofree gchar *path = g_build_filename(nostrc_test_bus_get_dir(f->bus), "grotto-restart.log", NULL);
    g_autofree gchar *log = NULL;
    if (!g_file_get_contents(path, &log, NULL, NULL)) return;
    gboolean ready = strstr(log, "BUNKER_STATE state=LISTENING") != NULL;
    if (ready || strstr(log, "BUNKER_START_FAILED")) {
        g_mutex_lock(&f->capture_mutex); f->restart_ok = ready; f->restarted = TRUE;
        g_cond_broadcast(&f->capture_cond); g_mutex_unlock(&f->capture_mutex);
    }
}
static gboolean restart_on_main(gpointer data) {
    Fixture *f = data;
    g_autofree gchar *path = g_build_filename(nostrc_test_bus_get_dir(f->bus), "grotto-restart.log", NULL);
    g_autoptr(GFile) file = g_file_new_for_path(path);
    f->restart_monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, NULL);
    g_assert_nonnull(f->restart_monitor);
    g_signal_connect(f->restart_monitor, "changed", G_CALLBACK(restart_log_changed), f);
    const gchar *command[] = {f->daemon_binary, "--bunker", f->headless ? "--no-approval-ui" : NULL, NULL};
    f->daemon = nostrc_test_bus_spawn_supervised(f->bus, "grotto-restart.log", NULL, command);
    restart_log_changed(NULL, NULL, NULL, 0, f); return G_SOURCE_REMOVE;
}
static gboolean restart_daemon(Fixture *f) {
    g_subprocess_send_signal(f->daemon, SIGTERM);
    if (!g_subprocess_wait(f->daemon, NULL, NULL)) return FALSE;
    g_mutex_lock(&f->capture_mutex); g_idle_add(restart_on_main, f);
    gint64 bound = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
    while (!f->restarted) {
        if (!g_cond_wait_until(&f->capture_cond, &f->capture_mutex, bound)) {
            g_mutex_unlock(&f->capture_mutex); return FALSE;
        }
    }
    gboolean ok = f->restart_ok; g_mutex_unlock(&f->capture_mutex); return ok;
}
static gboolean run_rpc_flow(Fixture *f)
{
    /* The remote lane may be in ERROR, but the exported local signer survives. */
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    REQUIRE(bus != NULL, "connect to isolated signer bus");
    g_autoptr(GVariant) local = g_dbus_connection_call_sync(bus,
        "org.nostr.Signer", "/org/nostr/signer", "org.freedesktop.DBus.Introspectable",
        "Introspect", NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
        4000, NULL, NULL);
    REQUIRE(local != NULL, "local signer must remain exported after bunker startup failure");
    const gchar *xml = NULL;
    g_variant_get(local, "(&s)", &xml);
    REQUIRE(strstr(xml, "org.nostr.Signer") != NULL, "local signer interface remains available");
    g_print("local org.nostr.Signer remains available\n");

    g_autofree gchar *encoded = g_uri_escape_string(bunker_test_relay_url(f->relay), NULL, FALSE);
    g_autofree gchar *uri = g_strdup_printf("bunker://%s?relay=%s", f->transport_pk, encoded);
    REQUIRE(nostr_nip46_client_connect(f->client, uri, NULL) == 0, "parse real daemon bunker endpoint");
    REQUIRE(nostr_nip46_client_set_secret(f->client, f->client_sk) == 0, "install disposable client key");
    nostr_nip46_client_set_timeout(f->client, 4000); /* failure bound, not readiness */

    g_autofree gchar *ack = NULL;
    int rc = nostr_nip46_client_connect_rpc(f->client, NULL,
        "get_public_key,sign_event,nip44_encrypt,nip44_decrypt", &ack);
    g_print("client connect rc=%d reply=%s\n", rc, ack ? ack : "(none)");
    REQUIRE(rc == 0 && g_strcmp0(ack, "ack") == 0,
        "real grotto-daemon --bunker must answer encrypted connect; no listener/reply is NOT RUNNING");

    /* Use the real management contract for explicit account/operation scopes.
     * Rapid synthetic UI decisions must not race the production prompt throttle;
     * no sleep, throttle disable or client-requested grant is involved. Kind 3
     * remains ungranted and exercises two interleaved deferred reviews below. */
    if (!f->headless) REQUIRE(grant_scoped_account_ops(f), "explicit scoped account operations");
    g_autofree gchar *pubkey = NULL;
    REQUIRE(nostr_nip46_client_get_public_key_rpc(f->client, &pubkey) == 0, "get_public_key RPC reply");
    REQUIRE(g_strcmp0(pubkey, f->account_pk) == 0, "get_public_key must return the disposable account key");
    REQUIRE(g_strcmp0(pubkey, f->transport_pk) != 0, "account identity differs from transport identity");
    g_print("get_public_key = configured account key != transport key\n");

    g_autofree gchar *unsigned_json = g_strdup_printf(
        "{\"kind\":%d,\"created_at\":%" G_GINT64_FORMAT ",\"tags\":[],\"content\":\"G00 bunker proof\"}",
        NOSTR_KIND_TEXT_NOTE, g_get_real_time() / G_USEC_PER_SEC);
    g_autofree gchar *signed_json = NULL;
    REQUIRE(nostr_nip46_client_sign_event(f->client, unsigned_json, &signed_json) == 0,
        "scoped sign_event RPC reply");
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
    cipher = plain = incoming = decrypted = NULL;
    /* NIP-04 is separately gated and uses the account, not transport key. */
    valid = valid && nostr_nip46_client_nip04_encrypt_rpc(f->client, f->peer_pk, "legacy outgoing", &cipher) == 0 &&
        nostr_nip46_client_nip04_decrypt(peer, f->account_pk, cipher, &plain) == 0 && !g_strcmp0(plain, "legacy outgoing") &&
        nostr_nip46_client_nip04_encrypt(peer, f->account_pk, "legacy incoming", &incoming) == 0 &&
        nostr_nip46_client_nip04_decrypt_rpc(f->client, f->peer_pk, incoming, &decrypted) == 0 && !g_strcmp0(decrypted, "legacy incoming");
    free(cipher); free(plain); free(incoming); free(decrypted);
    nostr_nip46_session_free(peer);
    REQUIRE(valid, "NIP-04/44 RPCs interoperate with an independent peer under the account key");
    if (!f->headless) {
    REQUIRE(nostr_nip46_client_connect(f->second, uri, NULL) == 0 &&
        nostr_nip46_client_set_secret(f->second, f->second_sk) == 0, "second authenticated client endpoint");
    nostr_nip46_client_set_timeout(f->second, 10000);
    g_clear_pointer(&ack, g_free);
    REQUIRE(nostr_nip46_client_connect_rpc(f->second, NULL, "sign_event", &ack) == 0, "second client connect consent");
    g_clear_pointer(&ack, g_free);
    REQUIRE(nostr_nip46_client_connect_rpc(f->second, NULL, "sign_event", &ack) == 0 && !g_strcmp0(ack, "ack"),
        "paired reconnect is idempotent, without another review or signing grant");
    g_atomic_int_set(&f->defer_sign, TRUE);
    DualSign first = {f->client, "interleaved first", NULL, -1};
    DualSign second = {f->second, "interleaved second", NULL, -1};
    GThread *other = g_thread_new("interleaved-signer", sign_worker, &second);
    sign_worker(&first); g_thread_join(other);
    REQUIRE(signed_as(&first, f->account_pk) && signed_as(&second, f->account_pk),
        "two independently attributed deferred requests are both signed only after their decisions");
    g_atomic_int_set(&f->defer_sign, FALSE);
    } else {
        g_autofree gchar *sensitive = g_strdup_printf("{\"kind\":%d,\"created_at\":%ld,\"tags\":[],\"content\":\"must need review\"}", NOSTR_KIND_FOLLOW_LIST, (long)time(NULL));
        g_autofree gchar *review = request_event(f, "headless-review", "sign_event", (const gchar *const *)&sensitive, 1, time(NULL), FALSE);
        REQUIRE(raw_expect(f, "headless-review", review, ORG_NOSTR_SIGNER_ERR_NO_APPROVER), "review-required headless request fails promptly");
    }
    g_autofree gchar *principal = g_strdup_printf("nip46:%s", f->client_pk);
    REQUIRE(control(f, "SetClientTrust", g_variant_new("(ss)", principal, "medium")), "explicit Medium trust for paired control operations");
    gint64 now = time(NULL);
    g_autofree gchar *ping = request_event(f, "raw-ping", "ping", NULL, 0, now, FALSE);
    REQUIRE(raw_expect(f, "raw-ping", ping, NULL), "paired ping routes through gate");
    g_autofree gchar *relays = request_event(f, "raw-relays", "get_relays", NULL, 0, now, FALSE);
    REQUIRE(raw_expect(f, "raw-relays", relays, NULL), "get_relays returns configured data");
    g_autofree gchar *replay = request_event(f, "raw-sign", "sign_event", (const gchar *const *)&unsigned_json, 1, now, FALSE);
    REQUIRE(raw_expect(f, "raw-sign", replay, NULL), "fresh sign request");
    REQUIRE(raw_expect(f, "raw-sign", replay, NULL), "identical replay uses cached response");
    g_autofree gchar *changed = request_event(f, "raw-sign", "get_public_key", NULL, 0, now, FALSE);
    REQUIRE(raw_expect(f, "raw-sign", changed, "duplicate_rpc_changed_params"), "changed RPC rejected");
    g_autofree gchar *stale = request_event(f, "raw-stale", "sign_event", (const gchar *const *)&unsigned_json, 1, now - 601, FALSE);
    REQUIRE(raw_expect(f, "raw-stale", stale, "stale_request"), "hostile relay backfill outside window rejected");
    g_autofree gchar *future = request_event(f, "raw-future", "sign_event", (const gchar *const *)&unsigned_json, 1, now + 1200, FALSE);
    REQUIRE(raw_expect(f, "raw-future", future, "stale_request"), "future request outside skew rejected");
    g_autofree gchar *wrong = request_event(f, "raw-wrong", "sign_event", (const gchar *const *)&unsigned_json, 1, now, TRUE);
    REQUIRE(raw_expect(f, "raw-wrong", wrong, "not_paired"), "verified wrong-client envelope cannot sign");
    g_autofree gchar *unsupported = request_event(f, "raw-switch", "switch_relays", NULL, 0, now, FALSE);
    REQUIRE(raw_expect(f, "raw-switch", unsupported, "unsupported_method"), "switch_relays has no fallthrough");
    if (!f->headless) {
        g_clear_pointer(&pubkey, g_free);
        REQUIRE(nostr_nip46_client_get_public_key_rpc(f->client, &pubkey) == 0, "fresh transport barrier before pending pause");
        g_autofree gchar *sensitive = g_strdup_printf("{\"kind\":%d,\"created_at\":%ld,\"tags\":[],\"content\":\"pending pause\"}", NOSTR_KIND_FOLLOW_LIST, (long)time(NULL));
        g_autofree gchar *pending = request_event(f, "pending-pause", "sign_event", (const gchar *const *)&sensitive, 1, time(NULL), FALSE);
        g_atomic_int_set(&f->cancel_mode, 1);
        REQUIRE(raw_expect(f, "pending-pause", pending, ORG_NOSTR_SIGNER_ERR_PAUSED), "pause cancels deferred gate continuation exactly once");
        g_atomic_int_set(&f->cancel_mode, 0);
    }
    REQUIRE(control(f, "SetPaused", g_variant_new("(b)", TRUE)), "pause management");
    REQUIRE(raw_expect(f, "raw-sign", replay, "paused"), "pause beats cached successful response");
    REQUIRE(control(f, "SetPaused", g_variant_new("(b)", FALSE)), "resume management");
    REQUIRE(raw_expect(f, "raw-sign", replay, "replay_policy_changed"), "pause generation invalidates old successful cache");
    REQUIRE(restart_daemon(f), "same profile restarts with persistent endpoint and pairings");
    REQUIRE(raw_expect(f, "raw-sign", replay, "replayed_request"), "persisted seen IDs prevent execution after restart/cache loss");
    g_clear_pointer(&pubkey, g_free);
    REQUIRE(nostr_nip46_client_get_public_key_rpc(f->client, &pubkey) == 0 && !g_strcmp0(pubkey, f->account_pk),
        "paired identity and scoped policy survive restart");
    if (!f->headless) {
        g_autofree gchar *sensitive = g_strdup_printf("{\"kind\":%d,\"created_at\":%ld,\"tags\":[],\"content\":\"pending revoke\"}", NOSTR_KIND_FOLLOW_LIST, (long)time(NULL));
        g_autofree gchar *pending = request_event(f, "pending-revoke", "sign_event", (const gchar *const *)&sensitive, 1, time(NULL), FALSE);
        g_atomic_int_set(&f->cancel_mode, 2);
        REQUIRE(raw_expect(f, "pending-revoke", pending, ORG_NOSTR_SIGNER_ERR_CANCELLED), "revoke cancels deferred gate continuation exactly once");
        g_atomic_int_set(&f->cancel_mode, 0);
    }
    REQUIRE(control(f, "RevokeClient", g_variant_new("(s)", principal)), "revoke management");
    REQUIRE(raw_expect(f, "raw-sign", replay, "not_paired"), "revoke beats replayed pre-revoke request");
    REQUIRE(control(f, "ResetBunkerEndpoint", NULL), "explicit endpoint reset");
    guint8 key[32] = {0}; g_assert_cmpint(nostr_nip55l_bunker_transport_load(key), ==, 0);
    gchar hex[65]; static const gchar digits[] = "0123456789abcdef";
    for (guint i = 0; i < 32; i++) { hex[2*i] = digits[key[i] >> 4]; hex[2*i+1] = digits[key[i] & 15]; }
    hex[64] = 0; g_autofree gchar *new_pubkey = nostr_key_get_public(hex);
    secure_wipe(key, sizeof(key)); secure_wipe(hex, sizeof(hex));
    REQUIRE(new_pubkey && strcmp(new_pubkey, f->transport_pk), "only explicit reset replaces transport custody");
    g_autoptr(GVariant) clients = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer/grotto",
        "org.nostr.Grotto.Control1", "ListClients", NULL, G_VARIANT_TYPE("(aa{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
    g_autoptr(GVariant) rows = clients ? g_variant_get_child_value(clients, 0) : NULL;
    REQUIRE(rows && g_variant_n_children(rows) == 0, "endpoint reset revokes all remote pairings");
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
    gboolean running = strstr(log, "BUNKER_STATE state=LISTENING") != NULL;
    gboolean failed = strstr(log, "BUNKER_START_FAILED: state=ERROR") != NULL;
    if (running && failed) {
        g_printerr("ASSERTION FAILED: daemon claimed RUNNING after listener failure\n");
        g_main_loop_quit(f->loop);
    } else if (running || failed) {
        f->ready = TRUE;
        guint connections, requests, published;
        bunker_test_relay_stats(f->relay, &connections, &requests, &published);
        g_print("daemon reported %s; before client: connections=%u REQ=%u\n",
            failed ? "ERROR (no RUNNING claim)" : "RUNNING", connections, requests);
        /* G01 fixes reporting only. Still exercise the nonfunctional daemon
         * and fail at encrypted connect; WILL_FAIL stays until G05. */
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
    nostr_nip46_client_set_secret(observer, f->transport_sk);
    NostrNip46Session *wrong_client = nostr_nip46_client_new();
    nostr_nip46_client_set_secret(wrong_client, f->peer_sk);
    gboolean valid = TRUE;
    for (guint i = 0; i < json_array_get_length(events); i++) {
        JsonNode *node = json_array_get_element(events, i);
        g_autofree gchar *event_json = json_to_string(node, FALSE);
        NostrEvent *event = nostr_event_new();
        gchar *plain = NULL;
        valid = event && nostr_event_deserialize(event, event_json) == 0 &&
            nostr_event_get_kind(event) == NOSTR_EVENT_KIND_NIP46 && nostr_event_check_signature(event);
        if (!valid) { nostr_event_free(event); break; }
        gboolean request = g_strcmp0(nostr_event_get_pubkey(event), f->transport_pk) != 0;
        g_autofree gchar *author = g_strdup(request ? nostr_event_get_pubkey(event) : f->transport_pk);
        const gchar *target = NULL;
        valid = !request || !g_strcmp0(author, f->client_pk) || !g_strcmp0(author, f->peer_pk) || !g_strcmp0(author, f->second_pk);
        JsonArray *tags = json_object_get_array_member(json_node_get_object(node), "tags");
        guint recipients = 0;
        for (guint j = 0; tags && j < json_array_get_length(tags); j++) {
            JsonArray *tag = json_array_get_array_element(tags, j);
            if (tag && json_array_get_length(tag) >= 2 && !g_strcmp0(json_array_get_string_element(tag, 0), "p")) {
                target = json_array_get_string_element(tag, 1); recipients++;
            }
        }
        gboolean addressed = recipients == 1 && (request ? !g_strcmp0(target, f->transport_pk) :
            !g_strcmp0(target, f->client_pk) || !g_strcmp0(target, f->peer_pk) || !g_strcmp0(target, f->second_pk));
        valid = valid && addressed && g_strcmp0(nostr_event_get_pubkey(event), author) == 0 &&
            nostr_nip46_client_nip44_decrypt(request ? observer : (!g_strcmp0(target, f->peer_pk) ? wrong_client : (!g_strcmp0(target, f->second_pk) ? f->second : f->client)), author,
                nostr_event_get_content(event), &plain) == 0;
        nostr_event_free(event);
        if (!valid) { free(plain); break; }
        if (request) {
            NostrNip46Request req = {0};
            valid = nostr_nip46_request_parse(plain, &req) == 0 && req.id && req.method;
            if (valid) {
                gchar *key = g_strdup_printf("%s:%s", author, req.id);
                g_hash_table_insert(requests, key, g_strdup(req.method));
                g_print("relay request method=%s id=%s signature=valid target=transport\n", req.method, req.id);
            }
            nostr_nip46_request_free(&req);
        } else {
            NostrNip46Response resp = {0};
            valid = nostr_nip46_response_parse(plain, &resp) == 0 && resp.id && (resp.result || resp.error);
            g_autofree gchar *key = valid ? g_strdup_printf("%s:%s", target, resp.id) : NULL;
            valid = valid && g_hash_table_contains(requests, key);
            if (valid) {
                g_hash_table_add(replies, g_strdup(key));
                g_print("relay response id=%s matched decrypted request\n", resp.id);
            }
            nostr_nip46_response_free(&resp);
        }
        free(plain);
        if (!valid) break;
    }
    nostr_nip46_session_free(observer); nostr_nip46_session_free(wrong_client);
    g_print("relay decrypted requests=%u matched replies=%u\n",
        g_hash_table_size(requests), g_hash_table_size(replies));
    REQUIRE(valid, "signed NIP-44 envelopes, transport targets and matching client/RPC identities");
    if (f->passed) {
        REQUIRE(g_hash_table_size(requests) >= 14 && g_hash_table_size(replies) == g_hash_table_size(requests),
            "every unique client/RPC identity has a matching response");
        const gchar *methods[] = {"connect", "get_public_key", "sign_event", "nip44_encrypt", "nip44_decrypt", "nip04_encrypt", "nip04_decrypt", "ping", "get_relays"};
        for (guint i = 0; i < G_N_ELEMENTS(methods); i++) {
            gboolean found = FALSE;
            GHashTableIter iter; gpointer value;
            g_hash_table_iter_init(&iter, requests);
            while (g_hash_table_iter_next(&iter, NULL, &value))
                if (g_strcmp0(value, methods[i]) == 0) found = TRUE;
            REQUIRE(found, "all nine NIP-46 methods observed at the relay");
        }
    }
    return TRUE;
}

static void approved(GObject *source, GAsyncResult *result, gpointer data) {
    Fixture *f = data; g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    g_assert_no_error(error); g_assert_nonnull(reply); f->approvals++;
}
static void approval_requested(GDBusConnection *bus, const gchar *sender, const gchar *path,
    const gchar *interface, const gchar *signal, GVariant *args, gpointer data) {
    (void)sender; (void)path; (void)interface; (void)signal;
    Fixture *f = data; const gchar *who, *identity, *op, *preview, *id;
    g_assert_false(f->headless); /* no ApprovalRequested/GUI summon in headless mode */
    g_variant_get(args, "(&s&s&s&s&s)", &who, &identity, &op, &preview, &id);
    g_autofree gchar *expected = g_strdup_printf("nip46:%s", f->client_pk);
    g_autofree gchar *expected_second = g_strdup_printf("nip46:%s", f->second_pk);
    g_assert_true(!g_strcmp0(who, expected) || !g_strcmp0(who, expected_second));
    gint cancel_mode = g_atomic_int_get(&f->cancel_mode);
    if (cancel_mode && !strcmp(op, "event")) {
        g_dbus_connection_call(bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1",
            cancel_mode == 1 ? "SetPaused" : "RevokeClient", cancel_mode == 1 ? g_variant_new("(b)", TRUE) : g_variant_new("(s)", who),
            NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, approved, f);
        return;
    }
    if (g_atomic_int_get(&f->defer_sign) && !strcmp(op, "event")) {
        g_ptr_array_add(f->deferred_ids, g_strdup(id));
        if (f->deferred_ids->len < 2) return; /* prove dispatcher remains live while waiting */
        for (guint i = 0; i < f->deferred_ids->len; i++)
            g_dbus_connection_call(bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer",
                "ApproveRequest", g_variant_new("(sbbt)", (gchar *)f->deferred_ids->pdata[i], TRUE, FALSE, (guint64)0),
                NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, approved, f);
        return;
    }
    g_dbus_connection_call(bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer",
        "ApproveRequest", g_variant_new("(sbbt)", id, TRUE, TRUE, (guint64)600), NULL,
        G_DBUS_CALL_FLAGS_NONE, 10000, NULL, approved, f);
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
        "bunker-allowed-pubkeys=['%s','%s']\nbunker-auto-approve-kinds=['%d']\n"
        "bunker-allowed-methods=['connect','get_public_key','sign_event','nip44_encrypt','nip44_decrypt','nip04_encrypt','nip04_decrypt','ping','get_relays']\n"
        "default-identity='%s'\n", bunker_test_relay_url(f->relay), f->client_pk, f->second_pk,
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

static void provision_headless(Fixture *f) {
    guint8 public_key[32]; gchar *identity = NULL;
    g_assert_true(nostr_hex2bin(public_key, f->account_pk, 32));
    g_assert_cmpint(nostr_nip19_encode_npub(public_key, &identity), ==, 0);
    g_autoptr(GrottoClients) clients = grotto_clients_new(NULL, NULL);
    g_autoptr(SignerPermissions) permissions = signer_permissions_new(NULL);
    g_assert_nonnull(clients); g_assert_true(signer_permissions_reload(permissions, NULL));
    const gchar *pubkeys[] = {f->client_pk, f->second_pk};
    const gchar *operations[] = {"connect", "nip04_encrypt", "nip04_decrypt", "nip44_encrypt", "nip44_decrypt"};
    for (guint i = 0; i < G_N_ELEMENTS(pubkeys); i++) {
        g_autofree gchar *principal = g_strdup_printf("nip46:%s", pubkeys[i]);
        g_assert_true(grotto_clients_record(clients, principal, "NIP46", "", identity, TRUE, FALSE, NULL));
        g_assert_true(grotto_clients_set_tier(clients, principal, SIGNER_TRUST_MEDIUM, NULL));
        g_assert_true(grotto_clients_set_pairing(clients, principal, TRUE, NULL, NULL));
        for (guint j = 0; j < G_N_ELEMENTS(operations); j++)
            g_assert_true(signer_permissions_remember(permissions, principal, identity, operations[j], -1, TRUE, 600, NULL));
    }
    free(identity);
}
int main(int argc, char **argv)
{
    if ((argc != 4 && argc != 5) || g_strcmp0(g_getenv("NOSTRC_TEST_ISOLATION"), "1") != 0) {
        g_printerr("Run through CTest: private HOME/XDG and NOSTRC_TEST_ISOLATION are required\n");
        return 1;
    }
    if (!nostrc_test_bus_available()) return 77;
    nostr_json_init();
    Fixture f = {0}; f.headless = argc == 5; f.daemon_binary = argv[1];
    f.account_sk = nostr_key_generate_private(); f.account_pk = nostr_key_get_public(f.account_sk);
    f.client_sk = nostr_key_generate_private(); f.client_pk = nostr_key_get_public(f.client_sk);
    f.second_sk = nostr_key_generate_private(); f.second_pk = nostr_key_get_public(f.second_sk);
    f.second = nostr_nip46_client_new(); f.deferred_ids = g_ptr_array_new_with_free_func(g_free);
    f.peer_sk = nostr_key_generate_private(); f.peer_pk = nostr_key_get_public(f.peer_sk);
    f.transport_sk = nostr_key_generate_private(); f.transport_pk = nostr_key_get_public(f.transport_sk);
    g_setenv("NOSTR_SIGNER_TEST_TRANSPORT_BACKEND", "file", TRUE);
    guint8 transport[32]; g_assert_true(nostr_hex2bin(transport, f.transport_sk, 32));
    g_assert_cmpint(nostr_nip55l_bunker_transport_store(transport), ==, 0); secure_wipe(transport, 32);
    g_autofree gchar *test_binary = g_canonicalize_filename(argv[0], NULL);
    g_setenv("NOSTR_SIGNER_TEST_APPROVERS", test_binary, TRUE);
    g_assert_nonnull(f.account_pk); g_assert_nonnull(f.client_pk); g_assert_nonnull(f.peer_pk);
    f.client = nostr_nip46_client_new();
    g_assert_nonnull(f.client);
    f.bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
    nostrc_test_bus_up(f.bus);
    f.approver = nostrc_test_bus_connect(f.bus);
    f.approver_name = g_bus_own_name_on_connection(f.approver, "org.nostr.Grotto", G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
    f.approval_subscription = g_dbus_connection_signal_subscribe(f.approver, NULL, "org.nostr.Signer",
        "ApprovalRequested", "/org/nostr/signer", NULL, G_DBUS_SIGNAL_FLAGS_NONE, approval_requested, &f, NULL);
    g_mutex_init(&f.capture_mutex); g_cond_init(&f.capture_cond);
    f.captured = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    f.relay = bunker_test_relay_new();
    bunker_test_relay_set_event_callback(f.relay, capture_response, &f);
    f.loop = g_main_loop_new(NULL, FALSE);
    f.log_path = g_build_filename(nostrc_test_bus_get_dir(f.bus), "grotto-daemon.log", NULL);
    gboolean configured = configure_schema(&f, argv[2], argv[3]);
    if (f.headless) provision_headless(&f);
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
        const gchar *command[] = {argv[1], "--bunker", f.headless ? "--no-approval-ui" : NULL, NULL};
        const gchar *wrapped[] = {wrapper, argv[1], "--bunker", f.headless ? "--no-approval-ui" : NULL, NULL};
        f.daemon = nostrc_test_bus_spawn_supervised(f.bus, "grotto-daemon.log", NULL, wrapper ? wrapped : command);
        timeout_id = g_timeout_add_seconds(40, deadline, &f);
        check_daemon_log(&f);
        g_main_loop_run(f.loop);
    }
    if (timeout_id && !f.timed_out) g_source_remove(timeout_id);
    if (monitor) g_file_monitor_cancel(monitor);
    if (f.worker) g_thread_join(f.worker);
    if (f.restart_monitor) { g_file_monitor_cancel(f.restart_monitor); g_object_unref(f.restart_monitor); }
    guint connections, requests, published;
    bunker_test_relay_stats(f.relay, &connections, &requests, &published);
    g_print("relay evidence: connections=%u REQ=%u published=%u\n",
        connections, requests, published);
    gboolean audited = audit_frames(&f);
    nostrc_test_bus_dump_log(f.bus, "grotto-daemon.log");
    nostr_nip46_session_free(f.client); nostr_nip46_session_free(f.second);
    g_ptr_array_unref(f.deferred_ids);
    bunker_test_relay_free(f.relay);
    g_dbus_connection_signal_unsubscribe(f.approver, f.approval_subscription);
    g_bus_unown_name(f.approver_name);
    nostrc_test_bus_down(f.bus);
    g_main_loop_unref(f.loop);
    g_hash_table_unref(f.captured); g_mutex_clear(&f.capture_mutex); g_cond_clear(&f.capture_cond);
    g_free(f.log_path);
    memset(f.account_sk, 0, strlen(f.account_sk)); free(f.account_sk); free(f.account_pk);
    memset(f.client_sk, 0, strlen(f.client_sk)); free(f.client_sk); free(f.client_pk);
    memset(f.second_sk, 0, strlen(f.second_sk)); free(f.second_sk); free(f.second_pk);
    memset(f.peer_sk, 0, strlen(f.peer_sk)); free(f.peer_sk); free(f.peer_pk);
    memset(f.transport_sk, 0, strlen(f.transport_sk)); free(f.transport_sk); free(f.transport_pk);
    g_unsetenv("NOSTR_SIGNER_TEST_TRANSPORT_BACKEND");
    g_unsetenv("NOSTR_SIGNER_TEST_APPROVERS");
    g_unsetenv("NOSTR_SIGNER_SECKEY_HEX");
    nostr_json_cleanup();
    return configured && f.ready && f.passed && audited && !f.timed_out ? 0 : 1;
}
