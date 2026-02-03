/*
 * gnostr-chess-card.c - NIP-64 Chess Game Card Widget
 *
 * GTK4 widget for displaying NIP-64 kind 64 chess game events.
 */

#include "gnostr-chess-card.h"
#include "gnostr-avatar-cache.h"
#include "../util/nip64_chess.h"
#include "../util/nip05.h"
#include "../util/utils.h"
#include <glib/gi18n.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

/* Default board size in pixels */
#define DEFAULT_BOARD_SIZE 320
#define MIN_BOARD_SIZE 200
#define MAX_BOARD_SIZE 600

/* Autoplay default interval */
#define DEFAULT_AUTOPLAY_INTERVAL_MS 1500

/* Board colors */
#define LIGHT_SQUARE_COLOR "#f0d9b5"
#define DARK_SQUARE_COLOR "#b58863"
#define HIGHLIGHT_FROM_COLOR "rgba(155, 199, 0, 0.5)"
#define HIGHLIGHT_TO_COLOR "rgba(155, 199, 0, 0.7)"

struct _GnostrChessCard {
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

  /* Game info */
  GtkWidget *lbl_white_player;
  GtkWidget *lbl_black_player;
  GtkWidget *lbl_result;
  GtkWidget *lbl_event_info;
  GtkWidget *lbl_opening;

  /* Chess board */
  GtkWidget *board_frame;
  GtkWidget *board_drawing;
  gint board_size;
  gboolean board_flipped;

  /* Move list */
  GtkWidget *moves_expander;
  GtkWidget *lbl_moves;

  /* Navigation buttons */
  GtkWidget *btn_first;
  GtkWidget *btn_prev;
  GtkWidget *btn_play;
  GtkWidget *btn_next;
  GtkWidget *btn_last;
  GtkWidget *btn_flip;

  /* Action buttons */
  GtkWidget *btn_copy_pgn;
  GtkWidget *btn_zap;
  GtkWidget *btn_bookmark;
  GtkWidget *btn_share;
  GtkWidget *btn_menu;
  GtkWidget *menu_popover;

  /* State */
  GnostrChessGame *game;
  gchar *event_id;
  gchar *pubkey_hex;
  gchar *author_lud16;
  gchar *nip05;
  gint64 created_at;
  gboolean is_bookmarked;
  gboolean is_logged_in;

  /* Autoplay */
  guint autoplay_source;
  guint autoplay_interval;

  /* Cancellables */
  GCancellable *nip05_cancellable;

#ifdef HAVE_SOUP3
  GCancellable *avatar_cancellable;
  /* Uses gnostr_get_shared_soup_session() instead of per-widget session */
#endif
};

G_DEFINE_TYPE(GnostrChessCard, gnostr_chess_card, GTK_TYPE_WIDGET)

enum {
  SIGNAL_OPEN_PROFILE,
  SIGNAL_OPEN_GAME,
  SIGNAL_SHARE_GAME,
  SIGNAL_COPY_PGN,
  SIGNAL_ZAP_REQUESTED,
  SIGNAL_BOOKMARK_TOGGLED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_avatar_clicked(GtkButton *btn, gpointer user_data);
static void on_author_name_clicked(GtkButton *btn, gpointer user_data);
static void on_first_clicked(GtkButton *btn, gpointer user_data);
static void on_prev_clicked(GtkButton *btn, gpointer user_data);
static void on_play_clicked(GtkButton *btn, gpointer user_data);
static void on_next_clicked(GtkButton *btn, gpointer user_data);
static void on_last_clicked(GtkButton *btn, gpointer user_data);
static void on_flip_clicked(GtkButton *btn, gpointer user_data);
static void on_copy_pgn_clicked(GtkButton *btn, gpointer user_data);
static void on_zap_clicked(GtkButton *btn, gpointer user_data);
static void on_bookmark_clicked(GtkButton *btn, gpointer user_data);
static void on_share_clicked(GtkButton *btn, gpointer user_data);
static void on_menu_clicked(GtkButton *btn, gpointer user_data);
static void update_navigation_buttons(GnostrChessCard *self);
static void update_board_display(GnostrChessCard *self);

/* ---- Widget lifecycle ---- */

static void gnostr_chess_card_dispose(GObject *object) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(object);

  gnostr_chess_card_stop_autoplay(self);

  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
  }

#ifdef HAVE_SOUP3
  if (self->avatar_cancellable) {
    g_cancellable_cancel(self->avatar_cancellable);
    g_clear_object(&self->avatar_cancellable);
  }
  /* Shared session is managed globally - do not clear here */
#endif

  if (self->menu_popover) {
    if (GTK_IS_POPOVER(self->menu_popover)) {
      gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
    }
    gtk_widget_unparent(self->menu_popover);
    self->menu_popover = NULL;
  }

  /* Unparent children */
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_widget_unparent(child);
    child = next;
  }

  G_OBJECT_CLASS(gnostr_chess_card_parent_class)->dispose(object);
}

static void gnostr_chess_card_finalize(GObject *obj) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(obj);

  gnostr_chess_game_free(self->game);
  g_free(self->event_id);
  g_free(self->pubkey_hex);
  g_free(self->author_lud16);
  g_free(self->nip05);

  G_OBJECT_CLASS(gnostr_chess_card_parent_class)->finalize(obj);
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

static void set_avatar_initials(GnostrChessCard *self, const char *display, const char *handle) {
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

/* ---- Board drawing ---- */

static gboolean parse_hex_color(const char *hex, double *r, double *g, double *b) {
  if (!hex || hex[0] != '#' || strlen(hex) != 7) return FALSE;

  unsigned int ri, gi, bi;
  if (sscanf(hex + 1, "%2x%2x%2x", &ri, &gi, &bi) != 3) return FALSE;

  *r = ri / 255.0;
  *g = gi / 255.0;
  *b = bi / 255.0;
  return TRUE;
}

static void draw_board(GtkDrawingArea *area, cairo_t *cr,
                       int width, int height, gpointer user_data) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(user_data);
  (void)area;

  if (!self->game) return;

  gint square_size = MIN(width, height) / 8;
  gint board_width = square_size * 8;
  gint board_height = square_size * 8;
  gint offset_x = (width - board_width) / 2;
  gint offset_y = (height - board_height) / 2;

  double light_r, light_g, light_b;
  double dark_r, dark_g, dark_b;
  parse_hex_color(LIGHT_SQUARE_COLOR, &light_r, &light_g, &light_b);
  parse_hex_color(DARK_SQUARE_COLOR, &dark_r, &dark_g, &dark_b);

  /* Draw squares */
  for (gint rank = 0; rank < 8; rank++) {
    for (gint file = 0; file < 8; file++) {
      gint display_file = self->board_flipped ? (7 - file) : file;
      gint display_rank = self->board_flipped ? rank : (7 - rank);

      gint x = offset_x + display_file * square_size;
      gint y = offset_y + display_rank * square_size;

      /* Square color */
      gboolean is_light = ((file + rank) % 2 == 0);
      if (is_light) {
        cairo_set_source_rgb(cr, light_r, light_g, light_b);
      } else {
        cairo_set_source_rgb(cr, dark_r, dark_g, dark_b);
      }
      cairo_rectangle(cr, x, y, square_size, square_size);
      cairo_fill(cr);

      /* Highlight last move */
      gint index = gnostr_chess_square_to_index(file, rank);
      if (self->game->last_move_from == index || self->game->last_move_to == index) {
        if (self->game->last_move_from == index) {
          cairo_set_source_rgba(cr, 0.6, 0.78, 0, 0.5);
        } else {
          cairo_set_source_rgba(cr, 0.6, 0.78, 0, 0.7);
        }
        cairo_rectangle(cr, x, y, square_size, square_size);
        cairo_fill(cr);
      }

      /* Draw piece */
      const GnostrChessSquare *sq = gnostr_chess_get_piece_at(self->game, file, rank);
      if (sq && sq->piece != GNOSTR_CHESS_PIECE_NONE) {
        const gchar *piece_str = gnostr_chess_piece_unicode(sq->piece, sq->color);

        /* Set up font for chess pieces */
        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, square_size * 0.75);

        /* Get text extents for centering */
        cairo_text_extents_t extents;
        cairo_text_extents(cr, piece_str, &extents);

        double text_x = x + (square_size - extents.width) / 2 - extents.x_bearing;
        double text_y = y + (square_size - extents.height) / 2 - extents.y_bearing;

        /* Draw piece with slight shadow for visibility */
        cairo_set_source_rgba(cr, 0, 0, 0, 0.3);
        cairo_move_to(cr, text_x + 1, text_y + 1);
        cairo_show_text(cr, piece_str);

        /* Main piece color */
        if (sq->color == GNOSTR_CHESS_COLOR_WHITE) {
          cairo_set_source_rgb(cr, 1, 1, 1);
        } else {
          cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        }
        cairo_move_to(cr, text_x, text_y);
        cairo_show_text(cr, piece_str);
      }
    }
  }

  /* Draw file/rank labels */
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 10);
  cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);

  for (gint i = 0; i < 8; i++) {
    /* File labels (a-h) */
    gint display_file = self->board_flipped ? (7 - i) : i;
    char file_char[2] = { (char)('a' + display_file), '\0' };
    cairo_move_to(cr, offset_x + i * square_size + square_size / 2 - 3, offset_y + board_height + 12);
    cairo_show_text(cr, file_char);

    /* Rank labels (1-8) */
    gint display_rank = self->board_flipped ? (i + 1) : (8 - i);
    char rank_char[2];
    g_snprintf(rank_char, sizeof(rank_char), "%d", display_rank);
    cairo_move_to(cr, offset_x - 12, offset_y + i * square_size + square_size / 2 + 4);
    cairo_show_text(cr, rank_char);
  }
}

/* ---- UI Building ---- */

static void build_ui(GnostrChessCard *self) {
  /* Root frame */
  self->root = gtk_frame_new(NULL);
  gtk_widget_set_hexpand(self->root, TRUE);
  gtk_widget_add_css_class(self->root, "chess-card");
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
  gtk_widget_add_css_class(self->lbl_author_name, "chess-author");
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
  gtk_widget_add_css_class(self->lbl_author_handle, "chess-meta");
  gtk_widget_add_css_class(self->lbl_author_handle, "dim-label");
  gtk_box_append(GTK_BOX(meta_row), self->lbl_author_handle);

  GtkWidget *separator = gtk_label_new("-");
  gtk_widget_add_css_class(separator, "chess-meta");
  gtk_widget_add_css_class(separator, "dim-label");
  gtk_box_append(GTK_BOX(meta_row), separator);

  self->lbl_publish_date = gtk_label_new(_("Just now"));
  gtk_label_set_xalign(GTK_LABEL(self->lbl_publish_date), 0.0);
  gtk_widget_add_css_class(self->lbl_publish_date, "chess-meta");
  gtk_widget_add_css_class(self->lbl_publish_date, "dim-label");
  gtk_box_append(GTK_BOX(meta_row), self->lbl_publish_date);

  /* Menu button */
  self->btn_menu = gtk_button_new_from_icon_name("open-menu-symbolic");
  gtk_widget_set_tooltip_text(self->btn_menu, _("More options"));
  gtk_widget_set_valign(self->btn_menu, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(self->btn_menu, "flat");
  g_signal_connect(self->btn_menu, "clicked", G_CALLBACK(on_menu_clicked), self);
  gtk_box_append(GTK_BOX(author_box), self->btn_menu);

  /* ---- Game info section ---- */
  GtkWidget *game_info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(game_info_box, "chess-game-info");
  gtk_box_append(GTK_BOX(main_box), game_info_box);

  /* Players row */
  GtkWidget *players_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(game_info_box), players_row);

  /* White player */
  GtkWidget *white_icon = gtk_label_new("\xe2\x99\x94");  /* White king */
  gtk_widget_add_css_class(white_icon, "chess-piece-icon");
  gtk_box_append(GTK_BOX(players_row), white_icon);

  self->lbl_white_player = gtk_label_new(_("White"));
  gtk_label_set_xalign(GTK_LABEL(self->lbl_white_player), 0.0);
  gtk_widget_add_css_class(self->lbl_white_player, "chess-player-name");
  gtk_box_append(GTK_BOX(players_row), self->lbl_white_player);

  GtkWidget *vs_label = gtk_label_new("vs");
  gtk_widget_add_css_class(vs_label, "dim-label");
  gtk_box_append(GTK_BOX(players_row), vs_label);

  /* Black player */
  GtkWidget *black_icon = gtk_label_new("\xe2\x99\x9a");  /* Black king */
  gtk_widget_add_css_class(black_icon, "chess-piece-icon");
  gtk_box_append(GTK_BOX(players_row), black_icon);

  self->lbl_black_player = gtk_label_new(_("Black"));
  gtk_label_set_xalign(GTK_LABEL(self->lbl_black_player), 0.0);
  gtk_widget_add_css_class(self->lbl_black_player, "chess-player-name");
  gtk_box_append(GTK_BOX(players_row), self->lbl_black_player);

  /* Spacer */
  GtkWidget *spacer = gtk_label_new("");
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(players_row), spacer);

  /* Result */
  self->lbl_result = gtk_label_new("*");
  gtk_widget_add_css_class(self->lbl_result, "chess-result");
  gtk_box_append(GTK_BOX(players_row), self->lbl_result);

  /* Event and opening info */
  self->lbl_event_info = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->lbl_event_info), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(self->lbl_event_info), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(self->lbl_event_info, "chess-event-info");
  gtk_widget_add_css_class(self->lbl_event_info, "dim-label");
  gtk_widget_set_visible(self->lbl_event_info, FALSE);
  gtk_box_append(GTK_BOX(game_info_box), self->lbl_event_info);

  self->lbl_opening = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->lbl_opening), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(self->lbl_opening), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(self->lbl_opening, "chess-opening");
  gtk_widget_set_visible(self->lbl_opening, FALSE);
  gtk_box_append(GTK_BOX(game_info_box), self->lbl_opening);

  /* ---- Chess board ---- */
  self->board_frame = gtk_frame_new(NULL);
  gtk_widget_add_css_class(self->board_frame, "chess-board-frame");
  gtk_widget_set_halign(self->board_frame, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(main_box), self->board_frame);

  self->board_drawing = gtk_drawing_area_new();
  gtk_widget_set_size_request(self->board_drawing, self->board_size, self->board_size);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->board_drawing),
                                  draw_board, self, NULL);
  gtk_frame_set_child(GTK_FRAME(self->board_frame), self->board_drawing);

  /* ---- Navigation buttons ---- */
  GtkWidget *nav_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_halign(nav_box, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(nav_box, "chess-navigation");
  gtk_box_append(GTK_BOX(main_box), nav_box);

  self->btn_first = gtk_button_new_from_icon_name("go-first-symbolic");
  gtk_widget_set_tooltip_text(self->btn_first, _("First move"));
  g_signal_connect(self->btn_first, "clicked", G_CALLBACK(on_first_clicked), self);
  gtk_box_append(GTK_BOX(nav_box), self->btn_first);

  self->btn_prev = gtk_button_new_from_icon_name("go-previous-symbolic");
  gtk_widget_set_tooltip_text(self->btn_prev, _("Previous move"));
  g_signal_connect(self->btn_prev, "clicked", G_CALLBACK(on_prev_clicked), self);
  gtk_box_append(GTK_BOX(nav_box), self->btn_prev);

  self->btn_play = gtk_button_new_from_icon_name("media-playback-start-symbolic");
  gtk_widget_set_tooltip_text(self->btn_play, _("Auto-play"));
  g_signal_connect(self->btn_play, "clicked", G_CALLBACK(on_play_clicked), self);
  gtk_box_append(GTK_BOX(nav_box), self->btn_play);

  self->btn_next = gtk_button_new_from_icon_name("go-next-symbolic");
  gtk_widget_set_tooltip_text(self->btn_next, _("Next move"));
  g_signal_connect(self->btn_next, "clicked", G_CALLBACK(on_next_clicked), self);
  gtk_box_append(GTK_BOX(nav_box), self->btn_next);

  self->btn_last = gtk_button_new_from_icon_name("go-last-symbolic");
  gtk_widget_set_tooltip_text(self->btn_last, _("Last move"));
  g_signal_connect(self->btn_last, "clicked", G_CALLBACK(on_last_clicked), self);
  gtk_box_append(GTK_BOX(nav_box), self->btn_last);

  /* Separator */
  GtkWidget *nav_sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_margin_start(nav_sep, 8);
  gtk_widget_set_margin_end(nav_sep, 8);
  gtk_box_append(GTK_BOX(nav_box), nav_sep);

  self->btn_flip = gtk_button_new_from_icon_name("object-flip-vertical-symbolic");
  gtk_widget_set_tooltip_text(self->btn_flip, _("Flip board"));
  g_signal_connect(self->btn_flip, "clicked", G_CALLBACK(on_flip_clicked), self);
  gtk_box_append(GTK_BOX(nav_box), self->btn_flip);

  /* ---- Move list expander ---- */
  self->moves_expander = gtk_expander_new(_("Moves"));
  gtk_widget_set_visible(self->moves_expander, FALSE);
  gtk_widget_add_css_class(self->moves_expander, "chess-moves-expander");
  gtk_box_append(GTK_BOX(main_box), self->moves_expander);

  GtkWidget *moves_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(moves_scroll), 100);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(moves_scroll), TRUE);
  gtk_expander_set_child(GTK_EXPANDER(self->moves_expander), moves_scroll);

  self->lbl_moves = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->lbl_moves), 0.0);
  gtk_label_set_wrap(GTK_LABEL(self->lbl_moves), TRUE);
  gtk_label_set_selectable(GTK_LABEL(self->lbl_moves), TRUE);
  gtk_widget_add_css_class(self->lbl_moves, "chess-moves-text");
  gtk_widget_add_css_class(self->lbl_moves, "monospace");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(moves_scroll), self->lbl_moves);

  /* ---- Action buttons ---- */
  GtkWidget *actions_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(actions_box, 4);
  gtk_box_append(GTK_BOX(main_box), actions_box);

  /* Copy PGN button */
  self->btn_copy_pgn = gtk_button_new();
  GtkWidget *copy_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *copy_icon = gtk_image_new_from_icon_name("edit-copy-symbolic");
  GtkWidget *copy_label = gtk_label_new(_("Copy PGN"));
  gtk_box_append(GTK_BOX(copy_box), copy_icon);
  gtk_box_append(GTK_BOX(copy_box), copy_label);
  gtk_button_set_child(GTK_BUTTON(self->btn_copy_pgn), copy_box);
  gtk_widget_set_tooltip_text(self->btn_copy_pgn, _("Copy game as PGN to clipboard"));
  g_signal_connect(self->btn_copy_pgn, "clicked", G_CALLBACK(on_copy_pgn_clicked), self);
  gtk_box_append(GTK_BOX(actions_box), self->btn_copy_pgn);

  /* Share button */
  self->btn_share = gtk_button_new_from_icon_name("emblem-shared-symbolic");
  gtk_widget_set_tooltip_text(self->btn_share, _("Share game"));
  g_signal_connect(self->btn_share, "clicked", G_CALLBACK(on_share_clicked), self);
  gtk_box_append(GTK_BOX(actions_box), self->btn_share);

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

static void gnostr_chess_card_class_init(GnostrChessCardClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);

  gclass->dispose = gnostr_chess_card_dispose;
  gclass->finalize = gnostr_chess_card_finalize;

  gtk_widget_class_set_layout_manager_type(wclass, GTK_TYPE_BOX_LAYOUT);

  /* Signals */
  signals[SIGNAL_OPEN_PROFILE] = g_signal_new("open-profile",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_GAME] = g_signal_new("open-game",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_SHARE_GAME] = g_signal_new("share-game",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_COPY_PGN] = g_signal_new("copy-pgn",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_ZAP_REQUESTED] = g_signal_new("zap-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIGNAL_BOOKMARK_TOGGLED] = g_signal_new("bookmark-toggled",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);
}

static void gnostr_chess_card_init(GnostrChessCard *self) {
  self->board_size = DEFAULT_BOARD_SIZE;
  self->board_flipped = FALSE;
  self->autoplay_interval = DEFAULT_AUTOPLAY_INTERVAL_MS;

  build_ui(self);

  gtk_widget_add_css_class(GTK_WIDGET(self), "chess-card");

  update_navigation_buttons(self);

#ifdef HAVE_SOUP3
  self->avatar_cancellable = g_cancellable_new();
  /* Uses shared session from gnostr_get_shared_soup_session() */
#endif
}

/* ---- Button handlers ---- */

static void on_avatar_clicked(GtkButton *btn, gpointer user_data) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(user_data);
  (void)btn;
  if (self->pubkey_hex && *self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey_hex);
  }
}

static void on_author_name_clicked(GtkButton *btn, gpointer user_data) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(user_data);
  (void)btn;
  if (self->pubkey_hex && *self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey_hex);
  }
}

static void on_first_clicked(GtkButton *btn, gpointer user_data) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(user_data);
  (void)btn;
  gnostr_chess_card_go_first(self);
}

static void on_prev_clicked(GtkButton *btn, gpointer user_data) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(user_data);
  (void)btn;
  gnostr_chess_card_go_prev(self);
}

static gboolean autoplay_tick(gpointer user_data) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(user_data);

  if (!self->game || self->game->current_ply >= (gint)self->game->moves_count) {
    gnostr_chess_card_stop_autoplay(self);
    return G_SOURCE_REMOVE;
  }

  gnostr_chess_card_go_next(self);
  return G_SOURCE_CONTINUE;
}

static void on_play_clicked(GtkButton *btn, gpointer user_data) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(user_data);
  (void)btn;

  if (self->autoplay_source) {
    gnostr_chess_card_stop_autoplay(self);
  } else {
    gnostr_chess_card_start_autoplay(self, self->autoplay_interval);
  }
}

static void on_next_clicked(GtkButton *btn, gpointer user_data) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(user_data);
  (void)btn;
  gnostr_chess_card_go_next(self);
}

static void on_last_clicked(GtkButton *btn, gpointer user_data) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(user_data);
  (void)btn;
  gnostr_chess_card_go_last(self);
}

static void on_flip_clicked(GtkButton *btn, gpointer user_data) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(user_data);
  (void)btn;
  gnostr_chess_card_set_flipped(self, !self->board_flipped);
}

static void on_copy_pgn_clicked(GtkButton *btn, gpointer user_data) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(user_data);
  (void)btn;

  gchar *pgn = gnostr_chess_card_get_pgn(self);
  if (pgn) {
    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
    gdk_clipboard_set_text(clipboard, pgn);

    g_signal_emit(self, signals[SIGNAL_COPY_PGN], 0, pgn);
    g_free(pgn);
  }
}

static void on_share_clicked(GtkButton *btn, gpointer user_data) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(user_data);
  (void)btn;

  if (self->event_id) {
    gchar *uri = g_strdup_printf("nostr:note1%s", self->event_id);
    g_signal_emit(self, signals[SIGNAL_SHARE_GAME], 0, uri);
    g_free(uri);
  }
}

static void on_zap_clicked(GtkButton *btn, gpointer user_data) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(user_data);
  (void)btn;
  if (self->event_id && self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_ZAP_REQUESTED], 0,
                  self->event_id, self->pubkey_hex, self->author_lud16);
  }
}

static void on_bookmark_clicked(GtkButton *btn, gpointer user_data) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(user_data);
  (void)btn;

  if (!self->event_id) return;

  self->is_bookmarked = !self->is_bookmarked;
  gtk_button_set_icon_name(GTK_BUTTON(self->btn_bookmark),
    self->is_bookmarked ? "user-bookmarks-symbolic" : "bookmark-new-symbolic");

  g_signal_emit(self, signals[SIGNAL_BOOKMARK_TOGGLED], 0,
                self->event_id, self->is_bookmarked);
}

static void on_menu_clicked(GtkButton *btn, gpointer user_data) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(user_data);
  (void)btn;

  if (!self->menu_popover) {
    self->menu_popover = gtk_popover_new();

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    /* Copy PGN */
    GtkWidget *copy_btn = gtk_button_new();
    GtkWidget *copy_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *copy_icon = gtk_image_new_from_icon_name("edit-copy-symbolic");
    GtkWidget *copy_label = gtk_label_new(_("Copy PGN"));
    gtk_box_append(GTK_BOX(copy_box), copy_icon);
    gtk_box_append(GTK_BOX(copy_box), copy_label);
    gtk_button_set_child(GTK_BUTTON(copy_btn), copy_box);
    gtk_button_set_has_frame(GTK_BUTTON(copy_btn), FALSE);
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy_pgn_clicked), self);
    gtk_box_append(GTK_BOX(box), copy_btn);

    /* View author */
    GtkWidget *profile_btn = gtk_button_new();
    GtkWidget *profile_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *profile_icon = gtk_image_new_from_icon_name("avatar-default-symbolic");
    GtkWidget *profile_label = gtk_label_new(_("View Author"));
    gtk_box_append(GTK_BOX(profile_box), profile_icon);
    gtk_box_append(GTK_BOX(profile_box), profile_label);
    gtk_button_set_child(GTK_BUTTON(profile_btn), profile_box);
    gtk_button_set_has_frame(GTK_BUTTON(profile_btn), FALSE);
    g_signal_connect(profile_btn, "clicked", G_CALLBACK(on_avatar_clicked), self);
    gtk_box_append(GTK_BOX(box), profile_btn);

    gtk_popover_set_child(GTK_POPOVER(self->menu_popover), box);
    gtk_widget_set_parent(self->menu_popover, GTK_WIDGET(self->btn_menu));
  }

  gtk_popover_popup(GTK_POPOVER(self->menu_popover));
}

/* ---- UI Update helpers ---- */

static void update_navigation_buttons(GnostrChessCard *self) {
  gboolean has_game = (self->game != NULL);
  gboolean at_start = !has_game || self->game->current_ply <= 0;
  gboolean at_end = !has_game || self->game->current_ply >= (gint)self->game->moves_count;

  gtk_widget_set_sensitive(self->btn_first, has_game && !at_start);
  gtk_widget_set_sensitive(self->btn_prev, has_game && !at_start);
  gtk_widget_set_sensitive(self->btn_next, has_game && !at_end);
  gtk_widget_set_sensitive(self->btn_last, has_game && !at_end);
  gtk_widget_set_sensitive(self->btn_play, has_game && !at_end);
}

static void update_board_display(GnostrChessCard *self) {
  gtk_widget_queue_draw(self->board_drawing);
  update_navigation_buttons(self);
}

/* ---- Public API ---- */

GnostrChessCard *gnostr_chess_card_new(void) {
  return g_object_new(GNOSTR_TYPE_CHESS_CARD, NULL);
}

gboolean gnostr_chess_card_set_pgn(GnostrChessCard *self, const char *pgn_text) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_CARD(self), FALSE);

  gnostr_chess_game_free(self->game);
  self->game = gnostr_chess_parse_pgn(pgn_text);

  if (!self->game) {
    return FALSE;
  }

  /* Update game info labels */
  if (self->game->white_player) {
    gtk_label_set_text(GTK_LABEL(self->lbl_white_player), self->game->white_player);
  }
  if (self->game->black_player) {
    gtk_label_set_text(GTK_LABEL(self->lbl_black_player), self->game->black_player);
  }

  gtk_label_set_text(GTK_LABEL(self->lbl_result),
    gnostr_chess_result_to_string(self->game->result));

  /* Event info */
  if (self->game->event_name || self->game->site || self->game->date) {
    GString *info = g_string_new(NULL);
    if (self->game->event_name && g_strcmp0(self->game->event_name, "?") != 0) {
      g_string_append(info, self->game->event_name);
    }
    if (self->game->site && g_strcmp0(self->game->site, "?") != 0) {
      if (info->len > 0) g_string_append(info, ", ");
      g_string_append(info, self->game->site);
    }
    if (self->game->date && g_strcmp0(self->game->date, "????.??.??") != 0) {
      if (info->len > 0) g_string_append(info, " - ");
      g_string_append(info, self->game->date);
    }
    if (info->len > 0) {
      gtk_label_set_text(GTK_LABEL(self->lbl_event_info), info->str);
      gtk_widget_set_visible(self->lbl_event_info, TRUE);
    }
    g_string_free(info, TRUE);
  }

  /* Opening info */
  if (self->game->opening || self->game->eco) {
    GString *opening = g_string_new(NULL);
    if (self->game->eco) {
      g_string_append(opening, self->game->eco);
    }
    if (self->game->opening) {
      if (opening->len > 0) g_string_append(opening, ": ");
      g_string_append(opening, self->game->opening);
    }
    if (opening->len > 0) {
      gtk_label_set_text(GTK_LABEL(self->lbl_opening), opening->str);
      gtk_widget_set_visible(self->lbl_opening, TRUE);
    }
    g_string_free(opening, TRUE);
  }

  /* Move list */
  if (self->game->moves_count > 0) {
    gchar *moves_str = gnostr_chess_format_moves(self->game, -1);
    gtk_label_set_text(GTK_LABEL(self->lbl_moves), moves_str);
    gtk_widget_set_visible(self->moves_expander, TRUE);
    g_free(moves_str);
  }

  /* Go to final position */
  gnostr_chess_game_last(self->game);
  update_board_display(self);

  return TRUE;
}

void gnostr_chess_card_set_event(GnostrChessCard *self,
                                  const char *event_id,
                                  const char *pubkey,
                                  gint64 created_at) {
  g_return_if_fail(GNOSTR_IS_CHESS_CARD(self));

  g_free(self->event_id);
  self->event_id = g_strdup(event_id);

  g_free(self->pubkey_hex);
  self->pubkey_hex = g_strdup(pubkey);

  self->created_at = created_at;

  gchar *date_str = format_timestamp(created_at);
  gtk_label_set_text(GTK_LABEL(self->lbl_publish_date), date_str);
  g_free(date_str);
}

void gnostr_chess_card_set_author(GnostrChessCard *self,
                                   const char *display_name,
                                   const char *handle,
                                   const char *avatar_url,
                                   const char *pubkey_hex) {
  g_return_if_fail(GNOSTR_IS_CHESS_CARD(self));

  g_free(self->pubkey_hex);
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

/* NIP-05 verification callback */
static void on_nip05_verified(GnostrNip05Result *result, gpointer user_data) {
  GnostrChessCard *self = GNOSTR_CHESS_CARD(user_data);

  if (!GNOSTR_IS_CHESS_CARD(self) || !GTK_IS_IMAGE(self->nip05_badge)) {
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

void gnostr_chess_card_set_nip05(GnostrChessCard *self,
                                  const char *nip05,
                                  const char *pubkey_hex) {
  g_return_if_fail(GNOSTR_IS_CHESS_CARD(self));

  g_free(self->nip05);
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

void gnostr_chess_card_set_author_lud16(GnostrChessCard *self,
                                         const char *lud16) {
  g_return_if_fail(GNOSTR_IS_CHESS_CARD(self));

  g_free(self->author_lud16);
  self->author_lud16 = g_strdup(lud16);

  gtk_widget_set_sensitive(self->btn_zap, lud16 && *lud16 && self->is_logged_in);
}

void gnostr_chess_card_set_bookmarked(GnostrChessCard *self,
                                       gboolean is_bookmarked) {
  g_return_if_fail(GNOSTR_IS_CHESS_CARD(self));

  self->is_bookmarked = is_bookmarked;
  gtk_button_set_icon_name(GTK_BUTTON(self->btn_bookmark),
    is_bookmarked ? "user-bookmarks-symbolic" : "bookmark-new-symbolic");
}

void gnostr_chess_card_set_logged_in(GnostrChessCard *self,
                                      gboolean logged_in) {
  g_return_if_fail(GNOSTR_IS_CHESS_CARD(self));

  self->is_logged_in = logged_in;
  gtk_widget_set_sensitive(self->btn_zap,
    logged_in && self->author_lud16 && *self->author_lud16);
  gtk_widget_set_sensitive(self->btn_bookmark, logged_in);
}

void gnostr_chess_card_go_first(GnostrChessCard *self) {
  g_return_if_fail(GNOSTR_IS_CHESS_CARD(self));
  if (!self->game) return;

  gnostr_chess_card_stop_autoplay(self);
  gnostr_chess_game_first(self->game);
  update_board_display(self);
}

void gnostr_chess_card_go_prev(GnostrChessCard *self) {
  g_return_if_fail(GNOSTR_IS_CHESS_CARD(self));
  if (!self->game) return;

  gnostr_chess_card_stop_autoplay(self);
  gnostr_chess_game_prev(self->game);
  update_board_display(self);
}

void gnostr_chess_card_go_next(GnostrChessCard *self) {
  g_return_if_fail(GNOSTR_IS_CHESS_CARD(self));
  if (!self->game) return;

  gnostr_chess_game_next(self->game);
  update_board_display(self);
}

void gnostr_chess_card_go_last(GnostrChessCard *self) {
  g_return_if_fail(GNOSTR_IS_CHESS_CARD(self));
  if (!self->game) return;

  gnostr_chess_card_stop_autoplay(self);
  gnostr_chess_game_last(self->game);
  update_board_display(self);
}

void gnostr_chess_card_start_autoplay(GnostrChessCard *self, guint interval_ms) {
  g_return_if_fail(GNOSTR_IS_CHESS_CARD(self));

  if (self->autoplay_source) {
    gnostr_chess_card_stop_autoplay(self);
  }

  /* LEGITIMATE TIMEOUT - Animation interval for chess move autoplay.
   * nostrc-b0h: Audited - animation timing is appropriate. */
  self->autoplay_interval = interval_ms;
  self->autoplay_source = g_timeout_add(interval_ms, autoplay_tick, self);

  /* Update play button icon */
  gtk_button_set_icon_name(GTK_BUTTON(self->btn_play), "media-playback-pause-symbolic");
}

void gnostr_chess_card_stop_autoplay(GnostrChessCard *self) {
  g_return_if_fail(GNOSTR_IS_CHESS_CARD(self));

  if (self->autoplay_source) {
    g_source_remove(self->autoplay_source);
    self->autoplay_source = 0;
  }

  /* Update play button icon */
  gtk_button_set_icon_name(GTK_BUTTON(self->btn_play), "media-playback-start-symbolic");
}

gboolean gnostr_chess_card_is_playing(GnostrChessCard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_CARD(self), FALSE);
  return self->autoplay_source != 0;
}

const char *gnostr_chess_card_get_event_id(GnostrChessCard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_CARD(self), NULL);
  return self->event_id;
}

gchar *gnostr_chess_card_get_pgn(GnostrChessCard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_CARD(self), NULL);
  if (!self->game) return NULL;
  return gnostr_chess_game_export_pgn(self->game);
}

void gnostr_chess_card_set_board_size(GnostrChessCard *self, gint size) {
  g_return_if_fail(GNOSTR_IS_CHESS_CARD(self));

  size = CLAMP(size, MIN_BOARD_SIZE, MAX_BOARD_SIZE);
  self->board_size = size;

  gtk_widget_set_size_request(self->board_drawing, size, size);
  gtk_widget_queue_draw(self->board_drawing);
}

void gnostr_chess_card_set_flipped(GnostrChessCard *self, gboolean flipped) {
  g_return_if_fail(GNOSTR_IS_CHESS_CARD(self));

  self->board_flipped = flipped;
  gtk_widget_queue_draw(self->board_drawing);
}

gboolean gnostr_chess_card_is_flipped(GnostrChessCard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_CARD(self), FALSE);
  return self->board_flipped;
}
