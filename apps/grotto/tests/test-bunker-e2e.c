/* G00's shipped-daemon proof, repaired and extended for G05. No mock service
 * or replacement signer stands in for grotto-daemon. */
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>
#include "nostrc-test-bus.h"
#include "bunker_test_relay.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip46/nip46_uri.h"
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
    BunkerTestRelay *relay, *relay2;
    NostrNip46Session *client;
    GMainLoop *loop;
    GThread *worker;
    gchar *log_path;
    gchar *account_sk, *account_pk, *client_sk, *client_pk, *peer_sk, *peer_pk, *transport_sk, *transport_pk;
    GDBusConnection *approver;
    guint approver_name, approval_subscription, approvals;
    GMutex capture_mutex; GCond capture_cond; GHashTable *captured, *capture_relays;
    NostrNip46Session *second;
    gchar *second_sk, *second_pk;
    gint defer_sign, cancel_mode, defer_connect;
    gchar *connect_review, *uri_ack_id;
    guint status_subscription, status_events;
    gint approver_pending;
    gboolean headless;
    const gchar *daemon_binary;
    GSubprocess *daemon;
    GFileMonitor *restart_monitor;
    gboolean restarted, restart_ok, reconnected;
    gint64 reconnect_since;
    GPtrArray *deferred_ids, *sensitive;
    gboolean ready, passed, timed_out;
} Fixture;

#define REQUIRE(condition, message) do { \
    if (!(condition)) { g_printerr("ASSERTION FAILED: %s\n", message); return FALSE; } \
} while (0)

typedef struct { Fixture *fixture; gchar *event; BunkerTestRelay *relay; } Injection;
static gboolean inject_on_main(gpointer data) {
    Injection *job = data; bunker_test_relay_inject(job->relay ? job->relay : job->fixture->relay, job->event);
    g_free(job->event); g_free(job); return G_SOURCE_REMOVE;
}
static void capture_response_on(const gchar *event_json, gpointer data, guint relay) {
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
    if (!plain && f->second) nostr_nip46_client_nip44_decrypt(f->second, f->transport_pk, nostr_event_get_content(event), &plain);
    nostr_event_free(event);
    NostrNip46Response reply = {0};
    if (plain && !nostr_nip46_response_parse(plain, &reply) && reply.id) {
        g_mutex_lock(&f->capture_mutex);
        g_hash_table_replace(f->captured, g_strdup(reply.id), g_strdup(plain));
        guint seen = GPOINTER_TO_UINT(g_hash_table_lookup(f->capture_relays, reply.id));
        g_hash_table_replace(f->capture_relays, g_strdup(reply.id), GUINT_TO_POINTER(seen | relay));
        g_cond_broadcast(&f->capture_cond); g_mutex_unlock(&f->capture_mutex);
    }
    nostr_nip46_response_free(&reply); free(plain);
}
static void capture_response_a(const gchar *event, gpointer data) { capture_response_on(event, data, 1); }
static void capture_response_b(const gchar *event, gpointer data) { capture_response_on(event, data, 2); }
static guint captured_lane(Fixture *f, const gchar *id) {
    g_mutex_lock(&f->capture_mutex); guint lane = GPOINTER_TO_UINT(g_hash_table_lookup(f->capture_relays, id));
    g_mutex_unlock(&f->capture_mutex); return lane;
}
static gchar *request_event(Fixture *f, const gchar *id, const gchar *method,
    const gchar *const *params, gsize count, gint64 created, guint lane) {
    NostrNip46Session *sender = nostr_nip46_client_new();
    const gchar *sk = lane == 2 ? f->second_sk : lane ? f->peer_sk : f->client_sk;
    nostr_nip46_client_set_secret(sender, sk);
    g_autofree gchar *plain = nostr_nip46_request_build(id, method, params, count);
    gchar *cipher = NULL; g_assert_cmpint(nostr_nip46_client_nip44_encrypt(sender, f->transport_pk, plain, &cipher), ==, 0);
    nostr_nip46_session_free(sender);
    NostrEvent *event = nostr_event_new(); nostr_event_set_kind(event, NOSTR_EVENT_KIND_NIP46);
    nostr_event_set_pubkey(event, lane == 2 ? f->second_pk : lane ? f->peer_pk : f->client_pk);
    nostr_event_set_created_at(event, created); nostr_event_set_content(event, cipher); free(cipher);
    nostr_event_set_tags(event, nostr_tags_new(1, nostr_tag_new("p", f->transport_pk, NULL)));
    g_assert_cmpint(nostr_event_sign(event, sk), ==, 0);
    gchar *serialized = nostr_event_serialize(event); nostr_event_free(event); return serialized;
}
static gchar *send_raw_on(Fixture *f, BunkerTestRelay *relay, const gchar *id, const gchar *event) {
    g_mutex_lock(&f->capture_mutex); g_hash_table_remove(f->captured, id); g_hash_table_remove(f->capture_relays, id);
    Injection *job = g_new0(Injection, 1); job->fixture = f; job->event = g_strdup(event); job->relay = relay;
    g_idle_add(inject_on_main, job);
    gint64 bound = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (!g_hash_table_contains(f->captured, id)) {
        if (!g_cond_wait_until(&f->capture_cond, &f->capture_mutex, bound)) {
            g_mutex_unlock(&f->capture_mutex); g_printerr("raw RPC %s had no response\n", id); return NULL;
        }
    }
    gchar *reply = g_strdup(g_hash_table_lookup(f->captured, id)); g_mutex_unlock(&f->capture_mutex); return reply;
}
static gchar *send_raw(Fixture *f, const gchar *id, const gchar *event) { return send_raw_on(f, f->relay, id, event); }
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
static gboolean grant_scoped_account_ops(Fixture *f, guint crypto_ttl) {
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
        g_variant_builder_add(&row, "{sv}", "expires-at", g_variant_new_uint64(time(NULL) + (i >= 3 ? crypto_ttl : 600)));
        g_variant_builder_add_value(&rows, g_variant_builder_end(&row));
    }
    gboolean ok = control(f, "SetPermissions", g_variant_new("(ss@aa{sv}t)", principal, identity, g_variant_builder_end(&rows), revision));
    free(identity); return ok;
}
typedef struct { Fixture *f; guint delay_ms; gboolean done; } ExpiryWait;
static gboolean expiry_elapsed(gpointer data) {
    ExpiryWait *wait = data;
    g_mutex_lock(&wait->f->capture_mutex); wait->done = TRUE;
    g_cond_broadcast(&wait->f->capture_cond); g_mutex_unlock(&wait->f->capture_mutex);
    return G_SOURCE_REMOVE;
}
static gboolean arm_expiry(gpointer data) {
    ExpiryWait *wait = data; g_timeout_add(wait->delay_ms, expiry_elapsed, wait); return G_SOURCE_REMOVE;
}
static void await_expiry(Fixture *f, gint64 expires) {
    /* The absolute permission expiry is the event under test, not a readiness
     * sleep/poll. Wake the worker once that boundary has elapsed. */
    ExpiryWait wait = {f, (guint)MAX(1, expires - time(NULL) + 1) * 1000, FALSE};
    g_mutex_lock(&f->capture_mutex); g_idle_add(arm_expiry, &wait);
    while (!wait.done) g_cond_wait(&f->capture_cond, &f->capture_mutex);
    g_mutex_unlock(&f->capture_mutex);
}
static guint64 activity_sequence(Fixture *f) {
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer/grotto",
        "org.nostr.Grotto.Control1", "GetStatus", NULL, G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
    g_assert_nonnull(reply); g_autoptr(GVariant) status = g_variant_get_child_value(reply, 0);
    guint64 sequence = 0; g_assert_true(g_variant_lookup(status, "activity_sequence", "t", &sequence)); return sequence;
}
static gboolean expiry_replay(Fixture *f) {
    const guint ttl = 5;
    REQUIRE(grant_scoped_account_ops(f, ttl), "explicit short-lived crypto scopes");
    gint64 expires = time(NULL) + ttl;
    const gchar *encrypt_params[] = {f->peer_pk, "cached expiry secret"};
    g_autofree gchar *encrypt = request_event(f, "expiry-encrypt", "nip44_encrypt", encrypt_params, 2, time(NULL), FALSE);
    NostrNip46Session *peer = nostr_nip46_client_new(); nostr_nip46_client_set_secret(peer, f->peer_sk);
    gchar *cipher = NULL;
    g_assert_cmpint(nostr_nip46_client_nip44_encrypt(peer, f->account_pk, "cached decrypted secret", &cipher), ==, 0);
    nostr_nip46_session_free(peer);
    const gchar *decrypt_params[] = {f->peer_pk, cipher};
    g_autofree gchar *decrypt = request_event(f, "expiry-decrypt", "nip44_decrypt", decrypt_params, 2, time(NULL), FALSE);
    free(cipher);
    REQUIRE(raw_expect(f, "expiry-encrypt", encrypt, NULL) && raw_expect(f, "expiry-decrypt", decrypt, NULL),
        "approved short-lived scopes produce encrypt and decrypt results");
    await_expiry(f, expires);
    guint64 before = activity_sequence(f);
    REQUIRE(raw_expect(f, "expiry-encrypt", encrypt, ORG_NOSTR_SIGNER_ERR_APPROVAL) &&
        raw_expect(f, "expiry-decrypt", decrypt, ORG_NOSTR_SIGNER_ERR_APPROVAL),
        "same RPC ids after absolute expiry deny cached ciphertext and plaintext");
    REQUIRE(activity_sequence(f) == before, "expired cache replays never enter gate/backend or record another key operation");
    REQUIRE(grant_scoped_account_ops(f, 600), "restore scopes for remaining lifecycle checks");
    guint8 public_key[32]; gchar *identity = NULL;
    g_assert_true(nostr_hex2bin(public_key, f->account_pk, 32));
    g_assert_cmpint(nostr_nip19_encode_npub(public_key, &identity), ==, 0);
    g_autofree gchar *principal = g_strdup_printf("nip46:%s", f->client_pk);
    g_autoptr(SignerPermissions) permissions = signer_permissions_new(NULL);
    REQUIRE(signer_permissions_reload(permissions, NULL) &&
        signer_permissions_remember(permissions, principal, identity, "nip44_decrypt", -1, FALSE, 600, NULL),
        "explicitly revoke just the decrypt scope while retaining pairing");
    free(identity); before = activity_sequence(f);
    REQUIRE(raw_expect(f, "expiry-decrypt", decrypt, ORG_NOSTR_SIGNER_ERR_APPROVAL), "scope revoke denies same RPC id");
    REQUIRE(activity_sequence(f) == before, "revoked cache replay has no gate/backend invocation");
    REQUIRE(grant_scoped_account_ops(f, 600), "restore explicitly provisioned scope");
    return TRUE;
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
static gboolean drain_approver(Fixture *f) {
    /* The relay refires history on a new REQ. Drain the fixture's explicit
     * decisions before deliberately killing its D-Bus method recipient. */
    g_mutex_lock(&f->capture_mutex); gint64 decision_bound = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (g_atomic_int_get(&f->approver_pending)) {
        if (!g_cond_wait_until(&f->capture_cond, &f->capture_mutex, decision_bound)) { g_mutex_unlock(&f->capture_mutex); return FALSE; }
    }
    g_mutex_unlock(&f->capture_mutex); return TRUE;
}
static gboolean restart_daemon(Fixture *f) {
    if (!drain_approver(f)) return FALSE;
    g_subprocess_send_signal(f->daemon, SIGTERM);
    if (!g_subprocess_wait_check(f->daemon, NULL, NULL)) {
        nostrc_test_bus_dump_log(f->bus, "grotto-daemon.log"); return FALSE;
    }
    g_mutex_lock(&f->capture_mutex); g_idle_add(restart_on_main, f);
    gint64 bound = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
    while (!f->restarted) {
        if (!g_cond_wait_until(&f->capture_cond, &f->capture_mutex, bound)) {
            g_mutex_unlock(&f->capture_mutex); return FALSE;
        }
    }
    gboolean ok = f->restart_ok; g_mutex_unlock(&f->capture_mutex); return ok;
}
static void reconnect_request(JsonArray *request, gpointer data) {
    Fixture *f = data;
    for (guint i = 2; i < json_array_get_length(request); i++) {
        JsonObject *filter = json_array_get_object_element(request, i);
        if (!json_object_has_member(filter, "#p")) continue;
        JsonArray *targets = json_object_get_array_member(filter, "#p");
        for (guint j = 0; j < json_array_get_length(targets); j++) {
            if (g_strcmp0(json_array_get_string_element(targets, j), f->transport_pk)) continue;
            g_mutex_lock(&f->capture_mutex);
            f->reconnect_since = json_object_get_int_member(filter, "since"); f->reconnected = TRUE;
            g_cond_broadcast(&f->capture_cond); g_mutex_unlock(&f->capture_mutex);
            bunker_test_relay_set_request_callback(f->relay, NULL, NULL); return;
        }
    }
}
static gboolean reconnect_on_main(gpointer data) {
    Fixture *f = data;
    bunker_test_relay_set_request_callback(f->relay, reconnect_request, f);
    bunker_test_relay_disconnect_recipient(f->relay, f->transport_pk); return G_SOURCE_REMOVE;
}
static gboolean reconnect_bunker(Fixture *f) {
    g_mutex_lock(&f->capture_mutex); g_idle_add(reconnect_on_main, f);
    gint64 bound = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
    while (!f->reconnected) {
        if (!g_cond_wait_until(&f->capture_cond, &f->capture_mutex, bound)) {
            g_mutex_unlock(&f->capture_mutex); return FALSE;
        }
    }
    gint64 since = f->reconnect_since, now = time(NULL); g_mutex_unlock(&f->capture_mutex);
    REQUIRE(since >= now - 601 && since <= now - 599, "daemon reconnect REQ stays in bounded recovery window, not rejected future timestamp");
    g_autofree gchar *normal = request_event(f, "after-future-reconnect", "get_public_key", NULL, 0, time(NULL), FALSE);
    return raw_expect(f, "after-future-reconnect", normal, NULL);
}
typedef struct { Fixture *f; const gchar *id, *event, *error; gboolean ok; } RawJob;
static gpointer raw_worker(gpointer data) {
    RawJob *job = data; job->ok = raw_expect(job->f, job->id, job->event, job->error); return NULL;
}
static gchar *await_connect_review(Fixture *f) {
    g_mutex_lock(&f->capture_mutex); gint64 bound = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (!f->connect_review) if (!g_cond_wait_until(&f->capture_cond, &f->capture_mutex, bound)) break;
    gchar *id = g_steal_pointer(&f->connect_review); g_mutex_unlock(&f->capture_mutex); return id;
}
static GVariant *create_offer(Fixture *f, const gchar *identity) {
    GVariant *reply = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer/grotto",
        "org.nostr.Grotto.Control1", "CreatePairing", g_variant_new("(ss)", identity, "Fixture client"),
        G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
    if (!reply) return NULL;
    GVariant *offer = g_variant_get_child_value(reply, 0); g_variant_unref(reply);
    const gchar *text = NULL; g_assert_true(g_variant_lookup(offer, "uri", "&s", &text));
    g_ptr_array_add(f->sensitive, g_strdup(text));
    NostrNip46BunkerURI parsed = {0}; g_assert_cmpint(nostr_nip46_uri_parse_bunker(text, &parsed), ==, 0);
    g_ptr_array_add(f->sensitive, g_strdup(parsed.secret)); secure_wipe(parsed.secret, strlen(parsed.secret)); nostr_nip46_uri_bunker_free(&parsed);
    return offer;
}
typedef struct { Fixture *f; BunkerTestRelay *relay; gboolean hold, done; } AckControl;
static gboolean ack_control_on_owner(gpointer data) {
    AckControl *c = data; bunker_test_relay_hold_acks(c->relay, c->hold);
    g_mutex_lock(&c->f->capture_mutex); c->done = TRUE; g_cond_broadcast(&c->f->capture_cond); g_mutex_unlock(&c->f->capture_mutex); return G_SOURCE_REMOVE;
}
static void hold_acks(Fixture *f, BunkerTestRelay *relay, gboolean hold) {
    AckControl c = {f, relay, hold, FALSE}; g_mutex_lock(&f->capture_mutex); g_idle_add(ack_control_on_owner, &c);
    while (!c.done) g_cond_wait(&f->capture_cond, &f->capture_mutex);
    g_mutex_unlock(&f->capture_mutex);
}
static gboolean client_pairing_state(Fixture *f, const gchar *client, gboolean paired, gboolean pending, gboolean wait) {
    g_autofree gchar *principal = g_strdup_printf("nip46:%s", client);
    gint64 bound = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    for (;;) {
        g_mutex_lock(&f->capture_mutex); guint revision = f->status_events; g_mutex_unlock(&f->capture_mutex);
        g_autoptr(GVariant) reply = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1",
            "ListClients", NULL, G_VARIANT_TYPE("(aa{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
        REQUIRE(reply, "read pairing publication state"); g_autoptr(GVariant) rows = g_variant_get_child_value(reply, 0);
        for (gsize i = 0; i < g_variant_n_children(rows); i++) {
            g_autoptr(GVariant) row = g_variant_get_child_value(rows, i); const gchar *p = NULL; gboolean actual = FALSE, awaiting = FALSE;
            g_variant_lookup(row, "principal", "&s", &p); g_variant_lookup(row, "paired", "b", &actual); g_variant_lookup(row, "pairing-pending-ack", "b", &awaiting);
            if (!g_strcmp0(p, principal) && actual == paired && awaiting == pending) return TRUE;
        }
        if (!wait) return FALSE;
        g_mutex_lock(&f->capture_mutex); gboolean timely = TRUE;
        while (f->status_events == revision && timely) timely = g_cond_wait_until(&f->capture_cond, &f->capture_mutex, bound);
        g_mutex_unlock(&f->capture_mutex); if (!timely) return FALSE;
    }
}
static gboolean configure_relays(Fixture *, gboolean, const gchar *const *, gsize);
static gboolean token_pairing(Fixture *f) {
    REQUIRE(!nostr_nip46_client_set_secret(f->client, f->client_sk), "capture lane has its client transport key before token tests");
    guint8 pk[32]; gchar *npub = NULL;
    REQUIRE(nostr_hex2bin(pk, f->account_pk, 32) && !nostr_nip19_encode_npub(pk, &npub), "token binds explicit account");
    g_autofree gchar *identity = npub;
    GVariant *offers[4] = {0};
    for (guint i = 0; i < 4; i++) { offers[i] = create_offer(f, identity); REQUIRE(offers[i], "four offers can be outstanding"); }
    g_autoptr(GVariant) fifth = create_offer(f, identity); REQUIRE(!fifth, "fifth outstanding offer fails closed");
    const gchar *uri_text = NULL, *offer_id = NULL; guint64 expires = 0;
    g_variant_lookup(offers[0], "uri", "&s", &uri_text); g_variant_lookup(offers[0], "pairing_id", "&s", &offer_id);
    REQUIRE(g_variant_lookup(offers[0], "expires_at", "t", &expires) && expires >= (guint64)time(NULL) + 8 && expires <= (guint64)time(NULL) + 10, "isolated test clock shortens the production five-minute offer");
    NostrNip46BunkerURI uri = {0}; REQUIRE(!nostr_nip46_uri_parse_bunker(uri_text, &uri), "issued URI uses existing parser");
    REQUIRE(uri.secret && strlen(uri.secret) == 43 && !strpbrk(uri.secret, "+/="), "32 random bytes are URL-safe encoded");
    REQUIRE(!strcmp(uri.remote_signer_pubkey_hex, f->transport_pk) && strcmp(uri.remote_signer_pubkey_hex, f->account_pk), "offer names transport, not account key");
    for (guint i = 1; i < 4; i++) {
        const gchar *id = NULL; g_variant_lookup(offers[i], "pairing_id", "&s", &id);
        REQUIRE(control(f, "CancelPairing", g_variant_new("(s)", id)), "cancel invalidates outstanding offer"); g_variant_unref(offers[i]);
    }
    const gchar *params[] = {f->transport_pk, uri.secret, "sign_event:1"};
    g_autofree gchar *peer_event = request_event(f, "token-peer", "connect", params, 3, time(NULL), TRUE);
    g_atomic_int_set(&f->defer_connect, TRUE);
    RawJob job = {f, "token-peer", peer_event, NULL, FALSE}; GThread *thread = g_thread_new("token-connect", raw_worker, &job);
    g_autofree gchar *review = await_connect_review(f); g_assert_nonnull(review);
    g_autofree gchar *principal = g_strdup_printf("nip46:%s", f->peer_pk);
    g_assert_true(control(f, "SetClientTrust", g_variant_new("(ss)", principal, "medium")));
    g_autoptr(GVariant) staged = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1",
        "ListClients", NULL, G_VARIANT_TYPE("(aa{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
    g_assert_nonnull(staged); g_autoptr(GVariant) staged_rows = g_variant_get_child_value(staged, 0);
    g_assert_cmpuint(g_variant_n_children(staged_rows), ==, 0); /* tier choice is not admission */
    g_autofree gchar *other = request_event(f, "token-other", "connect", params, 3, time(NULL), FALSE);
    g_assert_true(raw_expect(f, "token-other", other, "pairing_reserved"));
    hold_acks(f, f->relay, TRUE);
    g_autoptr(GVariant) approved = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer",
        "ApproveRequest", g_variant_new("(sbbt)", review, TRUE, FALSE, (guint64)0), NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
    g_assert_nonnull(approved); g_thread_join(thread); g_atomic_int_set(&f->defer_connect, FALSE);
    REQUIRE(job.ok, "explicit review consumes reserved offer and acknowledges its authenticated peer");
    REQUIRE(client_pairing_state(f, f->peer_pk, FALSE, TRUE, FALSE), "sent acknowledgement without relay OK remains pending, never paired");
    hold_acks(f, f->relay, FALSE);
    REQUIRE(client_pairing_state(f, f->peer_pk, TRUE, FALSE, TRUE), "relay OK completes durable pairing");
    GVariant *released[4] = {0};
    for (guint i = 0; i < 4; i++) { released[i] = create_offer(f, identity); REQUIRE(released[i], "consumed offer immediately releases all four capacity slots"); }
    for (guint i = 0; i < 4; i++) {
        const gchar *released_id = NULL; g_variant_lookup(released[i], "pairing_id", "&s", &released_id);
        REQUIRE(control(f, "CancelPairing", g_variant_new("(s)", released_id)), "cancel capacity probe"); g_variant_unref(released[i]);
    }
    g_autoptr(GVariant) admitted = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1",
        "ListClients", NULL, G_VARIANT_TYPE("(aa{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
    REQUIRE(admitted, "read atomic pairing and tier persistence");
    g_autofree gchar *admitted_text = g_variant_print(admitted, FALSE);
    REQUIRE(strstr(admitted_text, f->peer_pk) && strstr(admitted_text, "medium"), "explicit Medium choice applies only with successful pairing");
    REQUIRE(control(f, "SetClientTrust", g_variant_new("(ss)", principal, "low")), "lower trust without revoking pairing");
    REQUIRE(raw_expect(f, "token-peer", peer_event, NULL), "accepted connect retries acknowledge pairing even after unrelated permission changes");
    g_autofree gchar *reuse = request_event(f, "token-reuse", "connect", params, 3, time(NULL), FALSE);
    REQUIRE(raw_expect(f, "token-reuse", reuse, "invalid_pairing_token"), "consumed token does not admit a different even-legacy-admitted client");
    REQUIRE(control(f, "RevokeClient", g_variant_new("(s)", principal)), "revoke token-paired client");
    REQUIRE(raw_expect(f, "token-peer", peer_event, "invalid_pairing_token"), "revoke beats cached token acknowledgement");
    secure_wipe(uri.secret, strlen(uri.secret)); nostr_nip46_uri_bunker_free(&uri); g_variant_unref(offers[0]);
    g_autoptr(GVariant) cancelled = create_offer(f, identity); REQUIRE(cancelled, "offer capacity released by cancellation/revoke");
    const gchar *cancel_uri = NULL, *cancel_id = NULL;
    g_variant_lookup(cancelled, "uri", "&s", &cancel_uri); g_variant_lookup(cancelled, "pairing_id", "&s", &cancel_id);
    NostrNip46BunkerURI cancel = {0}; REQUIRE(!nostr_nip46_uri_parse_bunker(cancel_uri, &cancel), "cancel offer parses");
    const gchar *cancel_params[] = {f->transport_pk, cancel.secret};
    g_autofree gchar *pending = request_event(f, "token-cancel", "connect", cancel_params, 2, time(NULL), TRUE);
    g_atomic_int_set(&f->defer_connect, TRUE); RawJob cancelled_job = {f, "token-cancel", pending, ORG_NOSTR_SIGNER_ERR_CANCELLED, FALSE};
    thread = g_thread_new("cancel-connect", raw_worker, &cancelled_job);
    g_autofree gchar *pending_id = await_connect_review(f); g_assert_nonnull(pending_id);
    g_assert_true(control(f, "CancelPairing", g_variant_new("(s)", cancel_id)));
    g_thread_join(thread); g_atomic_int_set(&f->defer_connect, FALSE);
    secure_wipe(cancel.secret, strlen(cancel.secret)); nostr_nip46_uri_bunker_free(&cancel);
    REQUIRE(cancelled_job.ok, "pairing teardown cancels the pending shared-gate continuation");
    g_autoptr(GVariant) expiring = create_offer(f, identity); REQUIRE(expiring, "create expiring offer");
    const gchar *expiring_uri = NULL; guint64 expiry = 0;
    g_variant_lookup(expiring, "uri", "&s", &expiring_uri); g_variant_lookup(expiring, "expires_at", "t", &expiry);
    NostrNip46BunkerURI expired = {0}; REQUIRE(!nostr_nip46_uri_parse_bunker(expiring_uri, &expired), "expiry URI parses");
    await_expiry(f, expiry);
    const gchar *expired_params[] = {f->transport_pk, expired.secret};
    g_autofree gchar *expired_event = request_event(f, "token-expired", "connect", expired_params, 2, time(NULL), TRUE);
    REQUIRE(raw_expect(f, "token-expired", expired_event, "invalid_pairing_token"), "actual elapsed token lifetime rejects a fresh authenticated request");
    secure_wipe(expired.secret, strlen(expired.secret)); nostr_nip46_uri_bunker_free(&expired);
    g_autoptr(GVariant) denial = create_offer(f, identity); REQUIRE(denial, "create deny fixture");
    const gchar *denial_uri = NULL; g_variant_lookup(denial, "uri", "&s", &denial_uri);
    NostrNip46BunkerURI denied_token = {0}; REQUIRE(!nostr_nip46_uri_parse_bunker(denial_uri, &denied_token), "denial offer parses");
    const gchar *denial_params[] = {f->transport_pk, denied_token.secret};
    g_autofree gchar *denial_event = request_event(f, "token-denied", "connect", denial_params, 2, time(NULL), TRUE);
    g_atomic_int_set(&f->defer_connect, TRUE); RawJob denied_job = {f, "token-denied", denial_event, ORG_NOSTR_SIGNER_ERR_APPROVAL, FALSE};
    thread = g_thread_new("denied-connect", raw_worker, &denied_job);
    g_autofree gchar *denial_id = await_connect_review(f); REQUIRE(denial_id, "receive token review before denial");
    g_autoptr(GVariant) denied_reply = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer",
        "ApproveRequest", g_variant_new("(sbbt)", denial_id, FALSE, FALSE, (guint64)0), NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
    REQUIRE(denied_reply, "deny pairing explicitly"); g_thread_join(thread); g_atomic_int_set(&f->defer_connect, FALSE);
    REQUIRE(denied_job.ok, "denial returns terminal error");
    g_autofree gchar *retry_denial = request_event(f, "token-denied-retry", "connect", denial_params, 2, time(NULL), TRUE);
    REQUIRE(raw_expect(f, "token-denied-retry", retry_denial, "invalid_pairing_token"), "denial invalidates the reserved token, not just its RPC");
    secure_wipe(denied_token.secret, strlen(denied_token.secret)); nostr_nip46_uri_bunker_free(&denied_token);
    for (guint mode = 0; mode < 2; mode++) {
        g_autoptr(GVariant) invalidated = create_offer(f, identity); REQUIRE(invalidated, "create invalidation fixture");
        const gchar *text = NULL; g_variant_lookup(invalidated, "uri", "&s", &text);
        NostrNip46BunkerURI old = {0}; REQUIRE(!nostr_nip46_uri_parse_bunker(text, &old), "invalidation URI parses");
        if (!mode) {
            const gchar *urls[] = {bunker_test_relay_url(f->relay)};
            REQUIRE(configure_relays(f, TRUE, urls, 1), "changed method ceiling invalidates pending offers");
        } else {
            REQUIRE(control(f, "SetPaused", g_variant_new("(b)", TRUE)), "pause invalidates offers");
            REQUIRE(control(f, "SetPaused", g_variant_new("(b)", FALSE)), "resume does not restore offers");
        }
        const gchar *params_old[] = {f->transport_pk, old.secret};
        const gchar *id = mode ? "token-paused" : "token-reconfigured";
        g_autofree gchar *event = request_event(f, id, "connect", params_old, 2, time(NULL), TRUE);
        REQUIRE(raw_expect(f, id, event, "invalid_pairing_token"), "config/pause generations invalidate old tokens");
        secure_wipe(old.secret, strlen(old.secret)); nostr_nip46_uri_bunker_free(&old);
    }
    return TRUE;
}
static void runtime_changed(GDBusConnection *bus, const gchar *sender, const gchar *path,
    const gchar *interface, const gchar *signal, GVariant *args, gpointer data) {
    (void)bus; (void)sender; (void)path; (void)interface; (void)signal; (void)args; Fixture *f = data;
    g_mutex_lock(&f->capture_mutex); f->status_events++; g_cond_broadcast(&f->capture_cond); g_mutex_unlock(&f->capture_mutex);
}
static gboolean await_bunker_state(Fixture *f, const gchar *wanted) {
    gint64 bound = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
    for (;;) {
        g_mutex_lock(&f->capture_mutex); guint observed = f->status_events; g_mutex_unlock(&f->capture_mutex);
        g_autoptr(GVariant) reply = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer/grotto",
            "org.nostr.Grotto.Control1", "GetStatus", NULL, G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
        REQUIRE(reply, "read runtime state after emitted change");
        g_autoptr(GVariant) row = g_variant_get_child_value(reply, 0); const gchar *state = NULL;
        if (g_variant_lookup(row, "bunker_state", "&s", &state) && !g_strcmp0(state, wanted)) return TRUE;
        g_mutex_lock(&f->capture_mutex); gboolean timely = TRUE;
        while (f->status_events == observed && timely) timely = g_cond_wait_until(&f->capture_cond, &f->capture_mutex, bound);
        g_mutex_unlock(&f->capture_mutex); if (!timely) return FALSE;
    }
}
typedef struct { Fixture *f; BunkerTestRelay *relay; guint connections, requests, addressed; gboolean done; const gchar *recipient; } RelayStats;
static gboolean snapshot_relay(gpointer data) {
    RelayStats *snapshot = data; guint published;
    g_mutex_lock(&snapshot->f->capture_mutex);
    bunker_test_relay_stats(snapshot->relay, &snapshot->connections, &snapshot->requests, &published);
    JsonArray *events = bunker_test_relay_events(snapshot->relay);
    for (guint i = 0; snapshot->recipient && i < json_array_get_length(events); i++) {
        JsonObject *event = json_array_get_object_element(events, i);
        if (g_strcmp0(json_object_get_string_member(event, "pubkey"), snapshot->f->transport_pk)) continue;
        JsonArray *tags = json_object_get_array_member(event, "tags");
        for (guint j = 0; j < json_array_get_length(tags); j++) {
            JsonArray *tag = json_array_get_array_element(tags, j);
            if (json_array_get_length(tag) >= 2 && !g_strcmp0(json_array_get_string_element(tag, 0), "p") &&
                !g_strcmp0(json_array_get_string_element(tag, 1), snapshot->recipient)) snapshot->addressed++;
        }
    }
    snapshot->done = TRUE;
    g_cond_broadcast(&snapshot->f->capture_cond); g_mutex_unlock(&snapshot->f->capture_mutex); return G_SOURCE_REMOVE;
}
static RelayStats relay_client_stats(Fixture *f, BunkerTestRelay *relay, const gchar *recipient) {
    RelayStats snapshot = {.f = f, .relay = relay, .recipient = recipient}; g_mutex_lock(&f->capture_mutex); g_idle_add(snapshot_relay, &snapshot);
    while (!snapshot.done) g_cond_wait(&f->capture_cond, &f->capture_mutex);
    g_mutex_unlock(&f->capture_mutex); return snapshot;
}
static RelayStats relay_stats(Fixture *f, BunkerTestRelay *relay) { return relay_client_stats(f, relay, NULL); }
static gboolean configure_relays(Fixture *f, gboolean enabled, const gchar *const *relays, gsize count) {
    const gchar *methods[] = {"get_public_key", "sign_event", "nip04_encrypt", "nip04_decrypt", "nip44_encrypt", "nip44_decrypt", "ping", "get_relays", NULL};
    GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "enabled", g_variant_new_boolean(enabled));
    g_variant_builder_add(&b, "{sv}", "relays", g_variant_new_strv(relays, count));
    g_variant_builder_add(&b, "{sv}", "allowed-methods", g_variant_new_strv(methods, -1));
    return control(f, "ConfigureBunker", g_variant_new("(a{sv})", &b));
}
static gchar *canonical_relay(BunkerTestRelay *relay) {
    const gchar *urls[] = {bunker_test_relay_url(relay)}; gchar *json = NULL;
    g_assert_cmpint(nostr_nip55l_relays_from_list(urls, 1, &json), ==, 0);
    g_autoptr(JsonParser) parser = json_parser_new(); g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
    gchar *url = g_strdup(json_array_get_string_element(json_node_get_array(json_parser_get_root(parser)), 0)); free(json); return url;
}
static gboolean await_pairing_counter(Fixture *f, const gchar *field, guint wanted) {
    gint64 bound = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    for (;;) {
        g_mutex_lock(&f->capture_mutex); guint revision = f->status_events; g_mutex_unlock(&f->capture_mutex);
        g_autoptr(GVariant) reply = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1",
            "GetStatus", NULL, G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
        REQUIRE(reply, "read pending pairing counters"); g_autoptr(GVariant) row = g_variant_get_child_value(reply, 0); guint value = 0;
        REQUIRE(g_variant_lookup(row, field, "u", &value), "pending pairing counter is present");
        if (value == wanted) return TRUE;
        g_mutex_lock(&f->capture_mutex); gboolean timely = TRUE;
        while (f->status_events == revision && timely) timely = g_cond_wait_until(&f->capture_cond, &f->capture_mutex, bound);
        g_mutex_unlock(&f->capture_mutex); if (!timely) return FALSE;
    }
}
static gboolean cancelled_published_ack(Fixture *f) {
    g_autofree gchar *principal = g_strdup_printf("nip46:%s", f->second_pk);
    REQUIRE(control(f, "RevokeClient", g_variant_new("(s)", principal)), "remove second client's old pairing before publication-cancellation test");
    g_autoptr(GVariant) offer = create_offer(f, f->account_pk); REQUIRE(offer, "create config-fenced pairing");
    const gchar *text = NULL; g_variant_lookup(offer, "uri", "&s", &text);
    NostrNip46BunkerURI uri = {0}; REQUIRE(!nostr_nip46_uri_parse_bunker(text, &uri), "config-fenced offer parses");
    const gchar *params[] = {f->transport_pk, uri.secret};
    g_autofree gchar *event = request_event(f, "ack-cancel-config", "connect", params, 2, time(NULL), 2);
    g_atomic_int_set(&f->defer_connect, TRUE); RawJob job = {f, "ack-cancel-config", event, NULL, FALSE};
    GThread *thread = g_thread_new("config-fenced-connect", raw_worker, &job);
    g_autofree gchar *review = await_connect_review(f); REQUIRE(review, "receive config-fenced review");
    hold_acks(f, f->relay, TRUE);
    g_autoptr(GVariant) decision = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer",
        "ApproveRequest", g_variant_new("(sbbt)", review, TRUE, FALSE, (guint64)0), NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
    REQUIRE(decision, "approve config-fenced pairing"); g_thread_join(thread); g_atomic_int_set(&f->defer_connect, FALSE);
    REQUIRE(job.ok && client_pairing_state(f, f->second_pk, FALSE, TRUE, FALSE), "wire delivery alone leaves the client pending");
    g_autofree gchar *waiting = request_event(f, "waiting-for-pairing-ack", "get_public_key", NULL, 0, time(NULL), 2);
    RawJob waiter = {f, "waiting-for-pairing-ack", waiting, "pairing_unconfirmed", FALSE};
    thread = g_thread_new("pending-pairing-operation", raw_worker, &waiter);
    REQUIRE(await_pairing_counter(f, "bunker_pending_pairing_requests", 1), "account operation waits for actual pairing acceptance");
    const gchar *changed[] = {bunker_test_relay_url(f->relay), bunker_test_relay_url(f->relay2)};
    REQUIRE(configure_relays(f, TRUE, changed, 2), "config invalidates pending ack without disabling transport or changing its effective relay union");
    hold_acks(f, f->relay, FALSE);
    REQUIRE(await_pairing_counter(f, "bunker_pending_pairing_acks", 0), "process the now-successful but cancelled publication callback");
    g_thread_join(thread);
    REQUIRE(waiter.ok && client_pairing_state(f, f->second_pk, FALSE, TRUE, FALSE), "positive relay OK cannot resurrect a cancelled pairing or release account operations");
    const gchar *restored[] = {bunker_test_relay_url(f->relay)};
    REQUIRE(configure_relays(f, TRUE, restored, 1), "restore signer-wide configuration");
    secure_wipe(uri.secret, strlen(uri.secret)); nostr_nip46_uri_bunker_free(&uri); return TRUE;
}
static gboolean uri_pairing_and_reconfigure(Fixture *f) {
    if (!f->status_subscription) f->status_subscription = g_dbus_connection_signal_subscribe(f->approver, "org.nostr.Signer", "org.nostr.Grotto.Control1", "StatusChanged",
        "/org/nostr/signer/grotto", NULL, G_DBUS_SIGNAL_FLAGS_NONE, runtime_changed, f, NULL);
    guint8 pk[32]; gchar *npub = NULL; g_assert_true(nostr_hex2bin(pk, f->account_pk, 32));
    g_assert_cmpint(nostr_nip19_encode_npub(pk, &npub), ==, 0); g_autofree gchar *identity = npub;
    g_autofree gchar *relay = g_uri_escape_string(bunker_test_relay_url(f->relay2), NULL, FALSE);
    g_autofree gchar *uri = g_strdup_printf("nostrconnect://%s?relay=%s&secret=g06-client-secret&name=Unverified%%20fixture&perms=sign_event%%3A1", f->peer_pk, relay);
    g_ptr_array_add(f->sensitive, g_strdup(uri)); g_ptr_array_add(f->sensitive, g_strdup("g06-client-secret"));
    REQUIRE(configure_relays(f, FALSE, NULL, 0), "disable and clear signer-wide relays before a client-initiated pairing");
    REQUIRE(await_bunker_state(f, "DISABLED"), "disable closes subscriptions without restarting the daemon");
    g_autoptr(GError) disabled_error = NULL;
    g_autoptr(GVariant) disabled = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1",
        "AcceptConnectURI", g_variant_new("(ss)", uri, identity), G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &disabled_error);
    g_autofree gchar *disabled_name = g_dbus_error_get_remote_error(disabled_error);
    REQUIRE(!disabled && !g_strcmp0(disabled_name, "org.nostr.Grotto.Control1.Error.NotReady") && strstr(disabled_error->message, "Enable remote signing"), "disabled pairing returns a typed enable-first error");
    REQUIRE(await_bunker_state(f, "DISABLED"), "rejected URI never re-enables the global kill switch");
    const gchar *signer_relays[] = {bunker_test_relay_url(f->relay)};
    REQUIRE(configure_relays(f, TRUE, signer_relays, 1) && await_bunker_state(f, "LISTENING"), "only explicit ConfigureBunker enables remote signing");
    g_autoptr(GError) identity_error = NULL;
    g_autoptr(GVariant) unknown = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1",
        "AcceptConnectURI", g_variant_new("(ss)", uri, f->peer_pk), G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &identity_error);
    REQUIRE(!unknown && identity_error, "syntactically valid but unstored identity is refused");
    g_atomic_int_set(&f->defer_connect, TRUE);
    g_autofree gchar *denied_uri = g_strdup_printf("nostrconnect://%s?relay=%s&secret=g06-denied-secret&name=Denied", f->second_pk, relay);
    g_ptr_array_add(f->sensitive, g_strdup(denied_uri)); g_ptr_array_add(f->sensitive, g_strdup("g06-denied-secret"));
    g_autoptr(GVariant) denied = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1",
        "AcceptConnectURI", g_variant_new("(ss)", denied_uri, identity), G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
    REQUIRE(denied, "valid stored identity reaches explicit review");
    g_autofree gchar *denied_id = await_connect_review(f); REQUIRE(denied_id, "receive denial review");
    g_autoptr(GVariant) refusal = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer",
        "ApproveRequest", g_variant_new("(sbbt)", denied_id, FALSE, FALSE, (guint64)0), NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
    REQUIRE(refusal, "deny URI pairing");
    RelayStats uncontacted = relay_stats(f, f->relay2);
    REQUIRE(!uncontacted.connections && !uncontacted.requests, "disabled, invalid-identity and denied URI pairings never contact the supplied relay");
    g_autoptr(GError) accept_error = NULL;
    g_autoptr(GVariant) accepted = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1",
        "AcceptConnectURI", g_variant_new("(ss)", uri, identity), G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &accept_error);
    if (accept_error) g_printerr("AcceptConnectURI: %s\n", accept_error->message);
    REQUIRE(accepted, "client URI creates review, not a network connection");
    g_autofree gchar *review = await_connect_review(f); const gchar *returned_id = NULL; g_variant_get(accepted, "(&s)", &returned_id);
    REQUIRE(review && !strcmp(review, returned_id), "AcceptConnectURI returns the actual shared review ID");
    RelayStats before = relay_stats(f, f->relay2);
    REQUIRE(!before.connections && !before.requests, "client-supplied relay is not contacted before explicit consent");
    g_autoptr(GVariant) info = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer",
        "GetApprovalInfo", g_variant_new("(s)", review), G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
    REQUIRE(info, "pairing review metadata is available");
    g_autoptr(GVariant) info_row = g_variant_get_child_value(info, 0);
    g_autofree gchar *rendered = g_variant_print(info_row, FALSE);
    g_autofree gchar *expected_relay = canonical_relay(f->relay2);
    REQUIRE(!strstr(rendered, "g06-client-secret") && strstr(rendered, expected_relay), "preview includes exact relays but never the client's secret");
    hold_acks(f, f->relay2, TRUE);
    g_autoptr(GVariant) approved = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer",
        "ApproveRequest", g_variant_new("(sbbt)", review, TRUE, FALSE, (guint64)0), NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
    REQUIRE(approved, "explicitly approve client URI"); g_atomic_int_set(&f->defer_connect, FALSE);
    gint64 bound = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    g_mutex_lock(&f->capture_mutex);
    while (!f->uri_ack_id) {
        GHashTableIter it; gpointer value; g_hash_table_iter_init(&it, f->captured);
        while (g_hash_table_iter_next(&it, NULL, &value)) {
            NostrNip46Response response = {0};
            if (!nostr_nip46_response_parse(value, &response) && !g_strcmp0(response.result, "g06-client-secret") && !response.error)
                f->uri_ack_id = g_strdup(response.id);
            nostr_nip46_response_free(&response);
        }
        if (f->uri_ack_id) break;
        if (!g_cond_wait_until(&f->capture_cond, &f->capture_mutex, bound)) break;
    }
    gboolean acked = f->uri_ack_id != NULL; g_mutex_unlock(&f->capture_mutex);
    REQUIRE(acked, "encrypted signer-initiated connect is a RESPONSE with result=client secret, not a connect request");
    REQUIRE(client_pairing_state(f, f->peer_pk, FALSE, TRUE, FALSE), "URI pairing remains pending until its own relay accepts the ack");
    hold_acks(f, f->relay2, FALSE);
    REQUIRE(client_pairing_state(f, f->peer_pk, TRUE, FALSE, TRUE), "URI relay OK, not successful websocket send, commits paired");
    REQUIRE(captured_lane(f, f->uri_ack_id) == 2, "client B pairing ack is observed only on client B's relay");
    gchar *scoped_key = NULL;
    REQUIRE(!nostr_nip46_client_get_public_key_rpc(f->client, &scoped_key), "client A request on relay A completes"); free(scoped_key);
    RelayStats isolated = relay_client_stats(f, f->relay2, f->client_pk);
    REQUIRE(isolated.addressed == 0, "relay B has never received client A's replies");
    g_autofree gchar *b_request = request_event(f, "isolated-client-b", "get_public_key", NULL, 0, time(NULL), TRUE);
    g_autofree gchar *b_reply = send_raw_on(f, f->relay2, "isolated-client-b", b_request);
    REQUIRE(b_reply, "client B receives its scoped response on relay B");
    REQUIRE(captured_lane(f, "isolated-client-b") == 2, "client B RPC response never reaches relay A");
    REQUIRE(await_bunker_state(f, "LISTENING"), "new client relay reached EOSE");
    RelayStats preserved = relay_stats(f, f->relay2);
    REQUIRE(configure_relays(f, TRUE, signer_relays, 1), "retain the signer relay beside the URI client's relay");
    REQUIRE(await_bunker_state(f, "LISTENING"), "added signer relay reaches EOSE");
    gchar *barrier_key = NULL;
    gboolean barrier_ok = !nostr_nip46_client_get_public_key_rpc(f->client, &barrier_key) && !g_strcmp0(barrier_key, f->account_pk);
    free(barrier_key); REQUIRE(barrier_ok && drain_approver(f), "finish explicit decisions for replayed Low-tier history before removing its delivery relay");
    REQUIRE(configure_relays(f, TRUE, NULL, 0), "remove signer-wide list while retaining the client relay");
    REQUIRE(await_bunker_state(f, "LISTENING"), "per-client relay keeps the listener operational");
    RelayStats retained = relay_stats(f, f->relay2);
    REQUIRE(preserved.requests == retained.requests && preserved.connections == retained.connections, "unchanged relay subscription and socket are preserved");
    REQUIRE(configure_relays(f, TRUE, signer_relays, 1), "restore signer-wide relay without daemon restart");
    REQUIRE(await_bunker_state(f, "LISTENING"), "both configured relays reached EOSE again");
    retained = relay_stats(f, f->relay2);
    REQUIRE(preserved.requests == retained.requests && preserved.connections == retained.connections, "adding another relay never duplicates the retained client's REQ");
    gchar *account = NULL;
    gboolean barrier = !nostr_nip46_client_get_public_key_rpc(f->client, &account) && !g_strcmp0(account, f->account_pk);
    free(account); REQUIRE(barrier, "fresh paired RPC completes after the relay's history delivery and before restart");
    REQUIRE(cancelled_published_ack(f), "configuration invalidation fences accepted publication callbacks");
    return TRUE;
}
static gboolean client_relay_restored(Fixture *f) {
    REQUIRE(await_bunker_state(f, "LISTENING"), "restored configured and client subscriptions reach EOSE");
    RelayStats stats = relay_stats(f, f->relay2); REQUIRE(stats.requests >= 2, "restart resubscribed the persisted per-client relay");
    g_autoptr(GVariant) clients = g_dbus_connection_call_sync(f->approver, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1",
        "ListClients", NULL, G_VARIANT_TYPE("(aa{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
    REQUIRE(clients, "read durable client relay inventory");
    g_autofree gchar *rows = g_variant_print(clients, FALSE);
    g_autofree gchar *expected_relay = canonical_relay(f->relay2);
    REQUIRE(strstr(rows, f->peer_pk) && strstr(rows, expected_relay), "client key and its exact approved relays survived restart"); return TRUE;
}
static gboolean run_rpc_flow(Fixture *f)
{
    if (!f->status_subscription) f->status_subscription = g_dbus_connection_signal_subscribe(f->approver, "org.nostr.Signer", "org.nostr.Grotto.Control1", "StatusChanged",
        "/org/nostr/signer/grotto", NULL, G_DBUS_SIGNAL_FLAGS_NONE, runtime_changed, f, NULL);
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

    if (!f->headless) REQUIRE(token_pairing(f), "single-use token admission, reservation, cancellation and revoke");
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

    REQUIRE(client_pairing_state(f, f->client_pk, TRUE, FALSE, TRUE), "wait for relay-accepted durable pairing before editing scoped permissions");
    /* Use the real management contract for explicit account/operation scopes.
     * Rapid synthetic UI decisions must not race the production prompt throttle;
     * no sleep, throttle disable or client-requested grant is involved. Kind 3
     * remains ungranted and exercises two interleaved deferred reviews below. */
    if (!f->headless) REQUIRE(grant_scoped_account_ops(f, 600), "explicit scoped account operations");
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
    REQUIRE(client_pairing_state(f, f->second_pk, TRUE, FALSE, TRUE), "second client ack is accepted before issuing another connect");
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
    guint64 signing_sequence = activity_sequence(f);
    REQUIRE(raw_expect(f, "raw-sign", replay, NULL), "fresh sign request");
    REQUIRE(activity_sequence(f) == signing_sequence + 1, "exactly one gate operation produces the signature");
    REQUIRE(raw_expect(f, "raw-sign", replay, NULL), "identical replay uses cached response");
    REQUIRE(activity_sequence(f) == signing_sequence + 1, "cached replay does not submit a second signing operation");
    g_autofree gchar *changed = request_event(f, "raw-sign", "get_public_key", NULL, 0, now, FALSE);
    REQUIRE(raw_expect(f, "raw-sign", changed, "duplicate_rpc_changed_params"), "changed RPC rejected");
    REQUIRE(expiry_replay(f), "expiry and scope-revoke cache authority regression");
    g_autofree gchar *stale = request_event(f, "raw-stale", "sign_event", (const gchar *const *)&unsigned_json, 1, now - 601, FALSE);
    REQUIRE(raw_expect(f, "raw-stale", stale, "stale_request"), "hostile relay backfill outside window rejected");
    g_autofree gchar *future = request_event(f, "raw-future", "sign_event", (const gchar *const *)&unsigned_json, 1, now + 1200, FALSE);
    REQUIRE(raw_expect(f, "raw-future", future, "stale_request"), "future request outside skew rejected");
    REQUIRE(reconnect_bunker(f), "rejected future envelope cannot suppress a normal paired RPC after reconnect");
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
    if (!f->headless) REQUIRE(uri_pairing_and_reconfigure(f), "client URI preview/review, secret response and live relay diffs");
    NostrNip46BunkerURI lost = {0};
    if (!f->headless) {
        g_autofree gchar *second_principal = g_strdup_printf("nip46:%s", f->second_pk);
        REQUIRE(control(f, "RevokeClient", g_variant_new("(s)", second_principal)), "use an unpaired authenticated client for the restart token check");
        g_autoptr(GVariant) before_restart = create_offer(f, f->account_pk); REQUIRE(before_restart, "issue an offer immediately before restart");
        const gchar *text = NULL; g_variant_lookup(before_restart, "uri", "&s", &text);
        REQUIRE(!nostr_nip46_uri_parse_bunker(text, &lost), "pre-restart offer parses");
    }
    REQUIRE(restart_daemon(f), "same profile restarts with persistent endpoint and pairings");
    if (!f->headless) {
        const gchar *lost_params[] = {f->transport_pk, lost.secret};
        g_autofree gchar *lost_event = request_event(f, "token-before-restart", "connect", lost_params, 2, time(NULL), 2);
        REQUIRE(raw_expect(f, "token-before-restart", lost_event, "invalid_pairing_token"), "pre-restart offers are gone, even for a legacy-allowlisted client");
        secure_wipe(lost.secret, strlen(lost.secret)); nostr_nip46_uri_bunker_free(&lost);
    }
    if (!f->headless) REQUIRE(client_relay_restored(f), "per-client relay subscription survives restart");
    g_clear_pointer(&pubkey, g_free);
    REQUIRE(nostr_nip46_client_get_public_key_rpc(f->client, &pubkey) == 0 && !g_strcmp0(pubkey, f->account_pk),
        "paired identity and scoped policy survive restart; response-worker barrier drains historical replies before reinjection");
    REQUIRE(raw_expect(f, "raw-sign", replay, "replayed_request"), "persisted seen IDs prevent execution after restart/cache loss");
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
    REQUIRE(await_bunker_state(f, "LISTENING"), "rotated endpoint completes credential loading and relay readiness before clean shutdown");
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
static void count_rpc(GHashTable *counts, const gchar *key) {
    guint count = GPOINTER_TO_UINT(g_hash_table_lookup(counts, key));
    g_hash_table_replace(counts, g_strdup(key), GUINT_TO_POINTER(count + 1));
}
static gboolean audit_frames(Fixture *f)
{
    JsonArray *events = bunker_test_relay_events(f->relay);
    REQUIRE(events != NULL, "relay capture is an array");
    g_autoptr(GHashTable) requests = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_autoptr(GHashTable) replies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GHashTable) deliveries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GHashTable) signatures = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_autoptr(GHashTable) successes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    NostrNip46Session *observer = nostr_nip46_client_new();
    REQUIRE(observer != NULL, "create transport observation session");
    nostr_nip46_client_set_secret(observer, f->transport_sk);
    NostrNip46Session *wrong_client = nostr_nip46_client_new();
    nostr_nip46_client_set_secret(wrong_client, f->peer_sk);
    gboolean valid = TRUE;
    JsonArray *delivered = bunker_test_relay_deliveries(f->relay);
    for (guint i = 0; i < json_array_get_length(delivered); i++) {
        JsonObject *envelope = json_array_get_object_element(delivered, i);
        const gchar *author = json_object_get_string_member(envelope, "pubkey");
        if (!g_strcmp0(author, f->transport_pk)) continue;
        gchar *plain = NULL; NostrNip46Request req = {0};
        REQUIRE(!nostr_nip46_client_nip44_decrypt(observer, author,
            json_object_get_string_member(envelope, "content"), &plain), "decrypt counted request delivery");
        REQUIRE(!nostr_nip46_request_parse(plain, &req) && req.id, "counted delivery has RPC id");
        g_autofree gchar *key = g_strdup_printf("%s:%s", author, req.id);
        count_rpc(deliveries, key); nostr_nip46_request_free(&req); free(plain);
    }
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
                if (!g_hash_table_contains(requests, key)) g_hash_table_insert(requests, key, g_strdup(req.method));
                else g_free(key);
                g_print("relay request method=%s id=%s signature=valid target=transport\n", req.method, req.id);
            }
            nostr_nip46_request_free(&req);
        } else {
            NostrNip46Response resp = {0};
            valid = nostr_nip46_response_parse(plain, &resp) == 0 && resp.id && (resp.result || resp.error);
            g_autofree gchar *key = valid ? g_strdup_printf("%s:%s", target, resp.id) : NULL;
            gboolean uri_ack = valid && f->uri_ack_id && !g_strcmp0(resp.id, f->uri_ack_id) &&
                !g_strcmp0(target, f->peer_pk) && !g_strcmp0(resp.result, "g06-client-secret") && !resp.error;
            valid = valid && (uri_ack || g_hash_table_contains(requests, key));
            if (valid) {
                count_rpc(replies, key);
                if (resp.result && !resp.error && !g_strcmp0(g_hash_table_lookup(requests, key), "sign_event")) {
                    NostrEvent *signed_event = nostr_event_new();
                    valid = !nostr_event_deserialize(signed_event, resp.result) && nostr_event_check_signature(signed_event) &&
                        !g_strcmp0(nostr_event_get_pubkey(signed_event), f->account_pk);
                    nostr_event_free(signed_event);
                    const gchar *previous = g_hash_table_lookup(signatures, key);
                    valid = valid && (!previous || !strcmp(previous, resp.result));
                    if (!previous) g_hash_table_insert(signatures, g_strdup(key), g_strdup(resp.result));
                    count_rpc(successes, key);
                }
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
        GHashTableIter counts; gpointer key, value;
        g_hash_table_iter_init(&counts, deliveries);
        while (g_hash_table_iter_next(&counts, &key, &value)) {
            guint expected = GPOINTER_TO_UINT(value), actual = GPOINTER_TO_UINT(g_hash_table_lookup(replies, key));
            if (expected != actual) g_printerr("RPC %s deliveries=%u terminal replies=%u\n", (gchar *)key, expected, actual);
            REQUIRE(expected == actual, "exactly one terminal reply per delivered RPC, including intentional cache/backfill retries");
        }
        g_autofree gchar *raw_key = g_strdup_printf("%s:raw-sign", f->client_pk);
        REQUIRE(GPOINTER_TO_UINT(g_hash_table_lookup(successes, raw_key)) == 2,
            "one original signature and its identical cached delivery; zero successes after pause/revoke/restart");
        const gchar *cancelled[] = {"pending-pause", "pending-revoke", "headless-review", "raw-stale", "raw-future", "raw-wrong"};
        for (guint i = 0; i < G_N_ELEMENTS(cancelled); i++) {
            g_autofree gchar *denied = g_strdup_printf("%s:%s", i == 5 ? f->peer_pk : f->client_pk, cancelled[i]);
            REQUIRE(!g_hash_table_contains(successes, denied), "cancelled/revoked/rejected RPC has zero account signatures");
        }
        g_hash_table_iter_init(&counts, successes);
        while (g_hash_table_iter_next(&counts, &key, &value)) {
            if (!strcmp(key, raw_key)) continue;
            REQUIRE(GPOINTER_TO_UINT(value) == 1, "exactly one successful account signature per fresh RPC id");
        }
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
    if (error) {
        g_autofree gchar *log = NULL;
        if (g_file_get_contents(f->log_path, &log, NULL, NULL)) {
            const gchar *sanitizer = strstr(log, "ERROR: LeakSanitizer");
            if (!sanitizer) sanitizer = strstr(log, "ERROR: AddressSanitizer");
            if (sanitizer) g_printerr("daemon sanitizer: %s\n", sanitizer);
            else {
                g_auto(GStrv) lines = g_strsplit(log, "\n", -1); guint count = g_strv_length(lines);
                for (guint i = count > 12 ? count - 12 : 0; i < count; i++) g_printerr("daemon tail: %.250s\n", lines[i]);
            }
        }
    }
    g_mutex_lock(&f->capture_mutex); g_atomic_int_add(&f->approver_pending, -1);
    g_cond_broadcast(&f->capture_cond); g_mutex_unlock(&f->capture_mutex);
    g_assert_no_error(error); g_assert_nonnull(reply); f->approvals++;
}
static void approver_submit(Fixture *f, GDBusConnection *bus, const gchar *path, const gchar *interface, const gchar *method, GVariant *args) {
    g_atomic_int_inc(&f->approver_pending);
    g_dbus_connection_call(bus, "org.nostr.Signer", path, interface, method, args,
        NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, approved, f);
}
static void approval_requested(GDBusConnection *bus, const gchar *sender, const gchar *path,
    const gchar *interface, const gchar *signal, GVariant *args, gpointer data) {
    (void)sender; (void)path; (void)interface; (void)signal;
    Fixture *f = data; const gchar *who, *identity, *op, *preview, *id;
    g_assert_false(f->headless); /* no ApprovalRequested/GUI summon in headless mode */
    g_variant_get(args, "(&s&s&s&s&s)", &who, &identity, &op, &preview, &id);
    g_autofree gchar *expected = g_strdup_printf("nip46:%s", f->client_pk);
    g_autofree gchar *expected_second = g_strdup_printf("nip46:%s", f->second_pk);
    g_autofree gchar *expected_peer = g_strdup_printf("nip46:%s", f->peer_pk);
    g_assert_true(!g_strcmp0(who, expected) || !g_strcmp0(who, expected_second) || !g_strcmp0(who, expected_peer));
    if (g_atomic_int_get(&f->defer_connect) && !strcmp(op, "connect")) {
        g_mutex_lock(&f->capture_mutex); g_free(f->connect_review); f->connect_review = g_strdup(id);
        g_cond_broadcast(&f->capture_cond); g_mutex_unlock(&f->capture_mutex); return;
    }
    gint cancel_mode = g_atomic_int_get(&f->cancel_mode);
    if (cancel_mode && !strcmp(op, "event")) {
        approver_submit(f, bus, "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1",
            cancel_mode == 1 ? "SetPaused" : "RevokeClient", cancel_mode == 1 ? g_variant_new("(b)", TRUE) : g_variant_new("(s)", who));
        return;
    }
    if (g_atomic_int_get(&f->defer_sign) && !strcmp(op, "event")) {
        g_ptr_array_add(f->deferred_ids, g_strdup(id));
        if (f->deferred_ids->len < 2) return; /* prove dispatcher remains live while waiting */
        for (guint i = 0; i < f->deferred_ids->len; i++)
            approver_submit(f, bus, "/org/nostr/signer", "org.nostr.Signer", "ApproveRequest",
                g_variant_new("(sbbt)", (gchar *)f->deferred_ids->pdata[i], TRUE, FALSE, (guint64)0));
        return;
    }
    gboolean allow = g_strcmp0(who, expected_peer) || !strcmp(op, "connect");
    /* Pairing the peer must not silently approve a previously rejected event
     * that a relay replays after admission. Such a Low-tier request is denied. */
    approver_submit(f, bus, "/org/nostr/signer", "org.nostr.Signer", "ApproveRequest",
        g_variant_new("(sbbt)", id, allow, allow, (guint64)600));
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
static gboolean logs_secret_free(Fixture *f) {
    const gchar *names[] = {"grotto-daemon.log", "grotto-restart.log"};
    for (guint i = 0; i < (f->restarted ? 2u : 1u); i++) {
        g_autofree gchar *path = g_build_filename(nostrc_test_bus_get_dir(f->bus), names[i], NULL);
        gchar *log = NULL; gsize length = 0;
        REQUIRE(g_file_get_contents(path, &log, &length, NULL), "daemon log is available for secret audit");
        gboolean clean = TRUE;
        for (guint j = 0; j < f->sensitive->len; j++) if (strstr(log, f->sensitive->pdata[j])) clean = FALSE;
        secure_wipe(log, length); g_free(log);
        REQUIRE(clean, "daemon log contains neither pairing token nor full URI");
    }
    return TRUE;
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
    f.sensitive = g_ptr_array_new_with_free_func(g_free);
    g_setenv("GROTTO_TEST_PAIRING_TTL_MS", "10000", TRUE);
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
    f.status_subscription = g_dbus_connection_signal_subscribe(f.approver, "org.nostr.Signer", "org.nostr.Grotto.Control1", "StatusChanged",
        "/org/nostr/signer/grotto", NULL, G_DBUS_SIGNAL_FLAGS_NONE, runtime_changed, &f, NULL);
    f.approval_subscription = g_dbus_connection_signal_subscribe(f.approver, NULL, "org.nostr.Signer",
        "ApprovalRequested", "/org/nostr/signer", NULL, G_DBUS_SIGNAL_FLAGS_NONE, approval_requested, &f, NULL);
    g_mutex_init(&f.capture_mutex); g_cond_init(&f.capture_cond);
    f.captured = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    f.capture_relays = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    f.relay = bunker_test_relay_new(); f.relay2 = bunker_test_relay_new();
    bunker_test_relay_set_event_callback(f.relay2, capture_response_b, &f);
    bunker_test_relay_set_event_callback(f.relay, capture_response_a, &f);
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
        timeout_id = g_timeout_add_seconds(60, deadline, &f);
        check_daemon_log(&f);
        g_main_loop_run(f.loop);
    }
    if (timeout_id && !f.timed_out) g_source_remove(timeout_id);
    if (monitor) g_file_monitor_cancel(monitor);
    if (f.worker) g_thread_join(f.worker);
    if (f.daemon) {
        g_subprocess_send_signal(f.daemon, SIGTERM);
        if (!g_subprocess_wait_check(f.daemon, NULL, NULL)) f.passed = FALSE;
    }
    if (f.restart_monitor) { g_file_monitor_cancel(f.restart_monitor); g_object_unref(f.restart_monitor); }
    guint connections, requests, published;
    bunker_test_relay_stats(f.relay, &connections, &requests, &published);
    g_print("relay evidence: connections=%u REQ=%u published=%u\n",
        connections, requests, published);
    gboolean audited = audit_frames(&f) && logs_secret_free(&f);
    nostrc_test_bus_dump_log(f.bus, "grotto-daemon.log");
    if (f.restarted) nostrc_test_bus_dump_log(f.bus, "grotto-restart.log");
    nostr_nip46_session_free(f.client); nostr_nip46_session_free(f.second);
    g_ptr_array_unref(f.deferred_ids); g_ptr_array_unref(f.sensitive);
    bunker_test_relay_free(f.relay); bunker_test_relay_free(f.relay2);
    g_dbus_connection_signal_unsubscribe(f.approver, f.approval_subscription);
    if (f.status_subscription) g_dbus_connection_signal_unsubscribe(f.approver, f.status_subscription);
    g_bus_unown_name(f.approver_name);
    nostrc_test_bus_down(f.bus);
    g_main_loop_unref(f.loop);
    g_hash_table_unref(f.captured); g_hash_table_unref(f.capture_relays); g_mutex_clear(&f.capture_mutex); g_cond_clear(&f.capture_cond);
    g_free(f.log_path);
    secure_wipe(f.account_sk, strlen(f.account_sk)); free(f.account_sk); free(f.account_pk);
    secure_wipe(f.client_sk, strlen(f.client_sk)); free(f.client_sk); free(f.client_pk);
    secure_wipe(f.second_sk, strlen(f.second_sk)); free(f.second_sk); free(f.second_pk);
    secure_wipe(f.peer_sk, strlen(f.peer_sk)); free(f.peer_sk); free(f.peer_pk);
    secure_wipe(f.transport_sk, strlen(f.transport_sk)); free(f.transport_sk); free(f.transport_pk);
    g_unsetenv("NOSTR_SIGNER_TEST_TRANSPORT_BACKEND");
    g_unsetenv("NOSTR_SIGNER_TEST_APPROVERS"); g_unsetenv("GROTTO_TEST_PAIRING_TTL_MS");
    g_unsetenv("NOSTR_SIGNER_SECKEY_HEX");
    g_free(f.connect_review); g_free(f.uri_ack_id);
    nostr_json_cleanup();
    return configured && f.ready && f.passed && audited && !f.timed_out ? 0 : 1;
}
