#include "gh-hidden-page.h"
#include <nostr/nip19/nip19.h>
#include <glib/gi18n.h>
#include <nostr-kinds.h>
#include <string.h>

typedef struct {
  gboolean private_tag;
  gsize index;
} Entry;
struct _GhHiddenPage {
  AdwNavigationPage parent_instance;
  GhListsBackend backend;
  guint64 generation;
  gboolean refreshing;
  GNostrListDocument *snapshot;
  GtkStringList *items;
  GArray *entries;
  AdwComboRow *kind_row;
  AdwEntryRow *value_row;
  GtkLabel *status, *empty_label;
  GtkListView *list;
  GtkButton *retry_button;
  AdwAlertDialog *confirmation;
  gchar *pending_account;
};
G_DEFINE_TYPE(GhHiddenPage, gh_hidden_page, ADW_TYPE_NAVIGATION_PAGE)
static const gchar *names[] = { "p", "word", "t", "e" };
static const gchar *types[] = { N_("Hidden Account"), N_("Hidden Word"), N_("Hidden Hashtag"),
                                N_("Hidden Message Thread") };
static const gchar *input_titles[]
    = { N_("Account Public Key"), N_("Word or Phrase"), N_("Hashtag"), N_("Message ID") };

static void
set_status(GhHiddenPage *self, const gchar *message)
{
  gtk_label_set_text(self->status, message ? message : "");
  gtk_widget_set_visible(GTK_WIDGET(self->status), message && *message);
}
static gboolean
finish_confirmation_close(GtkWidget *dialog, GdkFrameClock *clock, gpointer data)
{
  (void)clock;
  (void)data;
  adw_dialog_force_close(ADW_DIALOG(dialog));
  return gtk_widget_get_parent(dialog) != NULL;
}

static void
close_confirmation(GhHiddenPage *self)
{
  if (self->confirmation) {
    g_signal_handlers_disconnect_by_data(self->confirmation, self);
    GtkWidget *dialog = GTK_WIDGET(self->confirmation);
    gtk_widget_set_sensitive(dialog, FALSE);
    gtk_widget_set_opacity(dialog, 0);
    /* AdwDialog 1.5 opens on later map ticks. Keep the close pending until
     * its host removes it, including when invalidated before the first frame. */
    gtk_widget_add_tick_callback(dialog, finish_confirmation_close, NULL, NULL);
    adw_dialog_force_close(ADW_DIALOG(self->confirmation));
    g_clear_object(&self->confirmation);
  }
  g_clear_pointer(&self->pending_account, g_free);
}
static gboolean
current(GhHiddenPage *self, GError **error)
{
  if (self->backend.owner && self->snapshot
      && self->generation == self->backend.generation(self->backend.owner))
    return TRUE;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, _("Account list storage is not open"));
  return FALSE;
}
static void
refresh(GhHiddenPage *self)
{
  if (!self->backend.owner || self->refreshing)
    return;
  self->refreshing = TRUE;
  guint64 generation = self->backend.generation(self->backend.owner);
  if (generation != self->generation) {
    close_confirmation(self);
    gtk_editable_set_text(GTK_EDITABLE(self->value_row), "");
  }
  self->generation = generation;
  g_clear_object(&self->snapshot);
  GError *error = NULL;
  self->snapshot = self->backend.ensure(self->backend.owner, NOSTR_KIND_MUTE_LIST, "", &error);
  guint old = g_list_model_get_n_items(G_LIST_MODEL(self->items));
  g_array_set_size(self->entries, 0);
  g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
  if (self->snapshot) {
    const NostrListDocument *codec = gnostr_list_document_get_codec(self->snapshot);
    for (guint p = 0; p < 2; p++)
      for (gsize i = 0; i < nostr_nip51_document_tag_count(codec, p); i++) {
        const NostrListTag *tag = nostr_nip51_document_tag(codec, p, i);
        const gchar *name = nostr_nip51_tag_field(tag, 0), *value = nostr_nip51_tag_field(tag, 1);
        if (!value)
          continue;
        for (guint k = 0; k < G_N_ELEMENTS(names); k++)
          if (!g_strcmp0(name, names[k])) {
            Entry entry = { p, i };
            g_array_append_val(self->entries, entry);
            g_ptr_array_add(labels, g_strdup(value));
            break;
          }
      }
  }
  g_ptr_array_add(labels, NULL);
  gtk_string_list_splice(self->items, 0, old, (const gchar *const *)labels->pdata);
  gtk_widget_set_visible(GTK_WIDGET(self->empty_label), self->entries->len == 0 && self->snapshot);
  gtk_widget_set_sensitive(GTK_WIDGET(self->kind_row), self->snapshot != NULL);
  gtk_widget_set_sensitive(GTK_WIDGET(self->value_row), self->snapshot != NULL);
  const gchar *projection_error
      = self->backend.visibility_error ? self->backend.visibility_error(self->backend.owner) : NULL;
  set_status(self, error ? error->message : projection_error);
  gtk_widget_set_visible(GTK_WIDGET(self->retry_button),
                         projection_error && self->backend.retry_visibility);
  g_clear_error(&error);
  self->refreshing = FALSE;
}
static void
changed(GObject *owner, GhHiddenPage *self)
{
  (void)owner;
  refresh(self);
}
static void
retry_clicked(GhHiddenPage *self)
{
  if (!self->backend.retry_visibility
      || self->generation != self->backend.generation(self->backend.owner))
    return;
  g_autoptr(GError) error = NULL;
  if (!self->backend.retry_visibility(self->backend.owner, &error))
    set_status(self, error ? error->message : _("Hidden messages could not be updated"));
  else
    refresh(self);
}
static void
kind_changed(GhHiddenPage *self)
{
  guint k = adw_combo_row_get_selected(self->kind_row);
  if (k < G_N_ELEMENTS(names))
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->value_row), _(input_titles[k]));
}
static gboolean
save(GhHiddenPage *self, GNostrListOperation *operation, GError **error)
{
  if (!current(self, error)) {
    gnostr_list_operation_unref(operation);
    return FALSE;
  }
  g_autoptr(GPtrArray) operations
      = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  g_ptr_array_add(operations, operation);
  g_autoptr(GNostrListDocument) saved = self->backend.apply(
      self->backend.owner, NOSTR_KIND_MUTE_LIST, "",
      gnostr_list_document_get_local_revision(self->snapshot), operations, error);
  if (!saved)
    return FALSE;
  refresh(self);
  return TRUE;
}
gboolean
gh_hidden_page_add(GhHiddenPage *self, const gchar *tag, const gchar *value, GError **error)
{
  if (!current(self, error))
    return FALSE;
  gboolean supported = FALSE;
  for (guint i = 0; i < G_N_ELEMENTS(names); i++)
    supported |= !g_strcmp0(tag, names[i]);
  if (!supported || !value || !*value || !g_utf8_validate(value, -1, NULL)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("Enter an account, word, hashtag or message ID"));
    return FALSE;
  }
  g_autofree gchar *normalized = NULL;
  if (!strcmp(tag, "p")) {
    guint8 bytes[32];
    if (nostr_nip19_decode_npub(value, bytes) == 0) {
      normalized = g_malloc0(65);
      for (guint i = 0; i < 32; i++)
        g_snprintf(normalized + i * 2, 3, "%02x", bytes[i]);
    } else {
      normalized = g_ascii_strdown(value, -1);
    }
    const gchar *account = gnostr_list_document_get_author(self->snapshot);
    if (!g_strcmp0(account, normalized)) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          _("The active account cannot hide itself"));
      return FALSE;
    }
  } else if (!strcmp(tag, "t")) {
    g_autofree gchar *folded = g_utf8_casefold(*value == '#' ? value + 1 : value, -1);
    normalized = g_utf8_normalize(folded, -1, G_NORMALIZE_NFC);
    if (!*normalized) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, _("Enter a hashtag"));
      return FALSE;
    }
  } else
    normalized = g_strdup(value);
  const gchar *fields[] = { tag, normalized };
  GNostrListOperation *op
      = gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, TRUE, fields, 2, 0, 0);
  if (!op) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("This hidden entry is not valid"));
    return FALSE;
  }
  return save(self, op, error);
}
gboolean
gh_hidden_page_remove(GhHiddenPage *self, guint position, GError **error)
{
  if (!current(self, error))
    return FALSE;
  if (position >= self->entries->len) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        _("Hidden entry no longer exists"));
    return FALSE;
  }
  Entry entry = g_array_index(self->entries, Entry, position);
  const NostrListDocument *codec = gnostr_list_document_get_codec(self->snapshot);
  const NostrListTag *tag = nostr_nip51_document_tag(codec, entry.private_tag, entry.index);
  guint occurrence = 0;
  for (gsize i = 0; i < entry.index; i++) {
    const NostrListTag *previous = nostr_nip51_document_tag(codec, entry.private_tag, i);
    gboolean same = nostr_nip51_tag_size(tag) == nostr_nip51_tag_size(previous);
    for (gsize j = 0; same && j < nostr_nip51_tag_size(tag); j++)
      same = !g_strcmp0(nostr_nip51_tag_field(tag, j), nostr_nip51_tag_field(previous, j));
    if (same)
      occurrence++;
  }
  GNostrListOperation *op = gnostr_list_operation_remove(entry.private_tag, tag, occurrence);
  return save(self, op, error);
}
static void
remove_clicked(GtkButton *button, GhHiddenPage *self)
{
  guint position = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "hidden-position")) - 1;
  GError *error = NULL;
  if (!gh_hidden_page_remove(self, position, &error)) {
    set_status(self, error->message);
    g_clear_error(&error);
  } else
    gtk_widget_grab_focus(self->entries->len ? GTK_WIDGET(self->list)
                                             : GTK_WIDGET(self->empty_label));
}
static void
setup_item(GtkSignalListItemFactory *factory, GtkListItem *item, GhHiddenPage *self)
{
  (void)factory;
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start(row, 12);
  gtk_widget_set_margin_end(row, 12);
  gtk_widget_set_margin_top(row, 12);
  gtk_widget_set_margin_bottom(row, 12);
  GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(labels, TRUE);
  GtkWidget *title = gtk_label_new(NULL), *subtitle = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(title), 0);
  gtk_label_set_xalign(GTK_LABEL(subtitle), 0);
  gtk_label_set_wrap(GTK_LABEL(title), TRUE);
  gtk_label_set_wrap(GTK_LABEL(subtitle), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(subtitle), PANGO_WRAP_WORD_CHAR);
  gtk_widget_add_css_class(title, "heading");
  gtk_widget_add_css_class(subtitle, "dim-label");
  gtk_box_append(GTK_BOX(labels), title);
  gtk_box_append(GTK_BOX(labels), subtitle);
  gtk_box_append(GTK_BOX(row), labels);
  g_object_set_data(G_OBJECT(item), "title", title);
  g_object_set_data(G_OBJECT(item), "subtitle", subtitle);
  GtkWidget *remove = gtk_button_new_from_icon_name("edit-delete-symbolic");
  gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(remove, _("Show Retained Messages"));
  gtk_accessible_update_property(GTK_ACCESSIBLE(remove), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 _("Remove Hidden Entry"), -1);
  gtk_widget_add_css_class(remove, "flat");
  gtk_box_append(GTK_BOX(row), remove);
  g_signal_connect_object(remove, "clicked", G_CALLBACK(remove_clicked), self, 0);
  g_object_set_data(G_OBJECT(item), "remove-button", remove);
  gtk_list_item_set_child(item, row);
}
static void
bind_item(GtkSignalListItemFactory *factory, GtkListItem *item, GhHiddenPage *self)
{
  (void)factory;
  guint position = gtk_list_item_get_position(item);
  if (!self->snapshot || position >= self->entries->len)
    return;
  Entry entry = g_array_index(self->entries, Entry, position);
  const NostrListTag *tag = nostr_nip51_document_tag(gnostr_list_document_get_codec(self->snapshot),
                                                     entry.private_tag, entry.index);
  const gchar *name = nostr_nip51_tag_field(tag, 0), *value = nostr_nip51_tag_field(tag, 1);
  GtkLabel *title = g_object_get_data(G_OBJECT(item), "title");
  GtkLabel *subtitle = g_object_get_data(G_OBJECT(item), "subtitle");
  for (guint i = 0; i < G_N_ELEMENTS(names); i++)
    if (!g_strcmp0(name, names[i]))
      gtk_label_set_text(title, _(types[i]));
  gtk_label_set_text(subtitle, value);
  g_object_set_data(G_OBJECT(g_object_get_data(G_OBJECT(item), "remove-button")), "hidden-position",
                    GUINT_TO_POINTER(position + 1));
}
static void
confirm_response(AdwAlertDialog *dialog, const gchar *answer, GhHiddenPage *self)
{
  (void)dialog;
  if (!g_strcmp0(answer, "hide")
      && self->generation == self->backend.generation(self->backend.owner)) {
    GError *error = NULL;
    if (!gh_hidden_page_add(self, "p", self->pending_account, &error)) {
      set_status(self, error->message);
      g_clear_error(&error);
    } else
      gtk_editable_set_text(GTK_EDITABLE(self->value_row), "");
  }
  close_confirmation(self);
}
static void
on_add(AdwEntryRow *row, GhHiddenPage *self)
{
  (void)row;
  guint k = adw_combo_row_get_selected(self->kind_row);
  const gchar *value = gtk_editable_get_text(GTK_EDITABLE(self->value_row));
  if (k >= G_N_ELEMENTS(names) || !*value)
    return;
  if (!k) {
    close_confirmation(self);
    self->pending_account = g_strdup(value);
    self->confirmation = ADW_ALERT_DIALOG(g_object_ref_sink(adw_alert_dialog_new(
        _("Hide Account?"),
        _("New messages from this account will not be saved. Removing this entry shows retained "
          "history, but cannot recover discarded messages."))));
    adw_alert_dialog_add_responses(self->confirmation, "cancel", _("_Cancel"), "hide",
                                   _("_Hide Account"), NULL);
    adw_alert_dialog_set_default_response(self->confirmation, "cancel");
    adw_alert_dialog_set_close_response(self->confirmation, "cancel");
    adw_alert_dialog_set_response_appearance(self->confirmation, "hide", ADW_RESPONSE_DESTRUCTIVE);
    g_signal_connect(self->confirmation, "response", G_CALLBACK(confirm_response), self);
    adw_dialog_present(ADW_DIALOG(self->confirmation), GTK_WIDGET(self));
    return;
  }
  GError *error = NULL;
  if (!gh_hidden_page_add(self, names[k], value, &error)) {
    set_status(self, error->message);
    g_clear_error(&error);
  } else
    gtk_editable_set_text(GTK_EDITABLE(self->value_row), "");
}
static void
dispose(GObject *object)
{
  GhHiddenPage *self = GH_HIDDEN_PAGE(object);
  close_confirmation(self);
  if (self->backend.owner)
    g_signal_handlers_disconnect_by_data(self->backend.owner, self);
  g_clear_object(&self->backend.owner);
  g_clear_object(&self->snapshot);
  gtk_widget_dispose_template(GTK_WIDGET(self), GH_TYPE_HIDDEN_PAGE);
  g_clear_object(&self->items);
  g_clear_pointer(&self->entries, g_array_unref);
  G_OBJECT_CLASS(gh_hidden_page_parent_class)->dispose(object);
}
static void
gh_hidden_page_class_init(GhHiddenPageClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = dispose;
  GtkWidgetClass *widget = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(widget, "/org/nostr/Groundhog/ui/gh-hidden-page.ui");
  gtk_widget_class_bind_template_child(widget, GhHiddenPage, kind_row);
  gtk_widget_class_bind_template_child(widget, GhHiddenPage, value_row);
  gtk_widget_class_bind_template_child(widget, GhHiddenPage, status);
  gtk_widget_class_bind_template_child(widget, GhHiddenPage, empty_label);
  gtk_widget_class_bind_template_child(widget, GhHiddenPage, list);
  gtk_widget_class_bind_template_child(widget, GhHiddenPage, retry_button);
  gtk_widget_class_bind_template_callback(widget, on_add);
  gtk_widget_class_bind_template_callback(widget, retry_clicked);
}
static void
gh_hidden_page_init(GhHiddenPage *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->entries = g_array_new(FALSE, FALSE, sizeof(Entry));
  self->items = gtk_string_list_new(NULL);
  GtkNoSelection *selection = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(self->items)));
  gtk_list_view_set_model(self->list, GTK_SELECTION_MODEL(selection));
  g_object_unref(selection);
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(setup_item), self);
  g_signal_connect(factory, "bind", G_CALLBACK(bind_item), self);
  gtk_list_view_set_factory(self->list, factory);
  g_object_unref(factory);
  const gchar *labels[]
      = { _("Account"), _("Word or Phrase"), _("Hashtag"), _("Message Thread"), NULL };
  GtkStringList *kinds = gtk_string_list_new(labels);
  adw_combo_row_set_model(self->kind_row, G_LIST_MODEL(kinds));
  g_object_unref(kinds);
  g_signal_connect_swapped(self->kind_row, "notify::selected", G_CALLBACK(kind_changed), self);
}
GhHiddenPage *
gh_hidden_page_new(const GhListsBackend *backend)
{
  GhHiddenPage *self = g_object_new(GH_TYPE_HIDDEN_PAGE, NULL);
  self->backend = *backend;
  self->backend.owner = g_object_ref(backend->owner);
  self->generation = backend->generation(backend->owner);
  g_signal_connect(backend->owner, "changed", G_CALLBACK(changed), self);
  refresh(self);
  return self;
}
