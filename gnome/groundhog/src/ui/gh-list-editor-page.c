#include "gh-list-editor-page.h"
#include <glib/gi18n.h>
#include <jansson.h>
#include <string.h>

struct _GhListEditorPage {
  AdwNavigationPage parent_instance;
  GhListsBackend backend;
  guint64 generation;
  GNostrListDocument *snapshot;
  GtkLabel *status;
  AdwPreferencesGroup *metadata;
  AdwEntryRow *title_row, *description_row, *image_row, *value_row, *extra_row, *raw_row;
  AdwComboRow *tag_type;
  AdwSwitchRow *private_row;
  GtkListView *entries;
  GtkButton *save_metadata, *save_entry, *new_entry, *move_up, *move_down, *remove_entry, *export_list;
  GtkSingleSelection *selection;
  GtkStringList *rows, *types;
  gboolean updating;
  GtkButton *publish_list, *retry_list;
  GCancellable *publication;
  GNostrListDocument *approval_snapshot;
  GNostrListApproval approval;
  AdwDialog *transient;
};
G_DEFINE_TYPE(GhListEditorPage, gh_list_editor_page, ADW_TYPE_NAVIGATION_PAGE)

typedef struct { gboolean private_tag; guint index, occurrence; const NostrListTag *tag; } Entry;

static gboolean
equal_tags(const NostrListTag *a, const NostrListTag *b)
{
  if (nostr_nip51_tag_size(a) != nostr_nip51_tag_size(b))
    return FALSE;
  for (gsize i = 0; i < nostr_nip51_tag_size(a); i++)
    if (strcmp(nostr_nip51_tag_field(a, i), nostr_nip51_tag_field(b, i)))
      return FALSE;
  return TRUE;
}

static gboolean
selected_entry(GhListEditorPage *self, Entry *entry)
{
  guint selected = gtk_single_selection_get_selected(self->selection), position = 0;
  const NostrListDocument *codec = gnostr_list_document_get_codec(self->snapshot);
  for (guint p = 0; p < 2; p++) {
    for (guint i = 0; i < nostr_nip51_document_tag_count(codec, p); i++) {
      const NostrListTag *tag = nostr_nip51_document_tag(codec, p, i);
      if (!strcmp(nostr_nip51_tag_field(tag, 0), "d"))
        continue;
      if (position++ != selected)
        continue;
      *entry = (Entry){ .private_tag = p, .index = i, .tag = tag };
      for (guint j = 0; j < i; j++)
        if (equal_tags(tag, nostr_nip51_document_tag(codec, p, j)))
          entry->occurrence++;
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
current(GhListEditorPage *self, GError **error)
{
  if (self->backend.generation(self->backend.owner) != self->generation ||
      !self->backend.service(self->backend.owner)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Account list editor is no longer active");
    return FALSE;
  }
  return TRUE;
}

static void
show_failure(GhListEditorPage *self, GError *error)
{
  const gchar *text = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE) ?
    _("Not saved — account list storage limit reached") :
    g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG) ?
    _("Not saved — this list changed; reopen it before editing") :
    _("Not saved — check the entry and account storage");
  gtk_label_set_text(self->status, text);
}

static gchar *
tag_json(const NostrListTag *tag, guint start)
{
  json_t *array = json_array();
  for (guint i = start; i < nostr_nip51_tag_size(tag); i++)
    json_array_append_new(array, json_string(nostr_nip51_tag_field(tag, i)));
  gchar *text = json_dumps(array, JSON_COMPACT);
  json_decref(array);
  return text;
}

static void
setup_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory; (void)data;
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  gtk_list_item_set_child(item, row);
}

static void
bind_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory; (void)data;
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(gtk_list_item_get_child(item)),
    gtk_string_object_get_string(GTK_STRING_OBJECT(gtk_list_item_get_item(item))));
}

static void
sync_status(GhListEditorPage *self)
{
  GNostrListService *service = self->backend.service(self->backend.owner);
  if (!service) return;
  GNostrListState state = gnostr_list_service_get_state(service,
    gnostr_list_document_get_kind(self->snapshot), gnostr_list_document_get_identifier(self->snapshot));
  gboolean sync = self->backend.get_sync && self->backend.get_sync(self->backend.owner);
  const gchar *text = !sync ? _("Saved on This Device · Sync Off") :
    state == GNOSTR_LIST_SYNCED ? _("Synced") :
    state == GNOSTR_LIST_PUBLISHING ? _("Publishing…") :
    state == GNOSTR_LIST_UNREADABLE ? _("Cannot decrypt the remote list — local entries retained") :
    state == GNOSTR_LIST_PARTIAL ? _("Some relays have not answered — automatic publication is paused") :
    state == GNOSTR_LIST_WAITING_FOR_SIGNER ? _("Waiting for Signer") :
    state == GNOSTR_LIST_READING ? _("Reading Lists…") :
    state == GNOSTR_LIST_FAILED ? _("Not Synced — Retry After Checking Your Signer and Relays") :
    _("Saved on This Device");
  gtk_label_set_text(self->status, text);
  gtk_widget_set_visible(GTK_WIDGET(self->retry_list), sync &&
    (state == GNOSTR_LIST_PARTIAL || state == GNOSTR_LIST_FAILED ||
     state == GNOSTR_LIST_UNREADABLE || state == GNOSTR_LIST_WAITING_FOR_SIGNER));
  gtk_button_set_label(self->publish_list, state == GNOSTR_LIST_PARTIAL ?
    _("Publish Local Version _Anyway…") : _("_Publish…"));
  gtk_widget_set_visible(GTK_WIDGET(self->publish_list), sync && state != GNOSTR_LIST_PUBLISHING &&
    state != GNOSTR_LIST_UNREADABLE && state != GNOSTR_LIST_WAITING_FOR_DEPENDENCY);
}

static void
retry_clicked(GtkButton *button, GhListEditorPage *self)
{
  (void)button;
  if (!current(self, NULL)) return;
  gnostr_list_service_retry(self->backend.service(self->backend.owner),
    gnostr_list_document_get_kind(self->snapshot), gnostr_list_document_get_identifier(self->snapshot));
  sync_status(self);
}

static void
refresh(GhListEditorPage *self)
{
  self->updating = TRUE;
  const NostrListDocument *codec = gnostr_list_document_get_codec(self->snapshot);
  gtk_editable_set_text(GTK_EDITABLE(self->title_row), "");
  gtk_editable_set_text(GTK_EDITABLE(self->description_row), "");
  gtk_editable_set_text(GTK_EDITABLE(self->image_row), "");
  gboolean seen_title = FALSE, seen_description = FALSE, seen_image = FALSE;
  gtk_string_list_splice(self->rows, 0, g_list_model_get_n_items(G_LIST_MODEL(self->rows)), NULL);
  for (guint p = 0; p < 2; p++) {
    for (gsize i = 0; i < nostr_nip51_document_tag_count(codec, p); i++) {
      const NostrListTag *tag = nostr_nip51_document_tag(codec, p, i);
      const gchar *name = nostr_nip51_tag_field(tag, 0), *value = nostr_nip51_tag_field(tag, 1);
      if (!strcmp(name, "d"))
        continue;
      g_autofree gchar *label = g_strdup_printf("%s · %s: %s", p ? _("Private") : _("Public"), name, value ? value : "");
      gtk_string_list_append(self->rows, label);
      if (!p) {
        if (!strcmp(name, "title") && !seen_title) {
          gtk_editable_set_text(GTK_EDITABLE(self->title_row), value ? value : ""); seen_title = TRUE;
        }
        if (!strcmp(name, "description") && !seen_description) {
          gtk_editable_set_text(GTK_EDITABLE(self->description_row), value ? value : ""); seen_description = TRUE;
        }
        if (!strcmp(name, "image") && !seen_image) {
          gtk_editable_set_text(GTK_EDITABLE(self->image_row), value ? value : ""); seen_image = TRUE;
        }
      }
    }
  }
  gtk_single_selection_set_selected(self->selection, GTK_INVALID_LIST_POSITION);
  gtk_widget_set_sensitive(GTK_WIDGET(self->move_up), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->move_down), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->remove_entry), FALSE);
  sync_status(self);
  self->updating = FALSE;
}

static gboolean
apply(GhListEditorPage *self, GPtrArray *ops, GError **error)
{
  if (!current(self, error))
    return FALSE;
  GNostrListDocument *saved = self->backend.apply(self->backend.owner,
    gnostr_list_document_get_kind(self->snapshot), gnostr_list_document_get_identifier(self->snapshot),
    gnostr_list_document_get_local_revision(self->snapshot), ops, error);
  if (!saved)
    return FALSE;
  g_set_object(&self->snapshot, saved);
  g_object_unref(saved);
  refresh(self);
  return TRUE;
}

gboolean
gh_list_editor_page_save_tag(GhListEditorPage *self, const gchar *wire, gboolean private_tag, GError **error)
{
  json_t *array = wire && strlen(wire) <= NOSTR_LIST_MAX_EVENT_BYTES ? json_loads(wire, JSON_REJECT_DUPLICATES, NULL) : NULL;
  const gchar *fields[NOSTR_LIST_MAX_FIELDS];
  gsize n = json_array_size(array);
  gboolean valid = json_is_array(array) && n > 0 && n <= NOSTR_LIST_MAX_FIELDS;
  for (gsize i = 0; valid && i < n; i++) {
    fields[i] = json_string_value(json_array_get(array, i));
    valid = fields[i] && strlen(fields[i]) == json_string_length(json_array_get(array, i));
  }
  NostrListTag *tag = valid ? nostr_nip51_tag_new(fields, n) : NULL;
  if (!tag) {
    json_decref(array);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid list tag array");
    return FALSE;
  }
  g_autoptr(GPtrArray) ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  Entry old;
  gboolean replacing = selected_entry(self, &old);
  if (replacing)
    g_ptr_array_add(ops, gnostr_list_operation_remove(old.private_tag, old.tag, old.occurrence));
  g_ptr_array_add(ops, gnostr_list_operation_append(private_tag, tag));
  if (replacing && old.private_tag == private_tag) {
    const NostrListDocument *codec = gnostr_list_document_get_codec(self->snapshot);
    guint occurrence = 0;
    for (guint i = 0; i < nostr_nip51_document_tag_count(codec, private_tag); i++)
      if (i != old.index && equal_tags(tag, nostr_nip51_document_tag(codec, private_tag, i)))
        occurrence++;
    g_ptr_array_add(ops, gnostr_list_operation_move(private_tag, tag, occurrence, old.index));
  }
  gboolean ok = apply(self, ops, error);
  if (ok) {
    const NostrListDocument *codec = gnostr_list_document_get_codec(self->snapshot);
    guint count = nostr_nip51_document_tag_count(codec, private_tag);
    guint index = replacing && old.private_tag == private_tag ? MIN(old.index, count - 1) : count - 1;
    guint position = 0;
    for (guint p = 0; p <= (guint)private_tag; p++) {
      guint limit = p == (guint)private_tag ? index : nostr_nip51_document_tag_count(codec, p);
      for (guint i = 0; i < limit; i++)
        if (strcmp(nostr_nip51_tag_field(nostr_nip51_document_tag(codec, p, i), 0), "d")) position++;
    }
    gtk_single_selection_set_selected(self->selection, position);
  }
  nostr_nip51_tag_free(tag);
  json_decref(array);
  if (!ok)
    show_failure(self, error ? *error : NULL);
  return ok;
}

static void
visibility_response(AdwAlertDialog *dialog, const gchar *response, GhListEditorPage *self)
{
  if (strcmp(response, "save"))
    return;
  g_autoptr(GError) error = NULL;
  gh_list_editor_page_save_tag(self, g_object_get_data(G_OBJECT(dialog), "tag"),
    GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dialog), "private")), &error);
}

static void
save_entry(GtkButton *button, GhListEditorPage *self)
{
  (void)button;
  Entry entry;
  gboolean private_tag = adw_switch_row_get_active(self->private_row);
  if (selected_entry(self, &entry) && entry.private_tag != private_tag) {
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Change Entry Visibility?"),
      _("This change is saved locally. Public entries can be revealed by a future public publication or a JSON export. Nothing is published now.")));
    adw_alert_dialog_add_responses(dialog, "cancel", _("_Cancel"), "save", _("_Save Local Change"), NULL);
    adw_alert_dialog_set_default_response(dialog, "cancel");
    adw_alert_dialog_set_close_response(dialog, "cancel");
    g_object_set_data_full(G_OBJECT(dialog), "tag", g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->raw_row))), g_free);
    g_object_set_data(G_OBJECT(dialog), "private", GINT_TO_POINTER(private_tag));
    g_signal_connect_object(dialog, "response", G_CALLBACK(visibility_response), self, 0);
    g_set_weak_pointer(&self->transient, ADW_DIALOG(dialog));
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
    return;
  }
  g_autoptr(GError) error = NULL;
  gh_list_editor_page_save_tag(self, gtk_editable_get_text(GTK_EDITABLE(self->raw_row)), private_tag, &error);
}

static void
raw_changed(GObject *object, GParamSpec *pspec, GhListEditorPage *self)
{
  (void)object; (void)pspec;
  const gchar *raw = gtk_editable_get_text(GTK_EDITABLE(self->raw_row));
  json_t *array = strlen(raw) <= NOSTR_LIST_MAX_EVENT_BYTES ? json_loads(raw, 0, NULL) : NULL;
  gsize n = json_array_size(array);
  gboolean valid = json_is_array(array) && n > 0 && n <= NOSTR_LIST_MAX_FIELDS;
  const gchar *fields[NOSTR_LIST_MAX_FIELDS];
  for (gsize i = 0; valid && i < n; i++) {
    fields[i] = json_string_value(json_array_get(array, i));
    valid = fields[i] && strlen(fields[i]) == json_string_length(json_array_get(array, i));
  }
  NostrListTag *tag = valid ? nostr_nip51_tag_new(fields, n) : NULL;
  if (tag) {
    valid = strcmp(fields[0], "d") && nostr_nip51_validate_item(
      nostr_nip51_schema_lookup(gnostr_list_document_get_kind(self->snapshot)), tag) != NOSTR_LIST_ITEM_INVALID;
    nostr_nip51_tag_free(tag);
  } else valid = FALSE;
  gtk_widget_set_sensitive(GTK_WIDGET(self->save_entry), valid);
  json_decref(array);
}

static void
select_entry(GObject *object, GParamSpec *pspec, GhListEditorPage *self)
{
  (void)object; (void)pspec;
  if (self->updating)
    return;
  Entry entry;
  gboolean selected = selected_entry(self, &entry);
  gtk_widget_set_sensitive(GTK_WIDGET(self->move_up), selected && entry.index > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(self->move_down), selected);
  gtk_widget_set_sensitive(GTK_WIDGET(self->remove_entry), selected);
  if (!selected)
    return;
  self->updating = TRUE;
  const gchar *name = nostr_nip51_tag_field(entry.tag, 0);
  guint type = GTK_INVALID_LIST_POSITION;
  for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(self->types)); i++)
    if (!strcmp(name, gtk_string_list_get_string(self->types, i))) type = i;
  if (type == GTK_INVALID_LIST_POSITION) {
    type = g_list_model_get_n_items(G_LIST_MODEL(self->types));
    gtk_string_list_append(self->types, name);
  }
  adw_combo_row_set_selected(self->tag_type, type);
  const gchar *value = nostr_nip51_tag_field(entry.tag, 1);
  gtk_editable_set_text(GTK_EDITABLE(self->value_row), value ? value : "");
  g_autofree gchar *extra = tag_json(entry.tag, 2), *raw = tag_json(entry.tag, 0);
  gtk_editable_set_text(GTK_EDITABLE(self->extra_row), extra);
  gtk_editable_set_text(GTK_EDITABLE(self->raw_row), raw);
  adw_switch_row_set_active(self->private_row, entry.private_tag);
  self->updating = FALSE;
}

static void
fields_changed(GObject *object, gpointer detail, GhListEditorPage *self)
{
  (void)object; (void)detail;
  if (self->updating)
    return;
  const gchar *extra = gtk_editable_get_text(GTK_EDITABLE(self->extra_row));
  json_t *trailing = strlen(extra) <= NOSTR_LIST_MAX_EVENT_BYTES ? json_loads(extra, 0, NULL) : NULL;
  const gchar *name = gtk_string_list_get_string(self->types, adw_combo_row_get_selected(self->tag_type));
  json_t *tag = json_pack("[ss]", name ? name : "", gtk_editable_get_text(GTK_EDITABLE(self->value_row)));
  if (json_is_array(trailing)) {
    for (gsize i = 0; i < json_array_size(trailing); i++) json_array_append(tag, json_array_get(trailing, i));
  }
  g_autofree gchar *raw = json_is_array(trailing) ? json_dumps(tag, JSON_COMPACT) : g_strdup("");
  gtk_editable_set_text(GTK_EDITABLE(self->raw_row), raw);
  json_decref(tag);
  json_decref(trailing);
}

static void
new_entry(GtkButton *button, GhListEditorPage *self)
{
  (void)button;
  gtk_single_selection_set_selected(self->selection, GTK_INVALID_LIST_POSITION);
  gtk_editable_set_text(GTK_EDITABLE(self->value_row), "");
  gtk_editable_set_text(GTK_EDITABLE(self->extra_row), "[]");
  adw_switch_row_set_active(self->private_row,
    nostr_nip51_schema_privacy(nostr_nip51_schema_lookup(gnostr_list_document_get_kind(self->snapshot))) != NOSTR_LIST_PRIVACY_PUBLIC);
  gtk_widget_grab_focus(GTK_WIDGET(self->value_row));
}

static void
reorder(GtkButton *button, GhListEditorPage *self)
{
  Entry entry;
  if (!selected_entry(self, &entry))
    return;
  g_autoptr(GPtrArray) ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  if (button == self->remove_entry)
    g_ptr_array_add(ops, gnostr_list_operation_remove(entry.private_tag, entry.tag, entry.occurrence));
  else
    g_ptr_array_add(ops, gnostr_list_operation_move(entry.private_tag, entry.tag, entry.occurrence,
      button == self->move_up ? (entry.index ? entry.index - 1 : 0) : entry.index + 1));
  guint position = gtk_single_selection_get_selected(self->selection);
  g_autoptr(GError) error = NULL;
  if (!apply(self, ops, &error)) {
    show_failure(self, error);
  } else {
    guint count = g_list_model_get_n_items(G_LIST_MODEL(self->rows));
    if (count) {
      if (button == self->move_up && position) position--;
      else if (button == self->move_down) position++;
      gtk_single_selection_set_selected(self->selection, MIN(position, count - 1));
      gtk_widget_grab_focus(GTK_WIDGET(self->entries));
    } else {
      gtk_widget_grab_focus(GTK_WIDGET(self->new_entry));
    }
  }
}

static void
save_metadata(GtkButton *button, GhListEditorPage *self)
{
  (void)button;
  const gchar *names[] = { "title", "description", "image" };
  AdwEntryRow *rows[] = { self->title_row, self->description_row, self->image_row };
  const NostrListDocument *codec = gnostr_list_document_get_codec(self->snapshot);
  Entry target;
  gboolean selected = selected_entry(self, &target);
  g_autofree gchar *raw = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->raw_row)));
  g_autofree gchar *value = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->value_row)));
  g_autofree gchar *extra = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->extra_row)));
  guint type = adw_combo_row_get_selected(self->tag_type);
  gboolean private_tag = adw_switch_row_get_active(self->private_row);
  g_autoptr(GPtrArray) ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
    const gchar *previous = "";
    for (gsize j = 0; j < nostr_nip51_document_tag_count(codec, FALSE); j++) {
      const NostrListTag *tag = nostr_nip51_document_tag(codec, FALSE, j);
      if (!strcmp(names[i], nostr_nip51_tag_field(tag, 0))) {
        previous = nostr_nip51_tag_field(tag, 1);
        break;
      }
    }
    const gchar *fields[] = { names[i], gtk_editable_get_text(GTK_EDITABLE(rows[i])) };
    if (g_strcmp0(previous ? previous : "", fields[1]))
      g_ptr_array_add(ops, gnostr_list_operation_new(GNOSTR_LIST_EDIT_METADATA, FALSE, fields, 2, 0, 0));
  }
  g_autoptr(GError) error = NULL;
  if (!apply(self, ops, &error)) {
    show_failure(self, error);
    return;
  }
  if (selected) {
    codec = gnostr_list_document_get_codec(self->snapshot);
    guint position = 0;
    for (guint p = 0; p <= (guint)target.private_tag; p++) {
      guint limit = p == (guint)target.private_tag ? target.index : nostr_nip51_document_tag_count(codec, p);
      for (guint i = 0; i < limit; i++)
        if (strcmp(nostr_nip51_tag_field(nostr_nip51_document_tag(codec, p, i), 0), "d")) position++;
    }
    gtk_single_selection_set_selected(self->selection, position);
    self->updating = TRUE;
    adw_combo_row_set_selected(self->tag_type, type);
    gtk_editable_set_text(GTK_EDITABLE(self->value_row), value);
    gtk_editable_set_text(GTK_EDITABLE(self->extra_row), extra);
    gtk_editable_set_text(GTK_EDITABLE(self->raw_row), raw);
    adw_switch_row_set_active(self->private_row, private_tag);
    self->updating = FALSE;
  }
}

static void
export_list(GtkButton *button, GhListEditorPage *self)
{
  (void)button;
  g_autoptr(GError) error = NULL;
  if (!current(self, &error))
    return;
  g_autofree gchar *json = self->backend.export(self->snapshot);
  AdwDialog *dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, _("Export List"));
  adw_dialog_set_content_width(dialog, 560);
  adw_dialog_set_content_height(dialog, 420);
  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *warning = gtk_label_new(_("This export includes private entries in plain text. Copy it only to a trusted location."));
  gtk_label_set_wrap(GTK_LABEL(warning), TRUE);
  gtk_box_append(GTK_BOX(box), warning);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scroll, TRUE);
  GtkWidget *text = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(text), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD_CHAR);
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)), json, -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(text), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Exported list JSON"), -1);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), text);
  gtk_box_append(GTK_BOX(box), scroll);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);
  adw_dialog_set_child(dialog, toolbar);
  g_set_weak_pointer(&self->transient, dialog);
  adw_dialog_present(dialog, GTK_WIDGET(self));
}

static void
publish_done(GObject *source, GAsyncResult *result, gpointer data)
{
  GhListEditorPage *self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) outcomes = gnostr_list_service_publish_finish(GNOSTR_LIST_SERVICE(source), result, &error);
  g_clear_object(&self->publication);
  if (current(self, NULL)) {
    sync_status(self);
    if (!outcomes) gtk_label_set_text(self->status, _("Not Published — Check the List, Signer and Relays"));
  }
  g_object_unref(self);
}
static void
publish_response(AdwAlertDialog *dialog, const gchar *response, GhListEditorPage *self)
{
  (void)dialog;
  if (g_strcmp0(response, "publish") || !current(self, NULL)) {
    g_clear_object(&self->approval_snapshot);
    return;
  }
  GNostrListService *service = self->backend.service(self->backend.owner);
  g_autoptr(GNostrListDocument) snapshot = gnostr_list_service_lookup(service,
    gnostr_list_document_get_kind(self->approval_snapshot), gnostr_list_document_get_identifier(self->approval_snapshot));
  if (!snapshot || gnostr_list_document_get_local_revision(snapshot) != gnostr_list_document_get_local_revision(self->approval_snapshot) ||
      g_strcmp0(gnostr_list_document_get_event_id(snapshot), gnostr_list_document_get_event_id(self->approval_snapshot))) {
    gtk_label_set_text(self->status, _("List changed — reopen it before publishing"));
    goto done;
  }
  self->publication = g_cancellable_new();
  gnostr_list_service_publish_async(service, gnostr_list_document_get_kind(snapshot),
    gnostr_list_document_get_identifier(snapshot), self->approval, self->publication, publish_done, g_object_ref(self));
 done:
  g_clear_object(&self->approval_snapshot);
}
static void
publish_clicked(GtkButton *button, GhListEditorPage *self)
{
  (void)button;
  if (self->transient || self->publication || !current(self, NULL)) return;
  GNostrListService *service = self->backend.service(self->backend.owner);
  GNostrListState state = gnostr_list_service_get_state(service,
    gnostr_list_document_get_kind(self->snapshot), gnostr_list_document_get_identifier(self->snapshot));
  self->approval_snapshot = g_object_ref(self->snapshot);
  self->approval = GNOSTR_LIST_APPROVE_PRIVATE | GNOSTR_LIST_APPROVE_PUBLIC;
  gboolean partial = state == GNOSTR_LIST_PARTIAL;
  if (partial) self->approval |= GNOSTR_LIST_APPROVE_PARTIAL;
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
    partial ? _("Publish Local Version Anyway?") : _("Publish This List?"),
    partial ? _("Some relays have not answered, so this may replace unseen changes. Public entries and list details will be visible to anyone. Private entries stay encrypted. Accepted publications cannot be recalled.") :
      _("Public entries and list details will be visible to anyone. Private entries stay encrypted. Relays see your account and publication time; accepted publications cannot be recalled.")));
  adw_alert_dialog_add_responses(dialog, "cancel", _("Cancel"), "publish", _("Publish"), NULL);
  adw_alert_dialog_set_close_response(dialog, "cancel");
  adw_alert_dialog_set_response_appearance(dialog, "publish", ADW_RESPONSE_DESTRUCTIVE);
  g_signal_connect_object(dialog, "response", G_CALLBACK(publish_response), self, 0);
  g_set_weak_pointer(&self->transient, ADW_DIALOG(dialog));
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}

static void
backend_changed(GObject *owner, GhListEditorPage *self)
{
  (void)owner;
  if (self->generation == self->backend.generation(self->backend.owner)) {
    sync_status(self);
    return;
  }
  if (self->publication) g_cancellable_cancel(self->publication);
  if (self->transient)
    adw_dialog_force_close(self->transient);
  gtk_widget_set_sensitive(GTK_WIDGET(self), FALSE);
  gtk_label_set_text(self->status, _("Account changed — reopen Lists and Sets"));
  GtkWidget *dialog = gtk_widget_get_ancestor(GTK_WIDGET(self), ADW_TYPE_PREFERENCES_DIALOG);
  if (dialog)
    adw_dialog_force_close(ADW_DIALOG(dialog));
}

static void
gh_list_editor_page_dispose(GObject *object)
{
  GhListEditorPage *self = GH_LIST_EDITOR_PAGE(object);
  if (self->transient)
    adw_dialog_force_close(self->transient);
  g_clear_weak_pointer(&self->transient);
  if (self->backend.owner)
    g_signal_handlers_disconnect_by_data(self->backend.owner, self);
  g_clear_object(&self->backend.owner);
  g_clear_object(&self->snapshot);
  g_clear_object(&self->approval_snapshot);
  if (self->publication) g_cancellable_cancel(self->publication);
  g_clear_object(&self->publication);
  gtk_widget_dispose_template(GTK_WIDGET(self), GH_TYPE_LIST_EDITOR_PAGE);
  g_clear_object(&self->selection);
  g_clear_object(&self->rows);
  g_clear_object(&self->types);
  G_OBJECT_CLASS(gh_list_editor_page_parent_class)->dispose(object);
}

static void
gh_list_editor_page_class_init(GhListEditorPageClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_list_editor_page_dispose;
  GtkWidgetClass *widget = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(widget, "/org/nostr/Groundhog/ui/gh-list-editor-page.ui");
#define CHILD(name) gtk_widget_class_bind_template_child(widget, GhListEditorPage, name)
  CHILD(status); CHILD(metadata); CHILD(title_row); CHILD(description_row); CHILD(image_row);
  CHILD(value_row); CHILD(extra_row); CHILD(raw_row); CHILD(tag_type); CHILD(private_row);
  CHILD(entries); CHILD(save_metadata); CHILD(save_entry); CHILD(new_entry);
  CHILD(move_up); CHILD(move_down); CHILD(remove_entry); CHILD(export_list); CHILD(publish_list); CHILD(retry_list);
#undef CHILD
}

static void
gh_list_editor_page_init(GhListEditorPage *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->rows = gtk_string_list_new(NULL);
  self->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(self->rows)));
  gtk_single_selection_set_autoselect(self->selection, FALSE);
  gtk_single_selection_set_can_unselect(self->selection, TRUE);
  gtk_list_view_set_model(self->entries, GTK_SELECTION_MODEL(self->selection));
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(setup_row), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(bind_row), NULL);
  gtk_list_view_set_factory(self->entries, factory);
  g_object_unref(factory);
  g_signal_connect(self->selection, "notify::selected", G_CALLBACK(select_entry), self);
  g_signal_connect(self->value_row, "notify::text", G_CALLBACK(fields_changed), self);
  g_signal_connect(self->extra_row, "notify::text", G_CALLBACK(fields_changed), self);
  g_signal_connect(self->tag_type, "notify::selected", G_CALLBACK(fields_changed), self);
  g_signal_connect(self->raw_row, "notify::text", G_CALLBACK(raw_changed), self);
  gtk_widget_set_sensitive(GTK_WIDGET(self->save_entry), FALSE);
  g_signal_connect(self->save_entry, "clicked", G_CALLBACK(save_entry), self);
  g_signal_connect(self->save_metadata, "clicked", G_CALLBACK(save_metadata), self);
  g_signal_connect(self->new_entry, "clicked", G_CALLBACK(new_entry), self);
  g_signal_connect(self->move_up, "clicked", G_CALLBACK(reorder), self);
  g_signal_connect(self->move_down, "clicked", G_CALLBACK(reorder), self);
  g_signal_connect(self->remove_entry, "clicked", G_CALLBACK(reorder), self);
  g_signal_connect(self->export_list, "clicked", G_CALLBACK(export_list), self);
  g_signal_connect(self->publish_list, "clicked", G_CALLBACK(publish_clicked), self);
  g_signal_connect(self->retry_list, "clicked", G_CALLBACK(retry_clicked), self);
}

GhListEditorPage *
gh_list_editor_page_new(const GhListsBackend *backend, GNostrListDocument *snapshot)
{
  GhListEditorPage *self = g_object_new(GH_TYPE_LIST_EDITOR_PAGE, NULL);
  self->backend = *backend;
  self->backend.owner = g_object_ref(backend->owner);
  self->generation = backend->generation(backend->owner);
  self->snapshot = g_object_ref(snapshot);
  const NostrListSchema *schema = nostr_nip51_schema_lookup(gnostr_list_document_get_kind(snapshot));
  g_auto(GStrv) names = g_strsplit(nostr_nip51_schema_items(schema), " ", -1);
  self->types = gtk_string_list_new((const gchar *const *)names);
  adw_combo_row_set_model(self->tag_type, G_LIST_MODEL(self->types));
  NostrListPrivacy privacy = nostr_nip51_schema_privacy(schema);
  adw_switch_row_set_active(self->private_row, privacy != NOSTR_LIST_PRIVACY_PUBLIC);
  gtk_widget_set_sensitive(GTK_WIDGET(self->private_row), privacy == NOSTR_LIST_PRIVACY_OPTIONAL);
  gtk_widget_set_visible(GTK_WIDGET(self->metadata), nostr_nip51_schema_requires_identifier(schema));
  gtk_widget_set_visible(GTK_WIDGET(self->save_metadata), nostr_nip51_schema_requires_identifier(schema));
  g_signal_connect(self->backend.owner, "changed", G_CALLBACK(backend_changed), self);
  if (g_signal_lookup("sync-state-changed", G_OBJECT_TYPE(self->backend.owner)))
    g_signal_connect(self->backend.owner, "sync-state-changed", G_CALLBACK(backend_changed), self);
  refresh(self);
  return self;
}

GNostrListDocument *gh_list_editor_page_get_snapshot(GhListEditorPage *self) { return self->snapshot; }
const gchar *gh_list_editor_page_get_status(GhListEditorPage *self) { return gtk_label_get_text(self->status); }
