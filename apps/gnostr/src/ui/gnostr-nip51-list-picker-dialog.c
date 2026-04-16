/* gnostr-nip51-list-picker-dialog.c — Pick a NIP-51 list to import.
 *
 * SPDX-License-Identifier: MIT
 *
 * nostrc-yg8j.8.
 */

#define G_LOG_DOMAIN "gnostr-nip51-list-picker"

#include "gnostr-nip51-list-picker-dialog.h"

#include <glib/gi18n.h>
#include <stddef.h>
#include <string.h>

#include <nostr/nip51/nip51.h>

#include "../model/gnostr-nip51-loader.h"

struct _GnostrNip51ListPickerDialog {
  AdwDialog parent_instance;

  /* Template children. The spinner is driven entirely by the blueprint
   * (spinning: true + stack page visibility), so no C-side binding is
   * needed. */
  GtkButton    *btn_cancel;
  GtkStack     *stack;
  AdwStatusPage *status_empty;
  GtkListBox   *list_box;

  /* State. */
  gchar                 *pubkey_hex;
  GCancellable          *cancellable;
  GnostrNip51UserLists  *lists;        /* NULL until loaded. */
};

G_DEFINE_FINAL_TYPE(GnostrNip51ListPickerDialog,
                    gnostr_nip51_list_picker_dialog,
                    ADW_TYPE_DIALOG)

enum {
  SIGNAL_LIST_SELECTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ------------------------------------------------------------------------
 * Row construction
 * ------------------------------------------------------------------------ */

/* Row data — attached to the GtkListBoxRow so we can map activation
 * back to the right list entry. We don't own the NostrList pointer;
 * the dialog does. */
typedef struct {
  const NostrList *list;   /* borrowed */
  gsize            index;  /* index inside @self->lists */
} RowData;

static void
row_data_free(gpointer data)
{
  g_free(data);
}

/* Count how many p-entries a list has (for the subtitle). */
static gsize
count_p_entries(const NostrList *list)
{
  if (!list || !list->entries) return 0;
  gsize n = 0;
  for (gsize i = 0; i < list->count; i++) {
    const NostrListEntry *e = list->entries[i];
    if (e && e->tag_name && strcmp(e->tag_name, "p") == 0) n++;
  }
  return n;
}

static GtkWidget *
build_row_for_list(GnostrNip51ListPickerDialog *self,
                   const NostrList *list,
                   gsize index)
{
  GtkWidget *row = adw_action_row_new();
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);

  /* Title: user-provided title → identifier → "Unnamed list". */
  const char *title = (list->title && *list->title) ? list->title
                      : (list->identifier && *list->identifier) ? list->identifier
                      : _("Unnamed list");
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);

  /* Subtitle: "<n> people" + description if present. */
  gsize p = count_p_entries(list);
  g_autofree gchar *subtitle = NULL;
  if (list->description && *list->description) {
    subtitle = g_strdup_printf(
        ngettext("%zu person · %s", "%zu people · %s", p),
        p, list->description);
  } else {
    subtitle = g_strdup_printf(
        ngettext("%zu person", "%zu people", p), p);
  }
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

  /* Chevron as a visual hint the row is actionable. */
  GtkWidget *chevron = gtk_image_new_from_icon_name("go-next-symbolic");
  gtk_widget_add_css_class(chevron, "dim-label");
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), chevron);

  RowData *rd = g_new0(RowData, 1);
  rd->list = list;
  rd->index = index;
  g_object_set_data_full(G_OBJECT(row), "row-data", rd, row_data_free);

  (void)self;  /* row activation is handled by the list_box signal */
  return row;
}

/* ------------------------------------------------------------------------
 * Activation
 * ------------------------------------------------------------------------ */

static void
on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  (void)box;
  GnostrNip51ListPickerDialog *self =
      GNOSTR_NIP51_LIST_PICKER_DIALOG(user_data);

  RowData *rd = g_object_get_data(G_OBJECT(row), "row-data");
  if (!rd || !rd->list) return;

  const NostrList *list = rd->list;
  const char *title = (list->title && *list->title)
                          ? list->title
                          : (list->identifier ? list->identifier : "");
  const char *identifier = list->identifier ? list->identifier : "";

  /* Emit first, then close — the handler may want to keep the dialog
   * visible if something went wrong. (In practice it doesn't, but
   * this ordering preserves the option.) */
  g_signal_emit(self, signals[SIGNAL_LIST_SELECTED], 0,
                title, identifier, (gpointer)list);

  adw_dialog_close(ADW_DIALOG(self));
}

/* ------------------------------------------------------------------------
 * Load completion
 * ------------------------------------------------------------------------ */

static void
populate_lists_ui(GnostrNip51ListPickerDialog *self)
{
  /* Clear any prior children (defensive — load only runs once). */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->list_box))) != NULL)
    gtk_list_box_remove(self->list_box, child);

  gsize n = self->lists ? gnostr_nip51_user_lists_get_count(self->lists) : 0;
  if (n == 0) {
    gtk_stack_set_visible_child_name(self->stack, "empty");
    return;
  }

  for (gsize i = 0; i < n; i++) {
    const NostrList *list = gnostr_nip51_user_lists_get_nth(self->lists, i);
    if (!list) continue;
    GtkWidget *row = build_row_for_list(self, list, i);
    gtk_list_box_append(self->list_box, row);
  }

  gtk_stack_set_visible_child_name(self->stack, "list");
}

static void
on_lists_loaded(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  (void)source_object;

  /* We took a strong ref when kicking off the load (see kick_off_load);
   * @self is therefore guaranteed alive here. If the user clicked
   * Cancel mid-load the dialog is hidden but not yet finalized, and
   * mutating its template widgets is harmless — GTK silently accepts
   * the updates and they're torn down when the autoptr below drops
   * the last ref. */
  g_autoptr(GnostrNip51ListPickerDialog) self =
      GNOSTR_NIP51_LIST_PICKER_DIALOG(user_data);

  g_autoptr(GError) err = NULL;
  GnostrNip51UserLists *lists =
      gnostr_nip51_load_user_lists_finish(res, &err);

  if (err) {
    g_warning("nip51-list-picker: load failed: %s", err->message);
    /* Fall through: show the empty state rather than leaving the
     * spinner up forever. */
  }

  g_clear_pointer(&self->lists, gnostr_nip51_user_lists_free);
  self->lists = lists;

  populate_lists_ui(self);
}

/* ------------------------------------------------------------------------
 * Construction
 * ------------------------------------------------------------------------ */

static void
on_cancel_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnostrNip51ListPickerDialog *self =
      GNOSTR_NIP51_LIST_PICKER_DIALOG(user_data);
  adw_dialog_close(ADW_DIALOG(self));
}

static void
kick_off_load(GnostrNip51ListPickerDialog *self)
{
  if (!self->pubkey_hex || strlen(self->pubkey_hex) != 64) {
    /* No / invalid pubkey — show empty state straight away; the
     * load API would reject the call anyway. */
    gtk_stack_set_visible_child_name(self->stack, "empty");
    return;
  }

  self->cancellable = g_cancellable_new();
  /* Keep @self alive for the duration of the async call. on_lists_loaded
   * takes ownership via g_autoptr on its parameter. */
  gnostr_nip51_load_user_lists_async(self->pubkey_hex,
                                      self->cancellable,
                                      on_lists_loaded,
                                      g_object_ref(self));
}

/* ------------------------------------------------------------------------
 * GObject plumbing
 * ------------------------------------------------------------------------ */

static void
gnostr_nip51_list_picker_dialog_dispose(GObject *object)
{
  GnostrNip51ListPickerDialog *self =
      GNOSTR_NIP51_LIST_PICKER_DIALOG(object);

  if (self->cancellable)
    g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);

  G_OBJECT_CLASS(gnostr_nip51_list_picker_dialog_parent_class)->dispose(object);
}

static void
gnostr_nip51_list_picker_dialog_finalize(GObject *object)
{
  GnostrNip51ListPickerDialog *self =
      GNOSTR_NIP51_LIST_PICKER_DIALOG(object);

  g_clear_pointer(&self->pubkey_hex, g_free);
  g_clear_pointer(&self->lists, gnostr_nip51_user_lists_free);

  G_OBJECT_CLASS(gnostr_nip51_list_picker_dialog_parent_class)->finalize(object);
}

static void
gnostr_nip51_list_picker_dialog_class_init(GnostrNip51ListPickerDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose  = gnostr_nip51_list_picker_dialog_dispose;
  object_class->finalize = gnostr_nip51_list_picker_dialog_finalize;

  gtk_widget_class_set_template_from_resource(
      widget_class,
      "/org/gnostr/ui/ui/dialogs/gnostr-nip51-list-picker-dialog.ui");

  gtk_widget_class_bind_template_child(widget_class,
      GnostrNip51ListPickerDialog, btn_cancel);
  gtk_widget_class_bind_template_child(widget_class,
      GnostrNip51ListPickerDialog, stack);
  gtk_widget_class_bind_template_child(widget_class,
      GnostrNip51ListPickerDialog, status_empty);
  gtk_widget_class_bind_template_child(widget_class,
      GnostrNip51ListPickerDialog, list_box);

  /**
   * GnostrNip51ListPickerDialog::list-selected:
   * @self: the dialog
   * @title: display title (nullable → empty string)
   * @identifier: the list's d-tag (nullable → empty string)
   * @nostr_list: (type gpointer): borrowed <type>NostrList*</type>;
   *   only valid during the emission.
   */
  signals[SIGNAL_LIST_SELECTED] = g_signal_new(
      "list-selected",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL,
      G_TYPE_NONE,
      3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER);
}

static void
gnostr_nip51_list_picker_dialog_init(GnostrNip51ListPickerDialog *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));

  g_signal_connect(self->btn_cancel, "clicked",
                   G_CALLBACK(on_cancel_clicked), self);
  g_signal_connect(self->list_box, "row-activated",
                   G_CALLBACK(on_row_activated), self);

  /* Start on the loading page; kick_off_load() may immediately flip
   * to "empty" when no pubkey is set. */
  gtk_stack_set_visible_child_name(self->stack, "loading");
}

/* ------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */

GtkWidget *
gnostr_nip51_list_picker_dialog_new(const gchar *pubkey_hex)
{
  GnostrNip51ListPickerDialog *self =
      g_object_new(GNOSTR_TYPE_NIP51_LIST_PICKER_DIALOG, NULL);

  self->pubkey_hex = (pubkey_hex && *pubkey_hex)
                         ? g_ascii_strdown(pubkey_hex, -1)
                         : NULL;

  kick_off_load(self);
  return GTK_WIDGET(self);
}
