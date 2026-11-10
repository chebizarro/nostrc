#include "gh-lists-page.h"
#include "gh-list-editor-page.h"
#include <glib/gi18n.h>
#include <jansson.h>
#include <nostr-kinds.h>
#include <string.h>

struct _GhListsPage {
  AdwNavigationPage parent_instance;
  GhListsBackend backend;
  guint64 generation;
  GtkLabel *status;
  GtkListView *documents;
  AdwComboRow *kind_row;
  AdwEntryRow *identifier_row, *title_row;
  GtkButton *create_list, *import_list;
  GArray *kinds;
  AdwDialog *transient;
};
G_DEFINE_TYPE(GhListsPage, gh_lists_page, ADW_TYPE_NAVIGATION_PAGE)

gboolean
gh_lists_page_kind_visible(gint kind)
{
  const NostrListSchema *schema = nostr_nip51_schema_lookup(kind);
  return schema && !nostr_nip51_schema_read_only(schema) &&
    (nostr_nip51_schema_requires_identifier(schema) || kind == NOSTR_KIND_SLASH_COMMAND_LIST) &&
    kind != NOSTR_KIND_BADGE_SETS && kind != NOSTR_KIND_EMOJI_SETS;
}

static gint
selected_kind(GhListsPage *self)
{
  guint i = adw_combo_row_get_selected(self->kind_row);
  return i < self->kinds->len ? g_array_index(self->kinds, gint, i) : -1;
}

static void
validate(GObject *object, gpointer detail, GhListsPage *self)
{
  (void)object; (void)detail;
  gint kind = selected_kind(self);
  const NostrListSchema *schema = nostr_nip51_schema_lookup(kind);
  gboolean needs_id = nostr_nip51_schema_requires_identifier(schema);
  gtk_widget_set_sensitive(GTK_WIDGET(self->identifier_row), needs_id);
  const gchar *id = needs_id ? gtk_editable_get_text(GTK_EDITABLE(self->identifier_row)) : NULL;
  NostrListDocument *codec = nostr_nip51_document_new(kind, id);
  gtk_widget_set_sensitive(GTK_WIDGET(self->create_list), codec && (!needs_id || (id && *id)));
  nostr_nip51_document_free(codec);
}

static void
open_editor(GhListsPage *self, GNostrListDocument *snapshot)
{
  GtkWidget *dialog = gtk_widget_get_ancestor(GTK_WIDGET(self), ADW_TYPE_PREFERENCES_DIALOG);
  if (dialog)
    adw_preferences_dialog_push_subpage(ADW_PREFERENCES_DIALOG(dialog),
      ADW_NAVIGATION_PAGE(gh_list_editor_page_new(&self->backend, snapshot)));
}

static void
create_list(GtkButton *button, GhListsPage *self)
{
  (void)button;
  if (self->generation != self->backend.generation(self->backend.owner))
    return;
  gint kind = selected_kind(self);
  const NostrListSchema *schema = nostr_nip51_schema_lookup(kind);
  const gchar *id = nostr_nip51_schema_requires_identifier(schema) ?
    gtk_editable_get_text(GTK_EDITABLE(self->identifier_row)) : "";
  g_autoptr(GError) error = NULL;
  g_autoptr(GNostrListDocument) snapshot = self->backend.ensure(self->backend.owner, kind, id, &error);
  if (!snapshot) {
    gtk_label_set_text(self->status, _("Could not open this list; check the address and account storage"));
    return;
  }
  if (gnostr_list_document_get_local_revision(snapshot) || gnostr_list_document_get_event_id(snapshot)) {
    open_editor(self, snapshot);
    return;
  }
  const gchar *fields[] = { "title", gtk_editable_get_text(GTK_EDITABLE(self->title_row)) };
  g_autoptr(GPtrArray) ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  g_ptr_array_add(ops, gnostr_list_operation_new(GNOSTR_LIST_EDIT_METADATA, FALSE, fields, 2, 0, 0));
  g_autoptr(GNostrListDocument) saved = self->backend.apply(self->backend.owner, kind, id, 0, ops, &error);
  if (saved)
    open_editor(self, saved);
  else
    gtk_label_set_text(self->status, _("List not saved; check account storage and the list address"));
}

static void
activate(GtkListView *view, guint position, GhListsPage *self)
{
  GtkSelectionModel *model = gtk_list_view_get_model(view);
  g_autoptr(GNostrListDocument) snapshot = g_list_model_get_item(G_LIST_MODEL(model), position);
  if (snapshot && self->generation == self->backend.generation(self->backend.owner))
    open_editor(self, snapshot);
}

static void
setup_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory; (void)data;
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  GtkWidget *arrow = gtk_image_new_from_icon_name("go-next-symbolic");
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), arrow);
  gtk_list_item_set_child(item, row);
}

static void
bind_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory; (void)data;
  GNostrListDocument *snapshot = gtk_list_item_get_item(item);
  AdwActionRow *row = ADW_ACTION_ROW(gtk_list_item_get_child(item));
  const gchar *id = gnostr_list_document_get_identifier(snapshot);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), *id ? id : _("Command Subscriptions"));
  g_autofree gchar *subtitle = g_strdup_printf(_("Kind %d · Saved on This Device"), gnostr_list_document_get_kind(snapshot));
  adw_action_row_set_subtitle(row, subtitle);
}

static gboolean
visible(gpointer item, gpointer data)
{
  (void)data;
  return gh_lists_page_kind_visible(gnostr_list_document_get_kind(item));
}

static void
backend_changed(GObject *owner, GhListsPage *self)
{
  (void)owner;
  if (self->generation == self->backend.generation(self->backend.owner))
    return;
  if (self->transient)
    adw_dialog_force_close(self->transient);
  gtk_widget_set_sensitive(GTK_WIDGET(self), FALSE);
  GtkWidget *dialog = gtk_widget_get_ancestor(GTK_WIDGET(self), ADW_TYPE_PREFERENCES_DIALOG);
  if (dialog)
    adw_dialog_force_close(ADW_DIALOG(dialog));
}

static void
import_response(GtkButton *button, GhListsPage *self)
{
  GtkTextBuffer *buffer = g_object_get_data(G_OBJECT(button), "buffer");
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree gchar *wire = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  if (self->generation != self->backend.generation(self->backend.owner))
    return;
  json_t *json = strlen(wire) <= NOSTR_LIST_MAX_FOLLOW_BYTES ? json_loads(wire, JSON_REJECT_DUPLICATES, NULL) : NULL;
  gboolean allowed = gh_lists_page_kind_visible(json_integer_value(json_object_get(json, "kind")));
  json_decref(json);
  g_autoptr(GError) error = NULL;
  g_autoptr(GNostrListDocument) saved = allowed ? self->backend.import(self->backend.owner, wire, &error) : NULL;
  GtkLabel *status = g_object_get_data(G_OBJECT(button), "status");
  if (!saved) {
    gtk_label_set_text(status, _("Not imported — use a supported list kind and an unused address"));
    return;
  }
  adw_dialog_close(self->transient);
  open_editor(self, saved);
}

static void
import_list(GtkButton *button, GhListsPage *self)
{
  (void)button;
  AdwDialog *dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, _("Import List"));
  adw_dialog_set_content_width(dialog, 560);
  adw_dialog_set_content_height(dialog, 420);
  GtkWidget *toolbar = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  GtkWidget *save = gtk_button_new_with_mnemonic(_("_Import"));
  gtk_widget_add_css_class(save, "suggested-action");
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), save);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *status = gtk_label_new(_("Paste a Groundhog list export. Private entries are plain text in this file. Import saves a new local list and publishes nothing."));
  gtk_label_set_wrap(GTK_LABEL(status), TRUE);
  gtk_box_append(GTK_BOX(box), status);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scroll, TRUE);
  GtkWidget *text = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD_CHAR);
  gtk_accessible_update_property(GTK_ACCESSIBLE(text), GTK_ACCESSIBLE_PROPERTY_LABEL, _("List JSON to import"), -1);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), text);
  gtk_box_append(GTK_BOX(box), scroll);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);
  adw_dialog_set_child(dialog, toolbar);
  g_object_set_data(G_OBJECT(save), "buffer", gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)));
  g_object_set_data(G_OBJECT(save), "status", status);
  g_signal_connect_object(save, "clicked", G_CALLBACK(import_response), self, 0);
  g_set_weak_pointer(&self->transient, dialog);
  adw_dialog_present(dialog, GTK_WIDGET(self));
  gtk_widget_grab_focus(text);
}

static void
gh_lists_page_dispose(GObject *object)
{
  GhListsPage *self = GH_LISTS_PAGE(object);
  if (self->transient)
    adw_dialog_force_close(self->transient);
  g_clear_weak_pointer(&self->transient);
  if (self->backend.owner)
    g_signal_handlers_disconnect_by_data(self->backend.owner, self);
  g_clear_object(&self->backend.owner);
  g_clear_pointer(&self->kinds, g_array_unref);
  gtk_widget_dispose_template(GTK_WIDGET(self), GH_TYPE_LISTS_PAGE);
  G_OBJECT_CLASS(gh_lists_page_parent_class)->dispose(object);
}

static void
gh_lists_page_class_init(GhListsPageClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_lists_page_dispose;
  GtkWidgetClass *widget = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(widget, "/org/nostr/Groundhog/ui/gh-lists-page.ui");
#define CHILD(name) gtk_widget_class_bind_template_child(widget, GhListsPage, name)
  CHILD(status); CHILD(documents); CHILD(kind_row); CHILD(identifier_row); CHILD(title_row);
  CHILD(create_list); CHILD(import_list);
#undef CHILD
}

static void
gh_lists_page_init(GhListsPage *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->kinds = g_array_new(FALSE, FALSE, sizeof(gint));
  GtkStringList *kinds = gtk_string_list_new(NULL);
  for (gsize i = 0; i < nostr_nip51_schema_count(); i++) {
    gint kind = nostr_nip51_schema_kind(nostr_nip51_schema_at(i));
    if (!gh_lists_page_kind_visible(kind))
      continue;
    g_array_append_val(self->kinds, kind);
    g_autofree gchar *label = g_strdup_printf(_("Kind %d — %s"), kind,
      nostr_nip51_schema_items(nostr_nip51_schema_lookup(kind)));
    gtk_string_list_append(kinds, label);
  }
  adw_combo_row_set_model(self->kind_row, G_LIST_MODEL(kinds));
  g_object_unref(kinds);
  g_signal_connect(self->kind_row, "notify::selected", G_CALLBACK(validate), self);
  g_signal_connect(self->identifier_row, "notify::text", G_CALLBACK(validate), self);
  g_signal_connect(self->create_list, "clicked", G_CALLBACK(create_list), self);
  g_signal_connect(self->import_list, "clicked", G_CALLBACK(import_list), self);
  g_signal_connect(self->documents, "activate", G_CALLBACK(activate), self);
}

GhListsPage *
gh_lists_page_new(const GhListsBackend *backend)
{
  GhListsPage *self = g_object_new(GH_TYPE_LISTS_PAGE, NULL);
  self->backend = *backend;
  self->backend.owner = g_object_ref(backend->owner);
  self->generation = backend->generation(backend->owner);
  GNostrListService *service = backend->service(backend->owner);
  GtkFilter *filter = GTK_FILTER(gtk_custom_filter_new(visible, NULL, NULL));
  GtkFilterListModel *filtered = gtk_filter_list_model_new(service ? G_LIST_MODEL(g_object_ref(service)) : NULL, filter);
  GtkSelectionModel *selection = GTK_SELECTION_MODEL(gtk_no_selection_new(G_LIST_MODEL(filtered)));
  gtk_list_view_set_model(self->documents, selection);
  g_object_unref(selection);
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(setup_row), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(bind_row), NULL);
  gtk_list_view_set_factory(self->documents, factory);
  g_object_unref(factory);
  g_signal_connect(self->backend.owner, "changed", G_CALLBACK(backend_changed), self);
  validate(NULL, NULL, self);
  return self;
}
