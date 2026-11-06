/* SPDX-License-Identifier: MIT */
#include "grotto-app-detail.h"
#include "grotto-panel-private.h"
#include "grotto-panel-labels.h"

typedef struct _GrottoAppDetail GrottoAppDetail;
typedef struct _GrottoAppDetailClass { GrottoPanelClass parent_class; } GrottoAppDetailClass;
typedef struct _PermissionEditor PermissionEditor;
struct _GrottoAppDetail {
  GrottoPanel parent_instance;
  GWeakRef parent;
  char *principal;
  AdwNavigationView *navigation;
  AdwNavigationPage *permissions_page, *root_page;
  AdwEntryRow *name;
  GtkImage *app_icon;
  AdwActionRow *app_identity, *verified, *fingerprint, *since, *permissions_link, *handoff;
  AdwBanner *feedback, *permission_feedback;
  GtkCheckButton *tiers[3];
  GtkWidget *access, *remove, *unblock;
  GtkListBox *permission_rows, *effective_rows, *clear_rows;
  AdwPreferencesGroup *clear_group;
  GListModel *apps, *permissions;
  GrottoPanelsItem *snapshot;
  PermissionEditor *editor;
  gboolean updating, busy, loading, clear_failed, unavailable;
  guint query_serial;
};
G_DEFINE_TYPE(GrottoAppDetail, grotto_app_detail, GROTTO_TYPE_PANEL)
#define DETAIL(o) ((GrottoAppDetail *)(o))
static void render(GrottoAppDetail *self);
static void load_permissions(GrottoAppDetail *self);
static void editor_refresh(GrottoAppDetail *self);
static gboolean needs_activation(const GrottoPanelsMetadata *m) {
  return !strcmp(m->decision, "policy-review") || !strcmp(m->decision, "review");
}
static GtkWidget *row(AdwPreferencesGroup *group, const char *title, const char *subtitle) {
  GtkWidget *widget = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(widget), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widget), title);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(widget), subtitle ? subtitle : "");
  adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(widget), 0);
  if (group) adw_preferences_group_add(group, widget);
  return widget;
}
static AdwPreferencesGroup *group(AdwPreferencesPage *page, const char *title) {
  AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  if (title) adw_preferences_group_set_title(g, title);
  adw_preferences_page_add(page, g); return g;
}
static void message(GrottoAppDetail *self, const char *text) {
  adw_banner_set_title(self->feedback, text); adw_banner_set_revealed(self->feedback, TRUE);
  if (self->permission_feedback) { adw_banner_set_title(self->permission_feedback, text); adw_banner_set_revealed(self->permission_feedback, TRUE); }
}
static void forward_toast(gpointer data, const char *value) {
  g_autoptr(GrottoPanel) parent = g_weak_ref_get(&DETAIL(data)->parent);
  if (parent) grotto_panel_host_invoke(parent, GROTTO_PANEL_HOST_TOAST, value, NULL);
}
static void forward_open(gpointer data, const char *value) {
  g_autoptr(GrottoPanel) parent = g_weak_ref_get(&DETAIL(data)->parent);
  if (parent) grotto_panel_host_invoke(parent, GROTTO_PANEL_HOST_OPEN_SIGNER, value, NULL);
}
static gboolean forward_can(gpointer data, GrottoPanelCommand command) {
  g_autoptr(GrottoPanel) parent = g_weak_ref_get(&DETAIL(data)->parent);
  return parent && grotto_panel_can_command(parent, command);
}
static void forward_command(gpointer data, GrottoPanelCommand command, const GrottoPanelCommandArgs *args,
    GCancellable *cancel, GAsyncReadyCallback callback, gpointer user_data) {
  g_autoptr(GrottoPanel) parent = g_weak_ref_get(&DETAIL(data)->parent);
  if (parent && !g_cancellable_is_cancelled(cancel)) grotto_panel_forward_command_async(parent, command, args, cancel, callback, user_data);
  else {
    GTask *task = g_task_new(NULL, cancel, callback, user_data);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Panel detached"); g_object_unref(task);
  }
}
static void change_done(GObject *source, GAsyncResult *result, gpointer data) {
  GrottoAppDetail *self = data; g_autoptr(GError) error = NULL;
  gboolean ok = grotto_panel_command_finish(GROTTO_PANEL(source), result, &error);
  self->busy = FALSE; render(self);
  if (ok) forward_toast(self, _("Saved. Existing permissions are retained when access is lowered."));
  else message(self, _("Not saved. Review the refreshed app settings and try again."));
  if (self->permissions_page) load_permissions(self);
  g_object_unref(self);
}
static void change(GrottoAppDetail *self, GrottoPanelCommand command, GrottoPanelCommandArgs *args) {
  if (self->busy || !self->snapshot) return;
  args->principal = self->principal;
  args->expected_generation = grotto_panels_item_get_metadata(self->snapshot)->owner_generation;
  self->busy = TRUE;
  grotto_panel_command_async(GROTTO_PANEL(self), command, args, change_done, g_object_ref(self)); render(self);
}
static void name_apply(AdwEntryRow *entry, gpointer data) {
  GrottoPanelCommandArgs args = {.label = gtk_editable_get_text(GTK_EDITABLE(entry))};
  change(data, GROTTO_PANEL_SET_CLIENT_LABEL, &args);
}
static void upgrade_done(GObject *source, GAsyncResult *result, gpointer data) {
  GrottoAppDetail *self = data;
  if (!strcmp(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result), "upgrade")) {
    GrottoPanelCommandArgs args = {.trust = "full"}; change(self, GROTTO_PANEL_SET_CLIENT_TRUST, &args);
  } else render(self);
  g_object_unref(self);
}
static void tier_changed(GtkCheckButton *button, gpointer data) {
  GrottoAppDetail *self = data;
  if (self->updating || self->busy || !self->snapshot || !gtk_check_button_get_active(button)) return;
  guint index = 0; while (index < 3 && self->tiers[index] != button) index++;
  if (index == 3) return;
  const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(self->snapshot);
  if (!index && !strcmp(m->access_policy, "Previous access policy")) {
    AdwDialog *dialog = adw_alert_dialog_new(_("Upgrade Access Policy?"), _("Use the current routine preset. Custom event types need their own permission. Existing saved permissions and blocks remain."));
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", _("Cancel"), "upgrade", _("Upgrade Access Policy"), NULL);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel"); adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(self), grotto_panel_get_cancellable(GROTTO_PANEL(self)), upgrade_done, g_object_ref(self)); return;
  }
  const char *tiers[] = {"full", "medium", "low"}; GrottoPanelCommandArgs args = {.trust = tiers[index]};
  change(self, GROTTO_PANEL_SET_CLIENT_TRUST, &args);
}
static void remove_done(GObject *source, GAsyncResult *result, gpointer data) {
  GrottoAppDetail *self = data;
  if (!strcmp(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result), "remove")) {
    GrottoPanelCommandArgs args = {0}; change(self, GROTTO_PANEL_REVOKE_CLIENT, &args);
  }
  g_object_unref(self);
}
static void remove_app(GtkButton *button, gpointer data) {
  (void)button; GrottoAppDetail *self = data;
  AdwDialog *dialog = adw_alert_dialog_new(_("Remove App?"), _("Revoke this app's pairing and permissions and cancel its pending requests. This does not block future requests."));
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", _("Cancel"), "remove", _("Remove App"), NULL);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel"); adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "remove", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(self), grotto_panel_get_cancellable(GROTTO_PANEL(self)), remove_done, g_object_ref(self));
}
static void unblock_done(GObject *source, GAsyncResult *result, gpointer data) {
  GrottoAppDetail *self = data;
  if (!strcmp(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result), "unblock")) {
    GrottoPanelCommandArgs args = {.trust = "low"}; change(self, GROTTO_PANEL_SET_CLIENT_TRUST, &args);
  }
  g_object_unref(self);
}
static void unblock_app(GtkButton *button, gpointer data) {
  (void)button; GrottoAppDetail *self = data;
  AdwDialog *dialog = adw_alert_dialog_new(_("Remove App Block?"), _("This app will ask unless permitted. Existing saved permissions still apply."));
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", _("Cancel"), "unblock", _("Remove App Block"), NULL);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel"); adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(self), grotto_panel_get_cancellable(GROTTO_PANEL(self)), unblock_done, g_object_ref(self));
}
static void handoff(AdwActionRow *row_widget, gpointer data) {
  (void)row_widget; GrottoAppDetail *self = data;
  g_autofree char *target = g_strconcat("app:", self->principal, NULL); forward_open(self, target);
}
static void activity(AdwActionRow *row_widget, gpointer data) {
  (void)row_widget; GrottoAppDetail *self = data;
  g_autofree char *target = g_strconcat("activity:", self->principal, NULL); forward_open(self, target);
}
struct _PermissionEditor {
  GrottoAppDetail *page;
  AdwDialog *dialog;
  GrottoPanelsItem *original;
  GListModel *snapshot;
  AdwComboRow *decision, *duration, *identity;
  GPtrArray *identities;
  AdwEntryRow *kind, *relay;
  AdwBanner *feedback;
  GtkButton *save, *remove;
  gboolean blocked, cleanup, cleanup_ready, retry;
  char *saved_identity;
};
static void editor_free(gpointer data) {
  PermissionEditor *e = data;
  if (e->page->editor == e) e->page->editor = NULL;
  g_object_unref(e->page); g_object_unref(e->original); g_clear_object(&e->snapshot);
  g_clear_pointer(&e->identities, g_ptr_array_unref); g_free(e->saved_identity); g_free(e);
}
static void editor_closed(AdwDialog *dialog, gpointer data) {
  (void)dialog; PermissionEditor *e = data; if (e->page->editor == e) e->page->editor = NULL;
}
static void editor_message(PermissionEditor *e, const char *text) {
  adw_banner_set_title(e->feedback, text); adw_banner_set_revealed(e->feedback, TRUE);
}
static gboolean same_permission(const GrottoPanelsMetadata *a, const GrottoPanelsMetadata *b) {
  return !g_strcmp0(a->public_key, b->public_key) && !g_strcmp0(a->operation, b->operation) &&
    a->event_kind == b->event_kind && !g_strcmp0(a->relay, b->relay) && !g_strcmp0(a->decision, b->decision) &&
    a->expires_at == b->expires_at && !g_strcmp0(a->state, b->state) && !g_strcmp0(a->reason, b->reason);
}
static void save_permission(PermissionEditor *e, gboolean remove);
static void permission_saved(GObject *source, GAsyncResult *result, gpointer data) {
  AdwDialog *dialog = data; PermissionEditor *e = g_object_get_data(G_OBJECT(dialog), "editor");
  g_autoptr(GError) error = NULL;
  gboolean ok = grotto_panel_command_finish(GROTTO_PANEL(source), result, &error);
  e->page->busy = FALSE; render(e->page);
  if (e->page->editor != e) { g_object_unref(dialog); return; }
  const GrottoPanelsMetadata *original = grotto_panels_item_get_metadata(e->original);
  if (ok && !e->cleanup && e->saved_identity && strcmp(e->saved_identity, original->public_key)) {
    e->cleanup = TRUE; e->cleanup_ready = TRUE; editor_message(e, _("Scoped permission saved. Refreshing before removing the legacy entry."));
    load_permissions(e->page);
  } else if (ok) {
    forward_toast(e->page, _("Permission saved")); load_permissions(e->page); adw_dialog_close(dialog);
  } else {
    e->blocked = TRUE; e->retry = TRUE;
    editor_message(e, e->cleanup ? _("Scoped permission saved, but the legacy entry was not removed. Refresh and retry removal.") :
      _("Permission not saved. Refreshing now; review your edit before retrying."));
    load_permissions(e->page);
  }
  g_object_unref(dialog);
}
static void save_permission(PermissionEditor *e, gboolean remove) {
  GrottoAppDetail *self = e->page;
  if (self->busy || self->loading || e->blocked || !e->snapshot) return;
  const GrottoPanelsMetadata *original = grotto_panels_item_get_metadata(e->original);
  const char *identity = original->public_key;
  if (e->identity && !remove) {
    guint selected = adw_combo_row_get_selected(e->identity);
    if (!selected || selected > e->identities->len) { editor_message(e, _("Choose an identity before allowing this permission.")); return; }
    identity = e->identities->pdata[selected - 1];
  }
  gint kind = original->event_kind;
  if (e->kind) {
    const char *value = gtk_editable_get_text(GTK_EDITABLE(e->kind)); char *end = NULL;
    gint64 parsed = g_ascii_strtoll(value, &end, 10);
    if (!*value) kind = -1;
    else if (end == value || *end || parsed < 0 || parsed > G_MAXUINT16) { editor_message(e, _("Use an event type number from 0 to 65535.")); return; }
    else kind = parsed;
  }
  const char *decision = adw_combo_row_get_selected(e->decision) ? "deny" : "allow";
  if (!remove && !strcmp(original->operation, "event") && !strcmp(decision, "allow") && kind < 0) {
    editor_message(e, _("Choose an exact event type before allowing this permission.")); return;
  }
  guint duration = adw_combo_row_get_selected(e->duration);
  const guint seconds[] = {0, 600, 3600, 86400, 0};
  guint64 expiry = duration && duration < G_N_ELEMENTS(seconds) ? (seconds[duration] ? (guint64)(g_get_real_time() / G_USEC_PER_SEC) + seconds[duration] : 0) : original->expires_at;
  const char *relay = gtk_editable_get_text(GTK_EDITABLE(e->relay));
  g_autoptr(GArray) records = g_array_new(FALSE, FALSE, sizeof(GrottoPanelPermission));
  guint64 revision = original->revision, generation = original->owner_generation;
  for (guint i = 0; i < g_list_model_get_n_items(e->snapshot); i++) {
    g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(e->snapshot, i);
    const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(item);
    revision = m->revision; generation = m->owner_generation;
    if (strcmp(identity, m->public_key) || same_permission(m, original)) continue;
    if (!remove && !strcmp(m->operation, original->operation) && m->event_kind == kind && !strcmp(m->relay, relay)) {
      editor_message(e, _("This exact scope already has a permission. Edit that entry instead.")); return;
    }
    GrottoPanelPermission p = {.operation = m->operation, .decision = m->decision, .event_kind = m->event_kind,
      .expires_at = m->expires_at, .relay = *m->relay ? m->relay : NULL}; g_array_append_val(records, p);
  }
  if (!remove) {
    GrottoPanelPermission p = {.operation = original->operation, .decision = decision, .event_kind = kind,
      .expires_at = expiry, .relay = *relay ? relay : NULL}; g_array_append_val(records, p);
  }
  GrottoPanelCommandArgs args = {.principal = self->principal, .public_key = identity,
    .permissions = (GrottoPanelPermission *)records->data, .n_permissions = records->len,
    .expected_revision = revision, .expected_generation = generation};
  g_free(e->saved_identity); e->saved_identity = g_strdup(identity);
  self->busy = TRUE; gtk_widget_set_sensitive(GTK_WIDGET(e->save), FALSE); gtk_widget_set_sensitive(GTK_WIDGET(e->remove), FALSE);
  grotto_panel_command_async(GROTTO_PANEL(self), GROTTO_PANEL_SET_PERMISSIONS, &args, permission_saved, g_object_ref(e->dialog));
}
static void editor_save(GtkButton *button, gpointer data) { (void)button; PermissionEditor *e = data; save_permission(e, e->cleanup); }
static void removal_confirmed(GObject *source, GAsyncResult *result, gpointer data) {
  AdwDialog *dialog = data; PermissionEditor *e = g_object_get_data(G_OBJECT(dialog), "editor");
  if (e->page->editor == e && !strcmp(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result), "remove")) save_permission(e, TRUE);
  g_object_unref(dialog);
}
static void editor_remove(GtkButton *button, gpointer data) {
  (void)button; PermissionEditor *e = data;
  gboolean inactive = needs_activation(grotto_panels_item_get_metadata(e->original));
  AdwDialog *dialog = adw_alert_dialog_new(inactive ? _("Discard Saved Permission?") : _("Remove Permission?"),
    inactive ? _("Discard this inactive entry without activating it. The app's trust level and other permissions remain unchanged.") :
    _("Removing a saved block can allow access under the app's preset or other permissions."));
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", _("Cancel"), "remove", inactive ? _("Discard") : _("Remove Permission"), NULL);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel"); adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "remove", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(e->page), grotto_panel_get_cancellable(GROTTO_PANEL(e->page)), removal_confirmed, g_object_ref(e->dialog));
}
static void editor_cancel(GtkButton *button, gpointer data) { (void)button; adw_dialog_close(data); }
static AdwComboRow *combo(AdwPreferencesGroup *g, const char *title, const char *const *values) {
  AdwComboRow *c = ADW_COMBO_ROW(adw_combo_row_new()); adw_preferences_row_set_title(ADW_PREFERENCES_ROW(c), title);
  GtkStringList *strings = gtk_string_list_new(values); adw_combo_row_set_model(c, G_LIST_MODEL(strings)); g_object_unref(strings);
  adw_preferences_group_add(g, GTK_WIDGET(c)); return c;
}
static void permission_kind_selected(AdwComboRow *combo_row, GParamSpec *spec, gpointer data) {
  (void)spec; PermissionEditor *e = data; guint selected = adw_combo_row_get_selected(combo_row);
  if (selected && selected <= G_N_ELEMENTS(kinds)) {
    g_autofree char *number = g_strdup_printf("%u", kinds[selected - 1].kind);
    gtk_editable_set_text(GTK_EDITABLE(e->kind), number);
  }
}
static void edit_permission(AdwActionRow *row_widget, gpointer data) {
  GrottoAppDetail *self = data;
  if (self->editor || self->busy || self->loading || !grotto_panel_can_command(GROTTO_PANEL(self), GROTTO_PANEL_SET_PERMISSIONS)) return;
  PermissionEditor *e = g_new0(PermissionEditor, 1); e->page = g_object_ref(self);
  e->original = g_object_ref(g_object_get_data(G_OBJECT(row_widget), "permission")); e->snapshot = g_object_ref(self->permissions);
  const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(e->original);
  gboolean inactive = needs_activation(m);
  e->dialog = adw_dialog_new(); adw_dialog_set_title(e->dialog, inactive ? _("Review Inactive Permission") : _("Edit Permission")); adw_dialog_set_content_width(e->dialog, 420); adw_dialog_set_content_height(e->dialog, 560);
  GtkWidget *toolbar = adw_toolbar_view_new(); adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  e->feedback = ADW_BANNER(adw_banner_new("")); adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(e->feedback));
  AdwPreferencesPage *prefs = ADW_PREFERENCES_PAGE(adw_preferences_page_new()); AdwPreferencesGroup *scope = group(prefs, _("Scope"));
  row(scope, _("App"), self->principal); row(scope, _("Operation"), operation_name(m->operation));
  if (!strcmp(m->public_key, "*") && !strcmp(m->state, "inactive")) {
    e->identities = g_ptr_array_new_with_free_func(g_free); GtkStringList *names = gtk_string_list_new(NULL); gtk_string_list_append(names, _("Choose an Identity"));
    GListModel *identities = grotto_panels_service_get_model(grotto_panel_get_service(GROTTO_PANEL(self)), GROTTO_PANELS_IDENTITIES);
    for (guint i = 0; identities && i < g_list_model_get_n_items(identities); i++) {
      g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(identities, i); const GrottoPanelsMetadata *id = grotto_panels_item_get_metadata(item);
      if (id->watch_only) continue;
      g_ptr_array_add(e->identities, g_strdup(id->public_key));
      g_autofree char *name = g_strdup_printf("%s · %s", *id->label ? id->label : _("Identity"), id->public_key); gtk_string_list_append(names, name);
    }
    e->identity = ADW_COMBO_ROW(adw_combo_row_new()); adw_preferences_row_set_title(ADW_PREFERENCES_ROW(e->identity), _("Identity"));
    adw_combo_row_set_model(e->identity, G_LIST_MODEL(names)); g_object_unref(names); adw_preferences_group_add(scope, GTK_WIDGET(e->identity));
  } else row(scope, _("Identity"), m->public_key);
  if (!strcmp(m->operation, "event")) {
    e->kind = ADW_ENTRY_ROW(adw_entry_row_new()); adw_preferences_row_set_title(ADW_PREFERENCES_ROW(e->kind), _("Exact Event Type (Number)"));
    g_autofree char *kind = m->event_kind >= 0 ? g_strdup_printf("%d", m->event_kind) : g_strdup("");
    gtk_editable_set_text(GTK_EDITABLE(e->kind), kind); adw_preferences_group_add(scope, GTK_WIDGET(e->kind));
    AdwComboRow *catalog = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(catalog), _("Choose an Event Type"));
    adw_combo_row_set_enable_search(catalog, TRUE);
    GtkStringList *names = gtk_string_list_new(NULL); gtk_string_list_append(names, _("Custom Number"));
    for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) {
      g_autofree char *caption = g_strdup_printf("%s (%u)", _(kinds[i].caption), kinds[i].kind);
      gtk_string_list_append(names, caption);
    }
    adw_combo_row_set_model(catalog, G_LIST_MODEL(names)); g_object_unref(names);
    adw_preferences_group_add(scope, GTK_WIDGET(catalog));
    g_signal_connect(catalog, "notify::selected", G_CALLBACK(permission_kind_selected), e);
  }
  e->relay = ADW_ENTRY_ROW(adw_entry_row_new()); adw_preferences_row_set_title(ADW_PREFERENCES_ROW(e->relay), _("Relay Scope"));
  gtk_editable_set_text(GTK_EDITABLE(e->relay), m->relay); adw_preferences_group_add(scope, GTK_WIDGET(e->relay));
  AdwPreferencesGroup *choice = group(prefs, _("Permission")); const char *decisions[] = {_("Allow"), _("Deny"), NULL};
  e->decision = combo(choice, _("Decision"), decisions); adw_combo_row_set_selected(e->decision, !strcmp(m->decision, "deny"));
  gtk_widget_set_visible(GTK_WIDGET(e->decision), !inactive);
  const char *durations[] = {_("Keep Current Expiry"), _("10 minutes"), _("1 hour"), _("24 hours"), _("Until Revoked"), NULL};
  e->duration = combo(choice, _("Duration"), durations);
  /* In particular, dormant policy-review expiry 0 stays until revoked. */
  adw_combo_row_set_selected(e->duration, 0);
  AdwPreferencesGroup *destructive = group(prefs, NULL);
  e->remove = GTK_BUTTON(gtk_button_new_with_label(inactive ? _("Discard Saved Permission…") : _("Remove Permission…"))); gtk_widget_add_css_class(GTK_WIDGET(e->remove), "destructive-action");
  gtk_widget_set_halign(GTK_WIDGET(e->remove), GTK_ALIGN_CENTER); adw_preferences_group_add(destructive, GTK_WIDGET(e->remove));
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12); gtk_box_set_homogeneous(GTK_BOX(bar), TRUE);
  gtk_widget_set_margin_start(bar, 12); gtk_widget_set_margin_end(bar, 12); gtk_widget_set_margin_top(bar, 12); gtk_widget_set_margin_bottom(bar, 12);
  GtkWidget *cancel = gtk_button_new_with_label(_("Cancel"));
  e->save = GTK_BUTTON(gtk_button_new_with_label(inactive ? _("Activate Saved Permission") : _("Save Permission")));
  gtk_label_set_wrap(GTK_LABEL(gtk_button_get_child(e->save)), TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(e->save), "suggested-action"); gtk_box_append(GTK_BOX(bar), cancel); gtk_box_append(GTK_BOX(bar), GTK_WIDGET(e->save));
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar), bar); adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(prefs));
  adw_dialog_set_child(e->dialog, toolbar); self->editor = e; g_object_set_data_full(G_OBJECT(e->dialog), "editor", e, editor_free);
  g_signal_connect(e->dialog, "closed", G_CALLBACK(editor_closed), e); g_signal_connect(cancel, "clicked", G_CALLBACK(editor_cancel), e->dialog);
  g_signal_connect(e->save, "clicked", G_CALLBACK(editor_save), e); g_signal_connect(e->remove, "clicked", G_CALLBACK(editor_remove), e);
  adw_dialog_present(e->dialog, GTK_WIDGET(self));
}
static void editor_refresh(GrottoAppDetail *self) {
  PermissionEditor *e = self->editor; if (!e || !self->permissions) return;
  const GrottoPanelsMetadata *original = grotto_panels_item_get_metadata(e->original); gboolean found = FALSE;
  for (guint i = 0; i < g_list_model_get_n_items(self->permissions); i++) {
    g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(self->permissions, i); const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(item);
    if (m->owner_generation == original->owner_generation && same_permission(m, original)) found = TRUE;
  }
  e->blocked = !found;
  if (!found) editor_message(e, _("The original permission or signer changed. Close this editor and review the updated entries."));
  else {
    g_set_object(&e->snapshot, self->permissions);
    if (e->cleanup) editor_message(e, _("Scoped permission saved. Remove the legacy entry to finish."));
    gtk_button_set_label(e->save, e->cleanup ? (e->retry ? _("Retry Removing Legacy Entry") : _("Remove Legacy Entry")) :
      (e->retry ? _("Retry Permission") : needs_activation(original) ? _("Activate Saved Permission") : _("Save Permission")));
  }
  gboolean writable = found && !self->busy && !self->loading && grotto_panel_can_command(GROTTO_PANEL(self), GROTTO_PANEL_SET_PERMISSIONS);
  gtk_widget_set_sensitive(GTK_WIDGET(e->save), writable); gtk_widget_set_sensitive(GTK_WIDGET(e->remove), writable);
  if (writable && e->cleanup_ready) { e->cleanup_ready = FALSE; save_permission(e, TRUE); }
}
typedef struct { GrottoAppDetail *self; GrottoPanelsItem *scope; } ClearScope;
static void clear_scope_free(ClearScope *clear) {
  g_object_unref(clear->self); g_object_unref(clear->scope); g_free(clear);
}
static void cleared(GObject *source, GAsyncResult *result, gpointer data) {
  ClearScope *clear = data; GrottoAppDetail *self = clear->self;
  g_autoptr(GError) error = NULL;
  self->clear_failed = !grotto_panel_command_finish(GROTTO_PANEL(source), result, &error);
  self->busy = FALSE;
  if (!self->clear_failed) forward_toast(self, _("Remembered permissions cleared. Trust level unchanged."));
  load_permissions(self); render(self); clear_scope_free(clear);
}
static void clear_confirmed(GObject *source, GAsyncResult *result, gpointer data) {
  ClearScope *clear = data; GrottoAppDetail *self = clear->self;
  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  if (strcmp(response, "clear")) { clear_scope_free(clear); return; }
  if (self->busy || self->loading) {
    message(self, _("Permissions are refreshing. Review the entries and try again."));
    clear_scope_free(clear); return;
  }
  const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(clear->scope);
  GrottoPanelCommandArgs args = {.principal = self->principal, .public_key = m->public_key,
    .expected_revision = m->revision, .expected_generation = m->owner_generation};
  self->busy = TRUE; render(self);
  grotto_panel_command_async(GROTTO_PANEL(self), GROTTO_PANEL_SET_PERMISSIONS, &args, cleared, clear);
}
static void clear_scope(AdwActionRow *widget, gpointer data) {
  GrottoAppDetail *self = data;
  if (self->busy || self->loading || !self->permissions ||
      !grotto_panel_can_command(GROTTO_PANEL(self), GROTTO_PANEL_SET_PERMISSIONS)) return;
  ClearScope *clear = g_new0(ClearScope, 1); clear->self = g_object_ref(self);
  clear->scope = g_object_ref(g_object_get_data(G_OBJECT(widget), "clear-scope"));
  const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(clear->scope);
  self->clear_failed = FALSE;
  g_autofree char *body = g_strdup_printf(_("Remove every remembered allow, deny and inactive entry for this app and identity %s? Other identities and the trust level remain unchanged."), m->public_key);
  AdwDialog *dialog = adw_alert_dialog_new(_("Clear Remembered Permissions?"), body);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", _("Cancel"), "clear", _("Clear Permissions"), NULL);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "clear", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(self), grotto_panel_get_cancellable(GROTTO_PANEL(self)), clear_confirmed, clear);
}
static void render_effective(GrottoAppDetail *self, const GrottoPanelsMetadata *m) {
  if (!self->effective_rows) return;
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->effective_rows)))) gtk_list_box_remove(self->effective_rows, child);
  if (!m) return;
  if (m->blocked) {
    gtk_list_box_append(self->effective_rows, row(NULL, _("App Blocked"), _("Saved permissions are retained, but inactive while the app is blocked.")));
    return;
  }
  const char *tier = !strcmp(m->trust, "full") ? _("Full") : !strcmp(m->trust, "medium") ? _("Medium") : _("Low");
  g_autofree char *from = g_strdup_printf(_("From %s trust"), tier);
  for (guint i = 0; m->effective_permissions && m->effective_permissions[i]; i++) {
    GtkWidget *widget = row(NULL, m->effective_permissions[i], from);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(widget), FALSE);
    gtk_list_box_append(self->effective_rows, widget);
  }
}
typedef struct { GrottoAppDetail *self; guint serial; } PermissionRead;
static void permissions_read(GObject *source, GAsyncResult *result, gpointer data) {
  PermissionRead *read = data; GrottoAppDetail *self = read->self; g_autoptr(GError) error = NULL;
  g_autoptr(GListModel) model = grotto_panels_service_query_finish(GROTTO_PANELS_SERVICE(source), result, &error);
  if (read->serial != self->query_serial) { g_object_unref(self); g_free(read); return; }
  self->loading = FALSE; g_set_object(&self->permissions, model);
  gtk_widget_set_sensitive(GTK_WIDGET(self->permission_rows), model != NULL);
  GtkWidget *child; while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->permission_rows)))) gtk_list_box_remove(self->permission_rows, child);
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->clear_rows)))) gtk_list_box_remove(self->clear_rows, child);
  g_autoptr(GHashTable) scopes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; model && i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(model, i); const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(item);
    const char *decision = !strcmp(m->decision, "allow") ? _("Allow") : !strcmp(m->decision, "deny") ? _("Deny") : _("Needs Review");
    g_autofree char *title = m->event_kind >= 0 ? g_strdup_printf(_("%s · %s"), decision, kind_name(m->event_kind)) :
      g_strdup_printf(_("%s · %s"), decision, operation_name(m->operation));
    g_autoptr(GDateTime) date = m->expires_at ? g_date_time_new_from_unix_local(m->expires_at) : NULL;
    g_autofree char *when = date ? g_date_time_format(date, "%F %R") : g_strdup(_("Until Revoked"));
    gboolean policy_review = !strcmp(m->decision, "policy-review") && !strcmp(m->state, "inactive");
    g_autofree char *status = policy_review ? g_strdup(_("Inactive — pending policy review")) : g_strdup_printf("%s · %s", m->state, m->reason);
    g_autofree char *subtitle = g_strdup_printf("%s · %s\n%s%s%s", status, when, m->public_key, *m->relay ? " · " : "", m->relay);
    if (g_hash_table_add(scopes, g_strdup(m->public_key))) {
      GtkWidget *clear = row(NULL, _("Clear Remembered Permissions…"), m->public_key);
      gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(clear), TRUE);
      g_object_set_data_full(G_OBJECT(clear), "clear-scope", g_object_ref(item), g_object_unref);
      g_signal_connect(clear, "activated", G_CALLBACK(clear_scope), self);
      gtk_list_box_append(self->clear_rows, clear);
    }
    GtkWidget *widget = row(NULL, title, subtitle);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(widget), grotto_panel_can_command(GROTTO_PANEL(self), GROTTO_PANEL_SET_PERMISSIONS));
    g_object_set_data_full(G_OBJECT(widget), "permission", g_object_ref(item), g_object_unref);
    g_signal_connect(widget, "activated", G_CALLBACK(edit_permission), self); gtk_list_box_append(self->permission_rows, widget);
  }
  gtk_widget_set_visible(GTK_WIDGET(self->clear_group), g_hash_table_size(scopes) > 0);
  if (model && !g_list_model_get_n_items(model))
    gtk_list_box_append(self->permission_rows, row(NULL, _("No Remembered Permissions"), _("Access from the trust level is shown separately.")));
  if (error) message(self, _("Permissions could not be loaded. Refresh to try again."));
  else {
    if (self->clear_failed) message(self, _("Not cleared. Permissions or the signer changed. Review the refreshed entries and confirm again."));
    else adw_banner_set_revealed(self->permission_feedback, FALSE);
    editor_refresh(self);
  }
  render(self);
  g_object_unref(self); g_free(read);
}
static void load_permissions(GrottoAppDetail *self) {
  if (!self->permission_rows || !grotto_panel_get_service(GROTTO_PANEL(self))) return;
  self->loading = TRUE; self->query_serial++;
  gtk_widget_set_sensitive(GTK_WIDGET(self->permission_rows), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->clear_rows), FALSE);
  if (self->editor) { self->editor->blocked = TRUE; gtk_widget_set_sensitive(GTK_WIDGET(self->editor->save), FALSE); gtk_widget_set_sensitive(GTK_WIDGET(self->editor->remove), FALSE); }
  PermissionRead *read = g_new0(PermissionRead, 1); *read = (PermissionRead){g_object_ref(self), self->query_serial};
  grotto_panels_service_query_async(grotto_panel_get_service(GROTTO_PANEL(self)), GROTTO_PANELS_QUERY_PERMISSIONS,
    self->principal, 0, grotto_panel_get_cancellable(GROTTO_PANEL(self)), permissions_read, read);
}
static void refresh_clicked(AdwBanner *banner, gpointer data) { (void)banner; load_permissions(data); }
static void open_permissions(AdwActionRow *widget, gpointer data) {
  (void)widget; GrottoAppDetail *self = data;
  if (!self->permissions_page) {
    GtkWidget *toolbar = adw_toolbar_view_new(), *header = adw_header_bar_new();
    grotto_panel_bind_header_bar(GROTTO_PANEL(self), ADW_HEADER_BAR(header));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    self->permission_feedback = ADW_BANNER(adw_banner_new("")); adw_banner_set_button_label(self->permission_feedback, _("Refresh Permissions"));
    g_signal_connect(self->permission_feedback, "button-clicked", G_CALLBACK(refresh_clicked), self);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(self->permission_feedback));
    AdwPreferencesPage *prefs = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *effective = group(prefs, _("Effective from Trust Level"));
    adw_preferences_group_set_description(effective, _("Explicit denials win. Saved permissions override presets. Ask-by-default event types and AUTH relays need a scoped first grant."));
    self->effective_rows = GTK_LIST_BOX(gtk_list_box_new()); gtk_list_box_set_selection_mode(self->effective_rows, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->effective_rows), "boxed-list"); adw_preferences_group_add(effective, GTK_WIDGET(self->effective_rows));
    AdwPreferencesGroup *g = group(prefs, _("Remembered Permissions"));
    adw_preferences_group_set_description(g, _("Explicit entries are editable. Status and reasons come from the signer. Expiry is not extended by use. Inactive entries cannot authorize signing."));
    self->permission_rows = GTK_LIST_BOX(gtk_list_box_new()); gtk_list_box_set_selection_mode(self->permission_rows, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->permission_rows), "boxed-list"); adw_preferences_group_add(g, GTK_WIDGET(self->permission_rows));
    self->clear_group = group(prefs, _("Clear Remembered Permissions"));
    adw_preferences_group_set_description(self->clear_group, _("Choose an identity. Clearing removes its explicit allows, denies and inactive entries, not its trust level."));
    self->clear_rows = GTK_LIST_BOX(gtk_list_box_new()); gtk_list_box_set_selection_mode(self->clear_rows, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->clear_rows), "boxed-list"); adw_preferences_group_add(self->clear_group, GTK_WIDGET(self->clear_rows));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(prefs)); self->permissions_page = ADW_NAVIGATION_PAGE(adw_navigation_page_new(toolbar, _("Permissions")));
    adw_navigation_view_add(self->navigation, self->permissions_page);
  }
  adw_navigation_view_push(self->navigation, self->permissions_page); load_permissions(self);
}
static void render(GrottoAppDetail *self) {
  g_clear_object(&self->snapshot);
  for (guint i = 0; self->apps && i < g_list_model_get_n_items(self->apps); i++) {
    g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(self->apps, i);
    if (!strcmp(grotto_panels_item_get_metadata(item)->id, self->principal)) { self->snapshot = g_object_ref(item); break; }
  }
  self->updating = TRUE;
  if (self->permission_rows) {
    gboolean edit = grotto_panel_can_command(GROTTO_PANEL(self), GROTTO_PANEL_SET_PERMISSIONS);
    gboolean writable = !self->busy && !self->loading && self->permissions != NULL && edit;
    gtk_widget_set_sensitive(GTK_WIDGET(self->permission_rows), !self->busy && !self->loading && self->permissions != NULL);
    gtk_widget_set_sensitive(GTK_WIDGET(self->clear_rows), writable);
    for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(self->permission_rows)); row; row = gtk_widget_get_next_sibling(row))
      gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), writable && g_object_get_data(G_OBJECT(row), "permission") != NULL);
  }
  const GrottoPanelsMetadata *m = self->snapshot ? grotto_panels_item_get_metadata(self->snapshot) : NULL;
  render_effective(self, m);
  gboolean editable = m && !self->busy && grotto_panel_can_command(GROTTO_PANEL(self), GROTTO_PANEL_SET_CLIENT_TRUST);
  gtk_widget_set_visible(self->unblock, m && m->blocked);
  gtk_widget_set_sensitive(self->unblock, editable);
  gtk_widget_set_sensitive(self->access, editable && !m->blocked); gtk_widget_set_sensitive(self->remove, m && !self->busy && grotto_panel_can_command(GROTTO_PANEL(self), GROTTO_PANEL_REVOKE_CLIENT));
  gboolean name_editable = m && !self->busy && grotto_panel_can_command(GROTTO_PANEL(self), GROTTO_PANEL_SET_CLIENT_LABEL);
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
  gboolean editing_name = focus && gtk_widget_is_ancestor(focus, GTK_WIDGET(self->name));
  if (editing_name && !name_editable) gtk_widget_grab_focus(GTK_WIDGET(self->permissions_link));
  gtk_widget_set_sensitive(GTK_WIDGET(self->name), name_editable);
  gtk_widget_set_visible(GTK_WIDGET(self->handoff), !editable && grotto_panel_host_can(GROTTO_PANEL(self), GROTTO_PANEL_HOST_OPEN_SIGNER));
  if (m) {
    if (self->unavailable) {
      adw_banner_set_revealed(self->feedback, FALSE);
      if (self->permission_feedback) adw_banner_set_revealed(self->permission_feedback, FALSE);
      self->unavailable = FALSE;
    }
    g_autoptr(GIcon) icon = NULL;
    g_autofree char *name = grotto_panels_app_display_info(self->principal, m->label, &icon);
    gtk_image_set_from_gicon(self->app_icon, icon);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->app_identity), name);
    adw_action_row_set_subtitle(self->app_identity, self->principal);
    adw_navigation_page_set_title(ADW_NAVIGATION_PAGE(self), name); adw_navigation_page_set_title(self->root_page, name);
    if (!editing_name) gtk_editable_set_text(GTK_EDITABLE(self->name), m->label);
    adw_action_row_set_subtitle(self->verified, *m->verified_as ? m->verified_as : self->principal);
    adw_action_row_set_subtitle(self->fingerprint, self->principal);
    g_autoptr(GDateTime) date = m->timestamp ? g_date_time_new_from_unix_local(m->timestamp) : NULL;
    g_autofree char *when = date ? g_date_time_format(date, "%F %R") : g_strdup(_("Waiting for Consent")); adw_action_row_set_subtitle(self->since, when);
    guint tier = !strcmp(m->trust, "full") ? 0 : !strcmp(m->trust, "medium") ? 1 : 2;
    /* An inherited Full policy must be upgraded deliberately, never by render. */
    for (guint i = 0; i < 3; i++) gtk_check_button_set_active(self->tiers[i], FALSE);
    if (!m->blocked && strcmp(m->access_policy, "Previous access policy")) gtk_check_button_set_active(self->tiers[tier], TRUE);
    if (!strcmp(m->access_policy, "Previous access policy")) message(self, _("Previous access policy. Choose routine access to review an explicit upgrade."));
  } else { self->unavailable = TRUE; message(self, _("App unavailable or removed")); }
  self->updating = FALSE;
}
static void model_changed(GListModel *model, guint p, guint r, guint a, gpointer data) {
  (void)model; (void)p; (void)r; (void)a; GrottoAppDetail *self = data; render(self); if (self->permission_rows) load_permissions(self);
}
static void attached(GrottoPanel *panel, gpointer data) {
  (void)data; GrottoAppDetail *self = DETAIL(panel); GrottoPanelsService *service = grotto_panel_get_service(panel);
  GListModel *apps = service ? grotto_panels_service_get_model(service, GROTTO_PANELS_APPS) : NULL;
  if (apps != self->apps) { if (self->apps) g_signal_handlers_disconnect_by_data(self->apps, self); g_set_object(&self->apps, apps); if (apps) g_signal_connect(apps, "items-changed", G_CALLBACK(model_changed), self); }
  render(self);
  if (self->editor) editor_refresh(self);
}
static void dispose(GObject *object) {
  GrottoAppDetail *self = DETAIL(object); if (self->apps) g_signal_handlers_disconnect_by_data(self->apps, self);
  g_clear_object(&self->apps); g_clear_object(&self->permissions); g_clear_object(&self->snapshot);
  adw_navigation_page_set_child(ADW_NAVIGATION_PAGE(self), NULL); G_OBJECT_CLASS(grotto_app_detail_parent_class)->dispose(object);
}
static void finalize(GObject *object) {
  GrottoAppDetail *self = DETAIL(object); g_weak_ref_clear(&self->parent); g_free(self->principal); G_OBJECT_CLASS(grotto_app_detail_parent_class)->finalize(object);
}
static void grotto_app_detail_class_init(GrottoAppDetailClass *klass) { G_OBJECT_CLASS(klass)->dispose = dispose; G_OBJECT_CLASS(klass)->finalize = finalize; }
static void grotto_app_detail_init(GrottoAppDetail *self) {
  g_weak_ref_init(&self->parent, NULL); self->navigation = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
  GtkWidget *toolbar = adw_toolbar_view_new(); adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  self->feedback = ADW_BANNER(adw_banner_new("")); adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(self->feedback));
  AdwPreferencesPage *prefs = ADW_PREFERENCES_PAGE(adw_preferences_page_new()); AdwPreferencesGroup *identity = group(prefs, NULL);
  self->app_identity = ADW_ACTION_ROW(row(identity, _("App"), NULL));
  adw_action_row_set_subtitle_selectable(self->app_identity, TRUE);
  self->name = ADW_ENTRY_ROW(adw_entry_row_new()); adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->name), _("Local Name")); adw_entry_row_set_show_apply_button(self->name, TRUE); adw_preferences_group_add(identity, GTK_WIDGET(self->name));
  self->app_icon = GTK_IMAGE(gtk_image_new());
  adw_action_row_add_prefix(self->app_identity, GTK_WIDGET(self->app_icon));
  AdwPreferencesGroup *access = group(prefs, _("Access")); self->access = GTK_WIDGET(access);
  adw_preferences_group_set_description(access, _("Blocked actions always stay blocked. Lowering access keeps saved permissions."));
  const char *titles[] = {_("Trust for Routine Operations"), _("Ask for Sensitive Operations"), _("Ask Unless Permitted")};
  for (guint i = 0; i < 3; i++) {
    GtkWidget *r = row(access, titles[i], NULL); self->tiers[i] = GTK_CHECK_BUTTON(gtk_check_button_new());
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->tiers[i]), GTK_ACCESSIBLE_PROPERTY_LABEL, titles[i], -1);
    if (i) gtk_check_button_set_group(self->tiers[i], self->tiers[0]);
    adw_action_row_add_prefix(ADW_ACTION_ROW(r), GTK_WIDGET(self->tiers[i])); adw_action_row_set_activatable_widget(ADW_ACTION_ROW(r), GTK_WIDGET(self->tiers[i]));
    g_signal_connect(self->tiers[i], "toggled", G_CALLBACK(tier_changed), self);
  }
  AdwPreferencesGroup *links = group(prefs, NULL); self->handoff = ADW_ACTION_ROW(row(links, _("Change in Grotto…"), NULL));
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(self->handoff), TRUE); g_signal_connect(self->handoff, "activated", G_CALLBACK(handoff), self);
  self->permissions_link = ADW_ACTION_ROW(row(links, _("Permissions"), _("Review and change saved permissions")));
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(self->permissions_link), TRUE); adw_action_row_add_suffix(self->permissions_link, gtk_image_new_from_icon_name("go-next-symbolic"));
  g_signal_connect(self->permissions_link, "activated", G_CALLBACK(open_permissions), self);
  GtkWidget *recent = row(links, _("Recent Activity"), NULL); gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(recent), TRUE);
  adw_action_row_add_suffix(ADW_ACTION_ROW(recent), gtk_image_new_from_icon_name("go-next-symbolic")); g_signal_connect(recent, "activated", G_CALLBACK(activity), self);
  AdwPreferencesGroup *details_group = group(prefs, NULL);
  AdwExpanderRow *details = ADW_EXPANDER_ROW(adw_expander_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(details), _("Details"));
  adw_preferences_group_add(details_group, GTK_WIDGET(details));
  self->verified = ADW_ACTION_ROW(row(NULL, _("Verified As"), NULL));
  self->fingerprint = ADW_ACTION_ROW(row(NULL, _("Fingerprint"), NULL));
  self->since = ADW_ACTION_ROW(row(NULL, _("Connected Since"), NULL));
  adw_action_row_set_subtitle_selectable(self->fingerprint, TRUE);
  adw_expander_row_add_row(details, GTK_WIDGET(self->verified));
  adw_expander_row_add_row(details, GTK_WIDGET(self->fingerprint));
  adw_expander_row_add_row(details, GTK_WIDGET(self->since));
  AdwPreferencesGroup *end = group(prefs, NULL);
  self->unblock = gtk_button_new_with_label(_("Remove App Block…"));
  gtk_widget_set_halign(self->unblock, GTK_ALIGN_CENTER); adw_preferences_group_add(end, self->unblock);
  g_signal_connect(self->unblock, "clicked", G_CALLBACK(unblock_app), self);
  self->remove = gtk_button_new_with_label(_("Remove App…"));
  gtk_widget_add_css_class(self->remove, "destructive-action"); gtk_widget_add_css_class(self->remove, "pill"); gtk_widget_set_halign(self->remove, GTK_ALIGN_CENTER); adw_preferences_group_add(end, self->remove);
  g_signal_connect(self->remove, "clicked", G_CALLBACK(remove_app), self); g_signal_connect(self->name, "apply", G_CALLBACK(name_apply), self);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(prefs));
  self->root_page = ADW_NAVIGATION_PAGE(adw_navigation_page_new(toolbar, _("App")));
  adw_navigation_view_add(self->navigation, self->root_page);
  adw_navigation_page_set_child(ADW_NAVIGATION_PAGE(self), GTK_WIDGET(self->navigation));
  g_signal_connect(self, "service-changed", G_CALLBACK(attached), NULL);
}
static void parent_changed(GrottoPanel *parent, GrottoAppDetail *self) {
  GrottoPanelsService *service = grotto_panel_get_service(parent);
  if (service != grotto_panel_get_service(GROTTO_PANEL(self))) {
    self->query_serial++; self->loading = FALSE;
    g_clear_object(&self->permissions);
    grotto_panel_set_service(GROTTO_PANEL(self), service);
    if (service && self->permissions_page) load_permissions(self);
  }
  render(self);
}
GtkWidget *grotto_app_detail_new(GrottoPanel *parent, const char *principal) {
  GrottoAppDetail *self = g_object_new(grotto_app_detail_get_type(), NULL); self->principal = g_strdup(principal); g_weak_ref_set(&self->parent, parent);
  g_object_bind_property(parent, "show-window-controls", self, "show-window-controls", G_BINDING_SYNC_CREATE);
  GrottoPanelsHost host = {.show_toast = forward_toast, .open_in_signer = forward_open, .can_command = forward_can, .command_async = forward_command};
  grotto_panel_set_host(GROTTO_PANEL(self), &host, self, NULL);
  g_signal_connect_object(parent, "service-changed", G_CALLBACK(parent_changed), self, 0);
  grotto_panel_set_service(GROTTO_PANEL(self), grotto_panel_get_service(parent)); return GTK_WIDGET(self);
}
