/* Real daemon + production controller; private bus/profile and disposable key.
 * No GUI or relay provenance is claimed by this test. */
#include "approval-controller.h"
#include "nostrc-test-bus.h"
#include <nostr-event.h>
#include <nostr-json.h>
#include <nostr-kinds.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

typedef struct { gboolean done; GVariant *value; GError *error; } Reply;
static NostrcTestBus *bus;
static gboolean online;
static guint prompts;
static gchar *client_principal, *client_identity;
static gboolean timed_out(gpointer data) {
  nostrc_test_bus_dump_log(bus, "g15-daemon.log");
  nostrc_test_bus_dump_log(bus, "g15-restarted.log");
  g_error("permission repeat timed out at line %d", GPOINTER_TO_INT(data));
  return G_SOURCE_REMOVE;
}
#define WAIT(test) do { guint t = g_timeout_add_seconds(20, timed_out, GINT_TO_POINTER(__LINE__)); \
  while (!(test)) g_main_context_iteration(NULL, TRUE); g_source_remove(t); } while (0)
static void done(GObject *source, GAsyncResult *result, gpointer data) {
  Reply *r = data; r->value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &r->error); r->done = TRUE;
}
static void decided(GObject *source, GAsyncResult *result, gpointer data) {
  Reply *r = data;
  if (grotto_approval_controller_decide_finish(GROTTO_APPROVAL_CONTROLLER(source), result, &r->error))
    r->value = g_variant_ref_sink(g_variant_new("(b)", TRUE));
  r->done = TRUE;
}
static void clear(Reply *r) { g_clear_pointer(&r->value, g_variant_unref); g_clear_error(&r->error); memset(r, 0, sizeof *r); }
static void appeared(GDBusConnection *c, const gchar *n, const gchar *o, gpointer d) { (void)c; (void)n; (void)o; (void)d; online = TRUE; }
static void service_ready(GDBusConnection *c, const gchar *n, const gchar *o, gpointer data) { (void)c; (void)n; (void)o; *(gboolean *)data = TRUE; }
static void vanished(GDBusConnection *c, const gchar *n, gpointer d) { (void)c; (void)n; (void)d; online = FALSE; }
static void prompted(GDBusConnection *c, const gchar *s, const gchar *p, const gchar *i, const gchar *n, GVariant *v, gpointer d) {
  (void)c; (void)s; (void)p; (void)i; (void)n; (void)v; (void)d; prompts++;
}
static void call(GDBusConnection *c, const gchar *method, GVariant *args, Reply *r) {
  g_dbus_connection_call(c, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer", method,
      args, G_VARIANT_TYPE("(s)"), 0, 30000, NULL, done, r);
}
static void sign_kind(guint kind, const gchar *relay, Reply *r) {
  g_autofree gchar *tags = kind == NOSTR_KIND_CLIENT_AUTHENTICATION ?
      g_strdup_printf("[[\"relay\",\"%s\"],[\"challenge\",\"fresh-request-%u\"]]", relay, g_random_int()) :
      g_strdup("[[\"e\",\"79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798\"]]");
  g_autofree gchar *json = g_strdup_printf("{\"kind\":%u,\"created_at\":%" G_GINT64_FORMAT ",\"tags\":%s,\"content\":\"\"}",
      kind, g_get_real_time() / G_USEC_PER_SEC, tags);
  call(nostrc_test_bus_connect(bus), "SignEvent", g_variant_new("(sss)", json, "", "g15-client"), r);
}
static gboolean ready(GrottoApprovalController *controller) {
  GListModel *items = grotto_approval_controller_get_pending(controller);
  if (!g_list_model_get_n_items(items)) return FALSE;
  g_autoptr(GrottoApprovalRequest) r = g_list_model_get_item(items, 0);
  return grotto_approval_request_get_state(r) == GROTTO_APPROVAL_READY;
}
static gboolean all_ready(GrottoApprovalController *controller, guint count) {
  GListModel *items = grotto_approval_controller_get_pending(controller);
  if (g_list_model_get_n_items(items) != count) return FALSE;
  for (guint i = 0; i < count; i++) {
    g_autoptr(GrottoApprovalRequest) r = g_list_model_get_item(items, i);
    if (grotto_approval_request_get_state(r) != GROTTO_APPROVAL_READY || !grotto_approval_request_get_info(r)) return FALSE;
  }
  return TRUE;
}
static void approve(GrottoApprovalController *controller, guint64 ttl) {
  GListModel *items = grotto_approval_controller_get_pending(controller);
  g_autoptr(GrottoApprovalRequest) r = g_list_model_get_item(items, 0);
  if (!client_principal) {
    GVariant *info = grotto_approval_request_get_info(r); const gchar *p, *identity;
    g_assert_true(g_variant_lookup(info, "principal", "&s", &p));
    g_assert_true(g_variant_lookup(info, "identity", "&s", &identity));
    client_principal = g_strdup(p); client_identity = g_strdup(identity);
#ifdef __APPLE__
    gboolean verified = TRUE; g_variant_lookup(info, "verified", "b", &verified); g_assert_false(verified);
#endif
  }
  Reply decision = {0};
  grotto_approval_controller_decide_async(controller, grotto_approval_request_get_id(r), TRUE, TRUE, ttl, NULL, decided, &decision);
  WAIT(decision.done); g_assert_no_error(decision.error); g_assert_nonnull(decision.value); clear(&decision);
}
static void verify(Reply *r, guint kind) {
  WAIT(r->done); g_assert_no_error(r->error); g_assert_nonnull(r->value);
  const gchar *json; g_variant_get(r->value, "(&s)", &json);
  NostrEvent *event = nostr_event_new(); g_assert_cmpint(nostr_event_deserialize(event, json), ==, 0);
  g_assert_true(nostr_event_check_signature(event)); g_assert_cmpint(nostr_event_get_kind(event), ==, (gint)kind); nostr_event_free(event);
}
static void crypto_matrix(GrottoApprovalController *controller, gboolean first) {
  const gchar *methods[] = {"NIP44EncryptForApp", "NIP44DecryptForApp", "NIP04EncryptForApp", "NIP04DecryptForApp"};
  const gchar *peer = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
  gchar *cipher = NULL;
  for (guint i = 0; i < G_N_ELEMENTS(methods); i++) {
    for (guint repeat = 0; repeat < 2; repeat++) {
      Reply r = {0}; guint before = prompts;
      call(nostrc_test_bus_connect(bus), methods[i], g_variant_new("(ssss)", i % 2 ? cipher : "G15 plaintext", peer, "", "g15-client"), &r);
      if (first && !repeat) { WAIT(ready(controller) || r.done); g_assert_no_error(r.error); g_assert_true(ready(controller)); approve(controller, 0); }
      WAIT(r.done); g_assert_no_error(r.error); g_assert_nonnull(r.value);
      const gchar *value; g_variant_get(r.value, "(&s)", &value);
      if (i % 2) g_assert_cmpstr(value, ==, "G15 plaintext");
      else { g_free(cipher); cipher = g_strdup(value); }
      if (!first || repeat) g_assert_cmpuint(prompts, ==, before);
      clear(&r);
      WAIT(g_list_model_get_n_items(grotto_approval_controller_get_pending(controller)) == 0);
    }
  }
  g_free(cipher);
}
static void management(GDBusConnection *connection, const gchar *method, GVariant *args, Reply *r) {
  g_dbus_connection_call(connection, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", method,
      args, NULL, 0, 30000, NULL, done, r);
}
static void atomic_and_pending(GDBusConnection *connection, GrottoApprovalController *controller) {
  Reply status = {0}; management(connection, "GetStatus", NULL, &status); WAIT(status.done); g_assert_no_error(status.error);
  g_autoptr(GVariant) info = g_variant_get_child_value(status.value, 0); guint64 revision;
  g_assert_true(g_variant_lookup(info, "permissions_revision", "t", &revision)); clear(&status);
  GVariantBuilder empty; g_variant_builder_init(&empty, G_VARIANT_TYPE("aa{sv}"));
  management(connection, "SetPermissions", g_variant_new("(ssaa{sv}t)", client_principal, client_identity, &empty, revision), &status);
  WAIT(status.done); g_assert_no_error(status.error); clear(&status);
  Reply note = {0}, reaction = {0};
  sign_kind(NOSTR_KIND_TEXT_NOTE, NULL, &note); WAIT(ready(controller) || note.done); g_assert_false(note.done);
  sign_kind(NOSTR_KIND_REACTION, NULL, &reaction);
  GListModel *items = grotto_approval_controller_get_pending(controller);
  WAIT(all_ready(controller, 2));
  gchar *id = NULL;
  for (guint i = 0; i < 2; i++) {
    g_autoptr(GrottoApprovalRequest) pending = g_list_model_get_item(items, i);
    guint kind = 0; g_variant_lookup(grotto_approval_request_get_info(pending), "event_kind", "u", &kind);
    if (kind == NOSTR_KIND_TEXT_NOTE) id = g_strdup(grotto_approval_request_get_id(pending));
  }
  g_assert_nonnull(id);
  Reply decision = {0}; management(connection, "DecideRequest", g_variant_new("(ss)", id, "trust-routine"), &decision); g_free(id);
  WAIT(decision.done); g_assert_no_error(decision.error); clear(&decision);
  verify(&note, NOSTR_KIND_TEXT_NOTE); verify(&reaction, NOSTR_KIND_REACTION); clear(&note); clear(&reaction);
  WAIT(g_list_model_get_n_items(items) == 0);
  guint before = prompts; sign_kind(NOSTR_KIND_REPOST, NULL, &note); verify(&note, NOSTR_KIND_REPOST); clear(&note);
  g_assert_cmpuint(prompts, ==, before);
  /* Preset excludes unknown/custom kinds and does not skip a first scoped grant. */
  const guint ask[] = {65534, NOSTR_KIND_PROFILE_METADATA};
  for (guint i = 0; i < G_N_ELEMENTS(ask); i++) {
    sign_kind(ask[i], NULL, &note); WAIT(ready(controller) || note.done); g_assert_false(note.done);
    g_autoptr(GrottoApprovalRequest) pending = g_list_model_get_item(items, 0);
    if (i == 0) {
      management(connection, "DecideRequest", g_variant_new("(ss)", grotto_approval_request_get_id(pending), "trust-routine"), &decision);
      WAIT(decision.done); g_assert_no_error(decision.error); clear(&decision);
      verify(&note, ask[i]); clear(&note);
      WAIT(g_list_model_get_n_items(items) == 0);
      /* Trusting routine operations on a custom request allows this request
       * once. It must not smuggle a durable custom-kind permission. */
      sign_kind(ask[i], NULL, &note); WAIT(ready(controller) || note.done); g_assert_false(note.done);
      g_clear_object(&pending); pending = g_list_model_get_item(items, 0);
    }
    grotto_approval_controller_decide_async(controller, grotto_approval_request_get_id(pending), FALSE, FALSE, 0, NULL, decided, &decision);
    WAIT(note.done && decision.done); g_assert_nonnull(note.error); g_assert_no_error(decision.error); clear(&note); clear(&decision);
    WAIT(g_list_model_get_n_items(items) == 0);
  }
  const gchar *blocks[] = {"block-operation", "block-app"};
  for (guint i = 0; i < G_N_ELEMENTS(blocks); i++) {
    guint kind = i ? NOSTR_KIND_PROFILE_METADATA : 65534;
    sign_kind(kind, NULL, &note); WAIT(ready(controller) || note.done); g_assert_false(note.done);
    g_autoptr(GrottoApprovalRequest) pending = g_list_model_get_item(items, 0);
    management(connection, "DecideRequest", g_variant_new("(ss)", grotto_approval_request_get_id(pending), blocks[i]), &decision);
    WAIT(decision.done && note.done); g_assert_no_error(decision.error); g_assert_nonnull(note.error); clear(&note); clear(&decision);
    WAIT(g_list_model_get_n_items(items) == 0);
    guint previous = prompts;
    sign_kind(i ? NOSTR_KIND_TEXT_NOTE : kind, NULL, &note); WAIT(note.done);
    g_assert_nonnull(note.error); g_assert_cmpuint(prompts, ==, previous); clear(&note);
  }
  /* Explicitly remove the app block; retained scoped blocks stay in force. */
  management(connection, "SetClientTrust", g_variant_new("(ss)", client_principal, "low"), &decision);
  WAIT(decision.done); g_assert_no_error(decision.error); clear(&decision);
}
static void stop_daemon(const gchar *pidfile) {
  g_autofree gchar *text = NULL; g_assert_true(g_file_get_contents(pidfile, &text, NULL, NULL));
  gint64 pid = g_ascii_strtoll(text, NULL, 10); g_assert_cmpint(pid, >, 1);
  g_assert_cmpint(kill((pid_t)pid, SIGTERM), ==, 0); WAIT(!online);
}
int main(int argc, char **argv) {
  if (argc == 4 && !strcmp(argv[1], "--daemon")) {
    g_autofree gchar *pid = g_strdup_printf("%ld", (long)getpid());
    if (!g_file_set_contents(argv[3], pid, -1, NULL)) return 1;
    execl(argv[2], argv[2], (char *)NULL); return 1;
  }
  g_assert_cmpint(argc, ==, 3);
  g_assert_cmpstr(g_getenv("NOSTRC_TEST_ISOLATION"), ==, "1");
  if (!nostrc_test_bus_available()) return 77;
  g_setenv("NOSTR_SIGNER_TEST_APPROVERS", argv[0], TRUE);
  g_setenv("NOSTR_SIGNER_SECKEY_HEX", "0000000000000000000000000000000000000000000000000000000000000001", TRUE);
  g_unsetenv("NOSTR_SIGNER_ENDPOINT"); g_unsetenv("NOSTR_SIGNER_SOCK"); g_unsetenv("NOSTR_SIGNER_TOKEN");
  g_test_init(&argc, &argv, NULL); nostr_json_init(); nostrc_test_bus_tolerate_ebadf();
  bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE); nostrc_test_bus_up(bus);
  g_autofree gchar *config = g_build_filename(nostrc_test_bus_get_dir(bus), "config", NULL);
  g_assert_cmpint(g_mkdir(config, 0700), ==, 0); g_setenv("XDG_CONFIG_HOME", config, TRUE);
  GDBusConnection *connection = nostrc_test_bus_connect(bus);
  g_autoptr(GVariant) owned = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "RequestName", g_variant_new("(su)", "org.nostr.Grotto", 0), NULL, 0, 5000, NULL, NULL); g_assert_nonnull(owned);
  guint watch = g_bus_watch_name_on_connection(connection, "org.nostr.Signer", 0, appeared, vanished, NULL, NULL);
  guint sub = g_dbus_connection_signal_subscribe(connection, "org.nostr.Signer", "org.nostr.Signer", "ApprovalRequested", "/org/nostr/signer", NULL, 0, prompted, NULL, NULL);
  g_autofree gchar *pidfile = g_build_filename(nostrc_test_bus_get_dir(bus), "daemon.pid", NULL);
  const gchar *command[] = {argv[0], "--daemon", argv[1], pidfile, NULL};
  nostrc_test_bus_spawn_supervised(bus, "g15-daemon.log", NULL, command); WAIT(online);
  g_autoptr(GApplication) app = g_application_new("org.test.G15Repeat", G_APPLICATION_NON_UNIQUE);
  GrottoApprovalController *controller = grotto_approval_controller_new(app, connection);
  const guint kinds[] = {0, 1, 3, 4, 5, 6, 7, 13, 14, 15, 16, 1059, 1111, 9734, 10002, 22242, 30023, 65535};
  for (guint phase = 0; phase < 2; phase++) {
    for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) {
      Reply r = {0}; guint before = prompts;
      sign_kind(kinds[i], "WSS://NOS.LOL:443", &r);
      if (!phase) {
        WAIT(ready(controller) || r.done); g_assert_no_error(r.error); g_assert_true(ready(controller));
        approve(controller, i % 2 ? 86400 : 0);
      }
      verify(&r, kinds[i]); clear(&r);
      WAIT(g_list_model_get_n_items(grotto_approval_controller_get_pending(controller)) == 0);
      guint approved_prompts = prompts;
      if (phase) g_assert_cmpuint(approved_prompts, ==, before);
      sign_kind(kinds[i], "wss://nos.lol/", &r); verify(&r, kinds[i]); clear(&r);
      g_assert_cmpuint(prompts, ==, approved_prompts);
    }
    crypto_matrix(controller, phase == 0);
    if (!phase) {
      grotto_approval_controller_stop(controller); g_object_unref(controller);
      stop_daemon(pidfile);
      nostrc_test_bus_spawn_supervised(bus, "g15-restarted.log", NULL, command); WAIT(online);
      controller = grotto_approval_controller_new(app, connection);
    }
  }
  /* A different AUTH relay is never covered by the first relay's permission. */
  Reply other = {0}; sign_kind(22242, "wss://relay.nostr.band", &other);
  WAIT(ready(controller) || other.done); g_assert_no_error(other.error); g_assert_true(ready(controller));
  g_autoptr(GrottoApprovalRequest) pending = g_list_model_get_item(grotto_approval_controller_get_pending(controller), 0);
  Reply denied = {0};
  grotto_approval_controller_decide_async(controller, grotto_approval_request_get_id(pending), FALSE, FALSE, 0, NULL, decided, &denied);
  WAIT(denied.done && other.done); g_assert_no_error(denied.error); g_assert_nonnull(other.error); clear(&denied); clear(&other);
  /* Malformed and stale AUTH never reach review or inherit relay permission. */
  const gchar *bad_tags[] = {
    "[[\"relay\",\"wss://nos.lol\"],[\"challenge\",\"\"]]",
    "[[\"relay\",\"wss://nos.lol\"],[\"relay\",\"wss://nos.lol\"],[\"challenge\",\"x\"]]",
    "[[\"relay\",\"https://nos.lol\"],[\"challenge\",\"x\"]]",
    "[[\"relay\",\"wss://nos.lol\"],[\"challenge\",\"x\"]]"
  };
  for (guint i = 0; i < G_N_ELEMENTS(bad_tags); i++) {
    Reply invalid = {0}; guint before = prompts;
    g_autofree gchar *json = g_strdup_printf("{\"kind\":22242,\"created_at\":%" G_GINT64_FORMAT ",\"tags\":%s,\"content\":\"\"}",
        i == 3 ? (gint64)1 : g_get_real_time() / G_USEC_PER_SEC, bad_tags[i]);
    call(nostrc_test_bus_connect(bus), "SignEvent", g_variant_new("(sss)", json, "", "g15-client"), &invalid);
    WAIT(invalid.done); g_assert_nonnull(invalid.error);
    g_autofree gchar *remote = g_dbus_error_get_remote_error(invalid.error);
    g_assert_cmpstr(remote, ==, "org.nostr.Signer.Error.InvalidInput");
    g_assert_cmpuint(prompts, ==, before); clear(&invalid);
  }
  /* Removed method is absent from the real bus, not merely from source XML. */
  Reply removed = {0}; call(connection, "NIP44DeriveConversationKey", g_variant_new("(sss)", "", "", ""), &removed);
  WAIT(removed.done); g_assert_error(removed.error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD); clear(&removed);
  atomic_and_pending(connection, controller);
#ifdef __APPLE__
  Reply direct = {0};
  call(nostrc_test_bus_connect(bus), "NIP44Encrypt", g_variant_new("(sss)", "claimless", "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", ""), &direct);
  WAIT(ready(controller) || direct.done); g_assert_false(direct.done);
  g_autoptr(GrottoApprovalRequest) claimless = g_list_model_get_item(grotto_approval_controller_get_pending(controller), 0);
  gboolean rememberable = TRUE;
  g_assert_true(g_variant_lookup(grotto_approval_request_get_info(claimless), "rememberable", "b", &rememberable)); g_assert_false(rememberable);
  Reply refusal = {0}; management(connection, "DecideRequest", g_variant_new("(ss)", grotto_approval_request_get_id(claimless), "allow"), &refusal);
  WAIT(refusal.done); g_assert_nonnull(refusal.error); g_assert_false(direct.done); clear(&refusal);
  grotto_approval_controller_decide_async(controller, grotto_approval_request_get_id(claimless), TRUE, FALSE, 0, NULL, decided, &refusal);
  WAIT(refusal.done && direct.done); g_assert_no_error(refusal.error); g_assert_no_error(direct.error); clear(&refusal); clear(&direct);
#endif
  grotto_approval_controller_stop(controller); g_object_unref(controller);
  stop_daemon(pidfile);
  /* Two real stored fixture identities: empty selectors follow the daemon's
   * default, but the grant and explanation remain pinned to the exact npub. */
  gboolean secrets_online = FALSE;
  guint secrets_watch = g_bus_watch_name_on_connection(connection, "org.freedesktop.secrets", 0, service_ready, NULL, &secrets_online, NULL);
  const gchar *secret_command[] = {argv[2], "--fake-secret-service", "plain",
      "0000000000000000000000000000000000000000000000000000000000000001",
      "0000000000000000000000000000000000000000000000000000000000000002", NULL};
  nostrc_test_bus_spawn_supervised(bus, "g15-secrets.log", NULL, secret_command); WAIT(secrets_online);
  g_unsetenv("NOSTR_SIGNER_SECKEY_HEX");
  nostrc_test_bus_spawn_supervised(bus, "g15-identities.log", NULL, command); WAIT(online);
  controller = grotto_approval_controller_new(app, connection);
  Reply drift = {0}, set = {0};
  management(connection, "SetDefaultIdentityForLegacy", g_variant_new("(s)", client_identity), &set);
  WAIT(set.done); g_assert_no_error(set.error); clear(&set);
  sign_kind(NOSTR_KIND_PROFILE_METADATA, NULL, &drift); WAIT(ready(controller) || drift.done); g_assert_false(drift.done);
  approve(controller, 0); verify(&drift, NOSTR_KIND_PROFILE_METADATA); clear(&drift);
  WAIT(g_list_model_get_n_items(grotto_approval_controller_get_pending(controller)) == 0);
  const gchar *identity_b = "npub1ccz8l9zpa47k6vz9gphftsrumpw80rjt3nhnefat4symjhrsnmjs38mnyd";
  management(connection, "SetDefaultIdentityForLegacy", g_variant_new("(s)", identity_b), &set);
  WAIT(set.done); g_assert_no_error(set.error); clear(&set);
  sign_kind(NOSTR_KIND_PROFILE_METADATA, NULL, &drift); WAIT(ready(controller) || drift.done); g_assert_false(drift.done);
  g_autoptr(GrottoApprovalRequest) switched = g_list_model_get_item(grotto_approval_controller_get_pending(controller), 0);
  GVariant *switched_info = grotto_approval_request_get_info(switched); gboolean explained = FALSE; const gchar *pinned = NULL;
  g_assert_true(g_variant_lookup(switched_info, "identity_drift", "b", &explained)); g_assert_true(explained);
  g_assert_true(g_variant_lookup(switched_info, "identity", "&s", &pinned)); g_assert_cmpstr(pinned, ==, identity_b);
  approve(controller, 0); verify(&drift, NOSTR_KIND_PROFILE_METADATA); clear(&drift);
  WAIT(g_list_model_get_n_items(grotto_approval_controller_get_pending(controller)) == 0);
  guint before_pinned = prompts;
  call(nostrc_test_bus_connect(bus), "SignEvent", g_variant_new("(sss)", "{\"kind\":0,\"tags\":[],\"content\":\"\"}", client_identity, "g15-client"), &drift);
  verify(&drift, NOSTR_KIND_PROFILE_METADATA); g_assert_cmpuint(prompts, ==, before_pinned); clear(&drift);
  grotto_approval_controller_stop(controller); g_object_unref(controller); stop_daemon(pidfile);
  g_bus_unwatch_name(secrets_watch);
  g_bus_unwatch_name(watch); g_dbus_connection_signal_unsubscribe(connection, sub);
  nostrc_test_bus_down(bus); nostr_json_cleanup(); g_free(client_principal); g_free(client_identity);
  g_print("G15: real-daemon remembered-kind matrix and canonical AUTH isolation passed across restart\n");
  return 0;
}
