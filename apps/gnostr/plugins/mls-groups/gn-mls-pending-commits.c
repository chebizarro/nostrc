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

/* Retry an uncertain publish every RETRY_SECONDS, at most RETRY_LIMIT times
 * per group and activation; the next activation starts over. */
#define RETRY_SECONDS 20
#define RETRY_LIMIT   15

static GHashTable *retries;   /* group id hex -> attempts made */

typedef struct {
  GnMlsEventRouter   *router;   /* strong */
  gchar              *gid;
  GnMlsCommitOutcome  outcome;
  GError             *publish_error;
} Resolve;

static void
resolve_free(gpointer data)
{
  Resolve *r = data;
  g_clear_object(&r->router);
  g_free(r->gid);
  g_clear_error(&r->publish_error);
  g_free(r);
}

static MarmotGobjectClient *
client_of(Resolve *r)
{
  GnMarmotService *service = gn_mls_event_router_get_service(r->router);
  return service ? gn_marmot_service_get_client(service) : NULL;
}

/* Gift-wrap and send the Welcomes of merged Adds, then empty the outbox. */
static void
send_unsent_welcomes(Resolve *r)
{
  MarmotGobjectClient *client = client_of(r);
  if (client == NULL) return;
  g_auto(GStrv) rumors = NULL;
  g_auto(GStrv) recipients = NULL;
  g_autoptr(GError) error = NULL;
  if (!marmot_gobject_client_get_unsent_welcomes(client, r->gid, &rumors, &recipients,
                                                 &error))
    {
      g_warning("MLS pending: cannot read unsent Welcomes of %s: %s", r->gid,
                error->message);
      return;
    }
  if (rumors == NULL || rumors[0] == NULL) return;
  for (guint i = 0; rumors[i] != NULL && recipients[i] != NULL; i++)
    gn_mls_event_router_send_welcome_async(r->router, recipients[i], rumors[i],
                                           NULL, NULL, NULL);
  if (!marmot_gobject_client_mark_welcomes_sent(client, r->gid, &error))
    g_warning("MLS pending: cannot mark Welcomes of %s sent: %s", r->gid, error->message);
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

static gboolean
retry_cb(gpointer user_data)
{
  Resolve *r = user_data;
  gn_mls_resolve_pending_commit_async(r->router, r->gid, NULL, NULL);
  resolve_free(r);
  return G_SOURCE_REMOVE;
}

static void
schedule_retry(Resolve *r)
{
  if (retries == NULL)
    retries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  guint attempts = GPOINTER_TO_UINT(g_hash_table_lookup(retries, r->gid));
  if (attempts >= RETRY_LIMIT)
    {
      g_warning("MLS pending: Commit of %s still unconfirmed after %u retries; "
                "it stays pending until a relay echoes it or the next start", r->gid,
                attempts);
      return;
    }
  g_hash_table_insert(retries, g_strdup(r->gid), GUINT_TO_POINTER(attempts + 1));
  Resolve *again = g_new0(Resolve, 1);
  again->router = g_object_ref(r->router);
  again->gid = g_strdup(r->gid);
  g_timeout_add_seconds(RETRY_SECONDS, retry_cb, again);
}

/* ── the resolution ───────────────────────────────────────────────── */

static void
finish(GTask *task, GnMlsCommitOutcome outcome, GError *error)
{
  Resolve *r = g_task_get_task_data(task);
  r->outcome = outcome;
  if (outcome != GN_MLS_COMMIT_UNCERTAIN && retries != NULL)
    g_hash_table_remove(retries, r->gid);
  if (error != NULL)
    g_task_return_error(task, error);
  else
    g_task_return_int(task, outcome);
  g_object_unref(task);
}

static void
on_cleared(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GTask *task = user_data;
  Resolve *r = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  if (!marmot_gobject_client_clear_pending_commit_finish(MARMOT_GOBJECT_CLIENT(source),
                                                         result, &error))
    g_warning("MLS pending: clearing the refused Commit of %s failed: %s", r->gid,
              error->message);
  finish(task, GN_MLS_COMMIT_REJECTED, g_steal_pointer(&r->publish_error));
}

static void
on_merged(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GTask *task = user_data;
  Resolve *r = g_task_get_task_data(task);
  GError *error = NULL;
  if (marmot_gobject_client_merge_pending_commit_finish(MARMOT_GOBJECT_CLIENT(source),
                                                        result, &error))
    {
      send_unsent_welcomes(r);
      finish(task, GN_MLS_COMMIT_MERGED, NULL);
    }
  else if (gn_mls_group_error_is_superseded(error))
    finish(task, GN_MLS_COMMIT_SUPERSEDED, error);
  else
    finish(task, GN_MLS_COMMIT_FAILED, error);
}

static void
on_published(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  GTask *task = user_data;
  Resolve *r = g_task_get_task_data(task);
  GError *error = NULL;
  g_autofree gchar *relay = gn_mls_publish_until_ack_finish(result, &error);
  MarmotGobjectClient *client = client_of(r);
  if (client == NULL)
    {
      g_clear_error(&error);
      finish(task, GN_MLS_COMMIT_FAILED,
             g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Plugin deactivated"));
      return;
    }
  if (relay != NULL)
    {
      g_info("MLS pending: Commit of %s accepted by %s; merging", r->gid, relay);
      marmot_gobject_client_merge_pending_commit_async(client, r->gid, NULL, on_merged, task);
      return;
    }
  if (g_error_matches(error, GN_MLS_PUBLISH_ERROR, GN_MLS_PUBLISH_UNCERTAIN))
    {
      /* A relay may have stored it: keep it pending (its echo merges it). */
      g_info("MLS pending: Commit of %s unconfirmed (%s); will retry", r->gid,
             error->message);
      schedule_retry(r);
      finish(task, GN_MLS_COMMIT_UNCERTAIN, error);
      return;
    }
  /* Every relay refused it, or there is none: certainly unpublished. */
  r->publish_error = error;
  marmot_gobject_client_clear_pending_commit_async(client, r->gid, NULL, on_cleared, task);
}

void
gn_mls_resolve_pending_commit_async(GnMlsEventRouter    *router,
                                    const gchar         *mls_group_id_hex,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
  g_return_if_fail(GN_IS_MLS_EVENT_ROUTER(router));
  g_return_if_fail(mls_group_id_hex != NULL);
  GTask *task = g_task_new(NULL, NULL, callback, user_data);
  Resolve *r = g_new0(Resolve, 1);
  r->router = g_object_ref(router);
  r->gid = g_strdup(mls_group_id_hex);
  g_task_set_task_data(task, r, resolve_free);

  MarmotGobjectClient *client = client_of(r);
  if (client == NULL)
    {
      finish(task, GN_MLS_COMMIT_FAILED,
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
      finish(task, GN_MLS_COMMIT_FAILED, error);
      return;
    }
  if (event_json == NULL)
    {
      /* Nothing pending -- but a merge (e.g. by relay echo) may have left
       * an Add's Welcomes to send. */
      send_unsent_welcomes(r);
      finish(task, GN_MLS_COMMIT_NONE, NULL);
      return;
    }
  if (superseded)
    {
      /* A competing Commit won: merging reports it and drops ours. */
      marmot_gobject_client_merge_pending_commit_async(client, r->gid, NULL, on_merged, task);
      return;
    }
  gsize relay_count = 0;
  g_auto(GStrv) relays = marmot_gobject_client_get_group_relay_urls(client, r->gid,
                                                                    &relay_count);
  gn_mls_publish_until_ack_async(ack_publish, ack_finish, ack_is_rejection, r->router,
                                 event_json, (const char * const *) relays, NULL,
                                 on_published, task);
}

GnMlsCommitOutcome
gn_mls_resolve_pending_commit_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), GN_MLS_COMMIT_FAILED);
  Resolve *r = g_task_get_task_data(G_TASK(result));
  GnMlsCommitOutcome outcome = r->outcome;
  GError *local = NULL;
  g_task_propagate_int(G_TASK(result), &local);
  if (local != NULL)
    g_propagate_error(error, local);
  return outcome;
}

void
gn_mls_resolve_all_pending_commits(GnMlsEventRouter *router)
{
  g_return_if_fail(GN_IS_MLS_EVENT_ROUTER(router));
  GnMarmotService *service = gn_mls_event_router_get_service(router);
  MarmotGobjectClient *client = service ? gn_marmot_service_get_client(service) : NULL;
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
