/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-create-group-dialog.c - Create Group Dialog
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-create-group-dialog.h"
#include "gn-member-row.h"
#include <gnostr-plugin-api.h>
#include <json-glib/json-glib.h>
#include <marmot-gobject-1.0/marmot-gobject.h>

/* ── Types ───────────────────────────────────────────────────────── */

typedef struct
{
  GnCreateGroupDialog *dialog;   /* weak — dialog owns this flow */
  gchar               *group_name;
  gchar               *group_description;
  GPtrArray           *member_pubkeys;       /* (element-type utf8) */
  GPtrArray           *key_package_jsons;    /* (element-type utf8) */
  gchar              **admin_pubkey_hexes;   /* NULL-terminated */
  gchar              **relay_urls;           /* NULL-terminated */
} CreateGroupFlowData;

static void create_group_flow_data_free(CreateGroupFlowData *data);

struct _GnCreateGroupDialog
{
  AdwDialog parent_instance;

  /* Dependencies (strong refs) */
  GnMarmotService     *service;
  GnMlsEventRouter   *router;
  GnostrPluginContext *plugin_context;   /* borrowed — host lifetime */

  /* Widgets */
  AdwEntryRow     *name_entry;
  AdwEntryRow     *description_entry;
  AdwEntryRow     *member_entry;
  GtkButton       *add_member_button;
  GtkListBox      *member_list;
  GtkButton       *create_button;
  GtkSpinner      *spinner;
  GtkLabel        *status_label;
  AdwPreferencesGroup *members_group;

  /* State */
  GPtrArray       *member_pubkeys;   /* (element-type utf8) */
  gboolean         creating;
};

enum
{
  SIGNAL_GROUP_CREATED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE(GnCreateGroupDialog, gn_create_group_dialog, ADW_TYPE_DIALOG)

/* ── Forward declarations ────────────────────────────────────────── */

static void start_create_group(GnCreateGroupDialog *self);

/* ── Helpers ─────────────────────────────────────────────────────── */

static gboolean
is_valid_hex_pubkey(const gchar *str)
{
  if (str == NULL) return FALSE;
  if (strlen(str) != 64) return FALSE;
  for (gsize i = 0; i < 64; i++)
    {
      char c = str[i];
      if (!g_ascii_isxdigit(c)) return FALSE;
    }
  return TRUE;
}

static void
update_create_button_sensitivity(GnCreateGroupDialog *self)
{
  if (self->creating)
    {
      gtk_widget_set_sensitive(GTK_WIDGET(self->create_button), FALSE);
      return;
    }

  const gchar *name = gtk_editable_get_text(GTK_EDITABLE(self->name_entry));
  gboolean has_name = (name != NULL && *name != '\0');

  /* Need at least a name to create (can create solo group) */
  gtk_widget_set_sensitive(GTK_WIDGET(self->create_button), has_name);
}

static void
set_status(GnCreateGroupDialog *self, const gchar *text, gboolean busy)
{
  gtk_label_set_text(self->status_label, text ? text : "");
  gtk_widget_set_visible(GTK_WIDGET(self->status_label), text != NULL);

  if (busy)
    {
      gtk_spinner_start(self->spinner);
      gtk_widget_set_visible(GTK_WIDGET(self->spinner), TRUE);
    }
  else
    {
      gtk_spinner_stop(self->spinner);
      gtk_widget_set_visible(GTK_WIDGET(self->spinner), FALSE);
    }
}

/* ── Member list management ──────────────────────────────────────── */

static void
on_member_remove_requested(GnMemberRow *row,
                           const gchar *pubkey_hex,
                           gpointer     user_data)
{
  GnCreateGroupDialog *self = GN_CREATE_GROUP_DIALOG(user_data);

  /* Remove from array */
  for (guint i = 0; i < self->member_pubkeys->len; i++)
    {
      const gchar *pk = g_ptr_array_index(self->member_pubkeys, i);
      if (g_strcmp0(pk, pubkey_hex) == 0)
        {
          g_ptr_array_remove_index(self->member_pubkeys, i);
          break;
        }
    }

  /* Remove from list box */
  GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(row));
  if (parent != NULL && GTK_IS_LIST_BOX(self->member_list))
    gtk_list_box_remove(self->member_list, parent);

  update_create_button_sensitivity(self);
}

static void
add_member_to_list(GnCreateGroupDialog *self, const gchar *pubkey_hex)
{
  /* Check for duplicates */
  for (guint i = 0; i < self->member_pubkeys->len; i++)
    {
      if (g_strcmp0(g_ptr_array_index(self->member_pubkeys, i), pubkey_hex) == 0)
        {
          set_status(self, "Member already added", FALSE);
          return;
        }
    }

  /* Check not self */
  const gchar *my_pk = gn_marmot_service_get_user_pubkey_hex(self->service);
  if (my_pk != NULL && g_strcmp0(my_pk, pubkey_hex) == 0)
    {
      set_status(self, "You are added automatically as creator", FALSE);
      return;
    }

  g_ptr_array_add(self->member_pubkeys, g_strdup(pubkey_hex));

  /* Create row widget */
  GnMemberRow *row = gn_member_row_new();
  gn_member_row_set_pubkey(row, pubkey_hex, FALSE, FALSE);
  gn_member_row_set_removable(row, TRUE);
  g_signal_connect(row, "remove-requested",
                   G_CALLBACK(on_member_remove_requested), self);

  gtk_list_box_append(self->member_list, GTK_WIDGET(row));

  set_status(self, NULL, FALSE);
  update_create_button_sensitivity(self);
}

static void
on_add_member_clicked(GtkButton *button, gpointer user_data)
{
  GnCreateGroupDialog *self = GN_CREATE_GROUP_DIALOG(user_data);

  const gchar *text = gtk_editable_get_text(GTK_EDITABLE(self->member_entry));
  if (text == NULL || *text == '\0')
    return;

  /* Trim whitespace */
  g_autofree gchar *trimmed = g_strstrip(g_strdup(text));

  if (!is_valid_hex_pubkey(trimmed))
    {
      set_status(self, "Invalid pubkey — enter 64-character hex", FALSE);
      return;
    }

  add_member_to_list(self, trimmed);
  gtk_editable_set_text(GTK_EDITABLE(self->member_entry), "");
}

static void
on_member_entry_activate(GtkEditable *editable, gpointer user_data)
{
  on_add_member_clicked(NULL, user_data);
}

static void
on_name_changed(GtkEditable *editable, gpointer user_data)
{
  update_create_button_sensitivity(GN_CREATE_GROUP_DIALOG(user_data));
}

/* ── Group creation flow ─────────────────────────────────────────── */

static void
create_group_flow_data_free(CreateGroupFlowData *data)
{
  if (data == NULL) return;
  g_free(data->group_name);
  g_free(data->group_description);
  g_ptr_array_unref(data->member_pubkeys);
  g_ptr_array_unref(data->key_package_jsons);
  g_strfreev(data->admin_pubkey_hexes);
  g_strfreev(data->relay_urls);
  g_free(data);
}

static void
on_welcomes_sent(GnCreateGroupDialog *self,
                 MarmotGobjectGroup  *group)
{
  self->creating = FALSE;
  set_status(self, NULL, FALSE);
  update_create_button_sensitivity(self);

  g_signal_emit(self, signals[SIGNAL_GROUP_CREATED], 0, group);
  adw_dialog_close(ADW_DIALOG(self));
}

static void
on_welcome_send_done(GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  GnMlsEventRouter *router = GN_MLS_EVENT_ROUTER(source);
  g_autoptr(GError) error = NULL;

  gboolean ok = gn_mls_event_router_send_welcome_finish(router, result, &error);
  if (!ok)
    g_warning("CreateGroupDialog: failed to send welcome: %s",
              error ? error->message : "unknown");

  /* Note: user_data is a ref to the dialog; we don't do anything special
   * per-welcome — the final welcome's completion doesn't gate the UI. */
  g_object_unref(user_data);
}

static void
on_group_created(GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  CreateGroupFlowData *flow = user_data;
  GnCreateGroupDialog *self = flow->dialog;
  MarmotGobjectClient *client = MARMOT_GOBJECT_CLIENT(source);

  g_autoptr(GError) error = NULL;
  g_auto(GStrv) welcome_jsons = NULL;
  g_autofree gchar *evolution_json = NULL;

  g_autoptr(MarmotGobjectGroup) group =
    marmot_gobject_client_create_group_finish(
      client, result, &welcome_jsons, &evolution_json, &error);

  if (group == NULL)
    {
      g_warning("CreateGroupDialog: group creation failed: %s",
                error ? error->message : "unknown");
      set_status(self, error ? error->message : "Group creation failed", FALSE);
      self->creating = FALSE;
      update_create_button_sensitivity(self);
      create_group_flow_data_free(flow);
      return;
    }

  g_info("CreateGroupDialog: group created — %s",
         marmot_gobject_group_get_name(group));

  /* Send welcomes to invited members */
  if (welcome_jsons != NULL)
    {
      set_status(self, "Sending invitations…", TRUE);

      for (guint i = 0; welcome_jsons[i] != NULL; i++)
        {
          /* Match welcome to member pubkey by index */
          const gchar *recipient_pk = NULL;
          if (i < flow->member_pubkeys->len)
            recipient_pk = g_ptr_array_index(flow->member_pubkeys, i);

          if (recipient_pk != NULL)
            {
              gn_mls_event_router_send_welcome_async(
                self->router,
                recipient_pk,
                welcome_jsons[i],
                NULL,
                on_welcome_send_done,
                g_object_ref(self));
            }
        }
    }

  /* Emit group-created and close dialog */
  on_welcomes_sent(self, group);
  create_group_flow_data_free(flow);
}

static void
fetch_key_packages_and_create(GnCreateGroupDialog *self,
                              CreateGroupFlowData *flow)
{
  set_status(self, "Fetching key packages…", TRUE);

  /*
   * For each member, query local storage for their latest kind:443 event.
   * In a production implementation, this would also request from relays,
   * but for now we use what's already been synced.
   */
  for (guint i = 0; i < flow->member_pubkeys->len; i++)
    {
      const gchar *pk = g_ptr_array_index(flow->member_pubkeys, i);

      g_autofree gchar *filter = g_strdup_printf(
        "{\"kinds\":[443],\"authors\":[\"%s\"],\"limit\":1}", pk);

      g_autoptr(GError) error = NULL;
      g_autoptr(GPtrArray) events =
        gnostr_plugin_context_query_events(self->plugin_context, filter, &error);

      if (events != NULL && events->len > 0)
        {
          const gchar *kp_json = g_ptr_array_index(events, 0);
          g_ptr_array_add(flow->key_package_jsons, g_strdup(kp_json));
          g_debug("CreateGroupDialog: found key package for %s", pk);
        }
      else
        {
          g_warning("CreateGroupDialog: no key package found for %s — "
                    "member will not be added", pk);
        }
    }

  if (flow->key_package_jsons->len == 0 && flow->member_pubkeys->len > 0)
    {
      set_status(self, "No key packages found for any member. "
                 "Members must publish key packages first.", FALSE);
      self->creating = FALSE;
      update_create_button_sensitivity(self);
      create_group_flow_data_free(flow);
      return;
    }

  /* Build NULL-terminated array of key package JSONs */
  g_ptr_array_add(flow->key_package_jsons, NULL);
  const gchar *const *kp_array =
    (const gchar *const *)flow->key_package_jsons->pdata;

  /* Get relay URLs */
  gsize n_urls = 0;
  flow->relay_urls =
    gnostr_plugin_context_get_relay_urls(self->plugin_context, &n_urls);

  /* Set creator as only admin */
  const gchar *my_pk = gn_marmot_service_get_user_pubkey_hex(self->service);
  flow->admin_pubkey_hexes = g_new0(gchar *, 2);
  flow->admin_pubkey_hexes[0] = g_strdup(my_pk);
  flow->admin_pubkey_hexes[1] = NULL;

  set_status(self, "Creating group…", TRUE);

  MarmotGobjectClient *client =
    gn_marmot_service_get_client(self->service);

  marmot_gobject_client_create_group_async(
    client,
    my_pk,
    kp_array,
    flow->group_name,
    flow->group_description,
    (const gchar *const *)flow->admin_pubkey_hexes,
    (const gchar *const *)flow->relay_urls,
    NULL,
    on_group_created,
    flow);   /* flow ownership transferred to callback */
}

static void
start_create_group(GnCreateGroupDialog *self)
{
  if (self->creating)
    return;

  self->creating = TRUE;
  update_create_button_sensitivity(self);

  const gchar *name = gtk_editable_get_text(GTK_EDITABLE(self->name_entry));
  const gchar *desc = gtk_editable_get_text(GTK_EDITABLE(self->description_entry));

  CreateGroupFlowData *flow = g_new0(CreateGroupFlowData, 1);
  flow->dialog            = self;   /* weak ref — dialog outlives flow */
  flow->group_name        = g_strdup(name);
  flow->group_description = (desc && *desc) ? g_strdup(desc) : NULL;
  flow->member_pubkeys    = g_ptr_array_new_with_free_func(g_free);
  flow->key_package_jsons = g_ptr_array_new_with_free_func(g_free);

  /* Copy member pubkeys */
  for (guint i = 0; i < self->member_pubkeys->len; i++)
    g_ptr_array_add(flow->member_pubkeys,
                    g_strdup(g_ptr_array_index(self->member_pubkeys, i)));

  fetch_key_packages_and_create(self, flow);
}

static void
on_create_clicked(GtkButton *button, gpointer user_data)
{
  start_create_group(GN_CREATE_GROUP_DIALOG(user_data));
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gn_create_group_dialog_dispose(GObject *object)
{
  GnCreateGroupDialog *self = GN_CREATE_GROUP_DIALOG(object);

  g_clear_object(&self->service);
  g_clear_object(&self->router);
  self->plugin_context = NULL;

  g_clear_pointer(&self->member_pubkeys, g_ptr_array_unref);

  G_OBJECT_CLASS(gn_create_group_dialog_parent_class)->dispose(object);
}

static void
gn_create_group_dialog_class_init(GnCreateGroupDialogClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = gn_create_group_dialog_dispose;

  /**
   * GnCreateGroupDialog::group-created:
   * @dialog: The dialog
   * @group: The newly created MarmotGobjectGroup
   */
  signals[SIGNAL_GROUP_CREATED] = g_signal_new(
    "group-created",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0,
    NULL, NULL,
    NULL,
    G_TYPE_NONE, 1,
    MARMOT_GOBJECT_TYPE_GROUP);
}

static void
gn_create_group_dialog_init(GnCreateGroupDialog *self)
{
  self->member_pubkeys = g_ptr_array_new_with_free_func(g_free);
  self->creating       = FALSE;

  adw_dialog_set_title(ADW_DIALOG(self), "New Group");
  adw_dialog_set_content_width(ADW_DIALOG(self), 400);
  adw_dialog_set_content_height(ADW_DIALOG(self), 520);

  /* ── Main content ──────────────────────────────────────────────── */
  GtkWidget *toolbar_view = adw_toolbar_view_new();

  /* Header bar */
  GtkWidget *header = adw_header_bar_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header);

  /* Scrollable content */
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), scroll);

  GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content_box);

  /* ── Group Details section ─────────────────────────────────────── */
  AdwPreferencesGroup *details_group = ADW_PREFERENCES_GROUP(
    adw_preferences_group_new());
  adw_preferences_group_set_title(details_group, "Group Details");
  adw_preferences_group_set_description(details_group,
    "Set a name and optional description for your group.");
  gtk_box_append(GTK_BOX(content_box), GTK_WIDGET(details_group));

  self->name_entry = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->name_entry), "Name");
  g_signal_connect(self->name_entry, "changed",
                   G_CALLBACK(on_name_changed), self);
  adw_preferences_group_add(details_group, GTK_WIDGET(self->name_entry));

  self->description_entry = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->description_entry),
                                "Description");
  adw_preferences_group_add(details_group, GTK_WIDGET(self->description_entry));

  /* ── Members section ───────────────────────────────────────────── */
  self->members_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(self->members_group, "Members");
  adw_preferences_group_set_description(self->members_group,
    "Add members by their Nostr public key (hex). "
    "Members must have published a key package (kind:443).");
  gtk_box_append(GTK_BOX(content_box), GTK_WIDGET(self->members_group));

  /* Add member input row */
  GtkWidget *add_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start(add_box, 12);
  gtk_widget_set_margin_end(add_box, 12);
  gtk_widget_set_margin_top(add_box, 6);
  gtk_widget_set_margin_bottom(add_box, 6);

  self->member_entry = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->member_entry),
                                "Member Pubkey (hex)");
  g_signal_connect(self->member_entry, "entry-activated",
                   G_CALLBACK(on_member_entry_activate), self);
  adw_preferences_group_add(self->members_group, GTK_WIDGET(self->member_entry));

  /* Add button as suffix on the entry */
  self->add_member_button = GTK_BUTTON(gtk_button_new_from_icon_name("list-add-symbolic"));
  gtk_widget_add_css_class(GTK_WIDGET(self->add_member_button), "flat");
  gtk_widget_set_valign(GTK_WIDGET(self->add_member_button), GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->add_member_button), "Add member");
  g_signal_connect(self->add_member_button, "clicked",
                   G_CALLBACK(on_add_member_clicked), self);
  adw_entry_row_add_suffix(self->member_entry, GTK_WIDGET(self->add_member_button));

  /* Member list */
  self->member_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->member_list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class(GTK_WIDGET(self->member_list), "boxed-list");
  adw_preferences_group_add(self->members_group, GTK_WIDGET(self->member_list));

  /* ── Status / action section ───────────────────────────────────── */
  GtkWidget *action_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(action_box, 24);
  gtk_widget_set_margin_end(action_box, 24);
  gtk_widget_set_margin_top(action_box, 12);
  gtk_widget_set_margin_bottom(action_box, 24);
  gtk_box_append(GTK_BOX(content_box), action_box);

  /* Status row (spinner + label) */
  GtkWidget *status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(status_row, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(action_box), status_row);

  self->spinner = GTK_SPINNER(gtk_spinner_new());
  gtk_widget_set_visible(GTK_WIDGET(self->spinner), FALSE);
  gtk_box_append(GTK_BOX(status_row), GTK_WIDGET(self->spinner));

  self->status_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(self->status_label), "dim-label");
  gtk_widget_set_visible(GTK_WIDGET(self->status_label), FALSE);
  gtk_box_append(GTK_BOX(status_row), GTK_WIDGET(self->status_label));

  /* Create button */
  self->create_button = GTK_BUTTON(gtk_button_new_with_label("Create Group"));
  gtk_widget_add_css_class(GTK_WIDGET(self->create_button), "suggested-action");
  gtk_widget_add_css_class(GTK_WIDGET(self->create_button), "pill");
  gtk_widget_set_halign(GTK_WIDGET(self->create_button), GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive(GTK_WIDGET(self->create_button), FALSE);
  g_signal_connect(self->create_button, "clicked",
                   G_CALLBACK(on_create_clicked), self);
  gtk_box_append(GTK_BOX(action_box), GTK_WIDGET(self->create_button));

  adw_dialog_set_child(ADW_DIALOG(self), toolbar_view);
}

/* ── Public API ──────────────────────────────────────────────────── */

GnCreateGroupDialog *
gn_create_group_dialog_new(GnMarmotService     *service,
                            GnMlsEventRouter   *router,
                            GnostrPluginContext *plugin_context)
{
  g_return_val_if_fail(GN_IS_MARMOT_SERVICE(service), NULL);
  g_return_val_if_fail(GN_IS_MLS_EVENT_ROUTER(router), NULL);
  g_return_val_if_fail(plugin_context != NULL, NULL);

  GnCreateGroupDialog *self =
    g_object_new(GN_TYPE_CREATE_GROUP_DIALOG, NULL);
  self->service        = g_object_ref(service);
  self->router         = g_object_ref(router);
  self->plugin_context = plugin_context;

  return self;
}
