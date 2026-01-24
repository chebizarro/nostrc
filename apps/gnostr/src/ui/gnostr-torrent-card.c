/*
 * gnostr-torrent-card.c - NIP-35 Torrent Card Widget
 *
 * GTK4 widget for displaying NIP-35 kind 2003 torrent events.
 */

#include "gnostr-torrent-card.h"
#include "gnostr-avatar-cache.h"
#include "../util/nip35_torrents.h"
#include "../util/nip05.h"
#include <glib/gi18n.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

/* Maximum files to show in collapsed view */
#define MAX_FILES_COLLAPSED 5

struct _GnostrTorrentCard {
  GtkWidget parent_instance;

  /* Root container */
  GtkWidget *root;

  /* Author section */
  GtkWidget *btn_avatar;
  GtkWidget *avatar_overlay;
  GtkWidget *avatar_image;
  GtkWidget *avatar_initials;
  GtkWidget *btn_author_name;
  GtkWidget *lbl_author_name;
  GtkWidget *lbl_author_handle;
  GtkWidget *lbl_publish_date;
  GtkWidget *nip05_badge;

  /* Title and description */
  GtkWidget *btn_title;
  GtkWidget *lbl_title;
  GtkWidget *lbl_description;

  /* Info section */
  GtkWidget *lbl_size;
  GtkWidget *lbl_file_count;
  GtkWidget *lbl_infohash;

  /* Files list */
  GtkWidget *files_expander;
  GtkWidget *files_list;

  /* Categories */
  GtkWidget *categories_box;

  /* External references */
  GtkWidget *references_box;

  /* Action buttons */
  GtkWidget *btn_copy_magnet;
  GtkWidget *btn_open_magnet;
  GtkWidget *btn_zap;
  GtkWidget *btn_bookmark;
  GtkWidget *btn_menu;
  GtkWidget *menu_popover;

  /* State */
  gchar *event_id;
  gchar *pubkey_hex;
  gchar *infohash;
  gchar *title;
  gchar *author_lud16;
  gchar *nip05;
  gint64 created_at;
  gint64 total_size;
  gboolean is_bookmarked;
  gboolean is_logged_in;

  /* Trackers for magnet generation */
  GPtrArray *trackers;

  /* Cancellables */
  GCancellable *nip05_cancellable;

#ifdef HAVE_SOUP3
  GCancellable *avatar_cancellable;
  SoupSession *session;
#endif
};

G_DEFINE_TYPE(GnostrTorrentCard, gnostr_torrent_card, GTK_TYPE_WIDGET)

enum {
  SIGNAL_OPEN_PROFILE,
  SIGNAL_OPEN_TORRENT,
  SIGNAL_OPEN_URL,
  SIGNAL_COPY_MAGNET,
  SIGNAL_OPEN_MAGNET,
  SIGNAL_ZAP_REQUESTED,
  SIGNAL_BOOKMARK_TOGGLED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_avatar_clicked(GtkButton *btn, gpointer user_data);
static void on_author_name_clicked(GtkButton *btn, gpointer user_data);
static void on_title_clicked(GtkButton *btn, gpointer user_data);
static void on_copy_magnet_clicked(GtkButton *btn, gpointer user_data);
static void on_open_magnet_clicked(GtkButton *btn, gpointer user_data);
static void on_zap_clicked(GtkButton *btn, gpointer user_data);
static void on_bookmark_clicked(GtkButton *btn, gpointer user_data);
static void on_menu_clicked(GtkButton *btn, gpointer user_data);

/* ---- Widget lifecycle ---- */

static void gnostr_torrent_card_dispose(GObject *obj) {
  GnostrTorrentCard *self = GNOSTR_TORRENT_CARD(obj);

  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
  }

#ifdef HAVE_SOUP3
  if (self->avatar_cancellable) {
    g_cancellable_cancel(self->avatar_cancellable);
    g_clear_object(&self->avatar_cancellable);
  }
  g_clear_object(&self->session);
#endif

  if (self->menu_popover) {
    gtk_widget_unparent(self->menu_popover);
    self->menu_popover = NULL;
  }

  /* Unparent the root widget */
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_widget_unparent(child);
    child = next;
  }

  G_OBJECT_CLASS(gnostr_torrent_card_parent_class)->dispose(obj);
}

static void gnostr_torrent_card_finalize(GObject *obj) {
  GnostrTorrentCard *self = GNOSTR_TORRENT_CARD(obj);

  g_clear_pointer(&self->event_id, g_free);
  g_clear_pointer(&self->pubkey_hex, g_free);
  g_clear_pointer(&self->infohash, g_free);
  g_clear_pointer(&self->title, g_free);
  g_clear_pointer(&self->author_lud16, g_free);
  g_clear_pointer(&self->nip05, g_free);

  if (self->trackers) {
    g_ptr_array_free(self->trackers, TRUE);
  }

  G_OBJECT_CLASS(gnostr_torrent_card_parent_class)->finalize(obj);
}

/* ---- Helper functions ---- */

static gchar *format_timestamp(gint64 timestamp) {
  if (timestamp <= 0) return g_strdup(_("Unknown date"));

  GDateTime *dt = g_date_time_new_from_unix_local(timestamp);
  if (!dt) return g_strdup(_("Unknown date"));

  GDateTime *now = g_date_time_new_now_local();
  GTimeSpan diff = g_date_time_difference(now, dt);
  g_date_time_unref(now);

  gchar *result;
  gint64 seconds = diff / G_TIME_SPAN_SECOND;

  if (seconds < 60) {
    result = g_strdup(_("Just now"));
  } else if (seconds < 3600) {
    gint minutes = (gint)(seconds / 60);
    result = g_strdup_printf(g_dngettext(NULL, "%d minute ago", "%d minutes ago", minutes), minutes);
  } else if (seconds < 86400) {
    gint hours = (gint)(seconds / 3600);
    result = g_strdup_printf(g_dngettext(NULL, "%d hour ago", "%d hours ago", hours), hours);
  } else if (seconds < 604800) {
    gint days = (gint)(seconds / 86400);
    result = g_strdup_printf(g_dngettext(NULL, "%d day ago", "%d days ago", days), days);
  } else {
    result = g_date_time_format(dt, "%B %d, %Y");
  }

  g_date_time_unref(dt);
  return result;
}

static void set_avatar_initials(GnostrTorrentCard *self, const char *display, const char *handle) {
  if (!GTK_IS_LABEL(self->avatar_initials)) return;

  const char *src = (display && *display) ? display : (handle && *handle ? handle : "AN");
  char initials[3] = {0};
  int i = 0;

  for (const char *p = src; *p && i < 2; p++) {
    if (g_ascii_isalnum(*p)) {
      initials[i++] = g_ascii_toupper(*p);
    }
  }
  if (i == 0) {
    initials[0] = 'A';
    initials[1] = 'N';
  }

  gtk_label_set_text(GTK_LABEL(self->avatar_initials), initials);
  if (self->avatar_image) gtk_widget_set_visible(self->avatar_image, FALSE);
  gtk_widget_set_visible(self->avatar_initials, TRUE);
}

/* ---- UI Building ---- */

static GtkWidget *create_file_row(const char *path, gint64 size) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(row, 8);
  gtk_widget_set_margin_end(row, 8);
  gtk_widget_set_margin_top(row, 4);
  gtk_widget_set_margin_bottom(row, 4);

  /* File icon */
  GtkWidget *icon = gtk_image_new_from_icon_name("text-x-generic-symbolic");
  gtk_box_append(GTK_BOX(row), icon);

  /* File path */
  GtkWidget *lbl_path = gtk_label_new(path);
  gtk_label_set_xalign(GTK_LABEL(lbl_path), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(lbl_path), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand(lbl_path, TRUE);
  gtk_widget_set_tooltip_text(lbl_path, path);
  gtk_widget_add_css_class(lbl_path, "torrent-file-path");
  gtk_box_append(GTK_BOX(row), lbl_path);

  /* File size */
  if (size >= 0) {
    gchar *size_str = gnostr_torrent_format_size(size);
    GtkWidget *lbl_size = gtk_label_new(size_str);
    gtk_widget_add_css_class(lbl_size, "torrent-file-size");
    gtk_widget_add_css_class(lbl_size, "dim-label");
    gtk_box_append(GTK_BOX(row), lbl_size);
    g_free(size_str);
  }

  return row;
}

static GtkWidget *create_category_pill(const char *category) {
  GtkWidget *pill = gtk_label_new(category);
  gtk_widget_add_css_class(pill, "torrent-category-pill");
  gtk_widget_add_css_class(pill, "pill");
  return pill;
}

static void build_ui(GnostrTorrentCard *self) {
  /* Root frame */
  self->root = gtk_frame_new(NULL);
  gtk_widget_set_hexpand(self->root, TRUE);
  gtk_widget_add_css_class(self->root, "torrent-card");
  gtk_widget_set_parent(self->root, GTK_WIDGET(self));

  /* Main content box */
  GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(main_box, 16);
  gtk_widget_set_margin_end(main_box, 16);
  gtk_widget_set_margin_top(main_box, 16);
  gtk_widget_set_margin_bottom(main_box, 12);
  gtk_frame_set_child(GTK_FRAME(self->root), main_box);

  /* ---- Author row ---- */
  GtkWidget *author_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_append(GTK_BOX(main_box), author_box);

  /* Avatar button */
  self->btn_avatar = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_avatar), FALSE);
  gtk_widget_set_tooltip_text(self->btn_avatar, _("View profile"));
  gtk_widget_set_valign(self->btn_avatar, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(self->btn_avatar, "flat");
  g_signal_connect(self->btn_avatar, "clicked", G_CALLBACK(on_avatar_clicked), self);
  gtk_box_append(GTK_BOX(author_box), self->btn_avatar);

  /* Avatar overlay */
  self->avatar_overlay = gtk_overlay_new();
  gtk_widget_set_size_request(self->avatar_overlay, 40, 40);
  gtk_widget_add_css_class(self->avatar_overlay, "avatar");
  gtk_button_set_child(GTK_BUTTON(self->btn_avatar), self->avatar_overlay);

  self->avatar_image = gtk_picture_new();
  gtk_picture_set_content_fit(GTK_PICTURE(self->avatar_image), GTK_CONTENT_FIT_COVER);
  gtk_widget_set_size_request(self->avatar_image, 40, 40);
  gtk_widget_set_visible(self->avatar_image, FALSE);
  gtk_overlay_set_child(GTK_OVERLAY(self->avatar_overlay), self->avatar_image);

  self->avatar_initials = gtk_label_new("AN");
  gtk_widget_set_halign(self->avatar_initials, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->avatar_initials, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(self->avatar_initials, "avatar-initials");
  gtk_overlay_add_overlay(GTK_OVERLAY(self->avatar_overlay), self->avatar_initials);

  /* Author info column */
  GtkWidget *author_info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_valign(author_info, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(author_info, TRUE);
  gtk_box_append(GTK_BOX(author_box), author_info);

  /* Author name row */
  GtkWidget *name_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(author_info), name_row);

  self->btn_author_name = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_author_name), FALSE);
  gtk_widget_set_tooltip_text(self->btn_author_name, _("View profile"));
  gtk_widget_add_css_class(self->btn_author_name, "flat");
  g_signal_connect(self->btn_author_name, "clicked", G_CALLBACK(on_author_name_clicked), self);
  gtk_box_append(GTK_BOX(name_row), self->btn_author_name);

  self->lbl_author_name = gtk_label_new(_("Anonymous"));
  gtk_label_set_xalign(GTK_LABEL(self->lbl_author_name), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(self->lbl_author_name), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(self->lbl_author_name, "torrent-author");
  gtk_button_set_child(GTK_BUTTON(self->btn_author_name), self->lbl_author_name);

  self->nip05_badge = gtk_image_new_from_icon_name("emblem-ok-symbolic");
  gtk_image_set_icon_size(GTK_IMAGE(self->nip05_badge), GTK_ICON_SIZE_INHERIT);
  gtk_widget_set_visible(self->nip05_badge, FALSE);
  gtk_widget_add_css_class(self->nip05_badge, "nip05-verified-badge");
  gtk_box_append(GTK_BOX(name_row), self->nip05_badge);

  /* Meta row (handle + date) */
  GtkWidget *meta_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(author_info), meta_row);

  self->lbl_author_handle = gtk_label_new("@anon");
  gtk_label_set_xalign(GTK_LABEL(self->lbl_author_handle), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(self->lbl_author_handle), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(self->lbl_author_handle, "torrent-meta");
  gtk_widget_add_css_class(self->lbl_author_handle, "dim-label");
  gtk_box_append(GTK_BOX(meta_row), self->lbl_author_handle);

  GtkWidget *separator = gtk_label_new("-");
  gtk_widget_add_css_class(separator, "torrent-meta");
  gtk_widget_add_css_class(separator, "dim-label");
  gtk_box_append(GTK_BOX(meta_row), separator);

  self->lbl_publish_date = gtk_label_new(_("Just now"));
  gtk_label_set_xalign(GTK_LABEL(self->lbl_publish_date), 0.0);
  gtk_widget_add_css_class(self->lbl_publish_date, "torrent-meta");
  gtk_widget_add_css_class(self->lbl_publish_date, "dim-label");
  gtk_box_append(GTK_BOX(meta_row), self->lbl_publish_date);

  /* Menu button */
  self->btn_menu = gtk_button_new_from_icon_name("open-menu-symbolic");
  gtk_widget_set_tooltip_text(self->btn_menu, _("More options"));
  gtk_widget_set_valign(self->btn_menu, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(self->btn_menu, "flat");
  g_signal_connect(self->btn_menu, "clicked", G_CALLBACK(on_menu_clicked), self);
  gtk_box_append(GTK_BOX(author_box), self->btn_menu);

  /* ---- Title ---- */
  self->btn_title = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_title), FALSE);
  gtk_widget_add_css_class(self->btn_title, "flat");
  g_signal_connect(self->btn_title, "clicked", G_CALLBACK(on_title_clicked), self);
  gtk_box_append(GTK_BOX(main_box), self->btn_title);

  GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_button_set_child(GTK_BUTTON(self->btn_title), title_box);

  GtkWidget *torrent_icon = gtk_image_new_from_icon_name("folder-download-symbolic");
  gtk_widget_add_css_class(torrent_icon, "torrent-icon");
  gtk_box_append(GTK_BOX(title_box), torrent_icon);

  self->lbl_title = gtk_label_new(_("Untitled Torrent"));
  gtk_label_set_xalign(GTK_LABEL(self->lbl_title), 0.0);
  gtk_label_set_wrap(GTK_LABEL(self->lbl_title), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(self->lbl_title), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_lines(GTK_LABEL(self->lbl_title), 2);
  gtk_label_set_ellipsize(GTK_LABEL(self->lbl_title), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(self->lbl_title, TRUE);
  gtk_widget_add_css_class(self->lbl_title, "torrent-title");
  gtk_box_append(GTK_BOX(title_box), self->lbl_title);

  /* ---- Description ---- */
  self->lbl_description = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->lbl_description), 0.0);
  gtk_label_set_wrap(GTK_LABEL(self->lbl_description), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(self->lbl_description), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_lines(GTK_LABEL(self->lbl_description), 3);
  gtk_label_set_ellipsize(GTK_LABEL(self->lbl_description), PANGO_ELLIPSIZE_END);
  gtk_widget_set_visible(self->lbl_description, FALSE);
  gtk_widget_add_css_class(self->lbl_description, "torrent-description");
  gtk_box_append(GTK_BOX(main_box), self->lbl_description);

  /* ---- Info row ---- */
  GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_add_css_class(info_box, "torrent-info-row");
  gtk_box_append(GTK_BOX(main_box), info_box);

  /* Size */
  GtkWidget *size_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *size_icon = gtk_image_new_from_icon_name("drive-harddisk-symbolic");
  gtk_widget_add_css_class(size_icon, "dim-label");
  gtk_box_append(GTK_BOX(size_box), size_icon);
  self->lbl_size = gtk_label_new(_("Unknown size"));
  gtk_widget_add_css_class(self->lbl_size, "torrent-info");
  gtk_box_append(GTK_BOX(size_box), self->lbl_size);
  gtk_box_append(GTK_BOX(info_box), size_box);

  /* File count */
  GtkWidget *files_box_info = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *files_icon = gtk_image_new_from_icon_name("folder-symbolic");
  gtk_widget_add_css_class(files_icon, "dim-label");
  gtk_box_append(GTK_BOX(files_box_info), files_icon);
  self->lbl_file_count = gtk_label_new(_("0 files"));
  gtk_widget_add_css_class(self->lbl_file_count, "torrent-info");
  gtk_box_append(GTK_BOX(files_box_info), self->lbl_file_count);
  gtk_box_append(GTK_BOX(info_box), files_box_info);

  /* Infohash (truncated) */
  GtkWidget *hash_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *hash_icon = gtk_image_new_from_icon_name("fingerprint-symbolic");
  gtk_widget_add_css_class(hash_icon, "dim-label");
  gtk_box_append(GTK_BOX(hash_box), hash_icon);
  self->lbl_infohash = gtk_label_new("...");
  gtk_label_set_ellipsize(GTK_LABEL(self->lbl_infohash), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(GTK_LABEL(self->lbl_infohash), 12);
  gtk_widget_add_css_class(self->lbl_infohash, "torrent-info");
  gtk_widget_add_css_class(self->lbl_infohash, "monospace");
  gtk_box_append(GTK_BOX(hash_box), self->lbl_infohash);
  gtk_box_append(GTK_BOX(info_box), hash_box);

  /* ---- Files expander ---- */
  self->files_expander = gtk_expander_new(_("Files"));
  gtk_widget_set_visible(self->files_expander, FALSE);
  gtk_widget_add_css_class(self->files_expander, "torrent-files-expander");
  gtk_box_append(GTK_BOX(main_box), self->files_expander);

  self->files_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(self->files_list, "torrent-files-list");
  gtk_expander_set_child(GTK_EXPANDER(self->files_expander), self->files_list);

  /* ---- Categories ---- */
  self->categories_box = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->categories_box), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->categories_box), 10);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->categories_box), 1);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->categories_box), 4);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->categories_box), 6);
  gtk_widget_set_visible(self->categories_box, FALSE);
  gtk_widget_add_css_class(self->categories_box, "torrent-categories");
  gtk_box_append(GTK_BOX(main_box), self->categories_box);

  /* ---- External references ---- */
  self->references_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_visible(self->references_box, FALSE);
  gtk_widget_add_css_class(self->references_box, "torrent-references");
  gtk_box_append(GTK_BOX(main_box), self->references_box);

  /* ---- Action buttons ---- */
  GtkWidget *actions_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(actions_box, 4);
  gtk_box_append(GTK_BOX(main_box), actions_box);

  /* Copy magnet button */
  self->btn_copy_magnet = gtk_button_new();
  GtkWidget *copy_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *copy_icon = gtk_image_new_from_icon_name("edit-copy-symbolic");
  GtkWidget *copy_label = gtk_label_new(_("Copy Magnet"));
  gtk_box_append(GTK_BOX(copy_box), copy_icon);
  gtk_box_append(GTK_BOX(copy_box), copy_label);
  gtk_button_set_child(GTK_BUTTON(self->btn_copy_magnet), copy_box);
  gtk_widget_set_tooltip_text(self->btn_copy_magnet, _("Copy magnet link to clipboard"));
  g_signal_connect(self->btn_copy_magnet, "clicked", G_CALLBACK(on_copy_magnet_clicked), self);
  gtk_box_append(GTK_BOX(actions_box), self->btn_copy_magnet);

  /* Open magnet button */
  self->btn_open_magnet = gtk_button_new_from_icon_name("emblem-downloads-symbolic");
  gtk_widget_set_tooltip_text(self->btn_open_magnet, _("Open in torrent client"));
  g_signal_connect(self->btn_open_magnet, "clicked", G_CALLBACK(on_open_magnet_clicked), self);
  gtk_box_append(GTK_BOX(actions_box), self->btn_open_magnet);

  /* Zap button */
  self->btn_zap = gtk_button_new_from_icon_name("camera-flash-symbolic");
  gtk_widget_set_tooltip_text(self->btn_zap, _("Zap"));
  gtk_widget_set_sensitive(self->btn_zap, FALSE);
  g_signal_connect(self->btn_zap, "clicked", G_CALLBACK(on_zap_clicked), self);
  gtk_box_append(GTK_BOX(actions_box), self->btn_zap);

  /* Bookmark button */
  self->btn_bookmark = gtk_button_new_from_icon_name("bookmark-new-symbolic");
  gtk_widget_set_tooltip_text(self->btn_bookmark, _("Bookmark"));
  gtk_widget_set_sensitive(self->btn_bookmark, FALSE);
  g_signal_connect(self->btn_bookmark, "clicked", G_CALLBACK(on_bookmark_clicked), self);
  gtk_box_append(GTK_BOX(actions_box), self->btn_bookmark);
}

/* ---- Class initialization ---- */

static void gnostr_torrent_card_class_init(GnostrTorrentCardClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);

  gclass->dispose = gnostr_torrent_card_dispose;
  gclass->finalize = gnostr_torrent_card_finalize;

  gtk_widget_class_set_layout_manager_type(wclass, GTK_TYPE_BOX_LAYOUT);

  /* Signals */
  signals[SIGNAL_OPEN_PROFILE] = g_signal_new("open-profile",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_TORRENT] = g_signal_new("open-torrent",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_URL] = g_signal_new("open-url",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_COPY_MAGNET] = g_signal_new("copy-magnet",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_MAGNET] = g_signal_new("open-magnet",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_ZAP_REQUESTED] = g_signal_new("zap-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIGNAL_BOOKMARK_TOGGLED] = g_signal_new("bookmark-toggled",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);
}

static void gnostr_torrent_card_init(GnostrTorrentCard *self) {
  self->total_size = -1;
  self->trackers = g_ptr_array_new_with_free_func(g_free);

  build_ui(self);

  gtk_widget_add_css_class(GTK_WIDGET(self), "torrent-card");

#ifdef HAVE_SOUP3
  self->avatar_cancellable = g_cancellable_new();
  self->session = soup_session_new();
  soup_session_set_timeout(self->session, 30);
#endif
}

/* ---- Button handlers ---- */

static void on_avatar_clicked(GtkButton *btn, gpointer user_data) {
  GnostrTorrentCard *self = GNOSTR_TORRENT_CARD(user_data);
  (void)btn;
  if (self->pubkey_hex && *self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey_hex);
  }
}

static void on_author_name_clicked(GtkButton *btn, gpointer user_data) {
  GnostrTorrentCard *self = GNOSTR_TORRENT_CARD(user_data);
  (void)btn;
  if (self->pubkey_hex && *self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey_hex);
  }
}

static void on_title_clicked(GtkButton *btn, gpointer user_data) {
  GnostrTorrentCard *self = GNOSTR_TORRENT_CARD(user_data);
  (void)btn;
  if (self->event_id && *self->event_id) {
    g_signal_emit(self, signals[SIGNAL_OPEN_TORRENT], 0, self->event_id);
  }
}

static void on_copy_magnet_clicked(GtkButton *btn, gpointer user_data) {
  GnostrTorrentCard *self = GNOSTR_TORRENT_CARD(user_data);
  (void)btn;

  gchar *magnet = gnostr_torrent_card_get_magnet(self);
  if (magnet) {
    /* Copy to clipboard */
    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
    gdk_clipboard_set_text(clipboard, magnet);

    g_signal_emit(self, signals[SIGNAL_COPY_MAGNET], 0, magnet);
    g_free(magnet);
  }
}

static void on_open_magnet_clicked(GtkButton *btn, gpointer user_data) {
  GnostrTorrentCard *self = GNOSTR_TORRENT_CARD(user_data);
  (void)btn;

  gchar *magnet = gnostr_torrent_card_get_magnet(self);
  if (magnet) {
    g_signal_emit(self, signals[SIGNAL_OPEN_MAGNET], 0, magnet);
    g_free(magnet);
  }
}

static void on_zap_clicked(GtkButton *btn, gpointer user_data) {
  GnostrTorrentCard *self = GNOSTR_TORRENT_CARD(user_data);
  (void)btn;
  if (self->event_id && self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_ZAP_REQUESTED], 0,
                  self->event_id, self->pubkey_hex, self->author_lud16);
  }
}

static void on_bookmark_clicked(GtkButton *btn, gpointer user_data) {
  GnostrTorrentCard *self = GNOSTR_TORRENT_CARD(user_data);
  (void)btn;

  if (!self->event_id) return;

  self->is_bookmarked = !self->is_bookmarked;
  gtk_button_set_icon_name(GTK_BUTTON(self->btn_bookmark),
    self->is_bookmarked ? "user-bookmarks-symbolic" : "bookmark-new-symbolic");

  g_signal_emit(self, signals[SIGNAL_BOOKMARK_TOGGLED], 0,
                self->event_id, self->is_bookmarked);
}

static void on_menu_clicked(GtkButton *btn, gpointer user_data) {
  GnostrTorrentCard *self = GNOSTR_TORRENT_CARD(user_data);
  (void)btn;

  if (!self->menu_popover) {
    self->menu_popover = gtk_popover_new();
    gtk_widget_set_parent(self->menu_popover, GTK_WIDGET(self->btn_menu));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    /* Copy infohash */
    GtkWidget *copy_hash_btn = gtk_button_new();
    GtkWidget *copy_hash_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *copy_hash_icon = gtk_image_new_from_icon_name("fingerprint-symbolic");
    GtkWidget *copy_hash_label = gtk_label_new(_("Copy Infohash"));
    gtk_box_append(GTK_BOX(copy_hash_box), copy_hash_icon);
    gtk_box_append(GTK_BOX(copy_hash_box), copy_hash_label);
    gtk_button_set_child(GTK_BUTTON(copy_hash_btn), copy_hash_box);
    gtk_button_set_has_frame(GTK_BUTTON(copy_hash_btn), FALSE);
    g_signal_connect_swapped(copy_hash_btn, "clicked", G_CALLBACK(gtk_popover_popdown), self->menu_popover);
    gtk_box_append(GTK_BOX(box), copy_hash_btn);

    /* View author */
    GtkWidget *profile_btn = gtk_button_new();
    GtkWidget *profile_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *profile_icon = gtk_image_new_from_icon_name("avatar-default-symbolic");
    GtkWidget *profile_label = gtk_label_new(_("View Uploader"));
    gtk_box_append(GTK_BOX(profile_box), profile_icon);
    gtk_box_append(GTK_BOX(profile_box), profile_label);
    gtk_button_set_child(GTK_BUTTON(profile_btn), profile_box);
    gtk_button_set_has_frame(GTK_BUTTON(profile_btn), FALSE);
    g_signal_connect(profile_btn, "clicked", G_CALLBACK(on_avatar_clicked), self);
    gtk_box_append(GTK_BOX(box), profile_btn);

    gtk_popover_set_child(GTK_POPOVER(self->menu_popover), box);
  }

  gtk_popover_popup(GTK_POPOVER(self->menu_popover));
}

/* ---- Public API ---- */

GnostrTorrentCard *gnostr_torrent_card_new(void) {
  return g_object_new(GNOSTR_TYPE_TORRENT_CARD, NULL);
}

void gnostr_torrent_card_set_torrent(GnostrTorrentCard *self,
                                      const char *event_id,
                                      const char *title,
                                      const char *description,
                                      const char *infohash,
                                      gint64 created_at) {
  g_return_if_fail(GNOSTR_IS_TORRENT_CARD(self));

  g_clear_pointer(&self->event_id, g_free);
  g_clear_pointer(&self->title, g_free);
  g_clear_pointer(&self->infohash, g_free);

  self->event_id = g_strdup(event_id);
  self->title = g_strdup(title);
  self->infohash = infohash ? g_ascii_strdown(infohash, -1) : NULL;
  self->created_at = created_at;

  /* Update UI */
  gtk_label_set_text(GTK_LABEL(self->lbl_title),
    (title && *title) ? title : _("Untitled Torrent"));

  if (description && *description) {
    gtk_label_set_text(GTK_LABEL(self->lbl_description), description);
    gtk_widget_set_visible(self->lbl_description, TRUE);
  } else {
    gtk_widget_set_visible(self->lbl_description, FALSE);
  }

  if (self->infohash) {
    gtk_label_set_text(GTK_LABEL(self->lbl_infohash), self->infohash);
    gtk_widget_set_tooltip_text(self->lbl_infohash, self->infohash);
  }

  gchar *date_str = format_timestamp(created_at);
  gtk_label_set_text(GTK_LABEL(self->lbl_publish_date), date_str);
  g_free(date_str);
}

void gnostr_torrent_card_set_author(GnostrTorrentCard *self,
                                     const char *display_name,
                                     const char *handle,
                                     const char *avatar_url,
                                     const char *pubkey_hex) {
  g_return_if_fail(GNOSTR_IS_TORRENT_CARD(self));

  g_clear_pointer(&self->pubkey_hex, g_free);
  self->pubkey_hex = g_strdup(pubkey_hex);

  gtk_label_set_text(GTK_LABEL(self->lbl_author_name),
    (display_name && *display_name) ? display_name : (handle ? handle : _("Anonymous")));

  gchar *handle_str = g_strdup_printf("@%s", (handle && *handle) ? handle : "anon");
  gtk_label_set_text(GTK_LABEL(self->lbl_author_handle), handle_str);
  g_free(handle_str);

  set_avatar_initials(self, display_name, handle);

#ifdef HAVE_SOUP3
  if (avatar_url && *avatar_url && GTK_IS_PICTURE(self->avatar_image)) {
    GdkTexture *cached = gnostr_avatar_try_load_cached(avatar_url);
    if (cached) {
      gtk_picture_set_paintable(GTK_PICTURE(self->avatar_image), GDK_PAINTABLE(cached));
      gtk_widget_set_visible(self->avatar_image, TRUE);
      gtk_widget_set_visible(self->avatar_initials, FALSE);
      g_object_unref(cached);
    } else {
      gnostr_avatar_download_async(avatar_url, self->avatar_image, self->avatar_initials);
    }
  }
#else
  (void)avatar_url;
#endif
}

void gnostr_torrent_card_add_file(GnostrTorrentCard *self,
                                   const char *path,
                                   gint64 size) {
  g_return_if_fail(GNOSTR_IS_TORRENT_CARD(self));
  if (!path) return;

  GtkWidget *row = create_file_row(path, size);
  gtk_box_append(GTK_BOX(self->files_list), row);

  /* Update file count */
  GtkWidget *child = gtk_widget_get_first_child(self->files_list);
  int count = 0;
  while (child) {
    count++;
    child = gtk_widget_get_next_sibling(child);
  }

  gchar *count_str = g_strdup_printf(g_dngettext(NULL, "%d file", "%d files", count), count);
  gtk_label_set_text(GTK_LABEL(self->lbl_file_count), count_str);
  g_free(count_str);

  /* Update total size */
  if (size >= 0) {
    if (self->total_size < 0) {
      self->total_size = size;
    } else {
      self->total_size += size;
    }
    gchar *size_str = gnostr_torrent_format_size(self->total_size);
    gtk_label_set_text(GTK_LABEL(self->lbl_size), size_str);
    g_free(size_str);
  }

  /* Show expander */
  gtk_widget_set_visible(self->files_expander, TRUE);
}

void gnostr_torrent_card_set_total_size(GnostrTorrentCard *self,
                                         gint64 total_size) {
  g_return_if_fail(GNOSTR_IS_TORRENT_CARD(self));

  self->total_size = total_size;
  if (total_size >= 0) {
    gchar *size_str = gnostr_torrent_format_size(total_size);
    gtk_label_set_text(GTK_LABEL(self->lbl_size), size_str);
    g_free(size_str);
  }
}

void gnostr_torrent_card_add_tracker(GnostrTorrentCard *self,
                                      const char *tracker_url) {
  g_return_if_fail(GNOSTR_IS_TORRENT_CARD(self));
  if (!tracker_url || !*tracker_url) return;

  g_ptr_array_add(self->trackers, g_strdup(tracker_url));
}

void gnostr_torrent_card_add_category(GnostrTorrentCard *self,
                                       const char *category) {
  g_return_if_fail(GNOSTR_IS_TORRENT_CARD(self));
  if (!category || !*category) return;

  GtkWidget *pill = create_category_pill(category);
  gtk_flow_box_append(GTK_FLOW_BOX(self->categories_box), pill);
  gtk_widget_set_visible(self->categories_box, TRUE);
}

static void on_reference_clicked(GtkButton *btn, gpointer user_data) {
  GnostrTorrentCard *self = GNOSTR_TORRENT_CARD(user_data);
  const char *url = g_object_get_data(G_OBJECT(btn), "reference-url");
  if (url && *url) {
    g_signal_emit(self, signals[SIGNAL_OPEN_URL], 0, url);
  }
}

void gnostr_torrent_card_add_reference(GnostrTorrentCard *self,
                                        const char *prefix,
                                        const char *value) {
  g_return_if_fail(GNOSTR_IS_TORRENT_CARD(self));
  if (!prefix || !value) return;

  GnostrTorrentReference ref = { .prefix = (gchar *)prefix, .value = (gchar *)value };
  gchar *url = gnostr_torrent_get_reference_url(&ref);

  /* Create button with icon/label */
  GtkWidget *btn = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
  gtk_widget_add_css_class(btn, "flat");
  gtk_widget_add_css_class(btn, "torrent-reference-link");

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *icon = gtk_image_new_from_icon_name("web-browser-symbolic");
  GtkWidget *label = gtk_label_new(prefix);
  gtk_widget_add_css_class(label, "caption");
  gtk_box_append(GTK_BOX(box), icon);
  gtk_box_append(GTK_BOX(box), label);
  gtk_button_set_child(GTK_BUTTON(btn), box);

  if (url) {
    gchar *tooltip = g_strdup_printf("%s: %s", prefix, value);
    gtk_widget_set_tooltip_text(btn, tooltip);
    g_free(tooltip);

    g_object_set_data_full(G_OBJECT(btn), "reference-url", url, g_free);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_reference_clicked), self);
  } else {
    gtk_widget_set_tooltip_text(btn, value);
    gtk_widget_set_sensitive(btn, FALSE);
  }

  gtk_box_append(GTK_BOX(self->references_box), btn);
  gtk_widget_set_visible(self->references_box, TRUE);
}

/* NIP-05 verification callback */
static void on_nip05_verified(GnostrNip05Result *result, gpointer user_data) {
  GnostrTorrentCard *self = GNOSTR_TORRENT_CARD(user_data);

  if (!GNOSTR_IS_TORRENT_CARD(self) || !GTK_IS_IMAGE(self->nip05_badge)) {
    gnostr_nip05_result_free(result);
    return;
  }

  gboolean verified = (result && result->status == GNOSTR_NIP05_STATUS_VERIFIED);
  gtk_widget_set_visible(self->nip05_badge, verified);

  if (verified && result->identifier) {
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->nip05_badge), result->identifier);
  }

  gnostr_nip05_result_free(result);
}

void gnostr_torrent_card_set_nip05(GnostrTorrentCard *self,
                                    const char *nip05,
                                    const char *pubkey_hex) {
  g_return_if_fail(GNOSTR_IS_TORRENT_CARD(self));

  g_clear_pointer(&self->nip05, g_free);
  self->nip05 = g_strdup(nip05);

  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
  }

  if (!nip05 || !*nip05 || !pubkey_hex) {
    gtk_widget_set_visible(self->nip05_badge, FALSE);
    return;
  }

  self->nip05_cancellable = g_cancellable_new();
  gnostr_nip05_verify_async(nip05, pubkey_hex, on_nip05_verified, self, self->nip05_cancellable);
}

void gnostr_torrent_card_set_author_lud16(GnostrTorrentCard *self,
                                           const char *lud16) {
  g_return_if_fail(GNOSTR_IS_TORRENT_CARD(self));

  g_clear_pointer(&self->author_lud16, g_free);
  self->author_lud16 = g_strdup(lud16);

  gtk_widget_set_sensitive(self->btn_zap, lud16 && *lud16 && self->is_logged_in);
}

void gnostr_torrent_card_set_bookmarked(GnostrTorrentCard *self,
                                         gboolean is_bookmarked) {
  g_return_if_fail(GNOSTR_IS_TORRENT_CARD(self));

  self->is_bookmarked = is_bookmarked;
  gtk_button_set_icon_name(GTK_BUTTON(self->btn_bookmark),
    is_bookmarked ? "user-bookmarks-symbolic" : "bookmark-new-symbolic");
}

void gnostr_torrent_card_set_logged_in(GnostrTorrentCard *self,
                                        gboolean logged_in) {
  g_return_if_fail(GNOSTR_IS_TORRENT_CARD(self));

  self->is_logged_in = logged_in;
  gtk_widget_set_sensitive(self->btn_zap,
    logged_in && self->author_lud16 && *self->author_lud16);
  gtk_widget_set_sensitive(self->btn_bookmark, logged_in);
}

gchar *gnostr_torrent_card_get_magnet(GnostrTorrentCard *self) {
  g_return_val_if_fail(GNOSTR_IS_TORRENT_CARD(self), NULL);

  if (!self->infohash) return NULL;

  /* Build torrent structure for magnet generation */
  GnostrTorrent torrent = {
    .infohash = self->infohash,
    .title = self->title,
    .total_size = self->total_size,
    .trackers = (gchar **)self->trackers->pdata,
    .trackers_count = self->trackers->len
  };

  return gnostr_torrent_generate_magnet(&torrent);
}

const char *gnostr_torrent_card_get_infohash(GnostrTorrentCard *self) {
  g_return_val_if_fail(GNOSTR_IS_TORRENT_CARD(self), NULL);
  return self->infohash;
}

const char *gnostr_torrent_card_get_event_id(GnostrTorrentCard *self) {
  g_return_val_if_fail(GNOSTR_IS_TORRENT_CARD(self), NULL);
  return self->event_id;
}
