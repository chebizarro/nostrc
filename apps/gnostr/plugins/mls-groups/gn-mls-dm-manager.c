/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-mls-dm-manager.c - MLS Direct Message Manager
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-mls-dm-manager.h"
#include <gnostr-plugin-api.h>
#include <json-glib/json-glib.h>
#include <marmot-gobject-1.0/marmot-gobject.h>

/*
 * Whitenoise convention: DirectMessage groups have a specific group name
 * prefix so they can be distinguished from regular groups.
 * The group name is "dm:<peer_pubkey_hex>" for the creator's perspective.
 */
#define DM_GROUP_NAME_PREFIX "dm:"

struct _GnMlsDmManager
{
  GObject parent_instance;

  GnMarmotService     *service;         /* strong ref */
  GnMlsEventRouter   *router;          /* strong ref */
  GnostrPluginContext *plugin_context;  /* borrowed */
};

G_DEFINE_TYPE(GnMlsDmManager, gn_mls_dm_manager, G_TYPE_OBJECT)

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gn_mls_dm_manager_dispose(GObject *object)
{
  GnMlsDmManager *self = GN_MLS_DM_MANAGER(object);

  g_clear_object(&self->service);
  g_clear_object(&self->router);
  self->plugin_context = NULL;

  G_OBJECT_CLASS(gn_mls_dm_manager_parent_class)->dispose(object);
}

static void
gn_mls_dm_manager_class_init(GnMlsDmManagerClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = gn_mls_dm_manager_dispose;
}

static void
gn_mls_dm_manager_init(GnMlsDmManager *self)
{
  self->service        = NULL;
  self->router         = NULL;
  self->plugin_context = NULL;
}

/* ── Helpers ─────────────────────────────────────────────────────── */

/*
 * Build the canonical DM group name for a peer.
 * We use "dm:<peer_pubkey_hex>" as a stable identifier.
 * The caller must free the returned string.
 */
static gchar *
make_dm_group_name(const gchar *my_pubkey_hex,
                   const gchar *peer_pubkey_hex)
{
  /*
   * Canonical name: sort the two pubkeys lexicographically so both
   * sides agree on the same name regardless of who created the group.
   */
  if (g_strcmp0(my_pubkey_hex, peer_pubkey_hex) < 0)
    return g_strdup_printf("%s%s+%s", DM_GROUP_NAME_PREFIX,
                           my_pubkey_hex, peer_pubkey_hex);
  else
    return g_strdup_printf("%s%s+%s", DM_GROUP_NAME_PREFIX,
                           peer_pubkey_hex, my_pubkey_hex);
}

/*
 * Check if a group is a DirectMessage group with the given peer.
 * Returns TRUE if the group name matches the canonical DM name.
 */
static gboolean
group_is_dm_with_peer(MarmotGobjectGroup *group,
                      const gchar        *my_pubkey_hex,
                      const gchar        *peer_pubkey_hex)
{
  const gchar *name = marmot_gobject_group_get_name(group);
  if (name == NULL) return FALSE;

  g_autofree gchar *expected = make_dm_group_name(my_pubkey_hex, peer_pubkey_hex);
  return g_strcmp0(name, expected) == 0;
}

/* ── Open DM async flow ──────────────────────────────────────────── */

typedef struct
{
  GnMlsDmManager *manager;       /* strong ref */
  GTask           *task;
  gchar           *peer_pubkey_hex;
  gchar           *kp_json;      /* fetched key package */
} OpenDmData;

static void
open_dm_data_free(OpenDmData *data)
{
  g_clear_object(&data->manager);
  g_free(data->peer_pubkey_hex);
  g_free(data->kp_json);
  g_free(data);
}

static void
on_dm_welcome_sent(GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  OpenDmData *data = user_data;
  g_autoptr(GError) error = NULL;

  gboolean ok = gn_mls_event_router_send_welcome_finish(
    GN_MLS_EVENT_ROUTER(source), result, &error);

  if (!ok)
    g_warning("MlsDmManager: failed to send DM welcome: %s",
              error ? error->message : "unknown");
  else
    g_info("MlsDmManager: DM welcome sent to %s", data->peer_pubkey_hex);

  /* Task already resolved — nothing more to do */
  open_dm_data_free(data);
}

static void
on_dm_group_created(GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  OpenDmData *data = user_data;
  MarmotGobjectClient *client = MARMOT_GOBJECT_CLIENT(source);
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) welcome_jsons = NULL;
  g_autofree gchar *evolution_json = NULL;

  g_autoptr(MarmotGobjectGroup) group =
    marmot_gobject_client_create_group_finish(
      client, result, &welcome_jsons, &evolution_json, &error);

  if (group == NULL)
    {
      g_task_return_error(data->task, g_steal_pointer(&error));
      g_object_unref(data->task);
      open_dm_data_free(data);
      return;
    }

  g_info("MlsDmManager: DM group created with %s", data->peer_pubkey_hex);

  /* Return the group to the caller */
  g_task_return_pointer(data->task, g_object_ref(group), g_object_unref);
  g_object_unref(data->task);

  /* Send welcome to peer (fire-and-forget — group is already returned) */
  if (welcome_jsons != NULL && welcome_jsons[0] != NULL)
    {
      gn_mls_event_router_send_welcome_async(
        data->manager->router,
        data->peer_pubkey_hex,
        welcome_jsons[0],
        NULL,
        on_dm_welcome_sent,
        data);   /* data ownership transferred */
    }
  else
    {
      open_dm_data_free(data);
    }
}

static void
create_dm_group(OpenDmData *data)
{
  GnMlsDmManager *self = data->manager;

  const gchar *my_pk = gn_marmot_service_get_user_pubkey_hex(self->service);
  if (my_pk == NULL)
    {
      g_task_return_new_error(data->task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                              "User identity not set");
      g_object_unref(data->task);
      open_dm_data_free(data);
      return;
    }

  g_autofree gchar *dm_name = make_dm_group_name(my_pk, data->peer_pubkey_hex);

  /* Build NULL-terminated key package array (peer's KP only) */
  const gchar *kp_array[] = { data->kp_json, NULL };

  /* Admin: only the creator */
  const gchar *admin_array[] = { my_pk, NULL };

  /* Relay URLs */
  gsize n_urls = 0;
  g_auto(GStrv) relay_urls =
    gnostr_plugin_context_get_relay_urls(self->plugin_context, &n_urls);

  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);

  marmot_gobject_client_create_group_async(
    client,
    my_pk,
    kp_array,
    dm_name,
    NULL,   /* no description for DMs */
    admin_array,
    (const gchar *const *)relay_urls,
    g_task_get_cancellable(data->task),
    on_dm_group_created,
    data);
}

/* ── Public API ──────────────────────────────────────────────────── */

GnMlsDmManager *
gn_mls_dm_manager_new(GnMarmotService     *service,
                       GnMlsEventRouter   *router,
                       GnostrPluginContext *plugin_context)
{
  g_return_val_if_fail(GN_IS_MARMOT_SERVICE(service), NULL);
  g_return_val_if_fail(GN_IS_MLS_EVENT_ROUTER(router), NULL);
  g_return_val_if_fail(plugin_context != NULL, NULL);

  GnMlsDmManager *self = g_object_new(GN_TYPE_MLS_DM_MANAGER, NULL);
  self->service        = g_object_ref(service);
  self->router         = g_object_ref(router);
  self->plugin_context = plugin_context;

  return self;
}

void
gn_mls_dm_manager_open_dm_async(GnMlsDmManager      *self,
                                  const gchar          *peer_pubkey_hex,
                                  GCancellable         *cancellable,
                                  GAsyncReadyCallback   callback,
                                  gpointer              user_data)
{
  g_return_if_fail(GN_IS_MLS_DM_MANAGER(self));
  g_return_if_fail(peer_pubkey_hex != NULL);

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gn_mls_dm_manager_open_dm_async);

  const gchar *my_pk = gn_marmot_service_get_user_pubkey_hex(self->service);
  if (my_pk == NULL)
    {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                              "User identity not set");
      g_object_unref(task);
      return;
    }

  /* Step 1: Check if a DM group already exists with this peer */
  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);
  if (client == NULL)
    {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                              "Marmot client not available");
      g_object_unref(task);
      return;
    }

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) all_groups =
    marmot_gobject_client_get_all_groups(client, &error);

  if (all_groups != NULL)
    {
      for (guint i = 0; i < all_groups->len; i++)
        {
          MarmotGobjectGroup *group = g_ptr_array_index(all_groups, i);

          if (marmot_gobject_group_get_state(group) !=
              MARMOT_GOBJECT_GROUP_STATE_ACTIVE)
            continue;

          if (group_is_dm_with_peer(group, my_pk, peer_pubkey_hex))
            {
              g_info("MlsDmManager: found existing DM group with %s",
                     peer_pubkey_hex);
              g_task_return_pointer(task, g_object_ref(group), g_object_unref);
              g_object_unref(task);
              return;
            }
        }
    }

  /* Step 2: No existing DM — fetch peer's key package */
  g_autofree gchar *filter = g_strdup_printf(
    "{\"kinds\":[443],\"authors\":[\"%s\"],\"limit\":1}", peer_pubkey_hex);

  g_autoptr(GError) kp_error = NULL;
  g_autoptr(GPtrArray) events =
    gnostr_plugin_context_query_events(self->plugin_context, filter, &kp_error);

  if (events == NULL || events->len == 0)
    {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              "No key package found for peer %s. "
                              "They must publish a key package (kind:443) first.",
                              peer_pubkey_hex);
      g_object_unref(task);
      return;
    }

  const gchar *kp_json = g_ptr_array_index(events, 0);

  /* Step 3: Create the 2-person DM group */
  OpenDmData *data = g_new0(OpenDmData, 1);
  data->manager         = g_object_ref(self);
  data->task            = task;
  data->peer_pubkey_hex = g_strdup(peer_pubkey_hex);
  data->kp_json         = g_strdup(kp_json);

  create_dm_group(data);
}

MarmotGobjectGroup *
gn_mls_dm_manager_open_dm_finish(GnMlsDmManager *self,
                                   GAsyncResult    *result,
                                   GError         **error)
{
  g_return_val_if_fail(GN_IS_MLS_DM_MANAGER(self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

GPtrArray *
gn_mls_dm_manager_get_dm_groups(GnMlsDmManager *self,
                                  GError         **error)
{
  g_return_val_if_fail(GN_IS_MLS_DM_MANAGER(self), NULL);

  const gchar *my_pk = gn_marmot_service_get_user_pubkey_hex(self->service);
  if (my_pk == NULL)
    {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                          "User identity not set");
      return NULL;
    }

  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);
  if (client == NULL)
    {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                          "Marmot client not available");
      return NULL;
    }

  g_autoptr(GError) local_error = NULL;
  g_autoptr(GPtrArray) all_groups =
    marmot_gobject_client_get_all_groups(client, &local_error);

  if (all_groups == NULL)
    {
      if (error != NULL)
        *error = g_steal_pointer(&local_error);
      return NULL;
    }

  GPtrArray *dm_groups = g_ptr_array_new_with_free_func(g_object_unref);

  for (guint i = 0; i < all_groups->len; i++)
    {
      MarmotGobjectGroup *group = g_ptr_array_index(all_groups, i);
      const gchar *name = marmot_gobject_group_get_name(group);

      if (name != NULL && g_str_has_prefix(name, DM_GROUP_NAME_PREFIX))
        {
          if (marmot_gobject_group_get_state(group) ==
              MARMOT_GOBJECT_GROUP_STATE_ACTIVE)
            g_ptr_array_add(dm_groups, g_object_ref(group));
        }
    }

  return dm_groups;
}
