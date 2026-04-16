/* gnostr-filter-switcher.c — Popover-backed filter-set picker.
 *
 * SPDX-License-Identifier: MIT
 *
 * Two GtkListBoxes — "predefined" and "custom" — are bound to filtered
 * views of the default #GnostrFilterSetManager's GListModel. The
 * predefined section is pinned at the top; custom sets live under a
 * separator that auto-hides when the list is empty.
 *
 * The widget is deliberately passive: it does not own any routing
 * logic. Selecting a row emits "filter-set-activated" with the
 * filter-set id, and a higher-level component (the main window)
 * decides whether to select an existing tab or open a new CUSTOM one.
 *
 * nostrc-yg8j.5
 */

#define G_LOG_DOMAIN "gnostr-filter-switcher"

#include "gnostr-filter-switcher.h"

#include <glib/gi18n.h>

#include "../model/gnostr-filter-set.h"
#include "../model/gnostr-filter-set-manager.h"
#include "gnostr-filter-set-dialog.h"

#define FALLBACK_ICON      "view-list-symbolic"
#define FALLBACK_LABEL_KEY N_("Feeds")

struct _GnostrFilterSwitcher {
  GtkWidget parent_instance;

  /* Template children — structural skeleton. */
  GtkMenuButton    *button;
  GtkImage         *button_icon;
  GtkLabel         *button_label;
  GtkPopover       *popover;
  GtkBox           *content_box;      /* vertical container inside the popover */

  /* Built programmatically inside content_box. */
  GtkListBox       *predefined_list;
  GtkListBox       *custom_list;
  GtkWidget        *custom_section;   /* box that contains separator + header + custom_list */
  GtkButton        *add_filter_btn;   /* footer "New Custom Filter…" button */
  GtkButton        *save_hashtag_btn; /* footer "Save #tag as filter set…" row, hidden by default */
  GtkLabel         *save_hashtag_label; /* inside save_hashtag_btn; rewritten per tag */

  /* Filtered views of the manager model. Owned by the widget. */
  GtkFilterListModel *predefined_model;
  GtkFilterListModel *custom_model;

  /* The manager model reference we took — released on dispose. */
  GListModel        *manager_model;

  /* Currently-active filter-set id (copy, nullable). Used for the
   * "checkmark" row indicator. */
  gchar             *active_id;

  /* Currently-visible timeline hashtag (without the leading '#'),
   * or NULL when the active tab is not a hashtag tab. Drives the
   * "Save #tag as filter set…" footer row. nostrc-yg8j.7. */
  gchar             *active_hashtag;
};

G_DEFINE_FINAL_TYPE(GnostrFilterSwitcher, gnostr_filter_switcher, GTK_TYPE_WIDGET)

enum {
  SIGNAL_FILTER_SET_ACTIVATED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ------------------------------------------------------------------------
 * Row factory
 * ------------------------------------------------------------------------ */

typedef struct {
  GtkListBoxRow *row;
  GtkImage      *checkmark;
  gchar         *id;
} RowData;

static void
row_data_free(gpointer data)
{
  RowData *rd = data;
  g_free(rd->id);
  g_free(rd);
}

static GtkWidget *
create_row_cb(gpointer item, gpointer user_data)
{
  (void)user_data;
  GnostrFilterSet *fs = GNOSTR_FILTER_SET(item);

  GtkWidget *row = gtk_list_box_row_new();
  gtk_widget_set_focusable(row, TRUE);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(hbox, 8);
  gtk_widget_set_margin_end  (hbox, 8);
  gtk_widget_set_margin_top  (hbox, 6);
  gtk_widget_set_margin_bottom(hbox, 6);

  const gchar *icon_name = gnostr_filter_set_get_icon(fs);
  GtkWidget *icon = gtk_image_new_from_icon_name(
      (icon_name && *icon_name) ? icon_name : FALLBACK_ICON);
  gtk_box_append(GTK_BOX(hbox), icon);

  const gchar *name = gnostr_filter_set_get_name(fs);
  GtkWidget *label = gtk_label_new(name && *name ? name : gnostr_filter_set_get_id(fs));
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(hbox), label);

  GtkWidget *checkmark = gtk_image_new_from_icon_name("emblem-ok-symbolic");
  gtk_widget_set_visible(checkmark, FALSE);
  gtk_box_append(GTK_BOX(hbox), checkmark);

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);

  RowData *rd = g_new0(RowData, 1);
  rd->row = GTK_LIST_BOX_ROW(row);
  rd->checkmark = GTK_IMAGE(checkmark);
  rd->id = g_strdup(gnostr_filter_set_get_id(fs));
  /* Attach as "filter-set-row-data" — freed with the row. */
  g_object_set_data_full(G_OBJECT(row), "filter-set-row-data", rd, row_data_free);

  return row;
}

/* ------------------------------------------------------------------------
 * Filter predicates for the two sub-lists.
 * ------------------------------------------------------------------------ */

static gboolean
is_predefined(gpointer item, gpointer user_data)
{
  (void)user_data;
  return GNOSTR_IS_FILTER_SET(item) &&
         gnostr_filter_set_get_source(GNOSTR_FILTER_SET(item)) ==
             GNOSTR_FILTER_SET_SOURCE_PREDEFINED;
}

static gboolean
is_custom(gpointer item, gpointer user_data)
{
  (void)user_data;
  return GNOSTR_IS_FILTER_SET(item) &&
         gnostr_filter_set_get_source(GNOSTR_FILTER_SET(item)) ==
             GNOSTR_FILTER_SET_SOURCE_CUSTOM;
}

/* ------------------------------------------------------------------------
 * Active-id indicator + button state
 * ------------------------------------------------------------------------ */

static void
update_row_checkmarks(GnostrFilterSwitcher *self)
{
  GtkListBox *boxes[] = { self->predefined_list, self->custom_list };
  for (size_t b = 0; b < G_N_ELEMENTS(boxes); b++) {
    GtkListBox *box = boxes[b];
    if (!box) continue;
    GtkListBoxRow *row = NULL;
    guint i = 0;
    while ((row = gtk_list_box_get_row_at_index(box, (gint)i))) {
      RowData *rd = g_object_get_data(G_OBJECT(row), "filter-set-row-data");
      if (rd && rd->checkmark) {
        gboolean match = self->active_id &&
                         g_strcmp0(rd->id, self->active_id) == 0;
        gtk_widget_set_visible(GTK_WIDGET(rd->checkmark), match);
      }
      i++;
    }
  }
}

static void
update_button_state(GnostrFilterSwitcher *self)
{
  const gchar *icon = FALLBACK_ICON;
  const gchar *name = NULL;

  if (self->active_id && *self->active_id) {
    GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();
    GnostrFilterSet *fs = mgr ?
        gnostr_filter_set_manager_get(mgr, self->active_id) : NULL;
    if (fs) {
      const gchar *fs_icon = gnostr_filter_set_get_icon(fs);
      const gchar *fs_name = gnostr_filter_set_get_name(fs);
      if (fs_icon && *fs_icon) icon = fs_icon;
      if (fs_name && *fs_name) name = fs_name;
    }
  }

  gtk_image_set_from_icon_name(self->button_icon, icon);
  gtk_label_set_text(self->button_label, name ? name : _(FALLBACK_LABEL_KEY));
}

/* ------------------------------------------------------------------------
 * Custom section visibility — hide when there are no custom sets.
 * ------------------------------------------------------------------------ */

static void
on_custom_items_n_items_changed(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  GnostrFilterSwitcher *self = GNOSTR_FILTER_SWITCHER(user_data);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(obj));
  if (self->custom_section)
    gtk_widget_set_visible(self->custom_section, n > 0);
}

/* ------------------------------------------------------------------------
 * Row activation
 * ------------------------------------------------------------------------ */

static void
on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  (void)box;
  GnostrFilterSwitcher *self = GNOSTR_FILTER_SWITCHER(user_data);
  RowData *rd = g_object_get_data(G_OBJECT(row), "filter-set-row-data");
  if (!rd || !rd->id || !*rd->id) return;

  g_signal_emit(self, signals[SIGNAL_FILTER_SET_ACTIVATED], 0, rd->id);
  gtk_popover_popdown(self->popover);
}

/* ------------------------------------------------------------------------
 * Custom-filter creation (footer "New Custom Filter…" button)
 * ------------------------------------------------------------------------ */

/* Re-emit the dialog's "filter-set-saved" as our own
 * "filter-set-activated" so a fresh save snaps the user straight into
 * the new feed. */
static void
on_dialog_filter_set_saved(GnostrFilterSetDialog *dialog,
                            const gchar *id,
                            gpointer user_data)
{
  (void)dialog;
  GnostrFilterSwitcher *self = GNOSTR_FILTER_SWITCHER(user_data);
  if (!GNOSTR_IS_FILTER_SWITCHER(self) || !id || !*id)
    return;
  g_signal_emit(self, signals[SIGNAL_FILTER_SET_ACTIVATED], 0, id);
}

/* Present the create dialog. We popdown() our popover first so the
 * dialog isn't dismissed the moment it grabs focus; we also present
 * on the idle loop to let the popover unmap cleanly before the
 * AdwDialog takes the keyboard focus.
 *
 * Optional @seed_hashtag / @seed_name (both heap-allocated, owned by
 * the context) pre-fill the dialog form so the "Save active hashtag"
 * flow ends up in a ready-to-save state. NULL means "leave empty". */
typedef struct {
  GWeakRef self_ref;     /* resolved back to the switcher in the idle callback */
  gchar   *seed_hashtag; /* nullable */
  gchar   *seed_name;    /* nullable */
} PresentIdleCtx;

static void
present_idle_ctx_free(gpointer data)
{
  PresentIdleCtx *ctx = data;
  g_weak_ref_clear(&ctx->self_ref);
  g_free(ctx->seed_hashtag);
  g_free(ctx->seed_name);
  g_free(ctx);
}

static gboolean
present_create_dialog_idle(gpointer data)
{
  PresentIdleCtx *ctx = data;
  g_autoptr(GnostrFilterSwitcher) self =
      (GnostrFilterSwitcher *)g_weak_ref_get(&ctx->self_ref);
  if (self) {
    GtkWidget *parent = GTK_WIDGET(self);
    GtkWidget *dlg;
    if ((ctx->seed_hashtag && *ctx->seed_hashtag) ||
        (ctx->seed_name && *ctx->seed_name)) {
      dlg = gnostr_filter_set_dialog_new_seeded(ctx->seed_hashtag,
                                                 ctx->seed_name);
    } else {
      dlg = gnostr_filter_set_dialog_new();
    }
    g_signal_connect_object(dlg, "filter-set-saved",
                             G_CALLBACK(on_dialog_filter_set_saved),
                             self, G_CONNECT_DEFAULT);
    adw_dialog_present(ADW_DIALOG(dlg), parent);
  }
  return G_SOURCE_REMOVE;
}

static void
queue_present_dialog(GnostrFilterSwitcher *self,
                      const gchar *seed_hashtag,
                      const gchar *seed_name)
{
  if (self->popover)
    gtk_popover_popdown(self->popover);

  PresentIdleCtx *ctx = g_new0(PresentIdleCtx, 1);
  g_weak_ref_init(&ctx->self_ref, self);
  ctx->seed_hashtag = (seed_hashtag && *seed_hashtag) ? g_strdup(seed_hashtag) : NULL;
  ctx->seed_name    = (seed_name && *seed_name)       ? g_strdup(seed_name)    : NULL;
  /* Use the _full variant so our destroy notify runs even if the
   * application shuts down before the idle fires; otherwise the
   * GWeakRef would leak. */
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                  present_create_dialog_idle,
                  ctx,
                  present_idle_ctx_free);
}

static void
on_add_custom_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnostrFilterSwitcher *self = GNOSTR_FILTER_SWITCHER(user_data);
  if (!GNOSTR_IS_FILTER_SWITCHER(self))
    return;
  queue_present_dialog(self, NULL, NULL);
}

/* Title-case the hashtag (just the first letter) so "bitcoin" becomes
 * "Bitcoin" in the seeded Name row — a minor polish that saves most
 * users a keystroke. */
static gchar *
propose_name_for_hashtag(const gchar *tag)
{
  if (!tag || !*tag) return NULL;
  gunichar c = g_utf8_get_char(tag);
  const gchar *rest = g_utf8_next_char(tag);
  gunichar upper = g_unichar_toupper(c);
  gchar buf[8];
  gint len = g_unichar_to_utf8(upper, buf);
  buf[len] = '\0';
  return g_strconcat(buf, rest, NULL);
}

static void
on_save_hashtag_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnostrFilterSwitcher *self = GNOSTR_FILTER_SWITCHER(user_data);
  if (!GNOSTR_IS_FILTER_SWITCHER(self) ||
      !self->active_hashtag || !*self->active_hashtag)
    return;

  g_autofree gchar *proposed = propose_name_for_hashtag(self->active_hashtag);
  queue_present_dialog(self, self->active_hashtag, proposed);
}

/* ------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */

void
gnostr_filter_switcher_set_active_id(GnostrFilterSwitcher *self,
                                      const gchar *id)
{
  g_return_if_fail(GNOSTR_IS_FILTER_SWITCHER(self));
  if (g_strcmp0(self->active_id, id) == 0)
    return;
  g_free(self->active_id);
  self->active_id = id ? g_strdup(id) : NULL;

  update_button_state(self);
  update_row_checkmarks(self);
}

void
gnostr_filter_switcher_set_active_hashtag(GnostrFilterSwitcher *self,
                                           const gchar *hashtag)
{
  g_return_if_fail(GNOSTR_IS_FILTER_SWITCHER(self));

  /* Strip any leading '#' so comparisons + labels stay consistent with
   * the rest of the filter-set plumbing, which always stores tags
   * without the prefix. Empty → NULL. */
  const gchar *tag = (hashtag && *hashtag == '#') ? hashtag + 1 : hashtag;
  if (tag && !*tag) tag = NULL;

  if (g_strcmp0(self->active_hashtag, tag) == 0)
    return;

  g_free(self->active_hashtag);
  self->active_hashtag = tag ? g_strdup(tag) : NULL;

  if (self->save_hashtag_btn) {
    gtk_widget_set_visible(GTK_WIDGET(self->save_hashtag_btn),
                           self->active_hashtag != NULL);
  }
  if (self->active_hashtag && self->save_hashtag_label) {
    g_autofree gchar *label =
        g_strdup_printf(_("Save \u201c#%s\u201d as filter set\u2026"),
                        self->active_hashtag);
    gtk_label_set_text(self->save_hashtag_label, label);
  }
}

gboolean
gnostr_filter_switcher_activate_position(GnostrFilterSwitcher *self,
                                          guint position)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SWITCHER(self), FALSE);
  if (position == 0 || !self->manager_model)
    return FALSE;

  guint n = g_list_model_get_n_items(self->manager_model);
  if (position > n)
    return FALSE;

  GnostrFilterSet *fs = g_list_model_get_item(self->manager_model, position - 1);
  if (!fs)
    return FALSE;

  const gchar *id = gnostr_filter_set_get_id(fs);
  gboolean ok = (id && *id);
  if (ok)
    g_signal_emit(self, signals[SIGNAL_FILTER_SET_ACTIVATED], 0, id);

  g_object_unref(fs);
  return ok;
}

/* ------------------------------------------------------------------------
 * Construction helpers
 * ------------------------------------------------------------------------ */

static GtkWidget *
section_header(const char *label_text)
{
  GtkWidget *lbl = gtk_label_new(label_text);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
  gtk_widget_add_css_class(lbl, "caption-heading");
  gtk_widget_add_css_class(lbl, "dim-label");
  gtk_widget_set_margin_start(lbl, 12);
  gtk_widget_set_margin_top(lbl, 8);
  gtk_widget_set_margin_bottom(lbl, 4);
  return lbl;
}

static void
build_popover_content(GnostrFilterSwitcher *self)
{
  /* Predefined header + list */
  gtk_box_append(self->content_box, section_header(_("Predefined")));

  self->predefined_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->predefined_list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class(GTK_WIDGET(self->predefined_list), "boxed-list");
  gtk_widget_set_margin_start (GTK_WIDGET(self->predefined_list), 6);
  gtk_widget_set_margin_end   (GTK_WIDGET(self->predefined_list), 6);
  gtk_box_append(self->content_box, GTK_WIDGET(self->predefined_list));
  g_signal_connect(self->predefined_list, "row-activated",
                   G_CALLBACK(on_row_activated), self);

  /* Custom section (separator + header + list) — hidden when empty. */
  self->custom_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(self->custom_section), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append(GTK_BOX(self->custom_section), section_header(_("Custom Filters")));

  self->custom_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->custom_list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class(GTK_WIDGET(self->custom_list), "boxed-list");
  gtk_widget_set_margin_start (GTK_WIDGET(self->custom_list), 6);
  gtk_widget_set_margin_end   (GTK_WIDGET(self->custom_list), 6);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self->custom_list), 6);
  gtk_box_append(GTK_BOX(self->custom_section), GTK_WIDGET(self->custom_list));

  gtk_box_append(self->content_box, self->custom_section);
  g_signal_connect(self->custom_list, "row-activated",
                   G_CALLBACK(on_row_activated), self);

  /* Footer: separator + "New Custom Filter…" button. Always visible —
   * the create flow is useful whether or not any custom sets already
   * exist. */
  gtk_box_append(self->content_box,
                 gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  self->add_filter_btn = GTK_BUTTON(gtk_button_new());
  gtk_widget_add_css_class(GTK_WIDGET(self->add_filter_btn), "flat");
  gtk_widget_set_margin_top   (GTK_WIDGET(self->add_filter_btn), 6);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self->add_filter_btn), 6);
  gtk_widget_set_margin_start (GTK_WIDGET(self->add_filter_btn), 6);
  gtk_widget_set_margin_end   (GTK_WIDGET(self->add_filter_btn), 6);

  GtkWidget *btn_child = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(btn_child),
                 gtk_image_new_from_icon_name("list-add-symbolic"));
  GtkWidget *btn_label = gtk_label_new(_("New Custom Filter…"));
  gtk_widget_set_hexpand(btn_label, TRUE);
  gtk_label_set_xalign(GTK_LABEL(btn_label), 0.0);
  gtk_box_append(GTK_BOX(btn_child), btn_label);
  gtk_button_set_child(self->add_filter_btn, btn_child);

  g_signal_connect(self->add_filter_btn, "clicked",
                   G_CALLBACK(on_add_custom_clicked), self);
  gtk_box_append(self->content_box, GTK_WIDGET(self->add_filter_btn));

  /* Quick-create: "Save active hashtag as filter set…". Hidden until
   * the main window calls gnostr_filter_switcher_set_active_hashtag()
   * with a non-NULL value. nostrc-yg8j.7. */
  self->save_hashtag_btn = GTK_BUTTON(gtk_button_new());
  gtk_widget_add_css_class(GTK_WIDGET(self->save_hashtag_btn), "flat");
  gtk_widget_set_margin_bottom(GTK_WIDGET(self->save_hashtag_btn), 6);
  gtk_widget_set_margin_start (GTK_WIDGET(self->save_hashtag_btn), 6);
  gtk_widget_set_margin_end   (GTK_WIDGET(self->save_hashtag_btn), 6);
  gtk_widget_set_visible(GTK_WIDGET(self->save_hashtag_btn), FALSE);

  GtkWidget *save_child = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(save_child),
                 gtk_image_new_from_icon_name("starred-symbolic"));
  self->save_hashtag_label = GTK_LABEL(
      gtk_label_new(_("Save active hashtag as filter set…")));
  gtk_widget_set_hexpand(GTK_WIDGET(self->save_hashtag_label), TRUE);
  gtk_label_set_xalign(self->save_hashtag_label, 0.0);
  gtk_label_set_ellipsize(self->save_hashtag_label, PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(save_child), GTK_WIDGET(self->save_hashtag_label));
  gtk_button_set_child(self->save_hashtag_btn, save_child);

  g_signal_connect(self->save_hashtag_btn, "clicked",
                   G_CALLBACK(on_save_hashtag_clicked), self);
  gtk_box_append(self->content_box, GTK_WIDGET(self->save_hashtag_btn));
}

static void
bind_models(GnostrFilterSwitcher *self)
{
  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();
  if (!mgr) {
    g_warning("filter switcher: default manager unavailable");
    return;
  }

  /* Hold a strong reference so we can unbind cleanly on dispose. */
  self->manager_model = g_object_ref(gnostr_filter_set_manager_get_model(mgr));

  GtkCustomFilter *pf =
      gtk_custom_filter_new((GtkCustomFilterFunc)is_predefined, NULL, NULL);
  self->predefined_model = gtk_filter_list_model_new(
      g_object_ref(self->manager_model), GTK_FILTER(pf));
  gtk_list_box_bind_model(self->predefined_list,
                          G_LIST_MODEL(self->predefined_model),
                          create_row_cb, self, NULL);

  GtkCustomFilter *cf =
      gtk_custom_filter_new((GtkCustomFilterFunc)is_custom, NULL, NULL);
  self->custom_model = gtk_filter_list_model_new(
      g_object_ref(self->manager_model), GTK_FILTER(cf));
  gtk_list_box_bind_model(self->custom_list,
                          G_LIST_MODEL(self->custom_model),
                          create_row_cb, self, NULL);

  /* Drive custom-section visibility off the filtered model's n-items. */
  g_signal_connect_object(self->custom_model, "notify::n-items",
                          G_CALLBACK(on_custom_items_n_items_changed),
                          self, G_CONNECT_DEFAULT);
  on_custom_items_n_items_changed(G_OBJECT(self->custom_model), NULL, self);
}

/* ------------------------------------------------------------------------
 * GObject plumbing
 * ------------------------------------------------------------------------ */

static void
on_popover_visible(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  if (!gtk_widget_get_visible(GTK_WIDGET(obj)))
    return;
  GnostrFilterSwitcher *self = GNOSTR_FILTER_SWITCHER(user_data);
  /* Re-apply the indicator in case the manager list changed while
   * closed or the active id was updated without an open popover. */
  update_row_checkmarks(self);
}

static void
gnostr_filter_switcher_init(GnostrFilterSwitcher *self)
{
  /* Build the button + popover programmatically (no template — the
   * widget is small enough that a .blp adds no clarity and would add
   * a resource-loading dependency). */
  GtkLayoutManager *layout = gtk_bin_layout_new();
  gtk_widget_set_layout_manager(GTK_WIDGET(self), layout);

  self->button = GTK_MENU_BUTTON(gtk_menu_button_new());
  gtk_menu_button_set_direction(self->button, GTK_ARROW_NONE);
  gtk_menu_button_set_always_show_arrow(self->button, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(self->button), "flat");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->button), _("Switch feed"));
  gtk_widget_set_parent(GTK_WIDGET(self->button), GTK_WIDGET(self));

  /* Button content: icon + label */
  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  self->button_icon = GTK_IMAGE(gtk_image_new_from_icon_name(FALLBACK_ICON));
  self->button_label = GTK_LABEL(gtk_label_new(_(FALLBACK_LABEL_KEY)));
  gtk_label_set_max_width_chars(self->button_label, 16);
  gtk_label_set_ellipsize(self->button_label, PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(btn_box), GTK_WIDGET(self->button_icon));
  gtk_box_append(GTK_BOX(btn_box), GTK_WIDGET(self->button_label));
  gtk_menu_button_set_child(self->button, btn_box);

  /* Popover scaffolding. */
  self->popover = GTK_POPOVER(gtk_popover_new());
  gtk_popover_set_has_arrow(self->popover, TRUE);
  /* Use g_signal_connect_object so the callback ties its lifetime to
   * @self; consistent with how bind_models() connects the custom
   * model's notify::n-items handler. */
  g_signal_connect_object(self->popover, "notify::visible",
                          G_CALLBACK(on_popover_visible), self,
                          G_CONNECT_DEFAULT);

  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 420);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  self->content_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_widget_set_size_request(GTK_WIDGET(self->content_box), 260, -1);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(self->content_box));

  gtk_popover_set_child(self->popover, scroll);
  gtk_menu_button_set_popover(self->button, GTK_WIDGET(self->popover));

  /* Build list content + bind models. */
  build_popover_content(self);
  bind_models(self);
  update_button_state(self);
}

static void
gnostr_filter_switcher_dispose(GObject *object)
{
  GnostrFilterSwitcher *self = GNOSTR_FILTER_SWITCHER(object);

  /* Drop model references FIRST. `g_signal_connect_object` disconnects
   * on finalize, not dispose, so the notify::n-items callback could
   * otherwise fire against dangling widget pointers during button
   * teardown. Clearing the filter models here drops our owning ref;
   * once the list boxes also release theirs (during widget teardown
   * below) the custom_model is finalized and its signal is
   * disconnected. */
  g_clear_object(&self->predefined_model);
  g_clear_object(&self->custom_model);
  g_clear_object(&self->manager_model);

  /* Now unparent the button — this destroys the subtree (popover,
   * list boxes, content box, etc). Null out every struct-field
   * pointer that referenced a child of the button so any accidental
   * later use (e.g. a late signal emission) becomes a clean no-op
   * rather than a use-after-free. */
  if (self->button) {
    gtk_widget_unparent(GTK_WIDGET(self->button));
    self->button          = NULL;
    self->button_icon     = NULL;
    self->button_label    = NULL;
    self->popover         = NULL;
    self->content_box     = NULL;
    self->predefined_list = NULL;
    self->custom_list     = NULL;
    self->custom_section  = NULL;
    self->add_filter_btn  = NULL;
    self->save_hashtag_btn   = NULL;
    self->save_hashtag_label = NULL;
  }

  g_clear_pointer(&self->active_id,      g_free);
  g_clear_pointer(&self->active_hashtag, g_free);

  G_OBJECT_CLASS(gnostr_filter_switcher_parent_class)->dispose(object);
}

static void
gnostr_filter_switcher_class_init(GnostrFilterSwitcherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_filter_switcher_dispose;

  gtk_widget_class_set_css_name(widget_class, "filterswitcher");

  /**
   * GnostrFilterSwitcher::filter-set-activated:
   * @self: the switcher
   * @filter_set_id: the id of the chosen filter set
   *
   * Emitted when a filter set is chosen from the popover or when
   * gnostr_filter_switcher_activate_position() activates a slot.
   */
  signals[SIGNAL_FILTER_SET_ACTIVATED] = g_signal_new(
      "filter-set-activated",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL,
      G_TYPE_NONE,
      1, G_TYPE_STRING);
}

/* ------------------------------------------------------------------------
 * Public constructor
 * ------------------------------------------------------------------------ */

GtkWidget *
gnostr_filter_switcher_new(void)
{
  return g_object_new(GNOSTR_TYPE_FILTER_SWITCHER, NULL);
}
