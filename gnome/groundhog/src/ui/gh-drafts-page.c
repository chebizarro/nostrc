#include "gh-drafts-page.h"

#include <glib/gi18n.h>
#include <string.h>

#define TITLE_CHARS 80
#define TOOLTIP_CHARS 600

struct _GhDraftsPage {
  AdwNavigationPage parent_instance;
  GhDraftsBackend backend;
  guint64 generation;
  AdwActionRow *sync_row;
  GtkButton *retry_button;
  AdwPreferencesGroup *conflicts_group;
  GtkListBox *conflicts_list;
  AdwStatusPage *empty;
};

G_DEFINE_FINAL_TYPE(GhDraftsPage, gh_drafts_page, ADW_TYPE_NAVIGATION_PAGE)

void
gh_drafts_conflict_free(GhDraftsConflict *conflict)
{
  if (!conflict)
    return;
  g_free(conflict->text);
  g_strfreev(conflict->npubs);
  g_free(conflict->room_key);
  g_free(conflict);
}

/* A discarded copy, kept for the toast's Undo. */
typedef struct {
  GhDraftsPage *page; /* weak */
  gchar *room_key, *text;
} Discarded;

static void
discarded_free(gpointer data)
{
  Discarded *d = data;
  if (d->page)
    g_object_remove_weak_pointer(G_OBJECT(d->page), (gpointer *) &d->page);
  g_free(d->room_key);
  g_free(d->text);
  g_free(d);
}

static void
discarded_closure_notify(gpointer data, GClosure *closure)
{
  (void) closure;
  discarded_free(data);
}

static AdwPreferencesDialog *
dialog_of(GhDraftsPage *self)
{
  GtkWidget *dialog = gtk_widget_get_ancestor(GTK_WIDGET(self), ADW_TYPE_PREFERENCES_DIALOG);
  return dialog ? ADW_PREFERENCES_DIALOG(dialog) : NULL;
}

static void
toast(GhDraftsPage *self, AdwToast *toast)
{
  AdwPreferencesDialog *dialog = dialog_of(self);
  if (dialog)
    adw_preferences_dialog_add_toast(dialog, toast);
  else
    g_object_unref(g_object_ref_sink(toast));
}

static gboolean
current(GhDraftsPage *self)
{
  return self->backend.owner && self->generation == self->backend.generation(self->backend.owner);
}

/* "npub1abcde…wxyz", as the conversation list abbreviates. */
static gchar *
short_npub(const gchar *npub)
{
  gsize length = strlen(npub);
  return length > 16 ? g_strdup_printf("%.10s…%s", npub, npub + length - 4) : g_strdup(npub);
}

static gchar *
first_line(const gchar *text, glong max_chars)
{
  const gchar *end = strchr(text, '\n');
  g_autofree gchar *line = end ? g_strndup(text, end - text) : g_strdup(text);
  if (g_utf8_strlen(line, -1) <= max_chars && !end)
    return g_steal_pointer(&line);
  gchar *cut = g_utf8_offset_to_pointer(line, MIN(g_utf8_strlen(line, -1), max_chars));
  *cut = '\0';
  return g_strdup_printf("%s…", line);
}

/* After a row went (the owner's "changed" rebuilt the list meanwhile): the
 * next surviving row, else the previous, else the page's first row. */
static void
focus_index(GhDraftsPage *self, gint index)
{
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(self->conflicts_list, index);
  if (!row && index > 0)
    row = gtk_list_box_get_row_at_index(self->conflicts_list, index - 1);
  gtk_widget_grab_focus(row ? GTK_WIDGET(row) : GTK_WIDGET(self->sync_row));
}

static void refresh(GhDraftsPage *self);

static void
undo_discard(AdwToast *toast, gpointer data)
{
  (void) toast;
  Discarded *d = data;
  if (!d->page || !current(d->page))
    return;
  g_autoptr(GError) error = NULL;
  if (!d->page->backend.keep(d->page->backend.owner, d->room_key, d->text, &error))
    g_message("Groundhog could not keep a draft copy again: %s", error->message);
}

static void
on_discard(GtkButton *button, GhDraftsPage *self)
{
  if (!current(self))
    return;
  GhDraftsConflict *conflict = g_object_get_data(G_OBJECT(button), "conflict");
  GtkListBoxRow *row = GTK_LIST_BOX_ROW(gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_LIST_BOX_ROW));
  gint index = gtk_list_box_row_get_index(row);
  Discarded *d = g_new0(Discarded, 1);
  d->page = self;
  g_object_add_weak_pointer(G_OBJECT(self), (gpointer *) &d->page);
  d->room_key = g_strdup(conflict->room_key);
  d->text = g_strdup(conflict->text);
  g_autoptr(GError) error = NULL;
  if (!self->backend.discard(self->backend.owner, conflict->id, &error)) {
    discarded_free(d);
    AdwToast *failed = adw_toast_new(_("Draft copy not discarded — check account storage"));
    toast(self, failed);
    return; /* the row and its focus stay */
  }
  focus_index(self, index);
  AdwToast *done = adw_toast_new(_("Draft copy discarded"));
  adw_toast_set_button_label(done, _("Undo"));
  g_signal_connect_data(done, "button-clicked", G_CALLBACK(undo_discard), d,
                        discarded_closure_notify, 0);
  toast(self, done);
}

static void
on_restore(GtkButton *button, GhDraftsPage *self)
{
  if (!current(self))
    return;
  GhDraftsConflict *conflict = g_object_get_data(G_OBJECT(button), "conflict");
  GtkListBoxRow *row = GTK_LIST_BOX_ROW(gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_LIST_BOX_ROW));
  gint index = gtk_list_box_row_get_index(row);
  g_autoptr(GError) error = NULL;
  if (!self->backend.restore(self->backend.owner, conflict->id, &error)) {
    toast(self, adw_toast_new(_("Draft not restored — check account storage")));
    return;
  }
  focus_index(self, index);
  toast(self, adw_toast_new(_("Draft restored to its conversation")));
}

static GtkWidget *
conflict_row(GhDraftsPage *self, GhDraftsConflict *conflict)
{
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
  /* Draft text is the user's: never markup. */
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  g_autofree gchar *title = first_line(conflict->text, TITLE_CHARS);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_action_row_set_title_lines(row, 1);
  g_autoptr(GPtrArray) shorts = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; conflict->npubs && conflict->npubs[i]; i++)
    g_ptr_array_add(shorts, short_npub(conflict->npubs[i]));
  g_ptr_array_add(shorts, NULL);
  g_autofree gchar *people = g_strjoinv(", ", (gchar **) shorts->pdata);
  g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(conflict->saved_at);
  g_autofree gchar *date = when ? g_date_time_format(when, "%x %R") : g_strdup("");
  /* TRANSLATORS: whose conversation the draft copy belongs to, then when it was kept. */
  g_autofree gchar *subtitle = g_strdup_printf(_("Draft for %s · %s"), people, date);
  adw_action_row_set_subtitle(row, subtitle);
  adw_action_row_set_subtitle_lines(row, 1);
  if (g_utf8_strlen(conflict->text, -1) > TITLE_CHARS || strchr(conflict->text, '\n')) {
    g_autofree gchar *tip = first_line(conflict->text, TOOLTIP_CHARS);
    gchar *full = strchr(conflict->text, '\n') && g_utf8_strlen(conflict->text, -1) <= TOOLTIP_CHARS
                    ? g_strdup(conflict->text) : g_steal_pointer(&tip);
    gtk_widget_set_tooltip_text(GTK_WIDGET(row), full);
    g_free(full);
  }

  GtkWidget *restore = gtk_button_new_with_mnemonic(_("_Restore"));
  gtk_widget_set_valign(restore, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(restore, "flat");
  g_object_set_data_full(G_OBJECT(restore), "conflict", conflict, (GDestroyNotify) gh_drafts_conflict_free);
  g_signal_connect(restore, "clicked", G_CALLBACK(on_restore), self);
  adw_action_row_add_suffix(row, restore);

  GtkWidget *discard = gtk_button_new_from_icon_name("user-trash-symbolic");
  gtk_widget_set_valign(discard, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(discard, "flat");
  gtk_widget_set_tooltip_text(discard, _("Discard"));
  gtk_accessible_update_property(GTK_ACCESSIBLE(discard), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 _("Discard draft copy"), -1);
  g_object_set_data(G_OBJECT(discard), "conflict", conflict); /* owned by the Restore button */
  g_signal_connect(discard, "clicked", G_CALLBACK(on_discard), self);
  adw_action_row_add_suffix(row, discard);
  adw_action_row_set_activatable_widget(row, restore);
  return GTK_WIDGET(row);
}

static void
refresh(GhDraftsPage *self)
{
  GObject *owner = self->backend.owner;
  const gchar *status = owner ? self->backend.status(owner) : NULL;
  gboolean enabled = owner && self->backend.enabled(owner);
  adw_action_row_set_subtitle(self->sync_row, status ? status :
    enabled ? _("Up to date") : _("Off — turn on Sync Drafts and Lists under Account & Storage"));
  gtk_widget_set_visible(GTK_WIDGET(self->retry_button), owner && self->backend.can_retry(owner));

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->conflicts_list))))
    gtk_list_box_remove(self->conflicts_list, child);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) conflicts = owner ? self->backend.list_conflicts(owner, &error) : NULL;
  if (conflicts) {
    g_ptr_array_set_free_func(conflicts, NULL); /* each row owns its copy */
    for (guint i = 0; i < conflicts->len; i++)
      gtk_list_box_append(self->conflicts_list, conflict_row(self, conflicts->pdata[i]));
  } else if (error)
    g_message("Groundhog could not list draft copies: %s", error->message);
  gboolean any = conflicts && conflicts->len > 0;
  gtk_widget_set_visible(GTK_WIDGET(self->conflicts_group), any);
  gtk_widget_set_visible(GTK_WIDGET(self->empty), !any);
}

static void
on_retry(GtkButton *button, GhDraftsPage *self)
{
  (void) button;
  if (current(self))
    self->backend.retry(self->backend.owner);
}

static void
owner_changed(GObject *owner, GhDraftsPage *self)
{
  (void) owner;
  if (!current(self)) {
    /* Another account (or none): this page spoke for the previous one. */
    AdwPreferencesDialog *dialog = dialog_of(self);
    if (dialog)
      adw_preferences_dialog_pop_subpage(dialog);
    return;
  }
  refresh(self);
}

gchar *
gh_drafts_page_describe(const GhDraftsBackend *backend)
{
  g_return_val_if_fail(backend != NULL && backend->owner != NULL, NULL);
  const gchar *status = backend->status(backend->owner);
  if (status)
    return g_strdup(status);
  if (!backend->enabled(backend->owner))
    return g_strdup(_("Not syncing"));
  g_autoptr(GPtrArray) conflicts = backend->list_conflicts(backend->owner, NULL);
  guint n = conflicts ? conflicts->len : 0;
  if (n)
    return g_strdup_printf(ngettext("%u conflict copy", "%u conflict copies", n), n);
  return g_strdup(_("Up to date"));
}

GhDraftsPage *
gh_drafts_page_new(const GhDraftsBackend *backend)
{
  g_return_val_if_fail(backend != NULL && G_IS_OBJECT(backend->owner), NULL);
  GhDraftsPage *self = g_object_new(GH_TYPE_DRAFTS_PAGE, NULL);
  self->backend = *backend;
  self->backend.owner = g_object_ref(backend->owner);
  self->generation = backend->generation(backend->owner);
  g_signal_connect(self->backend.owner, "changed", G_CALLBACK(owner_changed), self);
  refresh(self);
  return self;
}

static void
gh_drafts_page_dispose(GObject *object)
{
  GhDraftsPage *self = GH_DRAFTS_PAGE(object);
  if (self->backend.owner) {
    g_signal_handlers_disconnect_by_data(self->backend.owner, self);
    g_clear_object(&self->backend.owner);
  }
  gtk_widget_dispose_template(GTK_WIDGET(self), GH_TYPE_DRAFTS_PAGE);
  G_OBJECT_CLASS(gh_drafts_page_parent_class)->dispose(object);
}

static void
gh_drafts_page_class_init(GhDraftsPageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  G_OBJECT_CLASS(klass)->dispose = gh_drafts_page_dispose;
  gtk_widget_class_set_template_from_resource(widget_class, "/org/nostr/Groundhog/ui/gh-drafts-page.ui");
  gtk_widget_class_bind_template_child(widget_class, GhDraftsPage, sync_row);
  gtk_widget_class_bind_template_child(widget_class, GhDraftsPage, retry_button);
  gtk_widget_class_bind_template_child(widget_class, GhDraftsPage, conflicts_group);
  gtk_widget_class_bind_template_child(widget_class, GhDraftsPage, conflicts_list);
  gtk_widget_class_bind_template_child(widget_class, GhDraftsPage, empty);
}

static void
gh_drafts_page_init(GhDraftsPage *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  g_signal_connect(self->retry_button, "clicked", G_CALLBACK(on_retry), self);
}
