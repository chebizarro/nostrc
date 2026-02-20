/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-settings-view.c - Group Settings / Info View
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-group-settings-view.h"
#include "gn-member-row.h"
#include <gnostr-plugin-api.h>
#include <json-glib/json-glib.h>

struct _GnGroupSettingsView
{
  GtkBox parent_instance;

  /* Dependencies (strong refs) */
  GnMarmotService     *service;
  GnMlsEventRouter   *router;
  MarmotGobjectGroup  *group;
  GnostrPluginContext *plugin_context;   /* borrowed */

  /* Info widgets */
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

  /* Show/hide add-member controls based on admin status */
  gtk_widget_set_visible(GTK_WIDGET(self->add_member_entry), i_am_admin);
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

  /* Find key package */
  g_autofree gchar *filter = g_strdup_printf(
    "{\"kinds\":[443],\"authors\":[\"%s\"],\"limit\":1}", pk);

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) events =
    gnostr_plugin_context_query_events(self->plugin_context, filter, &error);

  if (events == NULL || events->len == 0)
    {
      gtk_label_set_text(self->member_status_label,
                         "No key package found for this pubkey. "
                         "They must publish a key package first.");
      gtk_widget_set_visible(GTK_WIDGET(self->member_status_label), TRUE);
      return;
    }

  const gchar *kp_json = g_ptr_array_index(events, 0);

  /* Disable button, show spinner */
  gtk_widget_set_sensitive(GTK_WIDGET(self->add_member_button), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->member_status_label), FALSE);
  gtk_spinner_start(self->member_spinner);
  gtk_widget_set_visible(GTK_WIDGET(self->member_spinner), TRUE);

  /*
   * TODO: In a full implementation, we would call an "add member to group"
   * marmot API that creates an MLS Add proposal + Commit, then sends a
   * Welcome to the new member. For now, we send an invitation (welcome)
   * using the existing group state — this is a simplified flow.
   *
   * The proper flow requires:
   * 1. marmot_add_member(group_id, key_package) → welcome + commit
   * 2. Publish commit as kind:445
   * 3. Send welcome via NIP-59 gift wrap
   *
   * For now we show the UI and log a TODO.
   */
  g_info("GroupSettings: add member %s requested — "
         "full MLS Add+Commit flow not yet implemented", pk);

  gtk_label_set_text(self->member_status_label,
                     "Add member via MLS proposal — coming soon");
  gtk_widget_set_visible(GTK_WIDGET(self->member_status_label), TRUE);
  gtk_spinner_stop(self->member_spinner);
  gtk_widget_set_visible(GTK_WIDGET(self->member_spinner), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->add_member_button), TRUE);

  gtk_editable_set_text(GTK_EDITABLE(self->add_member_entry), "");
}

static void
on_add_member_entry_activate(GtkEditable *editable, gpointer user_data)
{
  on_add_member_clicked(NULL, user_data);
}

/* ── Leave group ─────────────────────────────────────────────────── */

static void
on_leave_clicked(GtkButton *button, gpointer user_data)
{
  GnGroupSettingsView *self = GN_GROUP_SETTINGS_VIEW(user_data);

  /*
   * TODO: Call marmot leave_group API when available.
   * For now, emit signal so the host can navigate back.
   */
  g_info("GroupSettings: user requested to leave group %s",
         marmot_gobject_group_get_mls_group_id(self->group));

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
