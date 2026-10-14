#include "page-client-detail.h"
#include "page-history.h"
#include "app-resources.h"
#include "approval_dialog.h"
#include <signer_permission_policy.h>
static const struct { guint32 kind; const gchar *caption; } kind_catalog[] = {
#include "kind-catalog.inc"
};

struct _GrottoClientDetail {
  AdwNavigationPage parent_instance;
  AdwNavigationView *navigation;
  AdwEntryRow *label_entry;
  AdwActionRow *fingerprint, *connected_since, *permissions;
  GtkLabel *message;
  GtkCheckButton *full, *medium, *low;
  GtkButton *clear_permissions, *revoke;
  GtkListBox *activity, *explicit_list;
  AdwPreferencesGroup *effective;
  GPtrArray *effective_rows;
  GrottoHomeModel *model;
  GVariant *rows;
  GCancellable *cancel;
  gchar *principal, *tier, *epoch, *owner;
  guint64 revision;
  gboolean updating, loading, busy, snapshot_ready, client_present;
};
G_DEFINE_TYPE(GrottoClientDetail, grotto_client_detail, ADW_TYPE_NAVIGATION_PAGE)
static const gchar *text(GVariant *v, const gchar *key) {
  const gchar *s = ""; if (v) g_variant_lookup(v, key, "&s", &s); return s;
}
static void load_permissions(GrottoClientDetail *self);
static void render_permissions(GrottoClientDetail *self);
static void controls(GrottoClientDetail *self) {
  gboolean enabled = self->model && self->client_present && grotto_home_model_writable(self->model) && !self->busy;
  GtkWidget *widgets[] = {GTK_WIDGET(self->label_entry), GTK_WIDGET(self->full), GTK_WIDGET(self->medium), GTK_WIDGET(self->low), GTK_WIDGET(self->revoke)};
  for (guint i = 0; i < G_N_ELEMENTS(widgets); i++) gtk_widget_set_sensitive(widgets[i], enabled);
  gtk_widget_set_sensitive(GTK_WIDGET(self->clear_permissions), enabled && self->snapshot_ready && self->rows && g_variant_n_children(self->rows));
  if (self->explicit_list) gtk_widget_set_sensitive(GTK_WIDGET(self->explicit_list), enabled && self->snapshot_ready);
}
static void mutation_done(GObject *source, GAsyncResult *result, gpointer data) {
  GrottoClientDetail *self = data; g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  self->busy = FALSE; self->snapshot_ready = FALSE;
  gtk_label_set_text(self->message, reply ? "Saved. Explicit remembered permissions are retained when trust is lowered." :
      "Not saved. The signer may have changed or refused this update. Review the refreshed permissions before trying again.");
  grotto_home_model_refresh(self->model); load_permissions(self); controls(self); g_object_unref(self);
}
static void mutate(GrottoClientDetail *self, const gchar *method, GVariant *args) {
  if (!self->client_present || !grotto_home_model_writable(self->model) || self->busy) { if (args) g_variant_unref(g_variant_ref_sink(args)); return; }
  GDBusProxy *proxy = grotto_home_model_proxy(self->model); g_autofree gchar *owner = g_dbus_proxy_get_name_owner(proxy);
  if (!owner) { if (args) g_variant_unref(g_variant_ref_sink(args)); return; }
  self->busy = TRUE; controls(self);
  /* Address the snapshot's unique daemon, never let a restarted owner receive
   * a mutation based on the previous owner's permission revision. */
  g_dbus_connection_call(g_dbus_proxy_get_connection(proxy), owner, "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1",
      method, args, NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, mutation_done, g_object_ref(self));
}
static void label_apply(AdwEntryRow *entry, gpointer data) {
  GrottoClientDetail *self = data; mutate(self, "SetClientLabel", g_variant_new("(ss)", self->principal, gtk_editable_get_text(GTK_EDITABLE(entry))));
}
static void tier_changed(GtkCheckButton *button, gpointer data) {
  GrottoClientDetail *self = data; if (self->updating || !gtk_check_button_get_active(button)) return;
  const gchar *tier = button == self->full ? "full" : button == self->medium ? "medium" : "low";
  mutate(self, "SetClientTrust", g_variant_new("(ss)", self->principal, tier));
}
typedef struct { GrottoClientDetail *page; GVariant *rows; gchar *identity, *owner; guint64 revision; } Save;
static void save_free(Save *s) { if (!s) return; g_object_unref(s->page); g_clear_pointer(&s->rows, g_variant_unref); g_free(s->identity); g_free(s->owner); g_free(s); }
static void save_rows(Save *s) {
  GrottoClientDetail *self = s->page; GDBusProxy *proxy = grotto_home_model_proxy(self->model);
  g_autofree gchar *owner = proxy ? g_dbus_proxy_get_name_owner(proxy) : NULL;
  if (!self->snapshot_ready || !grotto_home_model_writable(self->model) || g_strcmp0(owner, s->owner)) {
    gtk_label_set_text(self->message, "Permissions changed. Reopen the editor before saving."); return;
  }
  mutate(self, "SetPermissions", g_variant_new("(ss@aa{sv}t)", self->principal, s->identity, g_variant_ref(s->rows), s->revision));
}
static void confirmed(GObject *source, GAsyncResult *result, gpointer data) {
  Save *s = data; const gchar *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  if (!strcmp(response, "save")) save_rows(s);
  save_free(s);
}
static void revoke_confirmed(GObject *source, GAsyncResult *result, gpointer data) {
  GrottoClientDetail *self = data;
  if (!strcmp(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result), "revoke")) mutate(self, "RevokeClient", g_variant_new("(s)", self->principal));
  g_object_unref(self);
}
static void revoke_clicked(GtkButton *button, gpointer data) {
  (void)button; GrottoClientDetail *self = data;
  AdwDialog *d = adw_alert_dialog_new("Revoke this client?", "Pending requests will be cancelled and all remembered permissions removed. The app must ask again.");
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", "Cancel", "revoke", "Revoke client", NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "revoke", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "cancel");
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(d), GTK_WIDGET(self), NULL, revoke_confirmed, g_object_ref(self));
}
/* Clearing is explicitly scoped to one identity: the replacement API is
 * identity-scoped. Each scope is displayed separately and never flattened. */
static void clear_scope(GtkButton *button, gpointer data) {
  GrottoClientDetail *self = data; if (!self->snapshot_ready) return;
  const gchar *identity = g_object_get_data(G_OBJECT(button), "identity");
  Save *s = g_new0(Save, 1); s->page = g_object_ref(self); s->identity = g_strdup(identity);
  s->revision = self->revision; s->owner = g_strdup(self->owner);
  GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("aa{sv}")); s->rows = g_variant_ref_sink(g_variant_builder_end(&b));
  g_autofree gchar *body = g_strdup_printf("Remove every explicit allow and deny for this client and identity %s? The trust tier remains unchanged.", *identity ? identity : "(identity-independent)");
  AdwDialog *d = adw_alert_dialog_new("Clear remembered permissions?", body);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", "Cancel", "save", "Clear", NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "save", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "cancel");
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(d), GTK_WIDGET(self), NULL, confirmed, s);
}
static void open_permissions(AdwActionRow *row, gpointer data) {
  (void)row; GrottoClientDetail *self = data;
  if (!self->explicit_list) {
    GtkWidget *toolbar = adw_toolbar_view_new();
    AdwNavigationPage *page = ADW_NAVIGATION_PAGE(adw_navigation_page_new(toolbar, "Permissions"));
    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    GtkWidget *prefs = adw_preferences_page_new(); self->effective = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(self->effective, "Effective from trust level");
    adw_preferences_group_set_description(self->effective, "Read-only policy. Explicit denials win; required-confirmation events and key exports still need review.");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(prefs), self->effective);
    AdwPreferencesGroup *explicit = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(explicit, "Remembered permissions");
    adw_preferences_group_set_description(explicit, "Exact client + identity + operation + event type. Expiry is absolute, not extended by activity. Needs-review entries cannot authorize signing.");
    self->explicit_list = GTK_LIST_BOX(gtk_list_box_new()); gtk_list_box_set_selection_mode(self->explicit_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->explicit_list), "boxed-list"); adw_preferences_group_add(explicit, GTK_WIDGET(self->explicit_list));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(prefs), explicit);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), prefs);
    adw_navigation_view_add(self->navigation, page);
    adw_navigation_view_push(self->navigation, page);
  } else {
    AdwNavigationPage *page = ADW_NAVIGATION_PAGE(gtk_widget_get_ancestor(GTK_WIDGET(self->explicit_list), ADW_TYPE_NAVIGATION_PAGE));
    if (page) adw_navigation_view_push(self->navigation, page);
  }
  load_permissions(self);
}
static void clear_clicked(GtkButton *button, gpointer data) { (void)button; open_permissions(NULL, data); }

typedef struct {
  Save *save; GVariant *original, *snapshot; AdwDialog *dialog;
  GtkDropDown *decision, *expiry; GtkSpinButton *kind; GtkCheckButton *all_kinds; GtkLabel *error;
} Editor;
static void kind_item_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
  (void)factory; (void)data; GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0); gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 22); gtk_list_item_set_child(item, label);
}
static void kind_item_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
  (void)factory; (void)data; const gchar *name = gtk_string_object_get_string(GTK_STRING_OBJECT(gtk_list_item_get_item(item)));
  GtkWidget *label = gtk_list_item_get_child(item); gtk_label_set_text(GTK_LABEL(label), name); gtk_widget_set_tooltip_text(label, name);
}
static void catalog_selected(GObject *object, GParamSpec *pspec, gpointer data) {
  (void)pspec; Editor *e = data; guint index = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
  if (index < G_N_ELEMENTS(kind_catalog)) {
    gtk_check_button_set_active(e->all_kinds, FALSE); gtk_spin_button_set_value(e->kind, kind_catalog[index].kind);
  }
}
static void editor_free(gpointer data) {
  Editor *e = data; save_free(e->save); g_variant_unref(e->original); g_variant_unref(e->snapshot); g_free(e);
}
static void edited(GtkButton *button, gpointer data) {
  Editor *e = data; gboolean remove = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "remove"));
  const gchar *decision = gtk_drop_down_get_selected(e->decision) ? "deny" : "allow";
  gint kind = e->kind && !gtk_check_button_get_active(e->all_kinds) ? gtk_spin_button_get_value_as_int(e->kind) : -1;
  if (!remove && e->kind && !strcmp(decision, "allow") && kind < 0) {
    gtk_label_set_text(e->error, "Choose a specific event type before allowing this permission."); return;
  }
  guint index = gtk_drop_down_get_selected(e->expiry);
  guint64 expires = 0; g_variant_lookup(e->original, "expires-at", "t", &expires);
  const guint64 durations[] = {0, 600, 3600, 86400, 0};
  if (index > 0 && index < G_N_ELEMENTS(durations)) expires = durations[index] ? g_get_real_time() / G_USEC_PER_SEC + durations[index] : 0;
  GVariantBuilder rows; g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
  for (gsize i = 0; i < g_variant_n_children(e->snapshot); i++) {
    g_autoptr(GVariant) row = g_variant_get_child_value(e->snapshot, i);
    if (strcmp(text(row, "identity"), e->save->identity)) continue;
    if (g_variant_equal(row, e->original)) {
      if (!remove) {
        GVariantDict d; g_variant_dict_init(&d, row); g_variant_dict_insert(&d, "decision", "s", decision);
        g_variant_dict_insert(&d, "event-kind", "i", kind); g_variant_dict_insert(&d, "expires-at", "t", expires);
        g_variant_builder_add_value(&rows, g_variant_dict_end(&d));
      }
    } else g_variant_builder_add_value(&rows, row);
  }
  g_clear_pointer(&e->save->rows, g_variant_unref); e->save->rows = g_variant_ref_sink(g_variant_builder_end(&rows));
  if (!remove && !expires) {
    Save *s = e->save; e->save = NULL;
    g_autofree gchar *event_scope = !e->kind ? g_strdup("") : kind < 0 ? g_strdup(" · all event types") : g_strdup_printf(" · %s (%d)", gnostr_approval_kind_name(kind), kind);
    g_autofree gchar *scope = g_strdup_printf("Remember %s for %s, identity %s, operation %s%s forever?", decision,
        s->page->principal, s->identity, text(e->original, "operation"), event_scope);
    AdwDialog *d = adw_alert_dialog_new("Remember forever?", scope);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", "Cancel", "save", "Remember", NULL);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "cancel");
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(d), GTK_WIDGET(s->page), NULL, confirmed, s);
  } else save_rows(e->save);
  adw_dialog_close(e->dialog);
}
static void edit_row(AdwActionRow *row, gpointer data) {
  GrottoClientDetail *self = data; if (!self->snapshot_ready || self->busy) return;
  Editor *e = g_new0(Editor, 1); e->original = g_variant_ref(g_object_get_data(G_OBJECT(row), "permission"));
  e->snapshot = g_variant_ref(self->rows); e->save = g_new0(Save, 1); e->save->page = g_object_ref(self);
  e->save->identity = g_strdup(text(e->original, "identity")); e->save->revision = self->revision; e->save->owner = g_strdup(self->owner);
  e->dialog = adw_dialog_new(); adw_dialog_set_title(e->dialog, "Edit remembered permission"); adw_dialog_set_content_width(e->dialog, 400);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18); gtk_widget_set_margin_top(box, 18); gtk_widget_set_margin_bottom(box, 18);
  g_autofree gchar *scope = g_strdup_printf("Client: %s\nIdentity: %s\nOperation: %s", self->principal, e->save->identity, text(e->original, "operation"));
  GtkWidget *label = gtk_label_new(scope); gtk_label_set_wrap(GTK_LABEL(label), TRUE); gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR); gtk_box_append(GTK_BOX(box), label);
  const gchar *decisions[] = {"Allow", "Deny", NULL}; e->decision = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(decisions));
  gtk_drop_down_set_selected(e->decision, !strcmp(text(e->original, "decision"), "deny")); gtk_box_append(GTK_BOX(box), GTK_WIDGET(e->decision));
  gtk_accessible_update_property(GTK_ACCESSIBLE(e->decision), GTK_ACCESSIBLE_PROPERTY_LABEL, "Decision", -1);
  if (!strcmp(text(e->original, "operation"), "event")) {
    gint kind = -1; g_variant_lookup(e->original, "event-kind", "i", &kind);
    gtk_box_append(GTK_BOX(box), gtk_label_new("Event type (custom numbers supported)"));
    e->all_kinds = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("All event types (deny only)"));
    gtk_check_button_set_active(e->all_kinds, kind < 0); gtk_box_append(GTK_BOX(box), GTK_WIDGET(e->all_kinds));
    e->kind = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, G_MAXUINT16, 1)); gtk_spin_button_set_value(e->kind, MAX(kind, 0));
    g_object_bind_property(e->all_kinds, "active", e->kind, "sensitive", G_BINDING_SYNC_CREATE | G_BINDING_INVERT_BOOLEAN);
    gtk_accessible_update_property(GTK_ACCESSIBLE(e->kind), GTK_ACCESSIBLE_PROPERTY_LABEL, "Event type", -1); gtk_box_append(GTK_BOX(box), GTK_WIDGET(e->kind));
    GtkStringList *catalog = gtk_string_list_new(NULL); guint selected = GTK_INVALID_LIST_POSITION;
    for (guint i = 0; i < G_N_ELEMENTS(kind_catalog); i++) {
      const gchar *caption = gnostr_approval_kind_name(kind_catalog[i].kind);
      g_autofree gchar *name = g_strdup_printf("%s (%u)", strcmp(caption, "Custom event") ? caption : kind_catalog[i].caption, kind_catalog[i].kind);
      gtk_string_list_append(catalog, name); if ((gint)kind_catalog[i].kind == kind) selected = i;
    }
    GtkWidget *picker = gtk_drop_down_new(G_LIST_MODEL(catalog), NULL);
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(kind_item_setup), NULL); g_signal_connect(factory, "bind", G_CALLBACK(kind_item_bind), NULL);
    gtk_drop_down_set_factory(GTK_DROP_DOWN(picker), factory); g_object_unref(factory);
    gtk_drop_down_set_enable_search(GTK_DROP_DOWN(picker), TRUE); gtk_drop_down_set_selected(GTK_DROP_DOWN(picker), selected);
    gtk_accessible_update_property(GTK_ACCESSIBLE(picker), GTK_ACCESSIBLE_PROPERTY_LABEL, "Search event types", -1);
    g_signal_connect(picker, "notify::selected", G_CALLBACK(catalog_selected), e); gtk_box_append(GTK_BOX(box), picker);
  }
  const gchar *durations[] = {"Keep current expiry", "10 minutes", "1 hour", "24 hours", "Forever", NULL};
  e->expiry = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(durations)); gtk_drop_down_set_selected(e->expiry, !strcmp(text(e->original, "state"), "needs-review") ? 1 : 0);
  gtk_accessible_update_property(GTK_ACCESSIBLE(e->expiry), GTK_ACCESSIBLE_PROPERTY_LABEL, "Permission expiry", -1); gtk_box_append(GTK_BOX(box), GTK_WIDGET(e->expiry));
  e->error = GTK_LABEL(gtk_label_new(NULL)); gtk_label_set_wrap(e->error, TRUE); gtk_box_append(GTK_BOX(box), GTK_WIDGET(e->error));
  GtkWidget *save = gtk_button_new_with_label("Save permission"); g_signal_connect(save, "clicked", G_CALLBACK(edited), e); gtk_box_append(GTK_BOX(box), save);
  GtkWidget *remove = gtk_button_new_with_label("Remove permission"); g_object_set_data(G_OBJECT(remove), "remove", GINT_TO_POINTER(1));
  g_signal_connect(remove, "clicked", G_CALLBACK(edited), e); gtk_box_append(GTK_BOX(box), remove);
  g_object_set_data_full(G_OBJECT(e->dialog), "editor", e, editor_free); adw_dialog_set_child(e->dialog, box); adw_dialog_present(e->dialog, GTK_WIDGET(self));
}
static void render_permissions(GrottoClientDetail *self) {
  if (!self->explicit_list) return;
  for (guint i = 0; i < self->effective_rows->len; i++) adw_preferences_group_remove(self->effective, self->effective_rows->pdata[i]);
  g_ptr_array_set_size(self->effective_rows, 0);
  const gchar *tier = self->tier ? self->tier : "low";
  const gchar *basics[] = {"Share public key (after first consent)", "Read relay list", "Check connection"};
  for (guint i = 0; i < (!strcmp(tier, "low") ? 1u : 3u); i++) {
    GtkWidget *row = adw_action_row_new(); adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), basics[i]);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), !strcmp(tier, "full") ? "From Full trust" : !strcmp(tier, "medium") ? "From Medium trust" : "From Low trust");
    adw_preferences_group_add(self->effective, row); g_ptr_array_add(self->effective_rows, row);
  }
  if (!strcmp(tier, "medium")) {
    for (guint i = 0; i < G_N_ELEMENTS(signer_public_publishing_kinds); i++) {
      guint32 kind = signer_public_publishing_kinds[i]; GtkWidget *row = adw_action_row_new();
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), gnostr_approval_kind_name(kind));
      adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "From Medium trust · required-confirmation settings still apply");
      adw_preferences_group_add(self->effective, row); g_ptr_array_add(self->effective_rows, row);
    }
  } else if (!strcmp(tier, "full")) {
    GtkWidget *row = adw_action_row_new(); adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "All permitted operations and event types");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "From Full trust · method allowlist applies · key exports and required-confirmation events excluded");
    adw_preferences_group_add(self->effective, row); g_ptr_array_add(self->effective_rows, row);
  }
  GtkWidget *child; while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->explicit_list)))) gtk_list_box_remove(self->explicit_list, child);
  g_autoptr(GHashTable) scopes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (gsize i = 0; self->rows && i < g_variant_n_children(self->rows); i++) {
    g_autoptr(GVariant) permission = g_variant_get_child_value(self->rows, i);
    const gchar *identity = text(permission, "identity"), *op = text(permission, "operation"); gint kind = -1; guint64 expires = 0;
    g_variant_lookup(permission, "event-kind", "i", &kind); g_variant_lookup(permission, "expires-at", "t", &expires);
    if (g_hash_table_add(scopes, g_strdup(identity))) {
      GtkWidget *clear = gtk_button_new_with_label("Clear this identity’s permissions…");
      g_object_set_data_full(G_OBJECT(clear), "identity", g_strdup(identity), g_free); g_signal_connect(clear, "clicked", G_CALLBACK(clear_scope), self);
      gtk_list_box_append(self->explicit_list, clear);
    }
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new()); adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    g_autofree gchar *title = kind >= 0 ? g_strdup_printf("%s · %s (%d)", text(permission, "decision"), gnostr_approval_kind_name(kind), kind) : g_strdup_printf("%s · %s", text(permission, "decision"), gn_page_history_operation_label(op));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    g_autoptr(GDateTime) date = expires ? g_date_time_new_from_unix_local(expires) : NULL;
    g_autofree gchar *when = date ? g_date_time_format(date, "%F %R") : g_strdup("Forever");
    g_autofree gchar *subtitle = g_strdup_printf("%s\n%s%s", identity, !strcmp(text(permission, "state"), "needs-review") ? "Needs review · Not executable · " : "Expires: ", when);
    adw_action_row_set_subtitle(row, subtitle); adw_action_row_set_subtitle_lines(row, 3); gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_object_set_data_full(G_OBJECT(row), "permission", g_variant_ref(permission), (GDestroyNotify)g_variant_unref);
    g_signal_connect(row, "activated", G_CALLBACK(edit_row), self); gtk_list_box_append(self->explicit_list, GTK_WIDGET(row));
  }
  if (!self->rows || !g_variant_n_children(self->rows)) gtk_list_box_append(self->explicit_list, gtk_label_new("No remembered permissions"));
  controls(self);
}
typedef struct { GWeakRef page; guint step; guint64 revision; gchar *epoch, *owner; GVariant *rows; } Read;
static void read_free(Read *r) { g_weak_ref_clear(&r->page); g_free(r->epoch); g_free(r->owner); g_clear_pointer(&r->rows, g_variant_unref); g_free(r); }
static void permissions_done(GObject *source, GAsyncResult *result, gpointer data) {
  Read *r = data; g_autoptr(GrottoClientDetail) self = g_weak_ref_get(&r->page);
  g_autoptr(GError) error = NULL; g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (!self) { read_free(r); return; }
  if (!reply) { self->loading = FALSE; self->snapshot_ready = FALSE; controls(self); read_free(r); return; }
  g_autoptr(GVariant) value = g_variant_get_child_value(reply, 0);
  if (r->step == 0) {
    r->epoch = g_strdup(text(value, "control_epoch")); g_variant_lookup(value, "permissions_revision", "t", &r->revision);
    r->step = 1;
    g_dbus_connection_call(G_DBUS_CONNECTION(source), r->owner, "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "ListPermissions",
        g_variant_new("(s)", self->principal), G_VARIANT_TYPE("(aa{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, self->cancel, permissions_done, r); return;
  }
  if (r->step == 1) {
    r->rows = g_variant_ref(value); r->step = 2;
    g_dbus_connection_call(G_DBUS_CONNECTION(source), r->owner, "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "GetStatus",
        NULL, G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, self->cancel, permissions_done, r); return;
  }
  guint64 revision = 0; g_variant_lookup(value, "permissions_revision", "t", &revision);
  g_autofree gchar *owner = grotto_home_model_proxy(self->model) ? g_dbus_proxy_get_name_owner(grotto_home_model_proxy(self->model)) : NULL;
  self->loading = FALSE;
  if (revision == r->revision && !g_strcmp0(r->epoch, text(value, "control_epoch")) && !g_strcmp0(owner, r->owner)) {
    g_clear_pointer(&self->rows, g_variant_unref); self->rows = g_variant_ref(r->rows); self->revision = revision;
    g_free(self->owner); self->owner = g_strdup(r->owner); self->snapshot_ready = TRUE; render_permissions(self);
  } else { self->snapshot_ready = FALSE; gtk_label_set_text(self->message, "Permissions changed while loading. Reopen Permissions to refresh."); }
  controls(self); read_free(r);
}
static void load_permissions(GrottoClientDetail *self) {
  self->snapshot_ready = FALSE; controls(self); if (self->loading || !grotto_home_model_writable(self->model)) return;
  GDBusProxy *proxy = grotto_home_model_proxy(self->model); gchar *owner = g_dbus_proxy_get_name_owner(proxy); if (!owner) return;
  self->loading = TRUE; Read *r = g_new0(Read, 1); g_weak_ref_init(&r->page, self); r->owner = owner;
  g_dbus_connection_call(g_dbus_proxy_get_connection(proxy), owner, "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "GetStatus",
      NULL, G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, self->cancel, permissions_done, r);
}
static void model_changed(GrottoHomeModel *model, gpointer data) {
  GrottoClientDetail *self = data; GVariant *clients = grotto_home_model_clients(model); GVariant *client = NULL;
  for (gsize i = 0; clients && i < g_variant_n_children(clients); i++) {
    GVariant *row = g_variant_get_child_value(clients, i);
    if (!strcmp(text(row, "principal"), self->principal)) { client = row; break; } g_variant_unref(row);
  }
  self->updating = TRUE; self->client_present = client != NULL;
  if (client) {
    guint permission_count = 0; g_variant_lookup(client, "permission-count", "u", &permission_count);
    g_autofree gchar *summary = g_strdup_printf("%u explicit remembered permission%s", permission_count, permission_count == 1 ? "" : "s");
    adw_action_row_set_subtitle(self->permissions, summary);
    adw_navigation_page_set_title(ADW_NAVIGATION_PAGE(self), *text(client, "label") ? text(client, "label") : "Client");
    if (!gtk_widget_has_focus(GTK_WIDGET(self->label_entry))) gtk_editable_set_text(GTK_EDITABLE(self->label_entry), text(client, "label"));
    adw_action_row_set_subtitle(self->fingerprint, self->principal);
    guint64 since = 0; g_variant_lookup(client, "first-consented-at", "t", &since);
    g_autoptr(GDateTime) date = since ? g_date_time_new_from_unix_local(since) : NULL;
    g_autofree gchar *when = date ? g_date_time_format(date, "%F %R") : g_strdup("Waiting for consent");
    gboolean pairing_pending = FALSE; g_variant_lookup(client, "pairing-pending-ack", "b", &pairing_pending);
    adw_action_row_set_subtitle(self->connected_since, pairing_pending ? "Approved — waiting for relay acknowledgement" : when);
    g_free(self->tier); self->tier = g_strdup(text(client, "trust-tier"));
    gtk_check_button_set_active(!strcmp(self->tier, "full") ? self->full : !strcmp(self->tier, "medium") ? self->medium : self->low, TRUE);
    g_variant_unref(client);
  } else gtk_label_set_text(self->message, "Client unavailable or revoked.");
  self->updating = FALSE; controls(self);
  if (self->explicit_list) load_permissions(self);
  GtkWidget *child; while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->activity)))) gtk_list_box_remove(self->activity, child);
  GVariant *activity = grotto_home_model_activity(model); guint shown = 0;
  for (gsize i = 0; activity && i < g_variant_n_children(activity) && shown < 10; i++) {
    g_autoptr(GVariant) item = g_variant_get_child_value(activity, i); if (strcmp(text(item, "principal"), self->principal)) continue;
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new()); adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    gint kind = -1; guint32 event_kind; if (g_variant_lookup(item, "event_kind", "u", &event_kind)) kind = event_kind;
    g_autofree gchar *title = g_strdup_printf("%s · %s", gn_page_history_operation_label(text(item, "operation")), kind >= 0 ? gnostr_approval_kind_name(kind) : "Operation");
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title); adw_action_row_set_subtitle(row, text(item, "outcome"));
    gtk_list_box_append(self->activity, GTK_WIDGET(row)); shown++;
  }
  if (!shown) gtk_list_box_append(self->activity, gtk_label_new("No recent activity in the latest records"));
}
static void dispose(GObject *object) {
  GrottoClientDetail *self = GROTTO_CLIENT_DETAIL(object); if (self->cancel) g_cancellable_cancel(self->cancel);
  if (self->model) g_signal_handlers_disconnect_by_data(self->model, self);
  g_clear_object(&self->model); g_clear_object(&self->cancel); g_clear_pointer(&self->rows, g_variant_unref);
  g_clear_pointer(&self->effective_rows, g_ptr_array_unref);
  adw_navigation_page_set_child(ADW_NAVIGATION_PAGE(self), NULL);
  gtk_widget_dispose_template(GTK_WIDGET(self), GROTTO_TYPE_CLIENT_DETAIL); G_OBJECT_CLASS(grotto_client_detail_parent_class)->dispose(object);
}
static void finalize(GObject *object) {
  GrottoClientDetail *self = GROTTO_CLIENT_DETAIL(object); g_free(self->principal); g_free(self->tier); g_free(self->epoch); g_free(self->owner);
  G_OBJECT_CLASS(grotto_client_detail_parent_class)->finalize(object);
}
static void grotto_client_detail_class_init(GrottoClientDetailClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass); oc->dispose = dispose; oc->finalize = finalize;
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass); gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/page-client-detail.ui");
#define BIND(x) gtk_widget_class_bind_template_child(wc, GrottoClientDetail, x)
  BIND(navigation); BIND(label_entry); BIND(fingerprint); BIND(connected_since); BIND(permissions); BIND(message);
  BIND(full); BIND(medium); BIND(low); BIND(clear_permissions); BIND(revoke); BIND(activity);
#undef BIND
}
static void grotto_client_detail_init(GrottoClientDetail *self) {
  gtk_widget_init_template(GTK_WIDGET(self)); self->cancel = g_cancellable_new(); self->effective_rows = g_ptr_array_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(self->fingerprint), FALSE);
  g_signal_connect(self->label_entry, "apply", G_CALLBACK(label_apply), self);
  g_signal_connect(self->full, "toggled", G_CALLBACK(tier_changed), self); g_signal_connect(self->medium, "toggled", G_CALLBACK(tier_changed), self); g_signal_connect(self->low, "toggled", G_CALLBACK(tier_changed), self);
  g_signal_connect(self->permissions, "activated", G_CALLBACK(open_permissions), self);
  g_signal_connect(self->clear_permissions, "clicked", G_CALLBACK(clear_clicked), self); g_signal_connect(self->revoke, "clicked", G_CALLBACK(revoke_clicked), self);
}
GtkWidget *grotto_client_detail_new(GrottoHomeModel *model, const gchar *principal) {
  GrottoClientDetail *self = g_object_new(GROTTO_TYPE_CLIENT_DETAIL, NULL); self->model = g_object_ref(model); self->principal = g_strdup(principal);
  g_signal_connect(model, "changed", G_CALLBACK(model_changed), self); model_changed(model, self); load_permissions(self); return GTK_WIDGET(self);
}
