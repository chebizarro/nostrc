#ifndef TEST_CONTROL_COMMON_H
#define TEST_CONTROL_COMMON_H
#include "grotto_control.h"
#include "grotto_clients.h"
#include "signer_caller.h"
#include "event_history.h"
#include "tests/common/nostrc-test-bus.h"
#include <glib/gstdio.h>
#include <string.h>
#include <sys/resource.h>

static const gchar TEST_KEY[] = "0000000000000000000000000000000000000000000000000000000000000001";
typedef struct { guint count; gchar *error, *value; } TestReply;
typedef struct {
  NostrcTestBus *bus;
  GDBusConnection *server, *client;
  SignerApprovalService *approval;
  GrottoControl *control;
  GPtrArray *pending;
  guint finished, watch;
} ControlFixture;
static gboolean failure_bound(gpointer data) { (void)data; g_error("Control1 test did not complete"); return G_SOURCE_REMOVE; }
static void wait_count(guint *count, guint target) {
  guint timeout = g_timeout_add_seconds(20, failure_bound, NULL);
  while (*count < target) g_main_context_iteration(NULL, TRUE);
  g_source_remove(timeout);
}
static void gate_reply(gpointer data, const gchar *error, const gchar *message, const gchar *value) {
  (void)message; TestReply *r = data; g_assert_cmpuint(++r->count, ==, 1);
  r->error = g_strdup(error); r->value = g_strdup(value);
}
static void clear_reply(TestReply *r) { g_free(r->error); g_free(r->value); }
static void observe(gpointer data, GVariant *row) {
  ControlFixture *f = data; const gchar *state = NULL; g_variant_lookup(row, "state", "&s", &state);
  if (!strcmp(state, "PENDING")) g_ptr_array_add(f->pending, g_variant_ref(row)); else f->finished++;
}
static void fixture_setup(ControlFixture *f, gconstpointer data) {
  (void)data; nostrc_test_bus_tolerate_ebadf();
  f->bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE); nostrc_test_bus_up(f->bus);
  f->server = nostrc_test_bus_connect(f->bus); f->client = nostrc_test_bus_connect(f->bus);
  f->approval = signer_approval_service_new(NULL);
  f->pending = g_ptr_array_new_with_free_func((GDestroyNotify)g_variant_unref);
  f->watch = signer_approval_service_subscribe(f->approval, observe, f);
  g_autoptr(GError) error = NULL;
  f->control = grotto_control_new(f->server, f->approval, NULL, &error);
  g_assert_no_error(error); g_assert_nonnull(f->control);
}
static void fixture_teardown(ControlFixture *f, gconstpointer data) {
  (void)data; signer_approval_service_shutdown(f->approval);
  grotto_control_free(f->control); signer_approval_service_unsubscribe(f->approval, f->watch);
  signer_approval_service_unref(f->approval); g_ptr_array_unref(f->pending); nostrc_test_bus_down(f->bus);
  const gchar *files[] = {"grotto/clients.ini", "grotto/activity.json", "gnostr/signer-permissions-v2.ini", "gnostr/signer-grants.ini"};
  for (guint i = 0; i < G_N_ELEMENTS(files); i++) {
    g_autofree gchar *path = g_build_filename(g_get_user_config_dir(), files[i], NULL); g_unlink(path);
  }
}
typedef struct { GVariant *value; GError *error; guint done; } BusReply;
static void call_done(GObject *object, GAsyncResult *result, gpointer data) {
  BusReply *r = data; r->value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &r->error); r->done++;
}
static GVariant *control_call_to(ControlFixture *f, const gchar *destination,
    const gchar *method, GVariant *args, GError **error) {
  BusReply r = {0};
  g_dbus_connection_call(f->client, destination, GROTTO_CONTROL_PATH, GROTTO_CONTROL_INTERFACE,
      method, args, NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, call_done, &r);
  wait_count(&r.done, 1);
  if (r.error) g_propagate_error(error, r.error);
  return r.value;
}
static GVariant *control_call(ControlFixture *f, const gchar *method, GVariant *args) {
  g_autoptr(GError) error = NULL;
  GVariant *r = control_call_to(f, g_dbus_connection_get_unique_name(f->server), method, args, &error);
  g_assert_no_error(error); g_assert_nonnull(r); return r;
}
static void expect_control_error(ControlFixture *f, const gchar *method, GVariant *args, const gchar *name) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) r = control_call_to(f, g_dbus_connection_get_unique_name(f->server), method, args, &error);
  g_assert_null(r); g_assert_nonnull(error);
  g_autofree gchar *remote = g_dbus_error_get_remote_error(error); g_assert_cmpstr(remote, ==, name);
}
static GVariant *get_status(ControlFixture *f) {
  g_autoptr(GVariant) tuple = control_call(f, "GetStatus", NULL);
  return g_variant_get_child_value(tuple, 0);
}
static GVariant *get_rows(ControlFixture *f, const gchar *method, GVariant *args) {
  g_autoptr(GVariant) tuple = control_call(f, method, args); return g_variant_get_child_value(tuple, 0);
}
static guint64 get_revision(ControlFixture *f) {
  g_autoptr(GVariant) status = get_status(f); guint64 revision;
  g_assert_true(g_variant_lookup(status, "permissions_revision", "t", &revision)); return revision;
}
static SignerGateHandle *submit_request(ControlFixture *f, SignerCaller *who, SignerTransport transport,
    SignerGateOp op, const gchar *argument, gint64 deadline, TestReply *reply) {
  SignerGateContext context = {.transport=transport, .no_identity=op==SIGNER_GATE_PING, .claimed_name="untrusted label", .deadline=deadline};
  return signer_approval_service_submit(f->approval, who, who->sender, op, argument, NULL, "", &context, gate_reply, reply);
}
static const gchar *pending_id(ControlFixture *f, guint index) {
  guint timeout = g_timeout_add_seconds(20, failure_bound, NULL);
  while (f->pending->len <= index) g_main_context_iteration(NULL, TRUE);
  g_source_remove(timeout); const gchar *id;
  g_assert_true(g_variant_lookup(f->pending->pdata[index], "request_id", "&s", &id)); return id;
}
static void decided(gpointer data, gboolean ok, const gchar *error, const gchar *message) {
  (void)ok; (void)error; (void)message; (*(guint *)data)++;
}
static void decision(ControlFixture *f, const gchar *id, gboolean allow, gboolean remember) {
  guint done = 0; signer_approval_service_decide(f->approval, id, allow, remember, 600, decided, &done);
  wait_count(&done, 1);
}
static void test_environment(const gchar *binary) {
  struct rlimit limit = {0, 0}; setrlimit(RLIMIT_CORE, &limit);
  g_setenv("NOSTR_SIGNER_SECKEY_HEX", TEST_KEY, TRUE);
  g_setenv("NOSTR_SIGNER_TEST_APPROVERS", binary, TRUE);
}
#endif
