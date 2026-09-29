/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-settings-view.c - Group Settings / Info View
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-group-settings-view.h"
#include "gn-member-row.h"
#include "../gn-key-package-discovery.h"
#include "../gn-mls-commit-publish.h"
#include "../gn-mls-group-error.h"
#include <gnostr-plugin-api.h>
#include <json-glib/json-glib.h>
#include <marmot/marmot.h>

struct _GnGroupSettingsView
{
  GtkBox parent_instance;

  /* Dependencies (strong refs) */
  GnMarmotService     *service;
  GnMlsEventRouter   *router;
  MarmotGobjectGroup  *group;
  GnostrPluginContext *plugin_context;   /* borrowed */

  /* Info widgets */
  AdwEntryRow *rename_entry;   /* admin-only */
  GtkLabel    *rename_status_label;
  GtkImage  *group_icon;
  GtkLabel  *group_name_label;
  GtkLabel  *group_desc_label;
  GtkLabel  *group_id_label;
  GtkLabel  *epoch_label;
  GtkLabel  *state_label;
  GtkLabel  *admin_count_label;

  /* Member management */
  GtkListBox      *member_list;
  AdwEntryRow     *add_member_entry;
  GtkButton       *add_member_button;
  GtkLabel        *member_status_label;
  GtkSpinner      *member_spinner;

  /* Actions */
  GtkButton *leave_button;

  /* Signal IDs */
  gulong sig_group_updated;
};

enum
{
  SIGNAL_MEMBER_ADDED,
  SIGNAL_LEFT_GROUP,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE(GnGroupSettingsView, gn_group_settings_view, GTK_TYPE_BOX)

/* ── Forward declarations ────────────────────────────────────────── */

static void refresh_group_info(GnGroupSettingsView *self);
static void rebuild_member_list(GnGroupSettingsView *self);

/* ── Group info display ──────────────────────────────────────────── */

static const gchar *
state_to_string(MarmotGobjectGroupState state)
{
  switch (state)
    {
    case MARMOT_GOBJECT_GROUP_STATE_ACTIVE:   return "Active";
    case MARMOT_GOBJECT_GROUP_STATE_INACTIVE: return "Inactive";
    case MARMOT_GOBJECT_GROUP_STATE_PENDING:  return "Pending";
    default: return "Unknown";
    }
}

static void
refresh_group_info(GnGroupSettingsView *self)
{
  if (self->group == NULL) return;

  const gchar *name = marmot_gobject_group_get_name(self->group);
  const gchar *desc = marmot_gobject_group_get_description(self->group);
  const gchar *mls_id = marmot_gobject_group_get_mls_group_id(self->group);
  guint64 epoch = marmot_gobject_group_get_epoch(self->group);
  MarmotGobjectGroupState state = marmot_gobject_group_get_state(self->group);
  guint admin_count = marmot_gobject_group_get_admin_count(self->group);

  gtk_label_set_text(self->group_name_label,
                     (name && *name) ? name : "(Unnamed Group)");
  gtk_editable_set_text(GTK_EDITABLE(self->rename_entry), name ? name : "");
  gtk_label_set_text(self->group_desc_label,
                     (desc && *desc) ? desc : "No description");
  gtk_widget_set_visible(GTK_WIDGET(self->group_desc_label),
                         desc != NULL && *desc != '\0');

  /* Group ID (truncated) */
  if (mls_id != NULL && strlen(mls_id) >= 16)
    {
      g_autofree gchar *short_id = g_strdup_printf("%.8s…%.8s",
                                                    mls_id, mls_id + strlen(mls_id) - 8);
      gtk_label_set_text(self->group_id_label, short_id);
    }
  else
    gtk_label_set_text(self->group_id_label, mls_id ? mls_id : "—");

  g_autofree gchar *epoch_str = g_strdup_printf("%" G_GUINT64_FORMAT, epoch);
  gtk_label_set_text(self->epoch_label, epoch_str);

  gtk_label_set_text(self->state_label, state_to_string(state));

  g_autofree gchar *admin_str = g_strdup_printf("%u", admin_count);
  gtk_label_set_text(self->admin_count_label, admin_str);

  rebuild_member_list(self);
}

/* ── Member list ─────────────────────────────────────────────────── */

static void
rebuild_member_list(GnGroupSettingsView *self)
{
  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->member_list))) != NULL)
    gtk_list_box_remove(self->member_list, child);

  if (self->group == NULL) return;

  const gchar *my_pk = gn_marmot_service_get_user_pubkey_hex(self->service);
  guint admin_count = marmot_gobject_group_get_admin_count(self->group);

  /* Check if current user is admin */
  gboolean i_am_admin = FALSE;
  for (guint i = 0; i < admin_count; i++)
    {
      g_autofree gchar *admin_pk =
        marmot_gobject_group_get_admin_pubkey_hex(self->group, i);
      if (admin_pk && my_pk && g_strcmp0(admin_pk, my_pk) == 0)
        {
          i_am_admin = TRUE;
          break;
        }
    }

  /* Add admin rows */
  for (guint i = 0; i < admin_count; i++)
    {
      g_autofree gchar *admin_pk =
        marmot_gobject_group_get_admin_pubkey_hex(self->group, i);
      if (admin_pk == NULL) continue;

      gboolean is_self = (my_pk && g_strcmp0(admin_pk, my_pk) == 0);

      GnMemberRow *row = gn_member_row_new();
      gn_member_row_set_pubkey(row, admin_pk, TRUE, is_self);
      /* Admins can't be removed via this simple UI (would need MLS proposal) */
      gn_member_row_set_removable(row, FALSE);
      gtk_list_box_append(self->member_list, GTK_WIDGET(row));
    }

  /*
   * Note: The MarmotGobjectGroup currently exposes admin pubkeys.
   * Full member list would require querying the MLS tree, which isn't
   * exposed yet. For now we show admins and indicate total membership
   * isn't fully enumerable. Future: add get_member_pubkeys() to API.
   */

  /* Show/hide admin controls based on admin status */
  gtk_widget_set_visible(GTK_WIDGET(self->add_member_entry), i_am_admin);
  gtk_widget_set_visible(GTK_WIDGET(self->rename_entry), i_am_admin);
}

/* ── Publishing a Commit (MIP-03 publish-before-merge, review B1) ──
 *
 * libmarmot keeps a Commit pending until we report that a group relay
 * accepted it (NIP-01 OK): merge it then, clear it when no relay did. */

static void
ack_publish(gpointer target, const char *event_json, const char *relay_url,
            GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  gnostr_plugin_context_publish_event_to_relay_ack_async(target, event_json, relay_url,
                                                         cancellable, callback, user_data);
}

static gboolean
ack_finish(gpointer target, GAsyncResult *result, GError **error)
{
  return gnostr_plugin_context_publish_event_to_relay_ack_finish(target, result, error);
}

static void
publish_commit_async(GnGroupSettingsView *self, const gchar *commit_json,
                     GAsyncReadyCallback callback, gpointer user_data)
{
  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);
  const gchar *group_id_hex = marmot_gobject_group_get_mls_group_id(self->group);
  gsize relay_count = 0;
  /* Without a plugin context nothing can be published: no relays, so the
   * attempt fails and the caller discards the pending Commit. */
  g_auto(GStrv) relays = (client && group_id_hex && self->plugin_context)
    ? marmot_gobject_client_get_group_relay_urls(client, group_id_hex, &relay_count)
    : NULL;
  gn_mls_publish_until_ack_async(ack_publish, ack_finish, self->plugin_context,
                                 commit_json, (const char * const *) relays, NULL,
                                 callback, user_data);
}

static void
show_status(GtkLabel *label, const gchar *text)
{
  gtk_label_set_text(label, text ? text : "");
  gtk_widget_set_visible(GTK_WIDGET(label), text != NULL && *text != '\0');
}

/* ── Add member flow ─────────────────────────────────────────────── */

typedef struct
{
  GnGroupSettingsView *view;   /* weak — view owns the flow */
  gchar               *pubkey_hex;
  gchar               *kp_json;
} AddMemberData;

static void
add_member_data_free(AddMemberData *data)
{
  g_free(data->pubkey_hex);
  g_free(data->kp_json);
  g_free(data);
}

static void
on_add_member_welcome_sent(GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  AddMemberData *data = user_data;
  GnGroupSettingsView *self = data->view;
  g_autoptr(GError) error = NULL;

  gboolean ok = gn_mls_event_router_send_welcome_finish(
    GN_MLS_EVENT_ROUTER(source), result, &error);

  gtk_spinner_stop(self->member_spinner);
  gtk_widget_set_visible(GTK_WIDGET(self->member_spinner), FALSE);

  if (!ok)
    {
      g_warning("GroupSettings: failed to send welcome: %s",
                error ? error->message : "unknown");
      gtk_label_set_text(self->member_status_label,
                         error ? error->message : "Failed to send invitation");
      gtk_widget_set_visible(GTK_WIDGET(self->member_status_label), TRUE);
    }
  else
    {
      gtk_label_set_text(self->member_status_label, "Invitation sent!");
      gtk_widget_set_visible(GTK_WIDGET(self->member_status_label), TRUE);
      g_signal_emit(self, signals[SIGNAL_MEMBER_ADDED], 0, data->pubkey_hex);
    }

  gtk_widget_set_sensitive(GTK_WIDGET(self->add_member_button), TRUE);
  add_member_data_free(data);
}

static void add_member_with_key_package(GnGroupSettingsView *self, const gchar *pk,
                                        const gchar *kp_json);

typedef struct {
  GnGroupSettingsView *self;   /* strong */
  gchar *pk;
} MemberKpLookup;

static void
on_member_key_package(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  MemberKpLookup *lookup = user_data;
  GnGroupSettingsView *self = lookup->self;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *kp_json = gn_kp_discover_finish(result, &error);

  if (kp_json == NULL)
    {
      g_debug("GroupSettings: %s", error ? error->message : "no key package");
      gtk_label_set_text(self->member_status_label,
                         "No key package found for this pubkey. "
                         "They must publish a key package first.");
      gtk_widget_set_visible(GTK_WIDGET(self->member_status_label), TRUE);
      gtk_spinner_stop(self->member_spinner);
      gtk_widget_set_visible(GTK_WIDGET(self->member_spinner), FALSE);
      gtk_widget_set_sensitive(GTK_WIDGET(self->add_member_button), TRUE);
    }
  else
    {
      add_member_with_key_package(self, lookup->pk, kp_json);
    }
  g_object_unref(lookup->self);
  g_free(lookup->pk);
  g_free(lookup);
}

static void
on_add_member_clicked(GtkButton *button, gpointer user_data)
{
  GnGroupSettingsView *self = GN_GROUP_SETTINGS_VIEW(user_data);

  const gchar *text = gtk_editable_get_text(GTK_EDITABLE(self->add_member_entry));
  if (text == NULL || *text == '\0')
    return;

  g_autofree gchar *pk = g_strstrip(g_strdup(text));
  if (strlen(pk) != 64)
    {
      gtk_label_set_text(self->member_status_label,
                         "Invalid pubkey — enter 64-character hex");
      gtk_widget_set_visible(GTK_WIDGET(self->member_status_label), TRUE);
      return;
    }

  /* Validate hex */
  for (gsize i = 0; i < 64; i++)
    {
      if (!g_ascii_isxdigit(pk[i]))
        {
          gtk_label_set_text(self->member_status_label,
                             "Invalid pubkey — enter 64-character hex");
          gtk_widget_set_visible(GTK_WIDGET(self->member_status_label), TRUE);
          return;
        }
    }

  /* Disable button, show spinner */
  gtk_widget_set_sensitive(GTK_WIDGET(self->add_member_button), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->member_status_label), FALSE);
  gtk_spinner_start(self->member_spinner);
  gtk_widget_set_visible(GTK_WIDGET(self->member_spinner), TRUE);

  /* nostrc-prqu.11: find the KeyPackage on the invitee's kind:10002 write
   * relays and choose it with marmot_gobject_select_key_package_event(). */
  MemberKpLookup *lookup = g_new0(MemberKpLookup, 1);
  lookup->self = g_object_ref(self);
  lookup->pk = g_ascii_strdown(pk, -1);
  GnKpBackend backend;
  gn_kp_backend_init_for_plugin(&backend, self->plugin_context);
  gn_kp_discover_async(&backend, lookup->pk, NULL, on_member_key_package, lookup);
}

typedef struct
{
  GnGroupSettingsView *view;   /* strong */
  gchar               *pk;
  guint8              *gid;
  gsize                gid_len;
  char               **welcomes;   /* malloc()ed by libmarmot */
  size_t               welcome_count;
} AddCommitData;

static void
add_commit_data_free(AddCommitData *data)
{
  for (size_t i = 0; i < data->welcome_count && data->welcomes; i++)
    free(data->welcomes[i]);
  free(data->welcomes);
  g_free(data->gid);
  g_free(data->pk);
  g_clear_object(&data->view);
  g_free(data);
}

static void
finish_add_member_ui(GnGroupSettingsView *self, const gchar *status)
{
  show_status(self->member_status_label, status);
  gtk_spinner_stop(self->member_spinner);
  gtk_widget_set_visible(GTK_WIDGET(self->member_spinner), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->add_member_button), TRUE);
}

static void
on_add_commit_published(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  AddCommitData *data = user_data;
  GnGroupSettingsView *self = data->view;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *relay = gn_mls_publish_until_ack_finish(result, &error);
  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);
  struct Marmot *m = client ? marmot_gobject_client_get_marmot(client) : NULL;
  MarmotGroupId gid = marmot_group_id_new(data->gid, data->gid_len);

  if (relay == NULL)
    {
      /* No relay took the Commit: the group stays as it was. */
      if (m != NULL) marmot_clear_pending_commit(m, &gid);
      g_warning("GroupSettings: add-member Commit not accepted by any relay: %s",
                error ? error->message : "unknown");
      g_autofree gchar *msg = g_strdup_printf("Could not publish the invitation: %s",
                                              error ? error->message : "no relay accepted it");
      finish_add_member_ui(self, msg);
    }
  else
    {
      MarmotError err = m ? marmot_merge_pending_commit(m, &gid) : MARMOT_ERR_INVALID_ARG;
      if (err == MARMOT_OK)
        {
          g_info("GroupSettings: add-member Commit accepted by %s and merged", relay);
          /* Send welcome(s) via NIP-59 gift wrap to the new member(s) */
          for (size_t i = 0; i < data->welcome_count && data->welcomes; i++)
            if (data->welcomes[i] != NULL)
              gn_mls_event_router_send_welcome_async(self->router, data->pk,
                                                     data->welcomes[i], NULL, NULL, NULL);
          finish_add_member_ui(self, "Member added");
          gtk_editable_set_text(GTK_EDITABLE(self->add_member_entry), "");
          g_signal_emit(self, signals[SIGNAL_MEMBER_ADDED], 0, data->pk);
        }
      else if (err == MARMOT_ERR_WRONG_EPOCH)
        finish_add_member_ui(self, "Another member's change reached the group first; "
                                   "the member was not added. Try again.");
      else
        {
          g_autofree gchar *msg = g_strdup_printf("Could not add the member: %s",
                                                  marmot_error_string(err));
          finish_add_member_ui(self, msg);
        }
    }
  marmot_group_id_free(&gid);
  add_commit_data_free(data);
}

static void
add_member_with_key_package(GnGroupSettingsView *self, const gchar *pk, const gchar *kp_json)
{

  /*
   * MLS Add+Commit flow:
   * 1. marmot_add_members(group_id, key_package) → welcome + commit
   * 2. Publish commit as kind:445 event
   * 3. Send welcome via NIP-59 gift wrap to the new member
   */
  const gchar *group_id_hex = marmot_gobject_group_get_mls_group_id(self->group);
  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);

  if (client == NULL || group_id_hex == NULL)
    {
      gtk_label_set_text(self->member_status_label, "Service not available");
      gtk_widget_set_visible(GTK_WIDGET(self->member_status_label), TRUE);
      gtk_spinner_stop(self->member_spinner);
      gtk_widget_set_visible(GTK_WIDGET(self->member_spinner), FALSE);
      gtk_widget_set_sensitive(GTK_WIDGET(self->add_member_button), TRUE);
      return;
    }

  struct Marmot *m = marmot_gobject_client_get_marmot(client);
  if (m == NULL)
    {
      gtk_label_set_text(self->member_status_label, "Marmot not initialized");
      gtk_widget_set_visible(GTK_WIDGET(self->member_status_label), TRUE);
      gtk_spinner_stop(self->member_spinner);
      gtk_widget_set_visible(GTK_WIDGET(self->member_spinner), FALSE);
      gtk_widget_set_sensitive(GTK_WIDGET(self->add_member_button), TRUE);
      return;
    }

  /* Convert group ID hex → MarmotGroupId */
  gsize hex_len = strlen(group_id_hex);
  gsize gid_len = hex_len / 2;
  g_autofree uint8_t *gid_bytes = g_malloc(gid_len);
  for (gsize i = 0; i < gid_len; i++)
    {
      guint byte_val = 0;
      sscanf(group_id_hex + (i * 2), "%02x", &byte_val);
      gid_bytes[i] = (uint8_t)byte_val;
    }
  MarmotGroupId mls_gid = marmot_group_id_new(gid_bytes, gid_len);

  /* Call marmot to create Add proposal + Commit + Welcome */
  const char *kp_array[] = { kp_json, NULL };
  char **welcome_jsons = NULL;
  size_t welcome_count = 0;
  char *commit_json = NULL;

  MarmotError err = marmot_add_members(m, &mls_gid,
                                        kp_array, 1,
                                        &welcome_jsons, &welcome_count,
                                        &commit_json);
  marmot_group_id_free(&mls_gid);

  if (err != MARMOT_OK)
    {
      g_warning("GroupSettings: marmot_add_members failed: %d", err);
      gtk_label_set_text(self->member_status_label,
                         "Failed to add member to MLS group");
      gtk_widget_set_visible(GTK_WIDGET(self->member_status_label), TRUE);
      gtk_spinner_stop(self->member_spinner);
      gtk_widget_set_visible(GTK_WIDGET(self->member_spinner), FALSE);
      gtk_widget_set_sensitive(GTK_WIDGET(self->add_member_button), TRUE);
      return;
    }

  g_info("GroupSettings: marmot_add_members made a pending Commit (%zu welcome(s))",
         welcome_count);

  /* Publish the Commit; merge and send the Welcomes only once a group relay
   * accepted it, otherwise discard it (MIP-03). */
  AddCommitData *data = g_new0(AddCommitData, 1);
  data->view = g_object_ref(self);
  data->pk = g_strdup(pk);
  data->gid = g_memdup2(gid_bytes, gid_len);
  data->gid_len = gid_len;
  data->welcomes = welcome_jsons;
  data->welcome_count = welcome_count;
  publish_commit_async(self, commit_json, on_add_commit_published, data);
  free(commit_json);
}

static void
on_add_member_entry_activate(GtkEditable *editable, gpointer user_data)
{
  on_add_member_clicked(NULL, user_data);
}

/* ── Rename (MIP-01 metadata Commit, nostrc-9ata) ────────────────── */

static void
finish_rename_ui(GnGroupSettingsView *self, const gchar *status)
{
  gtk_widget_set_sensitive(GTK_WIDGET(self->rename_entry), TRUE);
  show_status(self->rename_status_label, status);
  refresh_group_info(self);
}

static void
on_rename_merged(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GnGroupSettingsView *self = user_data;   /* strong */
  g_autoptr(GError) error = NULL;
  if (marmot_gobject_client_merge_pending_commit_finish(MARMOT_GOBJECT_CLIENT(source),
                                                        result, &error))
    finish_rename_ui(self, NULL);   /* ::group-updated shows the new name */
  else if (gn_mls_group_error_is_superseded(error))
    finish_rename_ui(self, "Another member's change reached the group first; "
                           "the group was not renamed.");
  else
    {
      g_autofree gchar *msg = g_strdup_printf("Rename failed: %s", error->message);
      finish_rename_ui(self, msg);
    }
  g_object_unref(self);
}

typedef struct
{
  GnGroupSettingsView *view;   /* strong */
  gchar               *reason;
} RenameClear;

static void
on_rename_cleared(GObject *source, GAsyncResult *result, gpointer user_data)
{
  RenameClear *rc = user_data;
  g_autoptr(GError) error = NULL;
  if (!marmot_gobject_client_clear_pending_commit_finish(MARMOT_GOBJECT_CLIENT(source),
                                                         result, &error))
    g_warning("GroupSettings: discarding the rename Commit failed: %s", error->message);
  g_autofree gchar *msg = g_strdup_printf("The group was not renamed: %s", rc->reason);
  finish_rename_ui(rc->view, msg);
  g_object_unref(rc->view);
  g_free(rc->reason);
  g_free(rc);
}

static void
on_rename_published(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  GnGroupSettingsView *self = user_data;   /* strong */
  g_autoptr(GError) error = NULL;
  g_autofree gchar *relay = gn_mls_publish_until_ack_finish(result, &error);
  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);
  const gchar *group_id_hex = marmot_gobject_group_get_mls_group_id(self->group);
  if (client == NULL || group_id_hex == NULL)
    {
      finish_rename_ui(self, "The group service is not available.");
      g_object_unref(self);
      return;
    }
  if (relay != NULL)
    {
      g_info("GroupSettings: rename Commit accepted by %s; merging", relay);
      marmot_gobject_client_merge_pending_commit_async(client, group_id_hex, NULL,
                                                       on_rename_merged, self);
      return;
    }
  /* No relay took it: the group stays as it was (MIP-03). */
  g_warning("GroupSettings: rename Commit not accepted by any relay: %s",
            error ? error->message : "unknown");
  RenameClear *rc = g_new0(RenameClear, 1);
  rc->view = self;   /* ref passed on */
  rc->reason = g_strdup(error ? error->message : "no relay accepted it");
  marmot_gobject_client_clear_pending_commit_async(client, group_id_hex, NULL,
                                                   on_rename_cleared, rc);
}

static void
on_rename_committed(GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  GnGroupSettingsView *self = user_data;   /* strong */
  MarmotGobjectClient *client = MARMOT_GOBJECT_CLIENT(source);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *commit_json =
    marmot_gobject_client_update_group_metadata_finish(client, result, &error);

  if (commit_json == NULL)
    {
      g_autofree gchar *msg = g_strdup_printf("Rename failed: %s",
                                              error ? error->message : "unknown");
      finish_rename_ui(self, msg);
      g_object_unref(self);
      return;
    }
  if (self->plugin_context == NULL)
    {
      /* Cannot publish: do not leave the Commit pending. */
      marmot_gobject_client_clear_pending_commit_async(
        client, marmot_gobject_group_get_mls_group_id(self->group), NULL, NULL, NULL);
      finish_rename_ui(self, "The group was not renamed: the plugin is shutting down.");
      g_object_unref(self);
      return;
    }
  /* The Commit (signed kind:445) goes to the group relays; libmarmot keeps
   * it pending until one of them accepts it. */
  publish_commit_async(self, commit_json, on_rename_published, self);   /* ref passed on */
}

static void
on_rename_apply(AdwEntryRow *row, gpointer user_data)
{
  GnGroupSettingsView *self = GN_GROUP_SETTINGS_VIEW(user_data);
  g_autofree gchar *name = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(row))));
  const gchar *current = marmot_gobject_group_get_name(self->group);
  const gchar *group_id_hex = marmot_gobject_group_get_mls_group_id(self->group);
  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);

  if (name[0] == '\0' || g_strcmp0(name, current) == 0 ||
      client == NULL || group_id_hex == NULL)
    {
      refresh_group_info(self);
      return;
    }

  gtk_widget_set_sensitive(GTK_WIDGET(self->rename_entry), FALSE);
  show_status(self->rename_status_label, "Publishing…");
  marmot_gobject_client_update_group_metadata_async(client, group_id_hex, name, NULL,
                                                    NULL, on_rename_committed,
                                                    g_object_ref(self));
}

/* ── Leave group ─────────────────────────────────────────────────── */

static void
on_leave_clicked(GtkButton *button, gpointer user_data)
{
  GnGroupSettingsView *self = GN_GROUP_SETTINGS_VIEW(user_data);

  const gchar *group_id_hex = marmot_gobject_group_get_mls_group_id(self->group);
  g_info("GroupSettings: user requested to leave group %s", group_id_hex);

  /* Leave the MLS group via libmarmot */
  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);
  if (client != NULL)
    {
      struct Marmot *m = marmot_gobject_client_get_marmot(client);
      if (m != NULL && group_id_hex != NULL)
        {
          gsize hex_len = strlen(group_id_hex);
          gsize gid_len = hex_len / 2;
          g_autofree uint8_t *gid_bytes = g_malloc(gid_len);

          for (gsize i = 0; i < gid_len; i++)
            {
              guint byte_val = 0;
              sscanf(group_id_hex + (i * 2), "%02x", &byte_val);
              gid_bytes[i] = (uint8_t)byte_val;
            }

          MarmotGroupId mls_gid = marmot_group_id_new(gid_bytes, gid_len);
          MarmotError err = marmot_leave_group(m, &mls_gid);
          marmot_group_id_free(&mls_gid);

          if (err != MARMOT_OK)
            g_warning("GroupSettings: marmot_leave_group failed: %d", err);
          else
            g_info("GroupSettings: left group %s", group_id_hex);
        }
    }

  /* Navigate back regardless — the group is now inactive locally */
  g_signal_emit(self, signals[SIGNAL_LEFT_GROUP], 0);
}

/* ── Signal handlers ─────────────────────────────────────────────── */

static void
on_group_updated(GnMarmotService    *service,
                 MarmotGobjectGroup *group,
                 gpointer            user_data)
{
  GnGroupSettingsView *self = GN_GROUP_SETTINGS_VIEW(user_data);

  const gchar *my_id = marmot_gobject_group_get_mls_group_id(self->group);
  const gchar *upd_id = marmot_gobject_group_get_mls_group_id(group);

  if (g_strcmp0(my_id, upd_id) != 0)
    return;

  /* Replace our group reference with the updated one */
  g_set_object(&self->group, group);
  refresh_group_info(self);
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gn_group_settings_view_dispose(GObject *object)
{
  GnGroupSettingsView *self = GN_GROUP_SETTINGS_VIEW(object);

  if (self->sig_group_updated > 0 && self->service != NULL)
    {
      g_signal_handler_disconnect(self->service, self->sig_group_updated);
      self->sig_group_updated = 0;
    }

  g_clear_object(&self->service);
  g_clear_object(&self->router);
  g_clear_object(&self->group);
  self->plugin_context = NULL;

  G_OBJECT_CLASS(gn_group_settings_view_parent_class)->dispose(object);
}

static void
gn_group_settings_view_class_init(GnGroupSettingsViewClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = gn_group_settings_view_dispose;

  signals[SIGNAL_MEMBER_ADDED] = g_signal_new(
    "member-added",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_LEFT_GROUP] = g_signal_new(
    "left-group",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);
}

static void
gn_group_settings_view_init(GnGroupSettingsView *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);

  /* Scrolled content */
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(self), scroll);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);
  gtk_widget_set_margin_top(content, 24);
  gtk_widget_set_margin_bottom(content, 24);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);

  /* ── Header: icon + name + description ─────────────────────────── */
  GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_halign(header_box, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_bottom(header_box, 24);
  gtk_box_append(GTK_BOX(content), header_box);

  self->group_icon = GTK_IMAGE(
    gtk_image_new_from_icon_name("system-users-symbolic"));
  gtk_image_set_pixel_size(self->group_icon, 64);
  gtk_widget_add_css_class(GTK_WIDGET(self->group_icon), "dim-label");
  gtk_widget_set_halign(GTK_WIDGET(self->group_icon), GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(header_box), GTK_WIDGET(self->group_icon));

  self->group_name_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(self->group_name_label), "title-1");
  gtk_label_set_ellipsize(self->group_name_label, PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(GTK_WIDGET(self->group_name_label), GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(header_box), GTK_WIDGET(self->group_name_label));

  self->group_desc_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(self->group_desc_label), "dim-label");
  gtk_label_set_wrap(self->group_desc_label, TRUE);
  gtk_label_set_justify(self->group_desc_label, GTK_JUSTIFY_CENTER);
  gtk_widget_set_halign(GTK_WIDGET(self->group_desc_label), GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(header_box), GTK_WIDGET(self->group_desc_label));

  /* ── Info section ──────────────────────────────────────────────── */
  AdwPreferencesGroup *info_group = ADW_PREFERENCES_GROUP(
    adw_preferences_group_new());
  adw_preferences_group_set_title(info_group, "Group Info");
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(info_group));

  /* Group name (admins only; committed and published as a Commit) */
  self->rename_entry = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->rename_entry), "Group Name");
  adw_entry_row_set_show_apply_button(self->rename_entry, TRUE);
  gtk_widget_set_visible(GTK_WIDGET(self->rename_entry), FALSE);
  g_signal_connect(self->rename_entry, "apply", G_CALLBACK(on_rename_apply), self);
  adw_preferences_group_add(info_group, GTK_WIDGET(self->rename_entry));
  self->rename_status_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(self->rename_status_label), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(self->rename_status_label), "caption");
  gtk_label_set_wrap(self->rename_status_label, TRUE);
  gtk_widget_set_margin_top(GTK_WIDGET(self->rename_status_label), 6);
  gtk_widget_set_visible(GTK_WIDGET(self->rename_status_label), FALSE);
  adw_preferences_group_add(info_group, GTK_WIDGET(self->rename_status_label));

  /* Group ID row */
  AdwActionRow *id_row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(id_row), "Group ID");
  self->group_id_label = GTK_LABEL(gtk_label_new("—"));
  gtk_widget_add_css_class(GTK_WIDGET(self->group_id_label), "dim-label");
  adw_action_row_add_suffix(id_row, GTK_WIDGET(self->group_id_label));
  adw_preferences_group_add(info_group, GTK_WIDGET(id_row));

  /* Epoch row */
  AdwActionRow *epoch_row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(epoch_row), "MLS Epoch");
  self->epoch_label = GTK_LABEL(gtk_label_new("0"));
  gtk_widget_add_css_class(GTK_WIDGET(self->epoch_label), "dim-label");
  adw_action_row_add_suffix(epoch_row, GTK_WIDGET(self->epoch_label));
  adw_preferences_group_add(info_group, GTK_WIDGET(epoch_row));

  /* State row */
  AdwActionRow *state_row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(state_row), "Status");
  self->state_label = GTK_LABEL(gtk_label_new("—"));
  gtk_widget_add_css_class(GTK_WIDGET(self->state_label), "dim-label");
  adw_action_row_add_suffix(state_row, GTK_WIDGET(self->state_label));
  adw_preferences_group_add(info_group, GTK_WIDGET(state_row));

  /* Admin count row */
  AdwActionRow *admin_row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(admin_row), "Admins");
  self->admin_count_label = GTK_LABEL(gtk_label_new("0"));
  gtk_widget_add_css_class(GTK_WIDGET(self->admin_count_label), "dim-label");
  adw_action_row_add_suffix(admin_row, GTK_WIDGET(self->admin_count_label));
  adw_preferences_group_add(info_group, GTK_WIDGET(admin_row));

  /* ── Members section ───────────────────────────────────────────── */
  AdwPreferencesGroup *members_group = ADW_PREFERENCES_GROUP(
    adw_preferences_group_new());
  adw_preferences_group_set_title(members_group, "Members");
  adw_preferences_group_set_description(members_group,
    "Group admins are shown below. Full member enumeration "
    "requires MLS tree traversal (coming soon).");
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(members_group));

  /* Member list */
  self->member_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->member_list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class(GTK_WIDGET(self->member_list), "boxed-list");
  adw_preferences_group_add(members_group, GTK_WIDGET(self->member_list));

  /* Add member input (admin-only, hidden by default) */
  self->add_member_entry = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->add_member_entry),
                                "Add Member (pubkey hex)");
  gtk_widget_set_visible(GTK_WIDGET(self->add_member_entry), FALSE);
  g_signal_connect(self->add_member_entry, "entry-activated",
                   G_CALLBACK(on_add_member_entry_activate), self);
  adw_preferences_group_add(members_group, GTK_WIDGET(self->add_member_entry));

  /* Add button as suffix on entry */
  self->add_member_button = GTK_BUTTON(
    gtk_button_new_from_icon_name("list-add-symbolic"));
  gtk_widget_add_css_class(GTK_WIDGET(self->add_member_button), "flat");
  gtk_widget_set_valign(GTK_WIDGET(self->add_member_button), GTK_ALIGN_CENTER);
  g_signal_connect(self->add_member_button, "clicked",
                   G_CALLBACK(on_add_member_clicked), self);
  adw_entry_row_add_suffix(self->add_member_entry,
                            GTK_WIDGET(self->add_member_button));

  /* Status row */
  GtkWidget *member_status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(member_status_box, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(member_status_box, 6);
  adw_preferences_group_add(members_group, member_status_box);

  self->member_spinner = GTK_SPINNER(gtk_spinner_new());
  gtk_widget_set_visible(GTK_WIDGET(self->member_spinner), FALSE);
  gtk_box_append(GTK_BOX(member_status_box), GTK_WIDGET(self->member_spinner));

  self->member_status_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(self->member_status_label), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(self->member_status_label), "caption");
  gtk_widget_set_visible(GTK_WIDGET(self->member_status_label), FALSE);
  gtk_box_append(GTK_BOX(member_status_box),
                 GTK_WIDGET(self->member_status_label));

  /* ── Danger Zone ───────────────────────────────────────────────── */
  AdwPreferencesGroup *danger_group = ADW_PREFERENCES_GROUP(
    adw_preferences_group_new());
  adw_preferences_group_set_title(danger_group, "");
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(danger_group));

  self->leave_button = GTK_BUTTON(gtk_button_new_with_label("Leave Group"));
  gtk_widget_add_css_class(GTK_WIDGET(self->leave_button), "destructive-action");
  gtk_widget_add_css_class(GTK_WIDGET(self->leave_button), "pill");
  gtk_widget_set_halign(GTK_WIDGET(self->leave_button), GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(GTK_WIDGET(self->leave_button), 24);
  g_signal_connect(self->leave_button, "clicked",
                   G_CALLBACK(on_leave_clicked), self);
  adw_preferences_group_add(danger_group, GTK_WIDGET(self->leave_button));
}

/* ── Public API ──────────────────────────────────────────────────── */

GnGroupSettingsView *
gn_group_settings_view_new(GnMarmotService      *service,
                            GnMlsEventRouter    *router,
                            MarmotGobjectGroup  *group,
                            GnostrPluginContext *plugin_context)
{
  g_return_val_if_fail(GN_IS_MARMOT_SERVICE(service), NULL);
  g_return_val_if_fail(GN_IS_MLS_EVENT_ROUTER(router), NULL);
  g_return_val_if_fail(MARMOT_GOBJECT_IS_GROUP(group), NULL);
  g_return_val_if_fail(plugin_context != NULL, NULL);

  GnGroupSettingsView *self =
    g_object_new(GN_TYPE_GROUP_SETTINGS_VIEW, NULL);
  self->service        = g_object_ref(service);
  self->router         = g_object_ref(router);
  self->group          = g_object_ref(group);
  self->plugin_context = plugin_context;

  /* Listen for group updates */
  self->sig_group_updated = g_signal_connect(
    service, "group-updated",
    G_CALLBACK(on_group_updated), self);

  /* Initial display */
  refresh_group_info(self);

  return self;
}
