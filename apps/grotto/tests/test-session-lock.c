#include "session-lock.h"
#include "grotto_control.h"
#include "nostrc-test-bus.h"
#include <unistd.h>

static NostrcTestBus *bus;
static GDBusConnection *server, *client;
static gboolean hint, fallback;
static guint uid, reads, changes, pid_queries, user_queries;
static GQueue deferred_reads = G_QUEUE_INIT;
static const char *deferred_peer;
static gboolean defer_read;
#define ROOT "/org/freedesktop/login1"
#define PATH ROOT "/session/test"
#define USER ROOT "/user/test"
#define IFACE "org.freedesktop.login1.Session"
static gboolean bound(gpointer data) {
  g_error("Session-lock readiness timeout at line %d (PID queries=%u, User queries=%u, snapshots=%u, changes=%u)",
      GPOINTER_TO_INT(data), pid_queries, user_queries, reads, changes);
  return G_SOURCE_REMOVE;
}
#define WAIT(condition) do { guint timeout = g_timeout_add_seconds(5, bound, GINT_TO_POINTER(__LINE__)); \
  while (!(condition)) { g_main_context_iteration(NULL, TRUE); } g_source_remove(timeout); } while (0)
static void own(gboolean enabled) {
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(server, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      enabled ? "RequestName" : "ReleaseName", enabled ? g_variant_new("(su)", "org.freedesktop.login1", 0) :
      g_variant_new("(s)", "org.freedesktop.login1"), NULL, 0, 2000, NULL, NULL);
  g_assert_nonnull(reply);
}
static GVariant *properties(void) {
  GVariantBuilder values; g_variant_builder_init(&values, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&values, "{sv}", "User", g_variant_new("(uo)", uid, USER));
  g_variant_builder_add(&values, "{sv}", "LockedHint", g_variant_new_boolean(hint));
  return g_variant_new("(a{sv})", &values);
}
static void method(GDBusConnection *connection, const char *sender, const char *path,
    const char *interface, const char *name, GVariant *args, GDBusMethodInvocation *inv, gpointer data) {
  (void)connection; (void)sender; (void)path; (void)interface; (void)args; (void)data;
  if (!strcmp(name, "GetSessionByPID")) {
    pid_queries++;
    if (fallback) g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.login1.NoSessionForPID", "User service outside a session");
    else g_dbus_method_invocation_return_value(inv, g_variant_new("(o)", PATH));
  } else if (!strcmp(name, "GetUser")) {
    user_queries++; g_dbus_method_invocation_return_value(inv, g_variant_new("(o)", USER));
  }
  else if (!strcmp(name, "Get")) g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", g_variant_new("(so)", "test", PATH)));
  else if (!strcmp(name, "GetAll")) {
    reads++;
    if (defer_read && !g_strcmp0(sender, deferred_peer)) g_queue_push_tail(&deferred_reads, g_object_ref(inv));
    else g_dbus_method_invocation_return_value(inv, properties());
  } else g_assert_not_reached();
}
static void event(const char *signal) {
  g_assert_true(g_dbus_connection_emit_signal(server, NULL, PATH, IFACE, signal, NULL, NULL));
}
static void changed(GrottoSessionLock *lock, gpointer data) { (void)lock; (void)data; changes++; }
static void hint_changed(gboolean locked) {
  hint = locked;
  GVariantBuilder values; g_variant_builder_init(&values, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&values, "{sv}", "LockedHint", g_variant_new_boolean(hint));
  const char *invalid[] = {NULL};
  g_assert_true(g_dbus_connection_emit_signal(server, NULL, PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
      g_variant_new("(sa{sv}^as)", IFACE, &values, invalid), NULL));
}
static void test_lifecycle(void) {
  own(TRUE); hint = FALSE; uid = getuid(); fallback = TRUE;
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Grotto");
  g_settings_set_boolean(settings, "pause-on-screen-lock", TRUE);
  g_autoptr(SignerApprovalService) approval = signer_approval_service_new(NULL);
  g_autoptr(GrottoSessionLock) lock = grotto_session_lock_new(approval, settings, client);
  g_signal_connect(lock, "changed", G_CALLBACK(changed), NULL);
  /* Policy enabled and observation not ready is fail closed, synchronously. */
  g_assert_true(signer_approval_service_is_paused(approval));
  g_assert_false(grotto_session_lock_can_resume(lock));
  WAIT(grotto_session_lock_available(lock));
  g_assert_true(grotto_session_lock_can_resume(lock));
  g_assert_true(signer_approval_service_is_paused(approval)); /* No automatic resume. */
  signer_approval_service_set_paused(approval, FALSE);
  guint64 generation = signer_approval_service_generation(approval);
  /* The broad owner subscription must ignore other Manager signatures. */
  g_assert_true(g_dbus_connection_emit_signal(server, NULL, ROOT, "org.freedesktop.login1.Manager", "UserNew",
      g_variant_new("(uo)", uid, USER), NULL));
  g_assert_true(g_dbus_connection_emit_signal(server, NULL, ROOT, "org.freedesktop.login1.Manager", "PrepareForSleep",
      g_variant_new("(b)", TRUE), NULL));
  /* No GUI, portal, background setting or notification controller exists. */
  g_settings_set_boolean(settings, "run-in-background", FALSE);
  event("Lock"); WAIT(grotto_session_lock_locked(lock));
  g_assert_true(signer_approval_service_is_paused(approval));
  g_assert_cmpuint(signer_approval_service_generation(approval), >, generation);
  g_assert_false(grotto_session_lock_can_resume(lock));
  guint before = changes;
  hint_changed(TRUE); WAIT(changes > before);
  before = changes;
  event("Unlock"); WAIT(changes > before);
  g_assert_true(grotto_session_lock_locked(lock)); /* Unlock is a request, not proof. */
  hint_changed(FALSE); WAIT(!grotto_session_lock_locked(lock));
  g_assert_true(signer_approval_service_is_paused(approval));
  signer_approval_service_set_paused(approval, FALSE);
  hint_changed(TRUE); WAIT(grotto_session_lock_locked(lock));
  g_assert_true(signer_approval_service_is_paused(approval));
  own(FALSE); WAIT(!grotto_session_lock_available(lock));
  g_assert_false(grotto_session_lock_can_resume(lock));
  own(TRUE); WAIT(grotto_session_lock_available(lock));
  g_assert_true(grotto_session_lock_locked(lock));
  grotto_session_lock_stop(lock);
}
static void test_wrong_user_and_enable(void) {
  own(TRUE); hint = FALSE; uid = getuid() + 1; fallback = FALSE;
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Grotto");
  g_settings_set_boolean(settings, "pause-on-screen-lock", FALSE);
  g_autoptr(SignerApprovalService) approval = signer_approval_service_new(NULL);
  g_autoptr(GrottoSessionLock) lock = grotto_session_lock_new(approval, settings, client);
  g_signal_connect(lock, "changed", G_CALLBACK(changed), NULL);
  guint before = changes; WAIT(changes > before);
  before = reads; WAIT(reads > before || reads > 0);
  /* Wait for completion rather than treating a server call as client readiness. */
  before = changes; WAIT(changes > before);
  g_assert_false(grotto_session_lock_available(lock));
  g_settings_set_boolean(settings, "pause-on-screen-lock", TRUE);
  g_assert_true(signer_approval_service_is_paused(approval));
  g_assert_false(grotto_session_lock_can_resume(lock));
  uid = getuid(); hint_changed(TRUE); WAIT(grotto_session_lock_available(lock));
  g_assert_true(grotto_session_lock_locked(lock));
  g_settings_set_boolean(settings, "pause-on-screen-lock", FALSE);
  g_assert_false(grotto_session_lock_can_resume(lock)); /* Known lock always forbids resume. */
  grotto_session_lock_stop(lock);
}
static void test_stale_read_and_stop(void) {
  own(TRUE); hint = FALSE; uid = getuid(); fallback = FALSE;
  /* A cancelled previous case can still reach the mock server. Defer only
   * this peer's snapshots, never mistake another generation's call for ours. */
  GDBusConnection *peer = nostrc_test_bus_connect(bus);
  deferred_peer = g_dbus_connection_get_unique_name(peer); defer_read = TRUE;
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Grotto");
  g_settings_set_boolean(settings, "pause-on-screen-lock", TRUE);
  g_autoptr(SignerApprovalService) approval = signer_approval_service_new(NULL);
  g_autoptr(GrottoSessionLock) lock = grotto_session_lock_new(approval, settings, peer);
  WAIT(!g_queue_is_empty(&deferred_reads));
  g_autoptr(GDBusMethodInvocation) first = g_queue_pop_head(&deferred_reads);
  event("Lock"); WAIT(grotto_session_lock_locked(lock));
  g_dbus_method_invocation_return_value(first, properties());
  /* A fresh hint read is an asynchronous barrier; the stale false must not unlock. */
  defer_read = FALSE; hint_changed(TRUE); WAIT(grotto_session_lock_available(lock));
  g_assert_true(grotto_session_lock_locked(lock));
  g_assert_false(grotto_session_lock_can_resume(lock));
  defer_read = TRUE; hint = FALSE;
  GVariantBuilder empty; g_variant_builder_init(&empty, G_VARIANT_TYPE_VARDICT);
  const char *invalid[] = {"LockedHint", NULL};
  g_assert_true(g_dbus_connection_emit_signal(server, NULL, PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
      g_variant_new("(sa{sv}^as)", IFACE, &empty, invalid), NULL));
  WAIT(!g_queue_is_empty(&deferred_reads));
  grotto_session_lock_stop(lock);
  while (!g_queue_is_empty(&deferred_reads)) {
    g_autoptr(GDBusMethodInvocation) pending = g_queue_pop_head(&deferred_reads);
    g_dbus_method_invocation_return_value(pending, properties());
  }
  defer_read = FALSE; deferred_peer = NULL;
  while (g_main_context_iteration(NULL, FALSE)) {}
  g_assert_true(signer_approval_service_is_paused(approval));
}
typedef struct { GVariant *value; GError *error; gboolean done; } Reply;
static void reply_done(GObject *source, GAsyncResult *result, gpointer data) {
  Reply *r = data; r->value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &r->error); r->done = TRUE;
}
static GVariant *control_call(const char *method, GVariant *args, GError **error) {
  Reply r = {0};
  g_dbus_connection_call(client, g_dbus_connection_get_unique_name(server), GROTTO_CONTROL_PATH, GROTTO_CONTROL_INTERFACE,
      method, args, NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, reply_done, &r);
  WAIT(r.done); if (r.error) g_propagate_error(error, r.error); return r.value;
}
static gboolean control_locked(gboolean expected) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) result = control_call("GetStatus", NULL, &error);
  g_assert_no_error(error); g_assert_nonnull(result);
  g_autoptr(GVariant) status = g_variant_get_child_value(result, 0);
  gboolean available = FALSE, locked = FALSE;
  g_assert_true(g_variant_lookup(status, "session_lock_available", "b", &available));
  g_assert_true(g_variant_lookup(status, "session_locked", "b", &locked));
  return available && locked == expected;
}
static void wait_control_locked(gboolean expected) {
  guint timeout = g_timeout_add_seconds(5, bound, GINT_TO_POINTER(__LINE__));
  /* Each call already runs the main loop through its async reply. Do not
   * block for a second event after consuming that reply's status update. */
  while (!control_locked(expected)) {}
  g_source_remove(timeout);
}
static void test_control_resume_guard(void) {
  own(TRUE); hint = FALSE; uid = getuid(); fallback = TRUE;
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Grotto");
  g_settings_set_boolean(settings, "pause-on-screen-lock", TRUE);
  g_settings_set_boolean(settings, "run-in-background", FALSE);
  g_autoptr(SignerApprovalService) approval = signer_approval_service_new(NULL);
  g_autoptr(GError) error = NULL;
  GrottoControl *control = grotto_control_new(server, approval, NULL, &error);
  g_assert_no_error(error); g_assert_nonnull(control);
  wait_control_locked(FALSE);
  g_autoptr(GVariant) result = control_call("SetPaused", g_variant_new("(b)", FALSE), &error);
  g_assert_no_error(error); g_assert_nonnull(result);
  event("Lock"); wait_control_locked(TRUE);
  g_assert_true(signer_approval_service_is_paused(approval));
  g_clear_pointer(&result, g_variant_unref);
  result = control_call("SetPaused", g_variant_new("(b)", FALSE), &error);
  g_assert_null(result); g_assert_nonnull(error);
  g_autofree gchar *remote = g_dbus_error_get_remote_error(error);
  g_assert_cmpstr(remote, ==, GROTTO_CONTROL_ERROR "SessionLocked");
  g_clear_error(&error);
  /* Policy off never bypasses a positively observed locked session. */
  g_settings_set_boolean(settings, "pause-on-screen-lock", FALSE);
  result = control_call("SetPaused", g_variant_new("(b)", FALSE), &error);
  g_assert_null(result); g_assert_nonnull(error); g_clear_error(&error);
  hint_changed(FALSE); wait_control_locked(FALSE);
  g_assert_true(signer_approval_service_is_paused(approval));
  result = control_call("SetPaused", g_variant_new("(b)", FALSE), &error);
  g_assert_no_error(error); g_assert_nonnull(result);
  g_assert_false(signer_approval_service_is_paused(approval));
  grotto_control_free(control);
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_autofree gchar *binary = g_canonicalize_filename(argv[0], NULL);
  g_setenv("NOSTR_SIGNER_TEST_APPROVERS", binary, TRUE);
  g_assert_true(nostrc_test_bus_available());
  bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE); nostrc_test_bus_up(bus);
  g_setenv("DBUS_SYSTEM_BUS_ADDRESS", nostrc_test_bus_get_address(bus), TRUE);
  /* Keep the system-bus singleton alive until the private daemon has stopped. */
  g_autoptr(GDBusConnection) system = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
  g_assert_nonnull(system);
  g_dbus_connection_set_exit_on_close(system, FALSE);
  server = nostrc_test_bus_connect(bus); client = nostrc_test_bus_connect(bus);
  const char *xml = "<node><interface name='org.freedesktop.login1.Manager'>"
      "<method name='GetSessionByPID'><arg type='u' direction='in'/><arg type='o' direction='out'/></method>"
      "<method name='GetUser'><arg type='u' direction='in'/><arg type='o' direction='out'/></method></interface>"
      "<interface name='org.freedesktop.DBus.Properties'>"
      "<method name='GetAll'><arg type='s' direction='in'/><arg type='a{sv}' direction='out'/></method>"
      "<method name='Get'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='out'/></method>"
      "</interface></node>";
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(xml, NULL);
  static const GDBusInterfaceVTable table = {.method_call = method};
  g_assert_cmpuint(g_dbus_connection_register_object(server, ROOT, node->interfaces[0], &table, NULL, NULL, NULL), >, 0);
  g_assert_cmpuint(g_dbus_connection_register_object(server, PATH, node->interfaces[1], &table, NULL, NULL, NULL), >, 0);
  g_assert_cmpuint(g_dbus_connection_register_object(server, USER, node->interfaces[1], &table, NULL, NULL, NULL), >, 0);
  nostrc_test_bus_add_func("/session-lock/daemon-only-lifecycle", test_lifecycle);
  nostrc_test_bus_add_func("/session-lock/own-user-policy", test_wrong_user_and_enable);
  nostrc_test_bus_add_func("/session-lock/stale-read-stop", test_stale_read_and_stop);
  nostrc_test_bus_add_func("/session-lock/control-resume-guard", test_control_resume_guard);
  int result = g_test_run();
  while (g_main_context_iteration(NULL, FALSE)) {}
  nostrc_test_bus_down(bus); return result;
}
