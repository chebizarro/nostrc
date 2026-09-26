/* test_systemd — what the Relays/Notifications switches mean: the pure
 * status model, the exact op plans, and those plans replayed against a
 * mock org.freedesktop.systemd1 Manager that tracks unit state.
 * SPDX-License-Identifier: MIT */
#include "nss-systemd.h"

#include <string.h>

/* ── pure model ── */

static NssUnitState
st(const gchar *load, const gchar *active, const gchar *sub, const gchar *file)
{
  NssUnitState s = { (gchar *)load, (gchar *)active, (gchar *)sub, (gchar *)file };
  return s;
}

static void
test_status_model(void)
{
  struct {
    NssUnitState sock, svc;
    NssServiceStatus want;
  } cases[] = {
    { st("not-found", "inactive", "dead", ""), st("not-found", "inactive", "dead", ""), NSS_SVC_NOT_INSTALLED },
    { st("masked", "inactive", "dead", "masked"), st("loaded", "inactive", "dead", "disabled"), NSS_SVC_MASKED },
    { st("loaded", "inactive", "dead", "disabled"), st("loaded", "inactive", "dead", "disabled"), NSS_SVC_OFF },
    { st("loaded", "inactive", "dead", "enabled"), st("loaded", "inactive", "dead", "disabled"), NSS_SVC_STOPPED },
    { st("loaded", "active", "listening", "enabled"), st("loaded", "inactive", "dead", "disabled"), NSS_SVC_LISTENING },
    { st("loaded", "active", "running", "enabled"), st("loaded", "activating", "start", "disabled"), NSS_SVC_STARTING },
    { st("loaded", "active", "running", "enabled"), st("loaded", "active", "running", "disabled"), NSS_SVC_RUNNING },
    /* Crash loop (nostrc-q9ba): socket fine, service auto-restarting. */
    { st("loaded", "active", "listening", "enabled"), st("loaded", "activating", "auto-restart", "disabled"), NSS_SVC_FAILED },
    { st("loaded", "active", "listening", "enabled"), st("loaded", "failed", "failed", "disabled"), NSS_SVC_FAILED },
    /* Started from a terminal but not enabled: runtime truth wins the
     * status row; the switch (enabled state) stays off. */
    { st("loaded", "active", "listening", "disabled"), st("loaded", "inactive", "dead", "disabled"), NSS_SVC_LISTENING },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    NssServiceStatus got = nss_service_status(&cases[i].sock, &cases[i].svc);
    if (got != cases[i].want)
      g_error("case %" G_GSIZE_FORMAT ": got %d want %d", i, got, cases[i].want);
    g_assert_nonnull(nss_service_status_label(got, TRUE));
  }
  NssUnitState en = st("loaded", "inactive", "dead", "enabled-runtime");
  g_assert_true(nss_unit_file_enabled(&en));
  NssUnitState stat = st("loaded", "inactive", "dead", "static");
  g_assert_false(nss_unit_file_enabled(&stat));
  /* Plain service (notify): no socket. */
  NssUnitState run = st("loaded", "active", "running", "enabled");
  g_assert_cmpint(nss_service_status(NULL, &run), ==, NSS_SVC_RUNNING);
  g_assert_true(nss_service_can_restart(NSS_SVC_FAILED));
  g_assert_false(nss_service_can_restart(NSS_SVC_LISTENING));
  g_assert_false(nss_service_can_restart(NSS_SVC_OFF));
}

static void
test_plans(void)
{
  guint n = 0;
  const NssUnitOp *on = nss_relay_plan(TRUE, &n);
  g_assert_cmpuint(n, ==, 3);
  g_assert_cmpint(on[0].kind, ==, NSS_OP_ENABLE);
  g_assert_cmpstr(on[0].unit, ==, NSS_RELAY_SOCKET);
  g_assert_cmpint(on[1].kind, ==, NSS_OP_RELOAD);
  g_assert_cmpint(on[2].kind, ==, NSS_OP_START);
  g_assert_cmpstr(on[2].unit, ==, NSS_RELAY_SOCKET);
  const NssUnitOp *off = nss_relay_plan(FALSE, &n);
  g_assert_cmpuint(n, ==, 4);
  /* Disable BEFORE stopping, service BEFORE socket. */
  g_assert_cmpint(off[0].kind, ==, NSS_OP_DISABLE);
  g_assert_cmpstr(off[0].unit, ==, NSS_RELAY_SOCKET);
  g_assert_cmpint(off[1].kind, ==, NSS_OP_RELOAD);
  g_assert_cmpint(off[2].kind, ==, NSS_OP_STOP);
  g_assert_cmpstr(off[2].unit, ==, NSS_RELAY_SERVICE);
  g_assert_cmpint(off[3].kind, ==, NSS_OP_STOP);
  g_assert_cmpstr(off[3].unit, ==, NSS_RELAY_SOCKET);
  const NssUnitOp *non = nss_notify_plan(TRUE, &n);
  g_assert_cmpuint(n, ==, 3);
  g_assert_cmpstr(non[2].unit, ==, NSS_NOTIFY_SERVICE);
}

/* ── mock systemd ── */

static const gchar MOCK_XML[] =
  "<node>"
  " <interface name='org.freedesktop.systemd1.Manager'>"
  "  <method name='LoadUnit'><arg type='s' direction='in'/><arg type='o' direction='out'/></method>"
  "  <method name='StartUnit'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='o' direction='out'/></method>"
  "  <method name='StopUnit'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='o' direction='out'/></method>"
  "  <method name='RestartUnit'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='o' direction='out'/></method>"
  "  <method name='EnableUnitFiles'><arg type='as' direction='in'/><arg type='b' direction='in'/><arg type='b' direction='in'/>"
  "   <arg type='b' direction='out'/><arg type='a(sss)' direction='out'/></method>"
  "  <method name='DisableUnitFiles'><arg type='as' direction='in'/><arg type='b' direction='in'/>"
  "   <arg type='a(sss)' direction='out'/></method>"
  "  <method name='Reload'/>"
  " </interface>"
  " <interface name='org.freedesktop.systemd1.Unit'>"
  "  <property name='LoadState' type='s' access='read'/>"
  "  <property name='ActiveState' type='s' access='read'/>"
  "  <property name='SubState' type='s' access='read'/>"
  "  <property name='UnitFileState' type='s' access='read'/>"
  " </interface>"
  "</node>";

typedef struct {
  gchar *name, *load, *active, *sub, *file;
} MockUnit;

static GHashTable *units;   /* name → MockUnit* ; also path → MockUnit* */
static GPtrArray  *calls;   /* "Method unit" */
static gchar      *fail_method;

static gchar *
unit_path(const gchar *name)
{
  GString *s = g_string_new("/org/freedesktop/systemd1/unit/");
  for (const gchar *c = name; *c; c++)
    g_string_append_c(s, g_ascii_isalnum(*c) ? *c : '_');
  return g_string_free(s, FALSE);
}

static void
set(gchar **f, const gchar *v)
{
  g_free(*f);
  *f = g_strdup(v);
}

static void
mgr_call(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
         const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer d)
{
  (void)c; (void)sender; (void)path; (void)iface; (void)d;
  const gchar *unit = NULL;
  if (g_str_equal(method, "LoadUnit") || g_str_has_suffix(method, "Unit"))
    g_variant_get_child(params, 0, "&s", &unit);
  g_autofree const gchar **files = NULL;
  if (g_str_has_suffix(method, "UnitFiles")) {
    g_autoptr(GVariant) a = g_variant_get_child_value(params, 0);
    files = g_variant_get_strv(a, NULL);
    unit = files[0];
  }
  if (!g_str_equal(method, "LoadUnit"))
    g_ptr_array_add(calls, g_strdup_printf("%s%s%s", method, unit ? " " : "", unit ? unit : ""));
  if (fail_method && g_str_equal(fail_method, method)) {
    g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.systemd1.NoSuchUnit",
                                               "mock failure");
    return;
  }
  MockUnit *u = unit ? g_hash_table_lookup(units, unit) : NULL;
  if (g_str_equal(method, "LoadUnit")) {
    if (u == NULL)
      u = g_hash_table_lookup(units, "missing.service");
    g_autofree gchar *p = unit_path(u->name);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(o)", p));
    return;
  }
  if (g_str_equal(method, "Reload")) {
    g_dbus_method_invocation_return_value(inv, NULL);
    return;
  }
  g_assert_nonnull(u);
  if (g_str_equal(method, "EnableUnitFiles")) {
    set(&u->file, "enabled");
    g_dbus_method_invocation_return_value(inv, g_variant_new_parsed("(true, @a(sss) [])"));
    return;
  }
  if (g_str_equal(method, "DisableUnitFiles")) {
    set(&u->file, "disabled");
    g_dbus_method_invocation_return_value(inv, g_variant_new_parsed("(@a(sss) [],)"));
    return;
  }
  if (g_str_equal(method, "StartUnit") || g_str_equal(method, "RestartUnit")) {
    set(&u->active, "active");
    set(&u->sub, g_str_has_suffix(u->name, ".socket") ? "listening" : "running");
  } else if (g_str_equal(method, "StopUnit")) {
    set(&u->active, "inactive");
    set(&u->sub, "dead");
    /* Requires= propagation: stopping the socket stops the service. */
    if (g_str_equal(u->name, NSS_RELAY_SOCKET)) {
      MockUnit *svc = g_hash_table_lookup(units, NSS_RELAY_SERVICE);
      set(&svc->active, "inactive");
      set(&svc->sub, "dead");
    }
  }
  g_dbus_method_invocation_return_value(inv, g_variant_new("(o)", "/org/freedesktop/systemd1/job/1"));
}

static GVariant *
unit_prop(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
          const gchar *prop, GError **error, gpointer d)
{
  (void)c; (void)sender; (void)iface; (void)error; (void)d;
  MockUnit *u = g_hash_table_lookup(units, path);
  g_assert_nonnull(u);
  const gchar *v = g_str_equal(prop, "LoadState") ? u->load
                 : g_str_equal(prop, "ActiveState") ? u->active
                 : g_str_equal(prop, "SubState") ? u->sub : u->file;
  return g_variant_new_string(v);
}

static const GDBusInterfaceVTable mgr_vt = { mgr_call, NULL, NULL, { 0 } };
static const GDBusInterfaceVTable unit_vt = { NULL, unit_prop, NULL, { 0 } };

static void
add_unit(GDBusConnection *srv, GDBusNodeInfo *node, const gchar *name, const gchar *load,
         const gchar *active, const gchar *file)
{
  MockUnit *u = g_new0(MockUnit, 1);
  u->name = g_strdup(name);
  u->load = g_strdup(load);
  u->active = g_strdup(active);
  u->sub = g_strdup(g_str_equal(active, "active") ? "running" : "dead");
  u->file = g_strdup(file);
  g_hash_table_insert(units, g_strdup(name), u);
  gchar *p = unit_path(name);
  g_hash_table_insert(units, p, u);
  GError *e = NULL;
  g_dbus_connection_register_object(srv, p, node->interfaces[1], &unit_vt, NULL, NULL, &e);
  g_assert_no_error(e);
}

/* Run sync core calls on a worker thread while this thread services the
 * mock (whose objects dispatch on the default main context). */
typedef struct {
  GDBusConnection *bus;
  const NssUnitOp *ops;
  guint            n;
  const gchar     *unit;
  NssUnitState     state;
  gboolean         ok;
  GError          *error;
  gboolean         done;
} Job;

static gpointer
job_thread(gpointer data)
{
  Job *j = data;
  if (j->ops)
    j->ok = nss_systemd_run(j->bus, j->ops, j->n, &j->error);
  else
    j->ok = nss_systemd_get_state(j->bus, j->unit, &j->state, &j->error);
  g_atomic_int_set(&j->done, TRUE);
  g_main_context_wakeup(NULL);
  return NULL;
}

static void
run_job(Job *j)
{
  GThread *t = g_thread_new("job", job_thread, j);
  while (!g_atomic_int_get(&j->done))
    g_main_context_iteration(NULL, TRUE);
  g_thread_join(t);
}

static NssServiceStatus
relay_status(GDBusConnection *bus, gboolean *enabled)
{
  Job a = { .bus = bus, .unit = NSS_RELAY_SOCKET }, b = { .bus = bus, .unit = NSS_RELAY_SERVICE };
  run_job(&a);
  run_job(&b);
  g_assert_no_error(a.error);
  g_assert_no_error(b.error);
  NssServiceStatus s = nss_service_status(&a.state, &b.state);
  *enabled = nss_unit_file_enabled(&a.state);
  nss_unit_state_clear(&a.state);
  nss_unit_state_clear(&b.state);
  return s;
}

static void
assert_calls(const gchar *const *want)
{
  g_ptr_array_add(calls, NULL);
  g_assert_cmpstrv((const gchar *const *)calls->pdata, want);
  g_ptr_array_set_size(calls, 0);
}

static void
test_mock_manager(void)
{
  units = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  calls = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GTestDBus) tbus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(tbus);
  GError *e = NULL;
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(MOCK_XML, &e);
  g_assert_no_error(e);
  g_autoptr(GDBusConnection) srv = g_dbus_connection_new_for_address_sync(
    g_test_dbus_get_bus_address(tbus),
    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
    NULL, NULL, &e);
  g_assert_no_error(e);
  g_dbus_connection_register_object(srv, "/org/freedesktop/systemd1", node->interfaces[0],
                                    &mgr_vt, NULL, NULL, &e);
  g_assert_no_error(e);
  add_unit(srv, node, NSS_RELAY_SOCKET, "loaded", "inactive", "disabled");
  add_unit(srv, node, NSS_RELAY_SERVICE, "loaded", "inactive", "disabled");
  add_unit(srv, node, NSS_NOTIFY_SERVICE, "loaded", "inactive", "disabled");
  add_unit(srv, node, "missing.service", "not-found", "inactive", "");
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(srv, "org.freedesktop.DBus",
    "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
    g_variant_new("(su)", "org.freedesktop.systemd1", 4u), G_VARIANT_TYPE("(u)"),
    G_DBUS_CALL_FLAGS_NONE, -1, NULL, &e);
  g_assert_no_error(e);
  g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
  g_assert_no_error(e);

  gboolean enabled = TRUE;
  g_assert_cmpint(relay_status(bus, &enabled), ==, NSS_SVC_OFF);
  g_assert_false(enabled);
  g_ptr_array_set_size(calls, 0);

  guint n = 0;
  Job on = { .bus = bus, .ops = nss_relay_plan(TRUE, &n) };
  on.n = n;
  run_job(&on);
  g_assert_no_error(on.error);
  const gchar *want_on[] = { "EnableUnitFiles " NSS_RELAY_SOCKET, "Reload",
                             "StartUnit " NSS_RELAY_SOCKET, NULL };
  assert_calls(want_on);
  g_assert_cmpint(relay_status(bus, &enabled), ==, NSS_SVC_LISTENING);
  g_assert_true(enabled);
  g_ptr_array_set_size(calls, 0);

  Job restart = { .bus = bus, .ops = (NssUnitOp[]){ { NSS_OP_RESTART, NSS_RELAY_SERVICE } }, .n = 1 };
  run_job(&restart);
  g_assert_no_error(restart.error);
  g_assert_cmpint(relay_status(bus, &enabled), ==, NSS_SVC_RUNNING);
  g_ptr_array_set_size(calls, 0);

  Job off = { .bus = bus, .ops = nss_relay_plan(FALSE, &n) };
  off.n = n;
  run_job(&off);
  g_assert_no_error(off.error);
  const gchar *want_off[] = { "DisableUnitFiles " NSS_RELAY_SOCKET, "Reload",
                              "StopUnit " NSS_RELAY_SERVICE, "StopUnit " NSS_RELAY_SOCKET, NULL };
  assert_calls(want_off);
  g_assert_cmpint(relay_status(bus, &enabled), ==, NSS_SVC_OFF);
  g_assert_false(enabled);
  g_ptr_array_set_size(calls, 0);

  /* Failure stops the plan and names the step. */
  fail_method = g_strdup("Reload");
  Job bad = { .bus = bus, .ops = nss_relay_plan(TRUE, &n) };
  bad.n = n;
  run_job(&bad);
  g_assert_false(bad.ok);
  g_assert_nonnull(bad.error);
  g_assert_nonnull(strstr(bad.error->message, "Reload"));
  g_clear_error(&bad.error);
  const gchar *want_bad[] = { "EnableUnitFiles " NSS_RELAY_SOCKET, "Reload", NULL };
  assert_calls(want_bad);
  g_clear_pointer(&fail_method, g_free);

  /* Unknown unit: not-found, no error. */
  Job miss = { .bus = bus, .unit = "nostr-nope.service" };
  run_job(&miss);
  g_assert_no_error(miss.error);
  g_assert_cmpint(nss_service_status(NULL, &miss.state), ==, NSS_SVC_NOT_INSTALLED);
  nss_unit_state_clear(&miss.state);

  g_dbus_connection_close_sync(srv, NULL, NULL);
  g_clear_object(&bus);  /* g_test_dbus_down() waits for the shared bus */
  g_test_dbus_down(tbus);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-settings/systemd/status-model", test_status_model);
  g_test_add_func("/nostr-settings/systemd/plans", test_plans);
  g_test_add_func("/nostr-settings/systemd/mock-manager", test_mock_manager);
  return g_test_run();
}
