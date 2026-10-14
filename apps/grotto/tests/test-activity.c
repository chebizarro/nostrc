#include "test-control-common.h"
#include <json-glib/json-glib.h>
#include <nostr-kinds.h>

static void signal_seen(GDBusConnection *connection, const gchar *sender, const gchar *path,
    const gchar *interface, const gchar *signal, GVariant *args, gpointer data) {
  (void)connection; (void)sender; (void)path; (void)interface;
  guint64 *sequences = data, value; g_variant_get(args, "(t)", &value);
  if (!strcmp(signal, "ActivityChanged")) { g_assert_cmpuint(value, >=, sequences[1]); sequences[1] = value; sequences[0]++; }
}
static void assert_outcome(ControlFixture *f, const gchar *outcome) {
  g_autoptr(GVariant) rows = get_rows(f, "ListActivity", g_variant_new("(tu)", (guint64)0, (guint)100));
  g_assert_cmpuint(g_variant_n_children(rows), >, 0);
  g_autoptr(GVariant) row = g_variant_get_child_value(rows, 0); const gchar *actual;
  g_assert_true(g_variant_lookup(row, "outcome", "&s", &actual)); g_assert_cmpstr(actual, ==, outcome);
  const gchar *forbidden[] = {"preview", "content", "reason", "flow_id", "uri", "event_json", "ciphertext", "event_summary", "result"};
  for (guint i = 0; i < G_N_ELEMENTS(forbidden); i++) g_assert_null(g_variant_lookup_value(row, forbidden[i], NULL));
}
static void test_dashboard(ControlFixture *f, gconstpointer data) {
  (void)data; SignerCaller who = {.kind=SIGNER_CALLER_EXE, .principal="exe:/dashboard", .sender="dashboard", .same_uid=TRUE};
  g_test_message("dashboard: first consent");
  TestReply consent = {0}; g_autoptr(SignerGateHandle) hc = submit_request(f, &who, SIGNER_TRANSPORT_DBUS, SIGNER_GATE_GET_PUBLIC_KEY, NULL, 0, &consent);
  decision(f, pending_id(f, 0), TRUE, FALSE); wait_count(&consent.count, 1); clear_reply(&consent);
  g_autoptr(GVariant) status = get_status(f); guint count;
  g_assert_true(g_variant_lookup(status, "client_count", "u", &count)); g_assert_cmpuint(count, ==, 1);
  g_assert_true(g_variant_lookup(status, "signed_today", "u", &count)); g_assert_cmpuint(count, ==, 0);
  g_autofree gchar *event = g_strdup_printf("{\"kind\":%u,\"created_at\":1700000000,\"tags\":[],\"content\":\"\"}", NOSTR_KIND_TEXT_NOTE);
  who.sender = "dashboard-sign"; /* Separate connection, same verified client. */
  TestReply signed_event = {0}; g_autoptr(SignerGateHandle) hs = submit_request(f, &who, SIGNER_TRANSPORT_DBUS, SIGNER_GATE_SIGN_EVENT, event, 0, &signed_event);
  g_test_message("dashboard: event submitted, replies=%u pending=%u", signed_event.count, f->pending->len);
  guint bound = g_timeout_add_seconds(20, failure_bound, NULL);
  while (f->pending->len < 2 && !signed_event.count) g_main_context_iteration(NULL, TRUE);
  g_source_remove(bound); g_test_message("dashboard: resolved=%u error=%s", signed_event.count, signed_event.error ? signed_event.error : "none");
  g_assert_null(signed_event.error); g_assert_cmpuint(signed_event.count, ==, 0);
  const gchar *id = pending_id(f, 1); g_test_message("dashboard: reviewing event"); g_clear_pointer(&status, g_variant_unref); status = get_status(f);
  g_assert_true(g_variant_lookup(status, "pending_count", "u", &count)); g_assert_cmpuint(count, ==, 1);
  decision(f, id, TRUE, FALSE); wait_count(&signed_event.count, 1); g_assert_null(signed_event.error); clear_reply(&signed_event);
  g_clear_pointer(&status, g_variant_unref); status = get_status(f);
  g_assert_true(g_variant_lookup(status, "signed_today", "u", &count)); g_assert_cmpuint(count, ==, 1);
  g_assert_true(g_variant_lookup(status, "pending_count", "u", &count)); g_assert_cmpuint(count, ==, 0);
  g_autoptr(GVariant) cleared = control_call(f, "ClearActivity", NULL);
  g_clear_pointer(&status, g_variant_unref); status = get_status(f);
  g_assert_true(g_variant_lookup(status, "signed_today", "u", &count)); g_assert_cmpuint(count, ==, 0);
}
static void test_truth(ControlFixture *f, gconstpointer data) {
  (void)data; guint64 seqs[2] = {0};
  guint signals = g_dbus_connection_signal_subscribe(f->client, g_dbus_connection_get_unique_name(f->server),
      GROTTO_CONTROL_INTERFACE, "ActivityChanged", GROTTO_CONTROL_PATH, NULL, 0, signal_seen, seqs, NULL);
  SignerCaller who = {.kind=SIGNER_CALLER_EXE, .principal="exe:/success", .sender="success", .same_uid=TRUE};
  TestReply success = {0}; g_autoptr(SignerGateHandle) hs = submit_request(f, &who, SIGNER_TRANSPORT_DBUS, SIGNER_GATE_GET_PUBLIC_KEY, NULL, 0, &success);
  decision(f, pending_id(f, 0), TRUE, FALSE); wait_count(&success.count, 1); g_assert_null(success.error); assert_outcome(f, "success"); clear_reply(&success);
  who.principal = "exe:/denied"; who.sender = "denied"; TestReply denied = {0};
  g_autoptr(SignerGateHandle) hd = submit_request(f, &who, SIGNER_TRANSPORT_NIP5F, SIGNER_GATE_GET_PUBLIC_KEY, NULL, 0, &denied);
  decision(f, pending_id(f, 1), FALSE, FALSE); wait_count(&denied.count, 1); assert_outcome(f, "denied"); clear_reply(&denied);
  who.principal = "exe:/expired"; who.sender = "expired"; TestReply expired = {0};
  g_autoptr(SignerGateHandle) he = submit_request(f, &who, SIGNER_TRANSPORT_DBUS, SIGNER_GATE_GET_PUBLIC_KEY, NULL, g_get_monotonic_time() - 1, &expired);
  wait_count(&expired.count, 1); assert_outcome(f, "expired"); clear_reply(&expired);
  who.principal = "exe:/disconnected"; who.sender = "disconnected"; TestReply gone = {0};
  g_autoptr(SignerGateHandle) hg = submit_request(f, &who, SIGNER_TRANSPORT_DBUS, SIGNER_GATE_GET_PUBLIC_KEY, NULL, 0, &gone);
  pending_id(f, 2); signer_approval_service_connection_closed(f->approval, who.sender);
  wait_count(&gone.count, 1); assert_outcome(f, "expired-caller"); clear_reply(&gone);
  who.principal = "exe:/paused"; who.sender = "paused"; TestReply paused = {0};
  g_autoptr(SignerGateHandle) hp = submit_request(f, &who, SIGNER_TRANSPORT_NIP46, SIGNER_GATE_GET_PUBLIC_KEY, NULL, 0, &paused);
  pending_id(f, 3); g_autoptr(GVariant) pause = control_call(f, "SetPaused", g_variant_new("(b)", TRUE));
  wait_count(&paused.count, 1); assert_outcome(f, "cancelled"); clear_reply(&paused);
  g_autoptr(GVariant) resume = control_call(f, "SetPaused", g_variant_new("(b)", FALSE));
  who.principal = "exe:/malformed"; who.sender = "malformed"; TestReply bad = {0};
  g_autoptr(SignerGateHandle) hf = submit_request(f, &who, SIGNER_TRANSPORT_DBUS, SIGNER_GATE_SIGN_EVENT, "invalid secret-payload-canary", 0, &bad);
  wait_count(&bad.count, 1); assert_outcome(f, "failed"); clear_reply(&bad);
  guint bound = g_timeout_add_seconds(20, failure_bound, NULL);
  while (seqs[0] < 6) g_main_context_iteration(NULL, TRUE); g_source_remove(bound); g_assert_cmpuint(seqs[1], ==, 6);
  g_dbus_connection_signal_unsubscribe(f->client, signals);
  g_autoptr(GVariant) rows = get_rows(f, "ListActivity", g_variant_new("(tu)", (guint64)4, (guint)2));
  g_assert_cmpuint(g_variant_n_children(rows), ==, 2);
  g_autoptr(GVariant) first = g_variant_get_child_value(rows, 0); guint64 seq; g_variant_lookup(first, "sequence", "t", &seq); g_assert_cmpuint(seq, ==, 3);
  expect_control_error(f, "ListActivity", g_variant_new("(tu)", (guint64)0, (guint)101), GROTTO_CONTROL_ERROR "InvalidInput");
  g_autofree gchar *path = g_build_filename(g_get_user_config_dir(), "grotto", "activity.json", NULL);
  g_autofree gchar *contents = NULL; g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
  g_assert_null(strstr(contents, "secret-payload-canary")); g_assert_null(strstr(contents, "untrusted label"));
  GStatBuf st; g_assert_cmpint(g_stat(path, &st), ==, 0); g_assert_cmpint(st.st_mode & 0777, ==, 0600);
  g_autoptr(GnEventHistory) reload = gn_event_history_new_activity(NULL); g_assert_true(gn_event_history_activity_healthy(reload));
  g_assert_cmpuint(gn_event_history_activity_sequence(reload), ==, 6);
  g_autoptr(GVariant) reloaded = gn_event_history_list_activity(reload, 0, 100); g_assert_cmpuint(g_variant_n_children(reloaded), ==, 6);
  g_autoptr(GVariant) cleared = control_call(f, "ClearActivity", NULL);
  g_autoptr(GVariant) empty = get_rows(f, "ListActivity", g_variant_new("(tu)", (guint64)0, (guint)100)); g_assert_cmpuint(g_variant_n_children(empty), ==, 0);
  g_clear_object(&reload); reload = gn_event_history_new_activity(NULL); g_assert_cmpuint(gn_event_history_activity_sequence(reload), ==, 6);
}
static GVariant *terminal_snapshot(const gchar *state) {
  return g_variant_ref_sink(g_variant_new_parsed("{'state': <%s>, 'request_id': <'test'>, 'transport': <'DBUS'>, 'principal': <'exe:/test'>, 'identity': <'public-identity'>, 'operation': <'sign_event'>, 'event_kind': <uint32 1>, 'preview': <'PRIVATE PREVIEW'>, 'reason': <'PRIVATE REASON'>, 'error_code': <'secret://PRIVATE ERROR'>}", state));
}
static void test_failure(ControlFixture *f, gconstpointer data) {
  (void)data;
  /* Logging failure is visible but never changes the successful gate response. */
  g_autofree gchar *path = g_build_filename(g_get_user_config_dir(), "grotto", "activity.json", NULL);
  g_autofree gchar *dir = g_path_get_dirname(path); g_mkdir_with_parents(dir, 0700); g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
  SignerCaller who = {.kind=SIGNER_CALLER_EXE, .principal="exe:/log-failure", .sender="log-failure", .same_uid=TRUE};
  TestReply reply = {0}; g_autoptr(SignerGateHandle) h = submit_request(f, &who, SIGNER_TRANSPORT_DBUS, SIGNER_GATE_GET_PUBLIC_KEY, NULL, 0, &reply);
  decision(f, pending_id(f, 0), TRUE, FALSE); wait_count(&reply.count, 1); g_assert_null(reply.error); g_assert_nonnull(reply.value); clear_reply(&reply);
  g_autoptr(GVariant) status = get_status(f); gboolean healthy;
  g_assert_true(g_variant_lookup(status, "activity_log_healthy", "b", &healthy)); g_assert_false(healthy);
  expect_control_error(f, "ClearActivity", NULL, GROTTO_CONTROL_ERROR "PersistenceFailed"); assert_outcome(f, "success");
  g_assert_cmpint(g_rmdir(path), ==, 0);
  g_autoptr(GVariant) cleared = control_call(f, "ClearActivity", NULL);
  g_clear_pointer(&status, g_variant_unref); status = get_status(f);
  g_assert_true(g_variant_lookup(status, "activity_log_healthy", "b", &healthy)); g_assert_true(healthy);
  g_autoptr(GnEventHistory) history = gn_event_history_new_activity(NULL);
  g_autoptr(GVariant) snapshot = terminal_snapshot("FAILED"); gn_event_history_record_activity(history, snapshot);
  g_autofree gchar *contents = NULL; g_file_get_contents(path, &contents, NULL, NULL);
  g_assert_null(strstr(contents, "PRIVATE"));
}
static void test_retention(ControlFixture *f, gconstpointer data) {
  (void)f; (void)data; g_autofree gchar *root = g_dir_make_tmp("g04-activity-XXXXXX", NULL);
  g_autofree gchar *dir = g_build_filename(root, "grotto", NULL); g_mkdir(dir, 0700);
  g_autofree gchar *path = g_build_filename(dir, "activity.json", NULL);
  GString *json = g_string_new("{\"version\":1,\"sequence\":10002,\"records\":[");
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  for (guint i = 10002; i > 0; i--) {
    if (i != 10002) g_string_append_c(json, ',');
    g_string_append_printf(json, "{\"sequence\":%u,\"timestamp\":%" G_GINT64_FORMAT ",\"request_id\":\"test\",\"transport\":\"DBUS\",\"principal\":\"p\",\"identity\":\"i\",\"operation\":\"ping\",\"outcome\":\"success\",\"error_code\":\"\"}", i, i == 10002 ? now - 31 * 24 * 60 * 60 : i == 10001 ? now - 2 * 24 * 60 * 60 : now);
  }
  g_string_append(json, "]}"); g_assert_true(g_file_set_contents(path, json->str, json->len, NULL)); g_string_free(json, TRUE);
  g_autoptr(GnEventHistory) history = gn_event_history_new_activity(root); g_assert_true(gn_event_history_activity_healthy(history));
  g_assert_cmpuint(gn_event_history_get_entry_count(history), ==, 10000);
  g_autoptr(GVariant) snapshot = terminal_snapshot("SUCCEEDED");
  g_assert_cmpuint(gn_event_history_signed_today(history), ==, 0);
  g_assert_cmpuint(gn_event_history_record_activity(history, snapshot), ==, 10003);
  gn_event_history_record_activity(history, snapshot); g_assert_cmpuint(gn_event_history_get_entry_count(history), ==, 10000);
  g_assert_cmpuint(gn_event_history_signed_today(history), ==, 2);
  g_autoptr(GVariant) denied = terminal_snapshot("DENIED"); gn_event_history_record_activity(history, denied);
  g_assert_cmpuint(gn_event_history_signed_today(history), ==, 2);
  gn_event_history_set_retention_days(history, 1);
  g_assert_cmpuint(gn_event_history_get_entry_count(history), ==, 9999);
  g_assert_cmpuint(gn_event_history_signed_today(history), ==, 2);
  g_autoptr(GVariant) rows = gn_event_history_list_activity(history, 0, 100); g_assert_cmpuint(g_variant_n_children(rows), ==, 100);
  g_assert_true(gn_event_history_clear_activity(history, NULL)); g_clear_object(&history);
  history = gn_event_history_new_activity(root); g_assert_cmpuint(gn_event_history_activity_sequence(history), ==, 10005);
  g_assert_true(g_file_set_contents(path, "{\"version\":2,\"sequence\":10004,\"records\":[]}", -1, NULL));
  g_clear_object(&history); history = gn_event_history_new_activity(root); g_assert_false(gn_event_history_activity_healthy(history));
  g_assert_false(gn_event_history_clear_activity(history, NULL));
  g_autofree gchar *contents = NULL; g_file_get_contents(path, &contents, NULL, NULL); g_assert_nonnull(strstr(contents, "\"version\":2"));
  g_unlink(path); g_rmdir(dir); g_rmdir(root);
}
/* Review F1: a read-only (corrupt or unsupported) store keeps recording while
 * signing continues; its in-memory records stay within the count bound with no
 * intervening ListActivity, and the file it could not read is never rewritten. */
static void record_past_cap(const gchar *contents) {
  g_autofree gchar *root = g_dir_make_tmp("g04-activity-XXXXXX", NULL);
  g_autofree gchar *dir = g_build_filename(root, "grotto", NULL); g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
  g_autofree gchar *path = g_build_filename(dir, "activity.json", NULL);
  g_assert_true(g_file_set_contents(path, contents, -1, NULL));
  g_autoptr(GnEventHistory) history = gn_event_history_new_activity(root);
  g_assert_false(gn_event_history_activity_healthy(history));
  g_autoptr(GVariant) snapshot = terminal_snapshot("SUCCEEDED");
  guint64 previous = gn_event_history_activity_sequence(history);
  for (guint i = 0; i < 10001; i++) {
    guint64 sequence = gn_event_history_record_activity(history, snapshot);
    g_assert_cmpuint(sequence, ==, previous + 1); previous = sequence;
  }
  g_assert_cmpuint(gn_event_history_get_entry_count(history), ==, 10000);
  g_assert_false(gn_event_history_activity_healthy(history));
  g_autofree gchar *after = NULL; g_assert_true(g_file_get_contents(path, &after, NULL, NULL));
  g_assert_cmpstr(after, ==, contents);
  g_clear_object(&history); g_unlink(path); g_rmdir(dir); g_rmdir(root);
}
static void test_unhealthy_bound(ControlFixture *f, gconstpointer data) {
  (void)f; (void)data;
  record_past_cap("{\"version\":1,\"sequence\":"); /* corrupt */
  record_past_cap("{\"version\":2,\"sequence\":0,\"records\":[]}"); /* unsupported schema */
  /* A stored error code obeys the writer's name-only rule, or the file is corrupt. */
  record_past_cap("{\"version\":1,\"sequence\":1,\"records\":[{\"sequence\":1,\"timestamp\":1,"
      "\"request_id\":\"r\",\"transport\":\"DBUS\",\"principal\":\"p\",\"identity\":\"i\","
      "\"operation\":\"ping\",\"outcome\":\"failed\",\"error_code\":\"secret://PRIVATE ERROR\"}]}");
}
int main(int argc, char **argv) {
  test_environment(argv[0]); g_test_init(&argc, &argv, NULL); if (!nostrc_test_bus_available()) return 77;
#define ADD(name, fn) g_test_add(name, ControlFixture, NULL, fixture_setup, fn, fixture_teardown)
  ADD("/activity/outcome-truth-private-bus", test_truth); ADD("/activity/write-failure-does-not-change-crypto", test_failure);
  ADD("/activity/dashboard-real-gate-status", test_dashboard);
  ADD("/activity/retention-cap-restart-schema", test_retention);
  ADD("/activity/unhealthy-store-stays-bounded", test_unhealthy_bound);
  return g_test_run();
}
