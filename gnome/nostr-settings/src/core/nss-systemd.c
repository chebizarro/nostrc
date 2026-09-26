/* nss-systemd.c — see nss-systemd.h.
 * SPDX-License-Identifier: MIT
 */
#include "nss-systemd.h"

#include <string.h>

#define SD_NAME  "org.freedesktop.systemd1"
#define SD_PATH  "/org/freedesktop/systemd1"
#define SD_MGR   "org.freedesktop.systemd1.Manager"
#define SD_UNIT  "org.freedesktop.systemd1.Unit"
#define TIMEOUT  5000
/* Start/stop wait for the job to be queued, not finished; a Type=notify
 * relay can still take a moment. */
#define JOB_TIMEOUT 25000

void
nss_unit_state_clear(NssUnitState *s)
{
  g_clear_pointer(&s->load_state, g_free);
  g_clear_pointer(&s->active_state, g_free);
  g_clear_pointer(&s->sub_state, g_free);
  g_clear_pointer(&s->unit_file_state, g_free);
}

gboolean
nss_unit_file_enabled(const NssUnitState *s)
{
  const gchar *u = s ? s->unit_file_state : NULL;
  return u && (g_str_equal(u, "enabled") || g_str_equal(u, "enabled-runtime") ||
               g_str_equal(u, "linked") || g_str_equal(u, "linked-runtime"));
}

static gboolean
eq(const gchar *a, const gchar *b)
{
  return a != NULL && g_str_equal(a, b);
}

static gboolean
masked(const NssUnitState *s)
{
  return s && (eq(s->load_state, "masked") || eq(s->unit_file_state, "masked") ||
               eq(s->unit_file_state, "masked-runtime"));
}

NssServiceStatus
nss_service_status(const NssUnitState *socket, const NssUnitState *service)
{
  const NssUnitState *primary = socket ? socket : service;
  if (primary == NULL || primary->load_state == NULL ||
      eq(primary->load_state, "not-found"))
    return NSS_SVC_NOT_INSTALLED;
  if (masked(socket) || masked(service))
    return NSS_SVC_MASKED;
  if (service && (eq(service->active_state, "failed") ||
                  eq(service->sub_state, "auto-restart")))
    return NSS_SVC_FAILED;
  if (service && (eq(service->active_state, "activating") ||
                  eq(service->active_state, "reloading")))
    return NSS_SVC_STARTING;
  if (service && eq(service->active_state, "active"))
    return NSS_SVC_RUNNING;
  if (socket && eq(socket->active_state, "failed"))
    return NSS_SVC_FAILED;
  if (socket && eq(socket->active_state, "active"))
    return NSS_SVC_LISTENING;
  return nss_unit_file_enabled(primary) ? NSS_SVC_STOPPED : NSS_SVC_OFF;
}

const gchar *
nss_service_status_label(NssServiceStatus st, gboolean socket_activated)
{
  switch (st) {
  case NSS_SVC_NOT_INSTALLED: return "Not installed";
  case NSS_SVC_MASKED:        return "Disabled by the administrator (masked)";
  case NSS_SVC_OFF:           return "Off";
  case NSS_SVC_STOPPED:       return socket_activated ? "Enabled but not listening"
                                                      : "Enabled but not running";
  case NSS_SVC_LISTENING:     return "Listening — starts when an app connects";
  case NSS_SVC_STARTING:      return "Starting…";
  case NSS_SVC_RUNNING:       return "Running";
  case NSS_SVC_FAILED:        return "Failed — see journalctl --user for details";
  }
  return "";
}

gboolean
nss_service_can_restart(NssServiceStatus st)
{
  return st == NSS_SVC_RUNNING || st == NSS_SVC_FAILED || st == NSS_SVC_STARTING;
}

static const NssUnitOp RELAY_ON[] = {
  { NSS_OP_ENABLE, NSS_RELAY_SOCKET },
  { NSS_OP_RELOAD, NULL },
  { NSS_OP_START,  NSS_RELAY_SOCKET },
};
static const NssUnitOp RELAY_OFF[] = {
  { NSS_OP_DISABLE, NSS_RELAY_SOCKET },
  { NSS_OP_RELOAD,  NULL },
  { NSS_OP_STOP,    NSS_RELAY_SERVICE },
  { NSS_OP_STOP,    NSS_RELAY_SOCKET },
};
static const NssUnitOp NOTIFY_ON[] = {
  { NSS_OP_ENABLE, NSS_NOTIFY_SERVICE },
  { NSS_OP_RELOAD, NULL },
  { NSS_OP_START,  NSS_NOTIFY_SERVICE },
};
static const NssUnitOp NOTIFY_OFF[] = {
  { NSS_OP_DISABLE, NSS_NOTIFY_SERVICE },
  { NSS_OP_RELOAD,  NULL },
  { NSS_OP_STOP,    NSS_NOTIFY_SERVICE },
};

const NssUnitOp *
nss_relay_plan(gboolean on, guint *n)
{
  *n = on ? G_N_ELEMENTS(RELAY_ON) : G_N_ELEMENTS(RELAY_OFF);
  return on ? RELAY_ON : RELAY_OFF;
}

const NssUnitOp *
nss_notify_plan(gboolean on, guint *n)
{
  *n = on ? G_N_ELEMENTS(NOTIFY_ON) : G_N_ELEMENTS(NOTIFY_OFF);
  return on ? NOTIFY_ON : NOTIFY_OFF;
}

static gchar *
unit_prop(GDBusConnection *bus, const gchar *path, const gchar *prop, GError **error)
{
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
    bus, SD_NAME, path, "org.freedesktop.DBus.Properties", "Get",
    g_variant_new("(ss)", SD_UNIT, prop), G_VARIANT_TYPE("(v)"),
    G_DBUS_CALL_FLAGS_NONE, TIMEOUT, NULL, error);
  if (r == NULL)
    return NULL;
  g_autoptr(GVariant) v = NULL;
  g_variant_get(r, "(v)", &v);
  return g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ? g_variant_dup_string(v, NULL)
                                                        : g_strdup("");
}

gboolean
nss_systemd_get_state(GDBusConnection *bus, const gchar *unit, NssUnitState *out,
                      GError **error)
{
  memset(out, 0, sizeof *out);
  /* LoadUnit (not GetUnit): works for units that are not loaded right now
   * and reports load_state "not-found" for units that do not exist. */
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
    bus, SD_NAME, SD_PATH, SD_MGR, "LoadUnit", g_variant_new("(s)", unit),
    G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, TIMEOUT, NULL, error);
  if (r == NULL)
    return FALSE;
  const gchar *path = NULL;
  g_variant_get(r, "(&o)", &path);
  if (!(out->load_state = unit_prop(bus, path, "LoadState", error)) ||
      !(out->active_state = unit_prop(bus, path, "ActiveState", error)) ||
      !(out->sub_state = unit_prop(bus, path, "SubState", error)) ||
      !(out->unit_file_state = unit_prop(bus, path, "UnitFileState", error))) {
    nss_unit_state_clear(out);
    return FALSE;
  }
  return TRUE;
}

static gboolean
run_op(GDBusConnection *bus, const NssUnitOp *op, GError **error)
{
  const gchar *method = NULL;
  GVariant *params = NULL;
  const GVariantType *reply = NULL;
  gint timeout = TIMEOUT;
  const gchar *units[] = { op->unit, NULL };
  switch (op->kind) {
  case NSS_OP_ENABLE:
    method = "EnableUnitFiles";
    params = g_variant_new("(^asbb)", units, FALSE, FALSE);
    reply = G_VARIANT_TYPE("(ba(sss))");
    break;
  case NSS_OP_DISABLE:
    method = "DisableUnitFiles";
    params = g_variant_new("(^asb)", units, FALSE);
    reply = G_VARIANT_TYPE("(a(sss))");
    break;
  case NSS_OP_RELOAD:
    method = "Reload";
    reply = G_VARIANT_TYPE_UNIT;
    timeout = JOB_TIMEOUT;
    break;
  case NSS_OP_START:
  case NSS_OP_STOP:
  case NSS_OP_RESTART:
    method = op->kind == NSS_OP_START ? "StartUnit"
           : op->kind == NSS_OP_STOP  ? "StopUnit" : "RestartUnit";
    params = g_variant_new("(ss)", op->unit, "replace");
    reply = G_VARIANT_TYPE("(o)");
    timeout = JOB_TIMEOUT;
    break;
  }
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
    bus, SD_NAME, SD_PATH, SD_MGR, method, params, reply, G_DBUS_CALL_FLAGS_NONE,
    timeout, NULL, error);
  if (r == NULL) {
    if (error && *error) {
      g_dbus_error_strip_remote_error(*error);
      g_prefix_error(error, "%s %s: ", method, op->unit ? op->unit : "");
    }
    return FALSE;
  }
  return TRUE;
}

gboolean
nss_systemd_run(GDBusConnection *bus, const NssUnitOp *ops, guint n, GError **error)
{
  for (guint i = 0; i < n; i++)
    if (!run_op(bus, &ops[i], error))
      return FALSE;
  return TRUE;
}

typedef struct {
  GDBusConnection *bus;
  NssUnitOp       *ops;
  guint            n;
} RunData;

static void
run_data_free(gpointer p)
{
  RunData *d = p;
  g_object_unref(d->bus);
  g_free(d->ops);
  g_free(d);
}

static void
run_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
  (void)src; (void)c;
  RunData *d = data;
  GError *err = NULL;
  if (nss_systemd_run(d->bus, d->ops, d->n, &err))
    g_task_return_boolean(task, TRUE);
  else
    g_task_return_error(task, err);
}

void
nss_systemd_run_async(GDBusConnection *bus, const NssUnitOp *ops, guint n,
                      GCancellable *cancellable, GAsyncReadyCallback cb, gpointer user_data)
{
  RunData *d = g_new0(RunData, 1);
  d->bus = g_object_ref(bus);
  d->ops = g_memdup2(ops, sizeof(NssUnitOp) * n);
  d->n = n;
  GTask *task = g_task_new(NULL, cancellable, cb, user_data);
  g_task_set_task_data(task, d, run_data_free);
  g_task_run_in_thread(task, run_thread);
  g_object_unref(task);
}

gboolean
nss_systemd_run_finish(GAsyncResult *res, GError **error)
{
  return g_task_propagate_boolean(G_TASK(res), error);
}
