/* np-fake-session-relay.c - Test double for org.nostr.SessionRelay1
 *
 * SPDX-License-Identifier: MIT
 */
#include "np-fake-session-relay.h"

#include <string.h>

static const gchar INTROSPECTION[] =
  "<node>"
  "  <interface name='org.nostr.SessionRelay1'>"
  "    <property name='FederationState' type='s' access='read'/>"
  "    <method name='GetEventUpstream'>"
  "      <arg name='event_id' type='s' direction='in'/>"
  "      <arg name='state' type='s' direction='out'/>"
  "      <arg name='detail' type='s' direction='out'/>"
  "      <arg name='relays' type='a(sssuxx)' direction='out'/>"
  "    </method>"
  "    <signal name='UpstreamStatusChanged'>"
  "      <arg name='event_id' type='s'/>"
  "      <arg name='relay_url' type='s'/>"
  "      <arg name='relay_state' type='s'/>"
  "      <arg name='reason' type='s'/>"
  "      <arg name='event_state' type='s'/>"
  "    </signal>"
  "  </interface>"
  "</node>";

struct _NpFakeSessionRelay {
  GMutex           lock;
  GCond            cond;
  gboolean         ready;
  gboolean         failed;
  gchar           *address;
  gchar           *federation_state;   /* NULL: property absent */
  gchar           *reply_state;
  gchar           *reply_detail;
  gchar          **reply_relays;
  gchar           *follow_url, *follow_relay_state, *follow_reason, *follow_event_state;
  guint            queries;
  gchar           *last_query;

  GThread         *thread;
  GMainContext    *ctx;
  GMainLoop       *loop;
  GDBusConnection *conn;
  GDBusNodeInfo   *node;
  guint            reg_id;
  guint            own_id;
};

gboolean
np_fake_session_relay_bus_available(void)
{
  g_autofree gchar *daemon = g_find_program_in_path("dbus-daemon");
  return daemon != NULL;
}

static gboolean
wake_noop(gpointer data)
{
  (void)data;
  return G_SOURCE_CONTINUE;
}

gboolean
np_wait_for(GMainContext *ctx, const gboolean *flag, guint timeout_ms)
{
  if (ctx == NULL)
    ctx = g_main_context_get_thread_default();
  if (ctx == NULL)
    ctx = g_main_context_default();
  gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
  /* Only bounds each blocking iteration so the deadline is honoured. */
  GSource *tick = g_timeout_source_new(50);
  g_source_set_callback(tick, wake_noop, NULL, NULL);
  g_source_attach(tick, ctx);
  while (!*flag && g_get_monotonic_time() < deadline)
    g_main_context_iteration(ctx, TRUE);
  g_source_destroy(tick);
  g_source_unref(tick);
  return *flag;
}

static void
emit_locked(NpFakeSessionRelay *f, const gchar *id, const gchar *url, const gchar *rs,
            const gchar *reason, const gchar *es)
{
  if (f->conn == NULL)
    return;
  g_dbus_connection_emit_signal(f->conn, NULL, "/org/nostr/SessionRelay1",
                                "org.nostr.SessionRelay1", "UpstreamStatusChanged",
                                g_variant_new("(sssss)", id, url ? url : "", rs ? rs : "",
                                              reason ? reason : "", es),
                                NULL);
}

static void
method_call(GDBusConnection *c, const gchar *sender, const gchar *path,
            const gchar *iface, const gchar *method, GVariant *params,
            GDBusMethodInvocation *inv, gpointer user_data)
{
  (void)c; (void)sender; (void)path; (void)iface;
  NpFakeSessionRelay *f = user_data;
  if (!g_str_equal(method, "GetEventUpstream")) {
    g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                          "No such method: %s", method);
    return;
  }
  const gchar *id = NULL;
  g_variant_get(params, "(&s)", &id);
  g_mutex_lock(&f->lock);
  f->queries++;
  g_free(f->last_query);
  f->last_query = g_strdup(id);
  GVariantBuilder rb;
  g_variant_builder_init(&rb, G_VARIANT_TYPE("a(sssuxx)"));
  for (guint i = 0; f->reply_relays && f->reply_relays[i]; i++) {
    g_auto(GStrv) parts = g_strsplit(f->reply_relays[i], " ", 3);
    g_variant_builder_add(&rb, "(sssuxx)", parts[0], parts[1] ? parts[1] : "pending",
                          parts[1] && parts[2] ? parts[2] : "", 1u, (gint64)0, (gint64)0);
  }
  g_dbus_method_invocation_return_value(
    inv, g_variant_new("(ssa(sssuxx))", f->reply_state ? f->reply_state : "unknown",
                       f->reply_detail ? f->reply_detail : "", &rb));
  if (f->follow_event_state != NULL)
    emit_locked(f, id, f->follow_url, f->follow_relay_state, f->follow_reason,
                f->follow_event_state);
  g_mutex_unlock(&f->lock);
}

static GVariant *
get_property(GDBusConnection *c, const gchar *sender, const gchar *path,
             const gchar *iface, const gchar *name, GError **error, gpointer user_data)
{
  (void)c; (void)sender; (void)path; (void)iface;
  NpFakeSessionRelay *f = user_data;
  GVariant *v = NULL;
  g_mutex_lock(&f->lock);
  if (g_str_equal(name, "FederationState") && f->federation_state != NULL)
    v = g_variant_new_string(f->federation_state);
  g_mutex_unlock(&f->lock);
  if (v == NULL)
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                "No such property: %s", name);
  return v;
}

static const GDBusInterfaceVTable VTABLE = { method_call, get_property, NULL, { 0 } };

static void
signal_ready(NpFakeSessionRelay *f, gboolean ok)
{
  g_mutex_lock(&f->lock);
  f->ready = TRUE;
  f->failed = !ok;
  g_cond_signal(&f->cond);
  g_mutex_unlock(&f->lock);
}

static void
on_acquired(GDBusConnection *c, const gchar *name, gpointer user_data)
{
  (void)c; (void)name;
  signal_ready(user_data, TRUE);
}

static void
on_lost(GDBusConnection *c, const gchar *name, gpointer user_data)
{
  (void)c; (void)name;
  NpFakeSessionRelay *f = user_data;
  if (!f->ready)
    signal_ready(f, FALSE);
}

static gpointer
fake_thread(gpointer data)
{
  NpFakeSessionRelay *f = data;
  g_main_context_push_thread_default(f->ctx);
  GError *err = NULL;
  f->conn = g_dbus_connection_new_for_address_sync(
    f->address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
    NULL, NULL, &err);
  if (f->conn == NULL) {
    g_printerr("fake session relay: %s\n", err->message);
    g_clear_error(&err);
    signal_ready(f, FALSE);
    g_main_context_pop_thread_default(f->ctx);
    return NULL;
  }
  f->node = g_dbus_node_info_new_for_xml(INTROSPECTION, NULL);
  f->reg_id = g_dbus_connection_register_object(f->conn, "/org/nostr/SessionRelay1",
                                                f->node->interfaces[0], &VTABLE, f,
                                                NULL, NULL);
  f->own_id = g_bus_own_name_on_connection(f->conn, "org.nostr.SessionRelay1",
                                           G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                                           on_acquired, on_lost, f, NULL);
  g_main_loop_run(f->loop);

  g_bus_unown_name(f->own_id);
  g_dbus_connection_unregister_object(f->conn, f->reg_id);
  g_dbus_connection_flush_sync(f->conn, NULL, NULL);
  g_dbus_connection_close_sync(f->conn, NULL, NULL);
  while (g_main_context_iteration(f->ctx, FALSE))
    ;
  g_main_context_pop_thread_default(f->ctx);
  return NULL;
}

NpFakeSessionRelay *
np_fake_session_relay_start(const gchar *federation_state)
{
  NpFakeSessionRelay *f = g_new0(NpFakeSessionRelay, 1);
  g_mutex_init(&f->lock);
  g_cond_init(&f->cond);
  f->address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  g_assert_nonnull(f->address);
  f->federation_state = g_strdup(federation_state);
  f->ctx = g_main_context_new();
  f->loop = g_main_loop_new(f->ctx, FALSE);
  f->thread = g_thread_new("fake-session-relay", fake_thread, f);
  g_mutex_lock(&f->lock);
  while (!f->ready)
    g_cond_wait(&f->cond, &f->lock);
  gboolean failed = f->failed;
  g_mutex_unlock(&f->lock);
  g_assert_false(failed);
  return f;
}

void
np_fake_session_relay_stop(NpFakeSessionRelay *f)
{
  if (f == NULL)
    return;
  g_main_loop_quit(f->loop);
  g_thread_join(f->thread);
  g_clear_object(&f->conn);
  g_dbus_node_info_unref(f->node);
  g_main_loop_unref(f->loop);
  g_main_context_unref(f->ctx);
  g_free(f->address);
  g_free(f->federation_state);
  g_free(f->reply_state);
  g_free(f->reply_detail);
  g_strfreev(f->reply_relays);
  g_free(f->follow_url);
  g_free(f->follow_relay_state);
  g_free(f->follow_reason);
  g_free(f->follow_event_state);
  g_free(f->last_query);
  g_mutex_clear(&f->lock);
  g_cond_clear(&f->cond);
  g_free(f);
}

void
np_fake_session_relay_set_reply(NpFakeSessionRelay *f, const gchar *state,
                                const gchar *detail, const gchar *const *relays)
{
  g_mutex_lock(&f->lock);
  g_free(f->reply_state);
  f->reply_state = g_strdup(state);
  g_free(f->reply_detail);
  f->reply_detail = g_strdup(detail);
  g_strfreev(f->reply_relays);
  f->reply_relays = g_strdupv((gchar **)relays);
  g_mutex_unlock(&f->lock);
}

void
np_fake_session_relay_set_followup(NpFakeSessionRelay *f, const gchar *relay_url,
                                   const gchar *relay_state, const gchar *reason,
                                   const gchar *event_state)
{
  g_mutex_lock(&f->lock);
  g_free(f->follow_url);
  f->follow_url = g_strdup(relay_url);
  g_free(f->follow_relay_state);
  f->follow_relay_state = g_strdup(relay_state);
  g_free(f->follow_reason);
  f->follow_reason = g_strdup(reason);
  g_free(f->follow_event_state);
  f->follow_event_state = g_strdup(event_state);
  g_mutex_unlock(&f->lock);
}

typedef struct {
  NpFakeSessionRelay *f;
  gchar *id, *url, *rs, *reason, *es;
} EmitJob;

static gboolean
emit_job(gpointer data)
{
  EmitJob *j = data;
  g_mutex_lock(&j->f->lock);
  emit_locked(j->f, j->id, j->url, j->rs, j->reason, j->es);
  g_mutex_unlock(&j->f->lock);
  g_free(j->id); g_free(j->url); g_free(j->rs); g_free(j->reason); g_free(j->es);
  g_free(j);
  return G_SOURCE_REMOVE;
}

void
np_fake_session_relay_emit(NpFakeSessionRelay *f, const gchar *event_id,
                           const gchar *relay_url, const gchar *relay_state,
                           const gchar *reason, const gchar *event_state)
{
  EmitJob *j = g_new0(EmitJob, 1);
  j->f = f;
  j->id = g_strdup(event_id);
  j->url = g_strdup(relay_url);
  j->rs = g_strdup(relay_state);
  j->reason = g_strdup(reason);
  j->es = g_strdup(event_state);
  g_main_context_invoke(f->ctx, emit_job, j);
}

guint
np_fake_session_relay_query_count(NpFakeSessionRelay *f)
{
  g_mutex_lock(&f->lock);
  guint n = f->queries;
  g_mutex_unlock(&f->lock);
  return n;
}

gchar *
np_fake_session_relay_last_query(NpFakeSessionRelay *f)
{
  g_mutex_lock(&f->lock);
  gchar *s = g_strdup(f->last_query);
  g_mutex_unlock(&f->lock);
  return s;
}
