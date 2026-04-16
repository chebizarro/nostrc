/* gnostr-filter-set-dialog.c — AdwDialog for creating / editing a
 * custom GnostrFilterSet.
 *
 * SPDX-License-Identifier: MIT
 *
 * Form layout is driven by `gnostr-filter-set-dialog.blp`. The only
 * programmatic pieces are:
 *
 *   - A horizontal row of `GtkCheckButton` kind-pickers inserted into
 *     the `kinds_box` slot (so users can tick 1 / 6 / 20 / 30023 with
 *     a single click).
 *   - A `GtkTextView` inside a `GtkScrolledWindow` dropped into the
 *     `authors_box` slot (one hex pubkey / npub per line).
 *
 * Save persists the newly-built GnostrFilterSet via the process-wide
 * #GnostrFilterSetManager and emits the "filter-set-saved" signal with
 * the resulting id so the filter switcher (or main window) can route
 * the user straight into the new feed.
 *
 * nostrc-yg8j.6.
 */

#define G_LOG_DOMAIN "gnostr-filter-set-dialog"

#include "gnostr-filter-set-dialog.h"

#include <glib/gi18n.h>
#include <stdlib.h>

#include <nostr-gobject-1.0/nostr_utils.h>

#include "../model/gnostr-filter-set-manager.h"
#include "../util/utils.h"

/* Common kind presets shown as quick-check pills. Anything else goes
 * into the free-form "Other kinds" entry row. */
static const struct {
  gint kind;
  const gchar *label;
} kKindPresets[] = {
  { 1,     "Notes"    },  /* short text notes */
  { 6,     "Reposts"  },  /* kind 6 reposts   */
  { 20,    "Pictures" },  /* picture events   */
  { 30023, "Articles" },  /* long-form NIP-23 */
};

struct _GnostrFilterSetDialog {
  AdwDialog parent_instance;

  /* Template children. */
  GtkLabel       *lbl_title;
  GtkButton      *btn_cancel;
  GtkButton      *btn_save;
  AdwEntryRow    *row_name;
  AdwEntryRow    *row_description;
  AdwEntryRow    *row_hashtags;
  GtkBox         *kinds_box;        /* populated with CheckButtons */
  AdwEntryRow    *row_other_kinds;
  GtkBox         *authors_box;      /* populated with scroll + textview */
  AdwExpanderRow *advanced_row;
  AdwSpinRow     *limit_row;
  AdwEntryRow    *row_since;
  AdwEntryRow    *row_until;
  GtkLabel       *summary_label;

  /* Built programmatically inside kinds_box / authors_box. */
  GtkCheckButton *kind_checks[G_N_ELEMENTS(kKindPresets)];
  GtkTextBuffer  *authors_buffer;

  /* Editing state. NULL → create mode. */
  gchar          *editing_id;
};

G_DEFINE_FINAL_TYPE(GnostrFilterSetDialog, gnostr_filter_set_dialog, ADW_TYPE_DIALOG)

enum {
  SIGNAL_FILTER_SET_SAVED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ------------------------------------------------------------------------
 * Parse helpers
 * ------------------------------------------------------------------------ */

/* Tokenize @text on whitespace and ','. Returns a newly-allocated
 * GStrv (NULL-terminated) of trimmed, non-empty tokens; caller frees
 * with g_strfreev(). `#` prefixes on hashtags are stripped. */
static gchar **
tokenize_multi(const gchar *text, gboolean strip_hash_prefix)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  if (text && *text) {
    /* g_strsplit_set returns an array that may include empty strings. */
    gchar **toks = g_strsplit_set(text, " ,\t\n\r", -1);
    for (gchar **p = toks; *p; p++) {
      gchar *t = g_strstrip(*p);
      if (!*t) continue;
      if (strip_hash_prefix && *t == '#') t++;
      if (!*t) continue;
      g_ptr_array_add(out, g_strdup(t));
    }
    g_strfreev(toks);
  }
  g_ptr_array_add(out, NULL);
  return (gchar **)g_ptr_array_free(out, FALSE);
}

/* Parse the authors text buffer. Normalizes each non-empty line via
 * gnostr_ensure_hex_pubkey(); invalid lines are silently dropped.
 * Returns NULL-terminated GStrv; free with g_strfreev(). */
static gchar **
parse_authors_buffer(GtkTextBuffer *buffer)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  if (buffer) {
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *raw = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    gchar **lines = g_strsplit(raw, "\n", -1);
    for (gchar **p = lines; *p; p++) {
      gchar *line = g_strstrip(*p);
      if (!*line) continue;
      gchar *hex = gnostr_ensure_hex_pubkey(line);
      if (hex && *hex)
        g_ptr_array_add(out, hex);
      else
        g_free(hex);
    }
    g_strfreev(lines);
    g_free(raw);
  }
  g_ptr_array_add(out, NULL);
  return (gchar **)g_ptr_array_free(out, FALSE);
}

/* Collect the ticked preset kinds + parsed "other kinds" numbers into
 * a newly-allocated gint array. Sets *@n_out to the count. Returns
 * NULL when no kinds are selected (caller treats this as "no
 * constraint"). Caller frees with g_free(). */
static gint *
collect_kinds(GnostrFilterSetDialog *self, gsize *n_out)
{
  GArray *out = g_array_new(FALSE, FALSE, sizeof(gint));
  /* Presets. */
  for (gsize i = 0; i < G_N_ELEMENTS(kKindPresets); i++) {
    if (self->kind_checks[i] &&
        gtk_check_button_get_active(self->kind_checks[i])) {
      gint k = kKindPresets[i].kind;
      g_array_append_val(out, k);
    }
  }
  /* Other kinds entry: comma / space separated ints. */
  const gchar *other = self->row_other_kinds
      ? gtk_editable_get_text(GTK_EDITABLE(self->row_other_kinds))
      : NULL;
  gchar **toks = tokenize_multi(other, FALSE);
  for (gchar **p = toks; *p; p++) {
    char *endp = NULL;
    long v = strtol(*p, &endp, 10);
    if (endp && *endp == '\0' && v >= 0 && v <= G_MAXINT) {
      gint k = (gint)v;
      /* Dedup against what we already collected. */
      gboolean seen = FALSE;
      for (guint j = 0; j < out->len; j++) {
        if (g_array_index(out, gint, j) == k) { seen = TRUE; break; }
      }
      if (!seen)
        g_array_append_val(out, k);
    }
  }
  g_strfreev(toks);

  if (out->len == 0) {
    g_array_free(out, TRUE);
    if (n_out) *n_out = 0;
    return NULL;
  }
  gsize n = out->len;
  gint *buf = (gint *)g_array_free(out, FALSE);
  if (n_out) *n_out = n;
  return buf;
}

/* Parse an AdwEntryRow's contents as a non-negative integer. Returns
 * `fallback` if empty or unparsable. */
static gint64
entry_to_int64(AdwEntryRow *row, gint64 fallback)
{
  if (!row) return fallback;
  const gchar *text = gtk_editable_get_text(GTK_EDITABLE(row));
  if (!text || !*text) return fallback;
  char *endp = NULL;
  gint64 v = g_ascii_strtoll(text, &endp, 10);
  if (!endp || *endp != '\0') return fallback;
  return v;
}

/* ------------------------------------------------------------------------
 * Summary / validation
 * ------------------------------------------------------------------------ */

/* Build a GnostrFilterSet from the current form state. Caller owns. */
static GnostrFilterSet *
build_filter_set_from_form(GnostrFilterSetDialog *self)
{
  GnostrFilterSet *fs = gnostr_filter_set_new();

  const gchar *name = gtk_editable_get_text(GTK_EDITABLE(self->row_name));
  gnostr_filter_set_set_name(fs, name ? name : "");

  const gchar *desc = gtk_editable_get_text(GTK_EDITABLE(self->row_description));
  if (desc && *desc)
    gnostr_filter_set_set_description(fs, desc);

  /* Hashtags. */
  const gchar *htext = gtk_editable_get_text(GTK_EDITABLE(self->row_hashtags));
  gchar **tags = tokenize_multi(htext, TRUE);
  if (tags && tags[0])
    gnostr_filter_set_set_hashtags(fs, (const gchar * const *)tags);
  g_strfreev(tags);

  /* Kinds. */
  gsize n_kinds = 0;
  gint *kinds = collect_kinds(self, &n_kinds);
  if (kinds && n_kinds > 0)
    gnostr_filter_set_set_kinds(fs, kinds, n_kinds);
  g_free(kinds);

  /* Authors. */
  gchar **authors = parse_authors_buffer(self->authors_buffer);
  if (authors && authors[0])
    gnostr_filter_set_set_authors(fs, (const gchar * const *)authors);
  g_strfreev(authors);

  /* Advanced: limit / since / until. */
  gint limit = self->limit_row
      ? (gint)adw_spin_row_get_value(self->limit_row)
      : 0;
  if (limit > 0)
    gnostr_filter_set_set_limit(fs, limit);

  gint64 since = entry_to_int64(self->row_since, 0);
  if (since > 0)
    gnostr_filter_set_set_since(fs, since);

  gint64 until = entry_to_int64(self->row_until, 0);
  if (until > 0)
    gnostr_filter_set_set_until(fs, until);

  gnostr_filter_set_set_source(fs, GNOSTR_FILTER_SET_SOURCE_CUSTOM);
  return fs;
}

/* Rebuild the summary label from the current form contents and
 * enable/disable the Save button based on name validity. */
static void
refresh_summary(GnostrFilterSetDialog *self)
{
  /* gtk_editable_get_text returns a pointer to the widget's internal
   * storage — never mutate it (not even via g_strchug). Copy first. */
  g_autofree gchar *name_trimmed =
      g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->row_name)));
  if (name_trimmed) g_strstrip(name_trimmed);
  gboolean has_name = name_trimmed && *name_trimmed;
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_save), has_name);

  g_autoptr(GnostrFilterSet) fs = build_filter_set_from_form(self);

  GString *s = g_string_new(NULL);

  /* Kinds. */
  gsize nk = 0;
  const gint *kinds = gnostr_filter_set_get_kinds(fs, &nk);
  if (kinds && nk > 0) {
    g_string_append(s, _("Kinds: "));
    for (gsize i = 0; i < nk; i++) {
      if (i > 0) g_string_append(s, ", ");
      g_string_append_printf(s, "%d", kinds[i]);
    }
    g_string_append(s, "\n");
  }

  /* Authors. */
  const gchar * const *authors = gnostr_filter_set_get_authors(fs);
  gsize n_auth = 0;
  if (authors) { for (const gchar * const *p = authors; *p; p++) n_auth++; }
  if (n_auth > 0) {
    g_string_append_printf(s, ngettext("Authors: %zu pubkey\n",
                                        "Authors: %zu pubkeys\n",
                                        n_auth),
                           n_auth);
  }

  /* Hashtags. */
  const gchar * const *tags = gnostr_filter_set_get_hashtags(fs);
  if (tags && tags[0]) {
    g_string_append(s, _("Hashtags: "));
    for (const gchar * const *p = tags; *p; p++) {
      if (p != tags) g_string_append(s, ", ");
      g_string_append_c(s, '#');
      g_string_append(s, *p);
    }
    g_string_append(s, "\n");
  }

  /* Advanced summary. */
  gint limit = gnostr_filter_set_get_limit(fs);
  if (limit > 0)
    g_string_append_printf(s, _("Limit: %d\n"), limit);

  gint64 since = gnostr_filter_set_get_since(fs);
  if (since > 0)
    g_string_append_printf(s, _("Since: %" G_GINT64_FORMAT "\n"), since);

  gint64 until = gnostr_filter_set_get_until(fs);
  if (until > 0)
    g_string_append_printf(s, _("Until: %" G_GINT64_FORMAT "\n"), until);

  if (s->len == 0) {
    g_string_append(s, has_name
                        ? _("No criteria — will match all notes.")
                        : _("Enter a name to enable save."));
  } else {
    /* Strip trailing newline. */
    if (s->str[s->len - 1] == '\n')
      g_string_truncate(s, s->len - 1);
  }

  gtk_label_set_text(self->summary_label, s->str);
  g_string_free(s, TRUE);
}

/* ------------------------------------------------------------------------
 * Change handlers
 * ------------------------------------------------------------------------ */

static void
on_any_changed(GtkWidget *widget, gpointer user_data)
{
  (void)widget;
  refresh_summary(GNOSTR_FILTER_SET_DIALOG(user_data));
}

static void
on_kind_toggled(GtkCheckButton *btn, gpointer user_data)
{
  (void)btn;
  refresh_summary(GNOSTR_FILTER_SET_DIALOG(user_data));
}

static void
on_authors_changed(GtkTextBuffer *buf, gpointer user_data)
{
  (void)buf;
  refresh_summary(GNOSTR_FILTER_SET_DIALOG(user_data));
}

static void
on_cancel_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnostrFilterSetDialog *self = GNOSTR_FILTER_SET_DIALOG(user_data);
  adw_dialog_close(ADW_DIALOG(self));
}

static void
on_save_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnostrFilterSetDialog *self = GNOSTR_FILTER_SET_DIALOG(user_data);

  /* Same rule as refresh_summary — copy before trimming. */
  g_autofree gchar *name_trimmed =
      g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->row_name)));
  if (name_trimmed) g_strstrip(name_trimmed);
  if (!name_trimmed || !*name_trimmed) {
    gtk_widget_add_css_class(GTK_WIDGET(self->row_name), "error");
    return;
  }
  gtk_widget_remove_css_class(GTK_WIDGET(self->row_name), "error");

  GnostrFilterSet *fs = build_filter_set_from_form(self);

  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();
  if (!mgr) {
    g_warning("filter-set-dialog: default manager unavailable; aborting save");
    g_object_unref(fs);
    adw_dialog_close(ADW_DIALOG(self));
    return;
  }

  const gchar *result_id = NULL;
  if (self->editing_id && *self->editing_id) {
    gnostr_filter_set_set_id(fs, self->editing_id);
    if (!gnostr_filter_set_manager_update(mgr, fs))
      g_warning("filter-set-dialog: update failed for id '%s'", self->editing_id);
    result_id = self->editing_id;
  } else {
    if (!gnostr_filter_set_manager_add(mgr, fs))
      g_warning("filter-set-dialog: add failed (duplicate id?)");
    result_id = gnostr_filter_set_get_id(fs);
  }

  /* Persist to disk; log but don't block the UI on a save failure. */
  g_autoptr(GError) err = NULL;
  if (!gnostr_filter_set_manager_save(mgr, &err))
    g_warning("filter-set-dialog: save failed: %s",
              err ? err->message : "unknown error");

  /* Emit before close so observers can find the manager entry. */
  if (result_id && *result_id)
    g_signal_emit(self, signals[SIGNAL_FILTER_SET_SAVED], 0, result_id);

  g_object_unref(fs);
  adw_dialog_close(ADW_DIALOG(self));
}

/* ------------------------------------------------------------------------
 * Population helpers (edit mode)
 * ------------------------------------------------------------------------ */

static void
populate_from_filter_set(GnostrFilterSetDialog *self, GnostrFilterSet *fs)
{
  if (!fs) return;

  const gchar *name = gnostr_filter_set_get_name(fs);
  gtk_editable_set_text(GTK_EDITABLE(self->row_name), name ? name : "");

  const gchar *desc = gnostr_filter_set_get_description(fs);
  gtk_editable_set_text(GTK_EDITABLE(self->row_description), desc ? desc : "");

  /* Hashtags → space-joined in the entry. */
  const gchar * const *tags = gnostr_filter_set_get_hashtags(fs);
  if (tags && tags[0]) {
    GString *s = g_string_new(NULL);
    for (const gchar * const *p = tags; *p; p++) {
      if (p != tags) g_string_append_c(s, ' ');
      g_string_append(s, *p);
    }
    gtk_editable_set_text(GTK_EDITABLE(self->row_hashtags), s->str);
    g_string_free(s, TRUE);
  }

  /* Kinds → tick matching presets; the rest into "other kinds". */
  gsize nk = 0;
  const gint *kinds = gnostr_filter_set_get_kinds(fs, &nk);
  if (kinds && nk > 0) {
    GString *other = g_string_new(NULL);
    for (gsize i = 0; i < nk; i++) {
      gboolean matched = FALSE;
      for (gsize j = 0; j < G_N_ELEMENTS(kKindPresets); j++) {
        if (kKindPresets[j].kind == kinds[i] && self->kind_checks[j]) {
          gtk_check_button_set_active(self->kind_checks[j], TRUE);
          matched = TRUE;
          break;
        }
      }
      if (!matched) {
        if (other->len > 0) g_string_append_c(other, ',');
        g_string_append_printf(other, "%d", kinds[i]);
      }
    }
    if (other->len > 0)
      gtk_editable_set_text(GTK_EDITABLE(self->row_other_kinds), other->str);
    g_string_free(other, TRUE);
  }

  /* Authors → one per line. */
  const gchar * const *authors = gnostr_filter_set_get_authors(fs);
  if (authors && authors[0]) {
    GString *s = g_string_new(NULL);
    for (const gchar * const *p = authors; *p; p++) {
      if (p != authors) g_string_append_c(s, '\n');
      g_string_append(s, *p);
    }
    gtk_text_buffer_set_text(self->authors_buffer, s->str, (gint)s->len);
    g_string_free(s, TRUE);
  }

  /* Advanced: expand when any advanced field is populated. */
  gint limit = gnostr_filter_set_get_limit(fs);
  if (limit > 0)
    adw_spin_row_set_value(self->limit_row, (double)limit);

  gint64 since = gnostr_filter_set_get_since(fs);
  if (since > 0) {
    gchar buf[32];
    g_snprintf(buf, sizeof(buf), "%" G_GINT64_FORMAT, since);
    gtk_editable_set_text(GTK_EDITABLE(self->row_since), buf);
  }

  gint64 until = gnostr_filter_set_get_until(fs);
  if (until > 0) {
    gchar buf[32];
    g_snprintf(buf, sizeof(buf), "%" G_GINT64_FORMAT, until);
    gtk_editable_set_text(GTK_EDITABLE(self->row_until), buf);
  }

  if (self->advanced_row && (limit > 0 || since > 0 || until > 0))
    adw_expander_row_set_expanded(self->advanced_row, TRUE);
}

static void
switch_to_edit_mode(GnostrFilterSetDialog *self, GnostrFilterSet *fs)
{
  if (!fs) return;
  g_free(self->editing_id);
  self->editing_id = g_strdup(gnostr_filter_set_get_id(fs));

  adw_dialog_set_title(ADW_DIALOG(self), _("Edit Filter Set"));
  gtk_label_set_text(self->lbl_title, _("Edit Filter Set"));
  gtk_button_set_label(self->btn_save, _("Save Changes"));

  populate_from_filter_set(self, fs);
  refresh_summary(self);
}

/* ------------------------------------------------------------------------
 * GObject plumbing
 * ------------------------------------------------------------------------ */

static void
gnostr_filter_set_dialog_finalize(GObject *object)
{
  GnostrFilterSetDialog *self = GNOSTR_FILTER_SET_DIALOG(object);
  g_clear_pointer(&self->editing_id, g_free);
  G_OBJECT_CLASS(gnostr_filter_set_dialog_parent_class)->finalize(object);
}

static void
gnostr_filter_set_dialog_class_init(GnostrFilterSetDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->finalize = gnostr_filter_set_dialog_finalize;

  gtk_widget_class_set_template_from_resource(
      widget_class,
      "/org/gnostr/ui/ui/dialogs/gnostr-filter-set-dialog.ui");

  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, lbl_title);
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, btn_cancel);
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, btn_save);
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, row_name);
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, row_description);
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, row_hashtags);
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, kinds_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, row_other_kinds);
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, authors_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, advanced_row);
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, limit_row);
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, row_since);
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, row_until);
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, summary_label);

  /**
   * GnostrFilterSetDialog::filter-set-saved:
   * @self: the dialog
   * @filter_set_id: id of the persisted filter set
   *
   * Emitted after the dialog has added or updated the filter set via
   * the default manager. The id can be routed directly through the
   * timeline tab dispatcher to focus the new feed.
   */
  signals[SIGNAL_FILTER_SET_SAVED] = g_signal_new(
      "filter-set-saved",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL,
      G_TYPE_NONE,
      1, G_TYPE_STRING);
}

static void
gnostr_filter_set_dialog_init(GnostrFilterSetDialog *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Populate the kinds box with CheckButtons. */
  for (gsize i = 0; i < G_N_ELEMENTS(kKindPresets); i++) {
    GtkWidget *chk = gtk_check_button_new_with_label(_(kKindPresets[i].label));
    self->kind_checks[i] = GTK_CHECK_BUTTON(chk);
    gtk_box_append(self->kinds_box, chk);
    g_signal_connect(chk, "toggled", G_CALLBACK(on_kind_toggled), self);
  }

  /* Populate the authors box with a scrolled TextView. */
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 72);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 180);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
  gtk_widget_add_css_class(scroll, "card");

  GtkWidget *tv = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_CHAR);
  gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(tv), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
  gtk_widget_set_vexpand(tv, TRUE);
  gtk_widget_set_hexpand(tv, TRUE);
  gtk_widget_set_margin_top(tv, 4);
  gtk_widget_set_margin_bottom(tv, 4);
  gtk_widget_set_margin_start(tv, 6);
  gtk_widget_set_margin_end(tv, 6);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), tv);
  gtk_box_append(self->authors_box, scroll);

  self->authors_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
  g_signal_connect(self->authors_buffer, "changed",
                   G_CALLBACK(on_authors_changed), self);

  /* Wire up form-change handlers. */
  g_signal_connect(self->row_name,          "changed", G_CALLBACK(on_any_changed), self);
  g_signal_connect(self->row_description,   "changed", G_CALLBACK(on_any_changed), self);
  g_signal_connect(self->row_hashtags,      "changed", G_CALLBACK(on_any_changed), self);
  g_signal_connect(self->row_other_kinds,   "changed", G_CALLBACK(on_any_changed), self);
  g_signal_connect(self->row_since,         "changed", G_CALLBACK(on_any_changed), self);
  g_signal_connect(self->row_until,         "changed", G_CALLBACK(on_any_changed), self);
  g_signal_connect(self->limit_row,         "notify::value",
                   G_CALLBACK(on_any_changed), self);

  /* Buttons. */
  g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel_clicked), self);
  g_signal_connect(self->btn_save,   "clicked", G_CALLBACK(on_save_clicked),   self);

  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_save), FALSE);

  /* Clear any leftover "error" styling when name becomes non-empty. */
  refresh_summary(self);
}

/* ------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */

GtkWidget *
gnostr_filter_set_dialog_new(void)
{
  return g_object_new(GNOSTR_TYPE_FILTER_SET_DIALOG, NULL);
}

GtkWidget *
gnostr_filter_set_dialog_new_for_edit(GnostrFilterSet *fs)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(fs), NULL);
  /* Editing predefined sets would produce a custom set colliding with
   * a predefined id, whose changes are silently discarded the next
   * time install_defaults() runs. Refuse up-front. */
  g_return_val_if_fail(
      gnostr_filter_set_get_source(fs) == GNOSTR_FILTER_SET_SOURCE_CUSTOM,
      NULL);
  GtkWidget *w = g_object_new(GNOSTR_TYPE_FILTER_SET_DIALOG, NULL);
  switch_to_edit_mode(GNOSTR_FILTER_SET_DIALOG(w), fs);
  return w;
}

void
gnostr_filter_set_dialog_present(GtkWidget *parent)
{
  GtkWidget *dlg = gnostr_filter_set_dialog_new();
  adw_dialog_present(ADW_DIALOG(dlg), parent);
}

void
gnostr_filter_set_dialog_present_edit(GtkWidget *parent,
                                       GnostrFilterSet *fs)
{
  g_return_if_fail(GNOSTR_IS_FILTER_SET(fs));
  GtkWidget *dlg = gnostr_filter_set_dialog_new_for_edit(fs);
  adw_dialog_present(ADW_DIALOG(dlg), parent);
}
