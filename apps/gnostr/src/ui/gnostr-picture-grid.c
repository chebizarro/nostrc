/**
 * gnostr-picture-grid.c - NIP-68 Picture Grid Widget Implementation
 *
 * A GTK4 widget for displaying NIP-68 pictures in a responsive grid layout.
 */

#include "gnostr-picture-grid.h"
#include "gnostr-picture-card.h"
#include "gnostr-image-viewer.h"
#include <glib/gi18n.h>
#include <string.h>

/* Grid configuration */
#define DEFAULT_SPACING 8
#define MIN_CARD_WIDTH 280
#define SCROLL_THRESHOLD 200  /* Pixels from bottom to trigger load-more */

/* Author info cache entry */
typedef struct {
  char *pubkey;
  char *display_name;
  char *avatar_url;
  char *nip05;
  char *lud16;
} AuthorInfo;

static void author_info_free(AuthorInfo *info) {
  if (!info) return;
  g_free(info->pubkey);
  g_free(info->display_name);
  g_free(info->avatar_url);
  g_free(info->nip05);
  g_free(info->lud16);
  g_free(info);
}

struct _GnostrPictureGrid {
  GtkWidget parent_instance;

  /* Main layout */
  GtkWidget *root_box;
  GtkWidget *scrolled_window;
  GtkWidget *grid_box;
  GtkWidget *flow_box;

  /* State widgets */
  GtkWidget *loading_spinner;
  GtkWidget *loading_more_box;
  GtkWidget *loading_more_spinner;
  GtkWidget *empty_box;
  GtkWidget *empty_label;

  /* Image overlay */
  GtkWidget *overlay_window;
  GnostrImageViewer *image_viewer;

  /* Data */
  GHashTable *pictures;        /* event_id -> GnostrPictureMeta */
  GHashTable *cards;           /* event_id -> GnostrPictureCard */
  GHashTable *author_cache;    /* pubkey -> AuthorInfo */
  GList *picture_order;        /* List of event_ids in display order */

  /* Configuration */
  GnostrPictureGridColumns columns;
  guint spacing;
  gboolean is_logged_in;
  gboolean is_compact;
  gboolean is_loading;
  gboolean is_loading_more;
  gchar *empty_message;

  /* Scroll tracking */
  gboolean load_more_triggered;
};

G_DEFINE_TYPE(GnostrPictureGrid, gnostr_picture_grid, GTK_TYPE_WIDGET)

enum {
  SIGNAL_PICTURE_CLICKED,
  SIGNAL_AUTHOR_CLICKED,
  SIGNAL_LOAD_MORE,
  SIGNAL_REFRESH_REQUESTED,
  SIGNAL_LIKE_CLICKED,
  SIGNAL_ZAP_CLICKED,
  SIGNAL_HASHTAG_CLICKED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void update_grid_columns(GnostrPictureGrid *self);
static void update_empty_state(GnostrPictureGrid *self);
static void on_card_image_clicked(GnostrPictureCard *card, gpointer user_data);
static void on_card_author_clicked(GnostrPictureCard *card, const char *pubkey, gpointer user_data);
static void on_card_like_clicked(GnostrPictureCard *card, gpointer user_data);
static void on_card_zap_clicked(GnostrPictureCard *card, gpointer user_data);
static void on_card_hashtag_clicked(GnostrPictureCard *card, const char *tag, gpointer user_data);
static void on_image_viewer_destroyed(GtkWidget *widget, gpointer user_data);

static void
gnostr_picture_grid_dispose(GObject *object) {
  GnostrPictureGrid *self = GNOSTR_PICTURE_GRID(object);

  if (self->overlay_window) {
    g_signal_handlers_disconnect_by_func(self->overlay_window,
                                          on_image_viewer_destroyed, self);
    gtk_window_destroy(GTK_WINDOW(self->overlay_window));
    self->overlay_window = NULL;
    self->image_viewer = NULL;
  }

  /* Unparent children */
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
  if (child) {
    gtk_widget_unparent(child);
  }

  G_OBJECT_CLASS(gnostr_picture_grid_parent_class)->dispose(object);
}

static void
gnostr_picture_grid_finalize(GObject *object) {
  GnostrPictureGrid *self = GNOSTR_PICTURE_GRID(object);

  g_hash_table_destroy(self->pictures);
  g_hash_table_destroy(self->cards);
  g_hash_table_destroy(self->author_cache);
  g_list_free_full(self->picture_order, g_free);
  g_free(self->empty_message);

  G_OBJECT_CLASS(gnostr_picture_grid_parent_class)->finalize(object);
}

static void
on_scroll_edge_reached(GtkScrolledWindow *sw, GtkPositionType pos,
                       gpointer user_data) {
  GnostrPictureGrid *self = GNOSTR_PICTURE_GRID(user_data);
  (void)sw;

  if (pos == GTK_POS_BOTTOM && !self->load_more_triggered && !self->is_loading_more) {
    self->load_more_triggered = TRUE;
    g_signal_emit(self, signals[SIGNAL_LOAD_MORE], 0);
  }
}

static void
on_size_allocate(GtkWidget *widget, int width, int height,
                 int baseline, gpointer user_data) {
  GnostrPictureGrid *self = GNOSTR_PICTURE_GRID(user_data);
  (void)widget; (void)height; (void)baseline;

  /* Update columns based on width if auto mode */
  if (self->columns == GNOSTR_PICTURE_GRID_AUTO) {
    int cols = width / MIN_CARD_WIDTH;
    if (cols < 1) cols = 1;
    if (cols > 5) cols = 5;

    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->flow_box), cols);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->flow_box), cols);
  }
}

static void
gnostr_picture_grid_class_init(GnostrPictureGridClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_picture_grid_dispose;
  object_class->finalize = gnostr_picture_grid_finalize;

  /* CSS name */
  gtk_widget_class_set_css_name(widget_class, "picture-grid");

  /* Layout manager */
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);

  /* Signals */
  signals[SIGNAL_PICTURE_CLICKED] = g_signal_new(
    "picture-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_AUTHOR_CLICKED] = g_signal_new(
    "author-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_LOAD_MORE] = g_signal_new(
    "load-more",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SIGNAL_REFRESH_REQUESTED] = g_signal_new(
    "refresh-requested",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SIGNAL_LIKE_CLICKED] = g_signal_new(
    "like-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_ZAP_CLICKED] = g_signal_new(
    "zap-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIGNAL_HASHTAG_CLICKED] = g_signal_new(
    "hashtag-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gnostr_picture_grid_init(GnostrPictureGrid *self) {
  /* Initialize data structures */
  self->pictures = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, (GDestroyNotify)gnostr_picture_meta_free);
  self->cards = g_hash_table_new_full(g_str_hash, g_str_equal,
                                       g_free, NULL); /* Cards are owned by flow_box */
  self->author_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, (GDestroyNotify)author_info_free);
  self->picture_order = NULL;

  self->columns = GNOSTR_PICTURE_GRID_AUTO;
  self->spacing = DEFAULT_SPACING;
  self->is_logged_in = FALSE;
  self->is_compact = FALSE;
  self->is_loading = FALSE;
  self->is_loading_more = FALSE;
  self->load_more_triggered = FALSE;
  self->empty_message = g_strdup(_("No pictures to display"));

  /* Build UI */

  /* Root box */
  self->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent(self->root_box, GTK_WIDGET(self));
  gtk_widget_set_hexpand(self->root_box, TRUE);
  gtk_widget_set_vexpand(self->root_box, TRUE);

  /* Initial loading spinner */
  self->loading_spinner = gtk_spinner_new();
  gtk_widget_set_halign(self->loading_spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->loading_spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(self->loading_spinner, 48);
  gtk_widget_set_margin_bottom(self->loading_spinner, 48);
  gtk_widget_set_visible(self->loading_spinner, FALSE);
  gtk_box_append(GTK_BOX(self->root_box), self->loading_spinner);

  /* Empty state */
  self->empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_halign(self->empty_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->empty_box, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(self->empty_box, 48);
  gtk_widget_set_margin_bottom(self->empty_box, 48);
  gtk_widget_add_css_class(self->empty_box, "picture-grid-empty");
  gtk_widget_set_visible(self->empty_box, FALSE);
  gtk_box_append(GTK_BOX(self->root_box), self->empty_box);

  GtkWidget *empty_icon = gtk_image_new_from_icon_name("image-x-generic-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(empty_icon), 64);
  gtk_widget_add_css_class(empty_icon, "dim-label");
  gtk_box_append(GTK_BOX(self->empty_box), empty_icon);

  self->empty_label = gtk_label_new(self->empty_message);
  gtk_widget_add_css_class(self->empty_label, "dim-label");
  gtk_box_append(GTK_BOX(self->empty_box), self->empty_label);

  /* Scrolled window */
  self->scrolled_window = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scrolled_window),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand(self->scrolled_window, TRUE);
  gtk_widget_set_vexpand(self->scrolled_window, TRUE);
  gtk_box_append(GTK_BOX(self->root_box), self->scrolled_window);

  g_signal_connect(self->scrolled_window, "edge-reached",
                   G_CALLBACK(on_scroll_edge_reached), self);

  /* Grid container box */
  self->grid_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scrolled_window),
                                 self->grid_box);

  /* Flow box for grid layout */
  self->flow_box = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->flow_box), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(self->flow_box), TRUE);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->flow_box), self->spacing);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->flow_box), self->spacing);
  gtk_widget_set_margin_start(self->flow_box, self->spacing);
  gtk_widget_set_margin_end(self->flow_box, self->spacing);
  gtk_widget_set_margin_top(self->flow_box, self->spacing);
  gtk_widget_set_margin_bottom(self->flow_box, self->spacing);
  gtk_widget_add_css_class(self->flow_box, "picture-grid-flow");
  gtk_box_append(GTK_BOX(self->grid_box), self->flow_box);

  /* Connect size allocate for responsive columns */
  g_signal_connect(self->flow_box, "size-allocate",
                   G_CALLBACK(on_size_allocate), self);

  /* Loading more indicator */
  self->loading_more_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(self->loading_more_box, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(self->loading_more_box, 16);
  gtk_widget_set_margin_bottom(self->loading_more_box, 16);
  gtk_widget_set_visible(self->loading_more_box, FALSE);
  gtk_box_append(GTK_BOX(self->grid_box), self->loading_more_box);

  self->loading_more_spinner = gtk_spinner_new();
  gtk_box_append(GTK_BOX(self->loading_more_box), self->loading_more_spinner);

  GtkWidget *loading_label = gtk_label_new(_("Loading more..."));
  gtk_widget_add_css_class(loading_label, "dim-label");
  gtk_box_append(GTK_BOX(self->loading_more_box), loading_label);

  /* Initial column setup */
  update_grid_columns(self);
}

GnostrPictureGrid *
gnostr_picture_grid_new(void) {
  return g_object_new(GNOSTR_TYPE_PICTURE_GRID, NULL);
}

void
gnostr_picture_grid_clear(GnostrPictureGrid *self) {
  g_return_if_fail(GNOSTR_IS_PICTURE_GRID(self));

  /* Remove all children from flow box */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(self->flow_box)) != NULL) {
    gtk_flow_box_remove(GTK_FLOW_BOX(self->flow_box), child);
  }

  /* Clear data structures */
  g_hash_table_remove_all(self->pictures);
  g_hash_table_remove_all(self->cards);
  g_list_free_full(self->picture_order, g_free);
  self->picture_order = NULL;

  self->load_more_triggered = FALSE;

  update_empty_state(self);
}

void
gnostr_picture_grid_add_picture(GnostrPictureGrid *self,
                                 const GnostrPictureMeta *meta) {
  g_return_if_fail(GNOSTR_IS_PICTURE_GRID(self));
  g_return_if_fail(meta != NULL);
  g_return_if_fail(meta->event_id != NULL);

  /* Check if already exists */
  if (g_hash_table_contains(self->pictures, meta->event_id)) {
    return;
  }

  /* Copy and store metadata */
  GnostrPictureMeta *copy = gnostr_picture_meta_copy(meta);
  g_hash_table_insert(self->pictures, g_strdup(meta->event_id), copy);
  self->picture_order = g_list_append(self->picture_order, g_strdup(meta->event_id));

  /* Create card widget */
  GnostrPictureCard *card = gnostr_picture_card_new();
  gnostr_picture_card_set_picture(card, copy);
  gnostr_picture_card_set_logged_in(card, self->is_logged_in);
  gnostr_picture_card_set_compact(card, self->is_compact);

  /* Apply cached author info if available */
  AuthorInfo *author = g_hash_table_lookup(self->author_cache, copy->pubkey);
  if (author) {
    gnostr_picture_card_set_author(card, author->display_name, NULL,
                                    author->avatar_url, author->nip05);
    gnostr_picture_card_set_author_lud16(card, author->lud16);
  }

  /* Connect signals */
  g_signal_connect(card, "image-clicked", G_CALLBACK(on_card_image_clicked), self);
  g_signal_connect(card, "author-clicked", G_CALLBACK(on_card_author_clicked), self);
  g_signal_connect(card, "like-clicked", G_CALLBACK(on_card_like_clicked), self);
  g_signal_connect(card, "zap-clicked", G_CALLBACK(on_card_zap_clicked), self);
  g_signal_connect(card, "hashtag-clicked", G_CALLBACK(on_card_hashtag_clicked), self);

  /* Store card reference */
  g_hash_table_insert(self->cards, g_strdup(meta->event_id), card);

  /* Add to flow box */
  gtk_flow_box_insert(GTK_FLOW_BOX(self->flow_box), GTK_WIDGET(card), -1);

  self->load_more_triggered = FALSE;

  update_empty_state(self);
}

void
gnostr_picture_grid_add_pictures(GnostrPictureGrid *self,
                                  const GnostrPictureMeta **pictures,
                                  size_t count) {
  g_return_if_fail(GNOSTR_IS_PICTURE_GRID(self));

  for (size_t i = 0; i < count; i++) {
    if (pictures[i]) {
      gnostr_picture_grid_add_picture(self, pictures[i]);
    }
  }
}

gboolean
gnostr_picture_grid_remove_picture(GnostrPictureGrid *self,
                                    const char *event_id) {
  g_return_val_if_fail(GNOSTR_IS_PICTURE_GRID(self), FALSE);
  g_return_val_if_fail(event_id != NULL, FALSE);

  GnostrPictureCard *card = g_hash_table_lookup(self->cards, event_id);
  if (!card) {
    return FALSE;
  }

  /* Remove from flow box */
  gtk_flow_box_remove(GTK_FLOW_BOX(self->flow_box), GTK_WIDGET(card));

  /* Remove from data structures */
  g_hash_table_remove(self->cards, event_id);
  g_hash_table_remove(self->pictures, event_id);

  /* Remove from order list */
  for (GList *l = self->picture_order; l != NULL; l = l->next) {
    if (g_strcmp0(l->data, event_id) == 0) {
      g_free(l->data);
      self->picture_order = g_list_delete_link(self->picture_order, l);
      break;
    }
  }

  update_empty_state(self);

  return TRUE;
}

gboolean
gnostr_picture_grid_update_picture(GnostrPictureGrid *self,
                                    const GnostrPictureMeta *meta) {
  g_return_val_if_fail(GNOSTR_IS_PICTURE_GRID(self), FALSE);
  g_return_val_if_fail(meta != NULL, FALSE);
  g_return_val_if_fail(meta->event_id != NULL, FALSE);

  GnostrPictureCard *card = g_hash_table_lookup(self->cards, meta->event_id);
  if (!card) {
    return FALSE;
  }

  /* Update stored metadata */
  GnostrPictureMeta *copy = gnostr_picture_meta_copy(meta);
  g_hash_table_replace(self->pictures, g_strdup(meta->event_id), copy);

  /* Update card */
  gnostr_picture_card_set_picture(card, copy);

  return TRUE;
}

void
gnostr_picture_grid_set_author_info(GnostrPictureGrid *self,
                                     const char *pubkey,
                                     const char *display_name,
                                     const char *avatar_url,
                                     const char *nip05,
                                     const char *lud16) {
  g_return_if_fail(GNOSTR_IS_PICTURE_GRID(self));
  g_return_if_fail(pubkey != NULL);

  /* Store in cache */
  AuthorInfo *info = g_new0(AuthorInfo, 1);
  info->pubkey = g_strdup(pubkey);
  info->display_name = g_strdup(display_name);
  info->avatar_url = g_strdup(avatar_url);
  info->nip05 = g_strdup(nip05);
  info->lud16 = g_strdup(lud16);
  g_hash_table_replace(self->author_cache, g_strdup(pubkey), info);

  /* Update all cards by this author */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->pictures);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnostrPictureMeta *meta = value;
    if (g_strcmp0(meta->pubkey, pubkey) == 0) {
      GnostrPictureCard *card = g_hash_table_lookup(self->cards, meta->event_id);
      if (card) {
        gnostr_picture_card_set_author(card, display_name, NULL, avatar_url, nip05);
        gnostr_picture_card_set_author_lud16(card, lud16);
      }
    }
  }
}

void
gnostr_picture_grid_set_reaction_counts(GnostrPictureGrid *self,
                                         const char *event_id,
                                         int likes,
                                         int zaps,
                                         gint64 zap_sats,
                                         int reposts,
                                         int replies) {
  g_return_if_fail(GNOSTR_IS_PICTURE_GRID(self));
  g_return_if_fail(event_id != NULL);

  GnostrPictureCard *card = g_hash_table_lookup(self->cards, event_id);
  if (card) {
    gnostr_picture_card_set_reaction_counts(card, likes, zaps, zap_sats, reposts, replies);
  }
}

void
gnostr_picture_grid_set_columns(GnostrPictureGrid *self,
                                 GnostrPictureGridColumns columns) {
  g_return_if_fail(GNOSTR_IS_PICTURE_GRID(self));

  self->columns = columns;
  update_grid_columns(self);
}

void
gnostr_picture_grid_set_spacing(GnostrPictureGrid *self,
                                 guint spacing) {
  g_return_if_fail(GNOSTR_IS_PICTURE_GRID(self));

  self->spacing = spacing;
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->flow_box), spacing);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->flow_box), spacing);
  gtk_widget_set_margin_start(self->flow_box, spacing);
  gtk_widget_set_margin_end(self->flow_box, spacing);
  gtk_widget_set_margin_top(self->flow_box, spacing);
  gtk_widget_set_margin_bottom(self->flow_box, spacing);
}

void
gnostr_picture_grid_set_loading(GnostrPictureGrid *self,
                                 gboolean loading) {
  g_return_if_fail(GNOSTR_IS_PICTURE_GRID(self));

  self->is_loading = loading;
  gtk_widget_set_visible(self->loading_spinner, loading);

  if (loading) {
    gtk_spinner_start(GTK_SPINNER(self->loading_spinner));
    gtk_widget_set_visible(self->scrolled_window, FALSE);
    gtk_widget_set_visible(self->empty_box, FALSE);
  } else {
    gtk_spinner_stop(GTK_SPINNER(self->loading_spinner));
    gtk_widget_set_visible(self->scrolled_window, TRUE);
    update_empty_state(self);
  }
}

void
gnostr_picture_grid_set_loading_more(GnostrPictureGrid *self,
                                      gboolean loading) {
  g_return_if_fail(GNOSTR_IS_PICTURE_GRID(self));

  self->is_loading_more = loading;
  gtk_widget_set_visible(self->loading_more_box, loading);

  if (loading) {
    gtk_spinner_start(GTK_SPINNER(self->loading_more_spinner));
  } else {
    gtk_spinner_stop(GTK_SPINNER(self->loading_more_spinner));
    self->load_more_triggered = FALSE;
  }
}

void
gnostr_picture_grid_set_empty_message(GnostrPictureGrid *self,
                                       const char *message) {
  g_return_if_fail(GNOSTR_IS_PICTURE_GRID(self));

  g_free(self->empty_message);
  self->empty_message = g_strdup(message ? message : _("No pictures to display"));
  gtk_label_set_text(GTK_LABEL(self->empty_label), self->empty_message);
}

void
gnostr_picture_grid_set_logged_in(GnostrPictureGrid *self,
                                   gboolean logged_in) {
  g_return_if_fail(GNOSTR_IS_PICTURE_GRID(self));

  self->is_logged_in = logged_in;

  /* Update all cards */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->cards);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    gnostr_picture_card_set_logged_in(GNOSTR_PICTURE_CARD(value), logged_in);
  }
}

void
gnostr_picture_grid_show_overlay(GnostrPictureGrid *self,
                                  const char *event_id) {
  g_return_if_fail(GNOSTR_IS_PICTURE_GRID(self));
  g_return_if_fail(event_id != NULL);

  GnostrPictureMeta *meta = g_hash_table_lookup(self->pictures, event_id);
  if (!meta) return;

  const char *url = gnostr_picture_get_thumbnail_url(meta);
  if (!url) return;

  /* Get parent window for transient */
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  GtkWindow *parent = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;

  /* Create image viewer (fresh each time — viewer is destroyed on close) */
  if (!self->image_viewer) {
    self->image_viewer = gnostr_image_viewer_new(parent);
    self->overlay_window = GTK_WIDGET(self->image_viewer);
    g_signal_connect(self->overlay_window, "destroy",
                     G_CALLBACK(on_image_viewer_destroyed), self);
  }

  /* Get all image URLs for gallery navigation */
  size_t url_count = 0;
  char **urls = gnostr_picture_get_all_image_urls(meta, &url_count);

  if (urls && url_count > 1) {
    gnostr_image_viewer_set_gallery(self->image_viewer, (const char * const *)urls, 0);
  } else {
    gnostr_image_viewer_set_image_url(self->image_viewer, url);
  }

  gnostr_image_viewer_present(self->image_viewer);

  if (urls) {
    g_strfreev(urls);
  }
}

void
gnostr_picture_grid_hide_overlay(GnostrPictureGrid *self) {
  g_return_if_fail(GNOSTR_IS_PICTURE_GRID(self));

  if (self->overlay_window) {
    gtk_widget_set_visible(self->overlay_window, FALSE);
  }
}

void
gnostr_picture_grid_scroll_to_top(GnostrPictureGrid *self) {
  g_return_if_fail(GNOSTR_IS_PICTURE_GRID(self));

  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
    GTK_SCROLLED_WINDOW(self->scrolled_window));
  gtk_adjustment_set_value(adj, 0);
}

guint
gnostr_picture_grid_get_picture_count(GnostrPictureGrid *self) {
  g_return_val_if_fail(GNOSTR_IS_PICTURE_GRID(self), 0);
  return g_hash_table_size(self->pictures);
}

const GnostrPictureMeta *
gnostr_picture_grid_find_picture(GnostrPictureGrid *self,
                                  const char *event_id) {
  g_return_val_if_fail(GNOSTR_IS_PICTURE_GRID(self), NULL);
  g_return_val_if_fail(event_id != NULL, NULL);

  return g_hash_table_lookup(self->pictures, event_id);
}

void
gnostr_picture_grid_set_compact(GnostrPictureGrid *self,
                                 gboolean compact) {
  g_return_if_fail(GNOSTR_IS_PICTURE_GRID(self));

  if (self->is_compact == compact) return;

  self->is_compact = compact;

  /* Update all cards */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->cards);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    gnostr_picture_card_set_compact(GNOSTR_PICTURE_CARD(value), compact);
  }
}

/* Internal: Update grid column configuration */
static void
update_grid_columns(GnostrPictureGrid *self) {
  int cols = self->columns;
  if (cols == GNOSTR_PICTURE_GRID_AUTO) {
    cols = 3;  /* Default, will be adjusted on size-allocate */
  }

  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->flow_box), cols);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->flow_box), cols);
}

/* Internal: Update empty state visibility */
static void
update_empty_state(GnostrPictureGrid *self) {
  gboolean is_empty = g_hash_table_size(self->pictures) == 0;

  if (is_empty && !self->is_loading) {
    gtk_widget_set_visible(self->empty_box, TRUE);
    gtk_widget_set_visible(self->scrolled_window, FALSE);
  } else {
    gtk_widget_set_visible(self->empty_box, FALSE);
    gtk_widget_set_visible(self->scrolled_window, TRUE);
  }
}

/* Image viewer destroy handler - clears stale pointers (nostrc-rvzp) */
static void
on_image_viewer_destroyed(GtkWidget *widget, gpointer user_data) {
  GnostrPictureGrid *self = GNOSTR_PICTURE_GRID(user_data);
  (void)widget;
  self->image_viewer = NULL;
  self->overlay_window = NULL;
}

/* Card signal handlers */
static void
on_card_image_clicked(GnostrPictureCard *card, gpointer user_data) {
  GnostrPictureGrid *self = GNOSTR_PICTURE_GRID(user_data);
  const char *event_id = gnostr_picture_card_get_event_id(card);

  if (event_id) {
    gnostr_picture_grid_show_overlay(self, event_id);
    g_signal_emit(self, signals[SIGNAL_PICTURE_CLICKED], 0, event_id);
  }
}

static void
on_card_author_clicked(GnostrPictureCard *card, const char *pubkey,
                       gpointer user_data) {
  GnostrPictureGrid *self = GNOSTR_PICTURE_GRID(user_data);
  (void)card;

  if (pubkey) {
    g_signal_emit(self, signals[SIGNAL_AUTHOR_CLICKED], 0, pubkey);
  }
}

static void
on_card_like_clicked(GnostrPictureCard *card, gpointer user_data) {
  GnostrPictureGrid *self = GNOSTR_PICTURE_GRID(user_data);
  const char *event_id = gnostr_picture_card_get_event_id(card);

  if (event_id) {
    g_signal_emit(self, signals[SIGNAL_LIKE_CLICKED], 0, event_id);
  }
}

static void
on_card_zap_clicked(GnostrPictureCard *card, gpointer user_data) {
  GnostrPictureGrid *self = GNOSTR_PICTURE_GRID(user_data);
  const char *event_id = gnostr_picture_card_get_event_id(card);
  const char *pubkey = gnostr_picture_card_get_pubkey(card);

  if (event_id && pubkey) {
    /* Get lud16 from author cache */
    AuthorInfo *author = g_hash_table_lookup(self->author_cache, pubkey);
    const char *lud16 = author ? author->lud16 : NULL;
    g_signal_emit(self, signals[SIGNAL_ZAP_CLICKED], 0, event_id, pubkey, lud16);
  }
}

static void
on_card_hashtag_clicked(GnostrPictureCard *card, const char *tag,
                        gpointer user_data) {
  GnostrPictureGrid *self = GNOSTR_PICTURE_GRID(user_data);
  (void)card;

  if (tag) {
    g_signal_emit(self, signals[SIGNAL_HASHTAG_CLICKED], 0, tag);
  }
}
