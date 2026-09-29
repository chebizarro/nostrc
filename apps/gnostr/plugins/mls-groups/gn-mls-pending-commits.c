/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-mls-pending-commits.c - Resolve our pending MLS Commits
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-mls-pending-commits.h"
#include "gn-mls-commit-publish.h"
#include "gn-mls-group-error.h"
#include <gnostr-plugin-api.h>
#include <marmot-gobject-1.0/marmot-gobject.h>

/* Retries of an uncertain publish: exponential backoff from RETRY_BASE_S,
 * capped at RETRY_MAX_S, with up to 25 % jitter, at most RETRY_LIMIT per
 * group and activation (the next activation starts over; a relay echo of
 * the Commit merges it in between). */
#define RETRY_BASE_S 5
#define RETRY_MAX_S  300
#define RETRY_LIMIT  12

/* Main-context state of an active plugin (W17b addendum N2). */
static GnMlsEventRouter *active_router;   /* strong while started */
static gulong            group_updated_handler;
static GHashTable       *inflight;        /* gid -> GPtrArray<GTask> waiting */
static GHashTable       *timers;          /* gid -> retry GSource id */
static GHashTable       *attempts;        /* gid -> retries made */
static GHashTable       *sending;         /* Welcome id hex -> in-flight send */
static GHashTable       *welcome_timers;  /* gid -> Welcome resend GSource id */
static GHashTable       *welcome_attempts;/* gid -> failed Welcome sends */

static void
ensure_tables(void)
{
  if (inflight == NULL)
    {
      inflight = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       (GDestroyNotify) g_ptr_array_unref);
      timers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
      attempts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
      sending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
      welcome_timers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
      welcome_attempts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
}

typedef struct {
  GnMlsEventRouter   *router;   /* strong */
  gchar              *gid;
  GError             *publish_error;
} Resolve;

static void
resolve_free(Resolve *r)
{
  g_clear_object(&r->router);
  g_free(r->gid);
  g_clear_error(&r->publish_error);
  g_free(r);
}

static MarmotGobjectClient *
client_of(GnMlsEventRouter *router)
{
  GnMarmotService *service = gn_mls_event_router_get_service(router);
  return service ? gn_marmot_service_get_client(service) : NULL;
}

/* ── Welcome outbox (W17b addendum C2) ───────────────────────────── */

static void send_unsent_welcomes(GnMlsEventRouter *router, const gchar *gid);

typedef struct {
  gchar *gid;
} WelcomeRetry;

static void
welcome_retry_free(gpointer data)
{
  WelcomeRetry *wr = data;
  g_free(wr->gid);
  g_free(wr);
}

static gboolean
welcome_retry_cb(gpointer user_data)
{
  WelcomeRetry *wr = user_data;
  if (welcome_timers != NULL)
    g_hash_table_remove(welcome_timers, wr->gid);
  if (active_router != NULL)
    send_unsent_welcomes(active_router, wr->gid);
  return G_SOURCE_REMOVE;
}

/* A Welcome send failed: resend the group's outbox on its own timer,
 * backing off like Commit retries (W17b addendum 2, low). */
static void
schedule_welcome_retry(const gchar *gid)
{
  if (active_router == NULL || g_hash_table_contains(welcome_timers, gid))
    return;
  guint n = GPOINTER_TO_UINT(g_hash_table_lookup(welcome_attempts, gid));
  if (n >= RETRY_LIMIT)
    {
      g_warning("MLS pending: Welcomes of %s still unsent after %u retries; they stay "
                "in the outbox until the next group update or start", gid, n);
      return;
    }
  g_hash_table_insert(welcome_attempts, g_strdup(gid), GUINT_TO_POINTER(n + 1));
  WelcomeRetry *wr = g_new0(WelcomeRetry, 1);
  wr->gid = g_strdup(gid);
  guint delay = gn_mls_retry_delay_seconds(n, RETRY_BASE_S, RETRY_MAX_S, g_random_int());
  guint id = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, delay, welcome_retry_cb, wr,
                                        welcome_retry_free);
  g_hash_table_insert(welcome_timers, g_strdup(gid), GUINT_TO_POINTER(id));
}

typedef struct {
  GnMlsEventRouter *router;   /* strong */
  gchar            *gid;
  gchar            *id;
} WelcomeSend;

static void
on_welcome_sent(GObject *source, GAsyncResult *result, gpointer user_data)
{
  WelcomeSend *ws = user_data;
  g_autoptr(GError) error = NULL;
  if (gn_mls_event_router_send_welcome_finish(GN_MLS_EVENT_ROUTER(source), result, &error))
    {
      /* Only this Welcome, and only now that its gift wrap went out. */
      MarmotGobjectClient *client = client_of(ws->router);
      const gchar *ids[] = { ws->id, NULL };
      g_autoptr(GError) mark_error = NULL;
      if (client != NULL &&
          !marmot_gobject_client_mark_welcomes_sent(client, ws->gid, ids, &mark_error))
        g_warning("MLS pending: cannot mark a Welcome of %s sent: %s", ws->gid,
                  mark_error->message);
      else if (welcome_attempts != NULL)
        g_hash_table_remove(welcome_attempts, ws->gid);
    }
  else
    {
      g_warning("MLS pending: sending a Welcome of %s failed (kept for a retry): %s",
                ws->gid, error ? error->message : "unknown");
      if (welcome_timers != NULL)
        schedule_welcome_retry(ws->gid);
    }
  if (sending != NULL)
    g_hash_table_remove(sending, ws->id);
  g_object_unref(ws->router);
  g_free(ws->gid);
  g_free(ws->id);
  g_free(ws);
}

/* Gift-wrap and send every unsent Welcome not already on its way. */
static void
send_unsent_welcomes(GnMlsEventRouter *router, const gchar *gid)
{
  MarmotGobjectClient *client = client_of(router);
  if (client == NULL) return;
  ensure_tables();
  g_auto(GStrv) ids = NULL;
  g_auto(GStrv) rumors = NULL;
  g_auto(GStrv) recipients = NULL;
  g_autoptr(GError) error = NULL;
  if (!marmot_gobject_client_get_unsent_welcomes(client, gid, &ids, &rumors, &recipients,
                                                 &error))
    {
      g_warning("MLS pending: cannot read unsent Welcomes of %s: %s", gid, error->message);
      return;
    }
  for (guint i = 0; ids && ids[i] && rumors[i] && recipients[i]; i++)
    {
      if (g_hash_table_contains(sending, ids[i]))
        continue;
      g_hash_table_add(sending, g_strdup(ids[i]));
      WelcomeSend *ws = g_new0(WelcomeSend, 1);
      ws->router = g_object_ref(router);
      ws->gid = g_strdup(gid);
      ws->id = g_strdup(ids[i]);
      gn_mls_event_router_send_welcome_async(router, recipients[i], rumors[i], NULL,
                                             on_welcome_sent, ws);
    }
}

/* ── relay access through the router (NULL context: plugin gone) ─── */

static const char cancelled_tag = 'x';

static void
ack_publish(gpointer target, const char *event_json, const char *relay_url,
            GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  GnostrPluginContext *ctx = gn_mls_event_router_get_context(target);
  if (ctx == NULL)
    {
      GTask *task = g_task_new(NULL, cancellable, callback, user_data);
      g_task_set_source_tag(task, (gpointer) &cancelled_tag);
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Plugin deactivated");
      g_object_unref(task);
      return;
    }
  gnostr_plugin_context_publish_event_to_relay_ack_async(ctx, event_json, relay_url,
                                                         cancellable, callback, user_data);
}

static gboolean
ack_finish(gpointer target, GAsyncResult *result, GError **error)
{
  if (G_IS_TASK(result) &&
      g_task_get_source_tag(G_TASK(result)) == (gpointer) &cancelled_tag)
    return g_task_propagate_boolean(G_TASK(result), error);
  GnostrPluginContext *ctx = gn_mls_event_router_get_context(target);
  if (ctx == NULL)
    {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Plugin deactivated");
      return FALSE;
    }
  return gnostr_plugin_context_publish_event_to_relay_ack_finish(ctx, result, error);
}

static gboolean
ack_is_rejection(const GError *error)
{
  return g_error_matches(error, GNOSTR_PLUGIN_ERROR, GNOSTR_PLUGIN_ERROR_RELAY_REJECTED);
}

/* ── retries ──────────────────────────────────────────────────────── */

typedef struct {
  gchar *gid;
} Retry;

static gboolean
retry_cb(gpointer user_data)
{
  Retry *rt = user_data;
  if (timers != NULL)
    g_hash_table_remove(timers, rt->gid);
  if (active_router != NULL)
    gn_mls_resolve_pending_commit_async(active_router, rt->gid, NULL, NULL);
  return G_SOURCE_REMOVE;
}

static void
retry_free(gpointer data)
{
  Retry *rt = data;
  g_free(rt->gid);
  g_free(rt);
}

static void
schedule_retry(const gchar *gid)
{
  if (active_router == NULL || g_hash_table_contains(timers, gid))
    return;   /* stopped, or a retry is already scheduled */
  guint n = GPOINTER_TO_UINT(g_hash_table_lookup(attempts, gid));
  if (n >= RETRY_LIMIT)
    {
      g_warning("MLS pending: Commit of %s still unconfirmed after %u retries; it stays "
                "pending until a relay echoes it or the next start", gid, n);
      return;
    }
  guint delay = gn_mls_retry_delay_seconds(n, RETRY_BASE_S, RETRY_MAX_S, g_random_int());
  g_hash_table_insert(attempts, g_strdup(gid), GUINT_TO_POINTER(n + 1));
  Retry *rt = g_new0(Retry, 1);
  rt->gid = g_strdup(gid);
  guint id = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, delay, retry_cb, rt, retry_free);
  g_hash_table_insert(timers, g_strdup(gid), GUINT_TO_POINTER(id));
}

/* ── the resolution (one per group at a time) ─────────────────────── */

static void
finish(Resolve *r, GnMlsCommitOutcome outcome, GError *error)
{
  ensure_tables();
  if (outcome == GN_MLS_COMMIT_UNCERTAIN &&
      !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    schedule_retry(r->gid);
  else if (outcome != GN_MLS_COMMIT_UNCERTAIN)
    g_hash_table_remove(attempts, r->gid);

  GPtrArray *waiters = NULL;
  gchar *key = NULL;
  if (g_hash_table_steal_extended(inflight, r->gid, (gpointer *) &key, (gpointer *) &waiters))
    {
      for (guint i = 0; i < waiters->len; i++)
        {
          GTask *task = g_ptr_array_index(waiters, i);
          g_task_set_task_data(task, GINT_TO_POINTER(outcome), NULL);
          if (error != NULL)
            g_task_return_error(task, g_error_copy(error));
          else
            g_task_return_int(task, outcome);
        }
      g_ptr_array_unref(waiters);
      g_free(key);
    }
  g_clear_error(&error);
  resolve_free(r);
}

static void
on_cleared(GObject *source, GAsyncResult *result, gpointer user_data)
{
  Resolve *r = user_data;
  g_autoptr(GError) error = NULL;
  if (!marmot_gobject_client_clear_pending_commit_finish(MARMOT_GOBJECT_CLIENT(source),
                                                         result, &error))
    g_warning("MLS pending: clearing the refused Commit of %s failed: %s", r->gid,
              error->message);
  finish(r, GN_MLS_COMMIT_REJECTED, g_steal_pointer(&r->publish_error));
}

static void
on_merged(GObject *source, GAsyncResult *result, gpointer user_data)
{
  Resolve *r = user_data;
  GError *error = NULL;
  if (marmot_gobject_client_merge_pending_commit_finish(MARMOT_GOBJECT_CLIENT(source),
                                                        result, &error))
    {
      send_unsent_welcomes(r->router, r->gid);
      finish(r, GN_MLS_COMMIT_MERGED, NULL);
    }
  else if (gn_mls_group_error_is_superseded(error))
    finish(r, GN_MLS_COMMIT_SUPERSEDED, error);
  else
    finish(r, GN_MLS_COMMIT_FAILED, error);
}

static void
on_published(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  Resolve *r = user_data;
  GError *error = NULL;
  g_autofree gchar *relay = gn_mls_publish_until_ack_finish(result, &error);
  MarmotGobjectClient *client = client_of(r->router);
  if (client == NULL || gn_mls_event_router_get_context(r->router) == NULL)
    {
      /* Deactivated meanwhile: leave it pending for the next start. */
      g_clear_error(&error);
      finish(r, GN_MLS_COMMIT_UNCERTAIN,
             g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Plugin deactivated"));
      return;
    }
  if (relay != NULL)
    {
      g_info("MLS pending: Commit of %s accepted by %s; merging", r->gid, relay);
      marmot_gobject_client_merge_pending_commit_async(client, r->gid, NULL, on_merged, r);
      return;
    }
  if (g_error_matches(error, GN_MLS_PUBLISH_ERROR, GN_MLS_PUBLISH_UNCERTAIN))
    {
      /* A relay may have stored it: keep it pending (its echo merges it). */
      g_info("MLS pending: Commit of %s unconfirmed (%s); will retry", r->gid,
             error->message);
      finish(r, GN_MLS_COMMIT_UNCERTAIN, error);
      return;
    }
  /* Every relay refused it, or there is none: certainly unpublished. */
  r->publish_error = error;
  marmot_gobject_client_clear_pending_commit_async(client, r->gid, NULL, on_cleared, r);
}

static void
start_resolution(Resolve *r)
{
  MarmotGobjectClient *client = client_of(r->router);
  if (client == NULL)
    {
      finish(r, GN_MLS_COMMIT_FAILED,
             g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                                 "Marmot client not available"));
      return;
    }
  GError *error = NULL;
  gboolean superseded = FALSE;
  g_autofree gchar *event_json =
    marmot_gobject_client_get_pending_commit(client, r->gid, &superseded, &error);
  if (error != NULL)
    {
      finish(r, GN_MLS_COMMIT_FAILED, error);
      return;
    }
  if (event_json == NULL)
    {
      /* Nothing pending -- but a merge (e.g. by relay echo) may have left
       * an Add's Welcomes to send. */
      send_unsent_welcomes(r->router, r->gid);
      finish(r, GN_MLS_COMMIT_NONE, NULL);
      return;
    }
  if (superseded)
    {
      /* A competing Commit won: merging reports it and drops ours. */
      marmot_gobject_client_merge_pending_commit_async(client, r->gid, NULL, on_merged, r);
      return;
    }
  gsize relay_count = 0;
  g_auto(GStrv) relays = marmot_gobject_client_get_group_relay_urls(client, r->gid,
                                                                    &relay_count);
  gn_mls_publish_until_ack_async(ack_publish, ack_finish, ack_is_rejection, r->router,
                                 event_json, (const char * const *) relays, NULL,
                                 on_published, r);
}

void
gn_mls_resolve_pending_commit_async(GnMlsEventRouter    *router,
                                    const gchar         *mls_group_id_hex,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
  g_return_if_fail(GN_IS_MLS_EVENT_ROUTER(router));
  g_return_if_fail(mls_group_id_hex != NULL);
  ensure_tables();
  GTask *task = g_task_new(NULL, NULL, callback, user_data);   /* owned by the waiters */
  GPtrArray *waiters = g_hash_table_lookup(inflight, mls_group_id_hex);
  if (waiters != NULL)
    {
      /* One resolution per group at a time: share its outcome. */
      g_ptr_array_add(waiters, task);
      return;
    }
  waiters = g_ptr_array_new_with_free_func(g_object_unref);
  g_ptr_array_add(waiters, task);
  g_hash_table_insert(inflight, g_strdup(mls_group_id_hex), waiters);
  /* An explicit resolution supersedes a scheduled retry. */
  gpointer timer = NULL;
  if (g_hash_table_steal_extended(timers, mls_group_id_hex, NULL, &timer))
    g_source_remove(GPOINTER_TO_UINT(timer));

  Resolve *r = g_new0(Resolve, 1);
  r->router = g_object_ref(router);
  r->gid = g_strdup(mls_group_id_hex);
  start_resolution(r);
}

GnMlsCommitOutcome
gn_mls_resolve_pending_commit_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), GN_MLS_COMMIT_FAILED);
  GnMlsCommitOutcome outcome =
    (GnMlsCommitOutcome) GPOINTER_TO_INT(g_task_get_task_data(G_TASK(result)));
  GError *local = NULL;
  g_task_propagate_int(G_TASK(result), &local);
  if (local != NULL)
    g_propagate_error(error, local);
  return outcome;
}

/* A group changed epoch -- possibly our pending Add merged by its relay
 * echo: flush its outbox (and settle whatever is pending). */
static void
on_group_updated(GnMarmotService *service, MarmotGobjectGroup *group, gpointer user_data)
{
  (void)service;
  (void)user_data;
  const gchar *gid = marmot_gobject_group_get_mls_group_id(group);
  if (active_router != NULL && gid != NULL)
    gn_mls_resolve_pending_commit_async(active_router, gid, NULL, NULL);
}

void
gn_mls_pending_commits_start(GnMlsEventRouter *router)
{
  g_return_if_fail(GN_IS_MLS_EVENT_ROUTER(router));
  gn_mls_pending_commits_stop();
  ensure_tables();
  active_router = g_object_ref(router);
  GnMarmotService *service = gn_mls_event_router_get_service(router);
  if (service != NULL)
    group_updated_handler = g_signal_connect(service, "group-updated",
                                             G_CALLBACK(on_group_updated), NULL);
  MarmotGobjectClient *client = client_of(router);
  if (client == NULL) return;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) groups = marmot_gobject_client_get_all_groups(client, &error);
  if (groups == NULL)
    {
      g_warning("MLS pending: cannot list groups: %s", error ? error->message : "unknown");
      return;
    }
  for (guint i = 0; i < groups->len; i++)
    {
      const gchar *gid = marmot_gobject_group_get_mls_group_id(g_ptr_array_index(groups, i));
      if (gid != NULL)
        gn_mls_resolve_pending_commit_async(router, gid, NULL, NULL);
    }
}

void
gn_mls_pending_commits_stop(void)
{
  if (active_router != NULL)
    {
      GnMarmotService *service = gn_mls_event_router_get_service(active_router);
      if (group_updated_handler != 0 && service != NULL)
        g_signal_handler_disconnect(service, group_updated_handler);
      group_updated_handler = 0;
      g_clear_object(&active_router);
    }
  if (timers != NULL)
    {
      GHashTableIter it;
      gpointer timer;
      g_hash_table_iter_init(&it, timers);
      while (g_hash_table_iter_next(&it, NULL, &timer))
        g_source_remove(GPOINTER_TO_UINT(timer));
      g_hash_table_remove_all(timers);
      g_hash_table_remove_all(attempts);
      g_hash_table_iter_init(&it, welcome_timers);
      while (g_hash_table_iter_next(&it, NULL, &timer))
        g_source_remove(GPOINTER_TO_UINT(timer));
      g_hash_table_remove_all(welcome_timers);
      g_hash_table_remove_all(welcome_attempts);
    }
  /* In-flight resolutions finish on their own (a deactivated context fails
   * their publishes as CANCELLED, which is not retried). */
}
