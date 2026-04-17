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
#include "../model/gnostr-nip51-loader.h"
#include "../util/follow_list.h"
#include "../util/trending-hashtags.h"
#include "../util/utils.h"
#include "gnostr-nip51-list-picker-dialog.h"

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

  /* Suggestions (trending hashtags) — populated asynchronously from NDB.
   * nostrc-yg8j.7 */
  AdwPreferencesGroup *suggestions_group;
  GtkFlowBox          *suggestions_box;
  GCancellable        *suggestions_cancellable;

  /* NIP-51 list import (nostrc-yg8j.8). */
  AdwPreferencesGroup *import_group;
  AdwActionRow        *import_row;
  gchar               *pubkey_hex;  /* owned copy, set by gnostr_filter_set_dialog_set_pubkey() */

  /* Built programmatically inside kinds_box / authors_box. */
  GtkCheckButton *kind_checks[G_N_ELEMENTS(kKindPresets)];
  GtkTextBuffer  *authors_buffer;

  /* Author scoping for trending hashtag suggestions (nostrc-spue). */
  GStrv           suggestion_authors;

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
 * Trending-hashtag suggestions (nostrc-yg8j.7)
 *
 * We kick off a local NDB scan on construction and, if any tags come
 * back, light up a row of pill chips above the Criteria group. Clicking
 * a chip appends its tag to the Hashtags row (skipping duplicates), so
 * the user only has to tick presets + review the name before hitting
 * Save. The computation is bounded (400 events / top 12) so it never
 * measurably delays dialog presentation, and the group stays hidden
 * until we have something useful to show.
 * ------------------------------------------------------------------------ */

/* Return TRUE if the comma/space-separated hashtag row already
 * contains @tag (case-insensitive, `#` prefix ignored). */
static gboolean
row_contains_hashtag(AdwEntryRow *row, const char *tag)
{
  if (!row || !tag || !*tag) return FALSE;
  const gchar *raw = gtk_editable_get_text(GTK_EDITABLE(row));
  if (!raw || !*raw) return FALSE;

  gchar **existing = g_strsplit_set(raw, " ,\t\n\r", -1);
  gboolean found = FALSE;
  for (gchar **p = existing; *p && !found; p++) {
    gchar *e = g_strstrip(*p);
    if (!*e) continue;
    if (*e == '#') e++;
    if (!*e) continue;
    if (g_ascii_strcasecmp(e, tag) == 0)
      found = TRUE;
  }
  g_strfreev(existing);
  return found;
}

static void
on_suggestion_chip_clicked(GtkButton *btn, gpointer user_data)
{
  GnostrFilterSetDialog *self = GNOSTR_FILTER_SET_DIALOG(user_data);
  const char *tag = g_object_get_data(G_OBJECT(btn), "hashtag");
  if (!tag || !*tag) return;

  if (row_contains_hashtag(self->row_hashtags, tag)) {
    /* Already present \u2014 nothing to do; briefly dim the chip so the
     * user sees their click landed somewhere. */
    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
    return;
  }

  const gchar *current = gtk_editable_get_text(GTK_EDITABLE(self->row_hashtags));
  g_autofree gchar *joined = NULL;
  if (current && *current) {
    /* Preserve the user's whitespace style by appending with a space
     * separator; tokenize_multi() handles both comma and whitespace. */
    joined = g_strdup_printf("%s %s", current, tag);
  } else {
    joined = g_strdup(tag);
  }
  gtk_editable_set_text(GTK_EDITABLE(self->row_hashtags), joined);
  /* refresh_summary() runs via row_hashtags "changed". */

  /* Dim the chip so the user sees it's been consumed. */
  gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
}

static void
populate_suggestions(GnostrFilterSetDialog *self, GPtrArray *hashtags)
{
  if (!self->suggestions_box || !self->suggestions_group) return;

  /* Clear prior children (defensive \u2014 init only runs once, but a future
   * refresh path could reuse this helper). */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->suggestions_box))) != NULL)
    gtk_flow_box_remove(self->suggestions_box, child);

  guint shown = 0;
  if (hashtags) {
    for (guint i = 0; i < hashtags->len; i++) {
      GnostrTrendingHashtag *ht = g_ptr_array_index(hashtags, i);
      if (!ht || !ht->tag || !*ht->tag) continue;

      g_autofree gchar *label = g_strdup_printf("#%s", ht->tag);
      GtkWidget *btn = gtk_button_new_with_label(label);
      gtk_widget_add_css_class(btn, "pill");
      gtk_widget_add_css_class(btn, "flat");

      /* If the tag is already in the row (seeded or manually typed),
       * present the chip as already-consumed. */
      if (row_contains_hashtag(self->row_hashtags, ht->tag))
        gtk_widget_set_sensitive(btn, FALSE);

      g_object_set_data_full(G_OBJECT(btn), "hashtag",
                             g_strdup(ht->tag), g_free);
      g_signal_connect(btn, "clicked",
                       G_CALLBACK(on_suggestion_chip_clicked), self);

      gtk_flow_box_append(self->suggestions_box, btn);
      shown++;
    }
  }

  gtk_widget_set_visible(GTK_WIDGET(self->suggestions_group), shown > 0);
}

/* Async delivery context. We can't hand @self directly to the
 * compute thread because the dialog may be dismissed (and finalized)
 * before the scan completes. A #GWeakRef plus a dedicated
 * #GCancellable gives us the belt-and-braces: the cancellable asks
 * the worker to skip delivery, and the weak ref guards against the
 * unlikely race where the callback fires after finalization anyway. */
typedef struct {
  GWeakRef      weak_self;
  GCancellable *cancellable;
} SuggestionsLoadCtx;

static void
suggestions_load_ctx_free(gpointer data)
{
  SuggestionsLoadCtx *ctx = data;
  if (!ctx) return;
  g_weak_ref_clear(&ctx->weak_self);
  g_clear_object(&ctx->cancellable);
  g_free(ctx);
}

static void
on_suggestions_ready(GPtrArray *hashtags, gpointer user_data)
{
  SuggestionsLoadCtx *ctx = user_data;

  /* If the dialog is gone, or its scan was cancelled, drop the
   * result. ctx itself is freed by the trending module's
   * user_data_free destroy notify on every completion path. */
  GObject *obj = g_weak_ref_get(&ctx->weak_self);
  if (!obj || g_cancellable_is_cancelled(ctx->cancellable)) {
    g_clear_object(&obj);
    if (hashtags) g_ptr_array_unref(hashtags);
    return;
  }

  GnostrFilterSetDialog *self = GNOSTR_FILTER_SET_DIALOG(obj);
  populate_suggestions(self, hashtags);

  g_object_unref(obj);
  if (hashtags) g_ptr_array_unref(hashtags);
}

static void
load_suggestions_async(GnostrFilterSetDialog *self)
{
  if (self->suggestions_cancellable) return;  /* already running */

  self->suggestions_cancellable = g_cancellable_new();

  SuggestionsLoadCtx *ctx = g_new0(SuggestionsLoadCtx, 1);
  g_weak_ref_init(&ctx->weak_self, self);
  ctx->cancellable = g_object_ref(self->suggestions_cancellable);

  /* Resolve author scoping: use explicit suggestion_authors if set,
   * otherwise derive from the connected user's follow list (NDB cache).
   * nostrc-spue: author-scoped trending hashtag suggestions. */
  g_auto(GStrv) authors = NULL;
  if (self->suggestion_authors) {
    authors = g_strdupv(self->suggestion_authors);
  } else if (self->pubkey_hex && *self->pubkey_hex) {
    authors = gnostr_follow_list_get_pubkeys_cached(self->pubkey_hex);
  }

  /* 400 recent kind-1 events, top 12 tags — enough for a useful
   * chip row without stressing the worker or crowding the form.
   * Pass suggestions_load_ctx_free as the user_data destroy notify
   * so ctx is freed on every completion path (success OR cancel),
   * closing the leak that would otherwise accumulate each time the
   * user opens + dismisses the dialog before the 400-event scan
   * finishes. */
  gnostr_compute_trending_hashtags_async(400, 12,
                                         (const char * const *)authors,
                                         on_suggestions_ready, ctx,
                                         suggestions_load_ctx_free,
                                         self->suggestions_cancellable);
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

  /* Trending suggestions are a create-time discovery aid; in edit
   * mode the Hashtags row is already populated from the existing
   * filter set, so the chip row would just echo pre-existing tags
   * back at the user. Cancel the in-flight scan (kicked off in _init
   * before we knew we were editing) and hide the group. */
  if (self->suggestions_cancellable) {
    g_cancellable_cancel(self->suggestions_cancellable);
    g_clear_object(&self->suggestions_cancellable);
  }
  if (self->suggestions_group)
    gtk_widget_set_visible(GTK_WIDGET(self->suggestions_group), FALSE);

  /* Importing a NIP-51 list in edit mode would reset_form() then
   * repopulate, which would update() the original set with an entirely
   * different payload under its original id. Forbid that up-front:
   * the import affordance is a create-time tool only. nostrc-yg8j.8. */
  if (self->import_group)
    gtk_widget_set_visible(GTK_WIDGET(self->import_group), FALSE);

  populate_from_filter_set(self, fs);
  refresh_summary(self);
}

/* ------------------------------------------------------------------------
 * NIP-51 list import (nostrc-yg8j.8)
 *
 * When a signer is connected, the dialog exposes an "Import from NIP-51
 * list…" row at the top of the form. Activation opens a picker dialog
 * that lists the user's kind-30000 categorized people lists (cached in
 * NDB); selecting one converts the list into a #GnostrFilterSet and
 * pre-populates the form. The name is left as the list's title so the
 * user can easily refine before saving.
 * ------------------------------------------------------------------------ */

/* Reset every field the dialog drives so a fresh import starts from a
 * clean slate instead of merging on top of whatever was there before.
 * Keeps the user's Save-button state in sync via refresh_summary(). */
static void
reset_form(GnostrFilterSetDialog *self)
{
  gtk_editable_set_text(GTK_EDITABLE(self->row_name), "");
  gtk_editable_set_text(GTK_EDITABLE(self->row_description), "");
  gtk_editable_set_text(GTK_EDITABLE(self->row_hashtags), "");
  gtk_editable_set_text(GTK_EDITABLE(self->row_other_kinds), "");
  gtk_editable_set_text(GTK_EDITABLE(self->row_since), "");
  gtk_editable_set_text(GTK_EDITABLE(self->row_until), "");

  for (gsize i = 0; i < G_N_ELEMENTS(kKindPresets); i++) {
    if (self->kind_checks[i])
      gtk_check_button_set_active(self->kind_checks[i], FALSE);
  }

  if (self->authors_buffer)
    gtk_text_buffer_set_text(self->authors_buffer, "", 0);

  if (self->limit_row)
    adw_spin_row_set_value(self->limit_row, 0.0);
}

static void
on_nip51_list_selected(GtkWidget   *picker,
                        const gchar *title,
                        const gchar *identifier,
                        gpointer     nostr_list_ptr,
                        gpointer     user_data)
{
  (void)picker;
  (void)identifier;
  GnostrFilterSetDialog *self = GNOSTR_FILTER_SET_DIALOG(user_data);
  if (!nostr_list_ptr) return;

  /* The picker hands us a *borrowed* NostrList* that is only valid
   * during this emission (see gnostr-nip51-list-picker-dialog.h).
   * Convert synchronously before the picker closes. */
  g_autoptr(GnostrFilterSet) fs =
      gnostr_nip51_list_to_filter_set(nostr_list_ptr, title);
  if (!fs) {
    /* Converter returns NULL for lists with no p-tag entries; the
     * picker already filters those out, so this is defensive. */
    g_warning("filter-set-dialog: NIP-51 list produced no filter set");
    return;
  }

  /* Replace rather than merge so the user sees exactly what the list
   * encodes. They can then tweak name/kinds/etc. before Save. */
  reset_form(self);
  populate_from_filter_set(self, fs);
  refresh_summary(self);
}

static void
on_import_row_activated(AdwActionRow *row, gpointer user_data)
{
  (void)row;
  GnostrFilterSetDialog *self = GNOSTR_FILTER_SET_DIALOG(user_data);
  if (!self->pubkey_hex || !*self->pubkey_hex)
    return;

  GtkWidget *picker = gnostr_nip51_list_picker_dialog_new(self->pubkey_hex);
  if (!picker) return;

  /* Tie the signal lifetime to @self so an unusual teardown path can't
   * fire the handler against a stale dialog pointer. */
  g_signal_connect_object(picker, "list-selected",
                           G_CALLBACK(on_nip51_list_selected), self,
                           G_CONNECT_DEFAULT);
  adw_dialog_present(ADW_DIALOG(picker), GTK_WIDGET(self));
}

/* ------------------------------------------------------------------------
 * GObject plumbing
 * ------------------------------------------------------------------------ */

static void
gnostr_filter_set_dialog_dispose(GObject *object)
{
  GnostrFilterSetDialog *self = GNOSTR_FILTER_SET_DIALOG(object);

  /* Cancel the trending-hashtags worker before we start tearing down
   * the template. The callback guards with a #GWeakRef on top of this,
   * but cancelling gives us an immediate “drop the result” signal that
   * avoids any accidental populate work during dispose. */
  if (self->suggestions_cancellable) {
    g_cancellable_cancel(self->suggestions_cancellable);
    g_clear_object(&self->suggestions_cancellable);
  }

  G_OBJECT_CLASS(gnostr_filter_set_dialog_parent_class)->dispose(object);
}

static void
gnostr_filter_set_dialog_finalize(GObject *object)
{
  GnostrFilterSetDialog *self = GNOSTR_FILTER_SET_DIALOG(object);
  g_clear_pointer(&self->editing_id, g_free);
  g_clear_pointer(&self->pubkey_hex, g_free);
  g_clear_pointer(&self->suggestion_authors, g_strfreev);
  G_OBJECT_CLASS(gnostr_filter_set_dialog_parent_class)->finalize(object);
}

static void
gnostr_filter_set_dialog_class_init(GnostrFilterSetDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose  = gnostr_filter_set_dialog_dispose;
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
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, suggestions_group);
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, suggestions_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, import_group);
  gtk_widget_class_bind_template_child(widget_class, GnostrFilterSetDialog, import_row);

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

  /* NIP-51 import row. The group stays invisible until a caller
   * hands us a pubkey via gnostr_filter_set_dialog_set_pubkey().
   * nostrc-yg8j.8 */
  if (self->import_row)
    g_signal_connect(self->import_row, "activated",
                     G_CALLBACK(on_import_row_activated), self);

  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_save), FALSE);

  /* Clear any leftover "error" styling when name becomes non-empty. */
  refresh_summary(self);

  /* Kick off the trending-hashtags suggestion scan. The group stays
   * hidden until at least one tag comes back, so a dark / empty NDB
   * just shows no extra UI. nostrc-yg8j.7 */
  load_suggestions_async(self);
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

GtkWidget *
gnostr_filter_set_dialog_new_seeded(const char *hashtag,
                                     const char *proposed_name)
{
  GnostrFilterSetDialog *self =
      g_object_new(GNOSTR_TYPE_FILTER_SET_DIALOG, NULL);

  /* Strip a leading '#' if the caller left it on — the Hashtags row is
   * tokenised without the prefix and treats it as a syntax artefact of
   * the display form. */
  if (hashtag && *hashtag) {
    const char *tag = (*hashtag == '#') ? hashtag + 1 : hashtag;
    if (*tag)
      gtk_editable_set_text(GTK_EDITABLE(self->row_hashtags), tag);
  }
  if (proposed_name && *proposed_name) {
    gtk_editable_set_text(GTK_EDITABLE(self->row_name), proposed_name);
  }
  /* refresh_summary() (fired through the entry "changed" signal from
   * gtk_editable_set_text) also flips btn_save sensitive when the
   * name row becomes non-empty, so no manual state-poking needed. */
  return GTK_WIDGET(self);
}

void
gnostr_filter_set_dialog_present(GtkWidget *parent)
{
  GtkWidget *dlg = gnostr_filter_set_dialog_new();
  adw_dialog_present(ADW_DIALOG(dlg), parent);
}

void
gnostr_filter_set_dialog_present_seeded(GtkWidget *parent,
                                         const char *hashtag,
                                         const char *proposed_name)
{
  GtkWidget *dlg = gnostr_filter_set_dialog_new_seeded(hashtag, proposed_name);
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

void
gnostr_filter_set_dialog_set_pubkey(GnostrFilterSetDialog *self,
                                     const gchar *pubkey_hex)
{
  g_return_if_fail(GNOSTR_IS_FILTER_SET_DIALOG(self));

  g_clear_pointer(&self->pubkey_hex, g_free);

  gboolean have_pubkey = pubkey_hex && *pubkey_hex;
  if (have_pubkey)
    self->pubkey_hex = g_strdup(pubkey_hex);

  if (self->import_group)
    gtk_widget_set_visible(GTK_WIDGET(self->import_group), have_pubkey);
}

void
gnostr_filter_set_dialog_set_suggestion_authors(GnostrFilterSetDialog *self,
                                                 const GStrv authors)
{
  g_return_if_fail(GNOSTR_IS_FILTER_SET_DIALOG(self));

  g_clear_pointer(&self->suggestion_authors, g_strfreev);
  if (authors && authors[0])
    self->suggestion_authors = g_strdupv(authors);
}
