/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-chess-games-browser.c - Chess Games Browser Widget
 *
 * Displays a list of chess games from Nostr relays (NIP-64).
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gnostr-chess-games-browser.h"
#include "gnostr-chess-card.h"
#include <string.h>

struct _GnostrChessGamesBrowser {
    GtkWidget parent_instance;

    /* UI Elements */
    GtkWidget *main_box;
    GtkWidget *header_box;
    GtkWidget *title_label;
    GtkWidget *refresh_button;
    GtkWidget *loading_spinner;
    GtkWidget *scroll;
    GtkWidget *games_list;
    GtkWidget *empty_label;

    /* Data */
    GHashTable *games;          /* Reference to plugin's games table */
    GHashTable *game_rows;      /* event_id -> GtkListBoxRow */
};

G_DEFINE_TYPE(GnostrChessGamesBrowser, gnostr_chess_games_browser, GTK_TYPE_WIDGET)

/* Signal IDs */
enum {
    SIGNAL_GAME_SELECTED,
    SIGNAL_REFRESH_REQUESTED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_refresh_clicked(GtkButton *button, gpointer user_data);
static void on_row_activated(GtkListBox *list_box, GtkListBoxRow *row, gpointer user_data);
static void populate_games_list(GnostrChessGamesBrowser *self);
static void clear_games_list(GnostrChessGamesBrowser *self);

/* ============================================================================
 * GObject Lifecycle
 * ============================================================================ */

static void
gnostr_chess_games_browser_dispose(GObject *object)
{
    GnostrChessGamesBrowser *self = GNOSTR_CHESS_GAMES_BROWSER(object);

    if (self->main_box) {
        gtk_widget_unparent(self->main_box);
        self->main_box = NULL;
    }

    g_clear_pointer(&self->game_rows, g_hash_table_destroy);

    G_OBJECT_CLASS(gnostr_chess_games_browser_parent_class)->dispose(object);
}

static void
gnostr_chess_games_browser_class_init(GnostrChessGamesBrowserClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = gnostr_chess_games_browser_dispose;

    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);

    /**
     * GnostrChessGamesBrowser::game-selected:
     * @browser: The browser
     * @event_id: The event ID of the selected game
     *
     * Emitted when a user clicks on a game in the list.
     */
    signals[SIGNAL_GAME_SELECTED] = g_signal_new(
        "game-selected",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0,
        NULL, NULL,
        NULL,
        G_TYPE_NONE, 1,
        G_TYPE_STRING);

    /**
     * GnostrChessGamesBrowser::refresh-requested:
     * @browser: The browser
     *
     * Emitted when the user clicks the refresh button.
     */
    signals[SIGNAL_REFRESH_REQUESTED] = g_signal_new(
        "refresh-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0,
        NULL, NULL,
        NULL,
        G_TYPE_NONE, 0);

    gtk_widget_class_set_css_name(widget_class, "chess-games-browser");
}

static void
gnostr_chess_games_browser_init(GnostrChessGamesBrowser *self)
{
    self->game_rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Main container */
    self->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_parent(self->main_box, GTK_WIDGET(self));

    /* Header with title and refresh button */
    self->header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(self->main_box), self->header_box);

    self->title_label = gtk_label_new("Games on Relays");
    gtk_widget_add_css_class(self->title_label, "heading");
    gtk_widget_set_halign(self->title_label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(self->title_label, TRUE);
    gtk_box_append(GTK_BOX(self->header_box), self->title_label);

    self->loading_spinner = gtk_spinner_new();
    gtk_widget_set_visible(self->loading_spinner, FALSE);
    gtk_box_append(GTK_BOX(self->header_box), self->loading_spinner);

    self->refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(self->refresh_button, "Refresh games");
    g_signal_connect(self->refresh_button, "clicked",
                     G_CALLBACK(on_refresh_clicked), self);
    gtk_box_append(GTK_BOX(self->header_box), self->refresh_button);

    /* Scrolled list */
    self->scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(self->scroll, TRUE);
    gtk_box_append(GTK_BOX(self->main_box), self->scroll);

    self->games_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->games_list),
                                     GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(self->games_list, "boxed-list");
    g_signal_connect(self->games_list, "row-activated",
                     G_CALLBACK(on_row_activated), self);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scroll),
                                   self->games_list);

    /* Empty state label */
    self->empty_label = gtk_label_new("No games found.\nClick refresh to load games from relays.");
    gtk_label_set_wrap(GTK_LABEL(self->empty_label), TRUE);
    gtk_label_set_justify(GTK_LABEL(self->empty_label), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class(self->empty_label, "dim-label");
    gtk_widget_set_valign(self->empty_label, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(self->empty_label, TRUE);
    gtk_widget_set_visible(self->empty_label, TRUE);
    gtk_box_append(GTK_BOX(self->main_box), self->empty_label);

    /* Initially hide the scrolled list until we have games */
    gtk_widget_set_visible(self->scroll, FALSE);
}

/* ============================================================================
 * Signal Handlers
 * ============================================================================ */

static void
on_refresh_clicked(GtkButton *button, gpointer user_data)
{
    GnostrChessGamesBrowser *self = GNOSTR_CHESS_GAMES_BROWSER(user_data);
    (void)button;

    g_signal_emit(self, signals[SIGNAL_REFRESH_REQUESTED], 0);
}

static void
on_row_activated(GtkListBox *list_box, GtkListBoxRow *row, gpointer user_data)
{
    GnostrChessGamesBrowser *self = GNOSTR_CHESS_GAMES_BROWSER(user_data);
    (void)list_box;

    const char *event_id = g_object_get_data(G_OBJECT(row), "event-id");
    if (event_id) {
        g_signal_emit(self, signals[SIGNAL_GAME_SELECTED], 0, event_id);
    }
}

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static gint
compare_games_by_date(gconstpointer a, gconstpointer b)
{
    const GnostrChessGame *game_a = *(const GnostrChessGame **)a;
    const GnostrChessGame *game_b = *(const GnostrChessGame **)b;

    /* Sort by created_at descending (newest first) */
    if (game_a->created_at > game_b->created_at) return -1;
    if (game_a->created_at < game_b->created_at) return 1;
    return 0;
}

static void
clear_games_list(GnostrChessGamesBrowser *self)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->games_list)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(self->games_list), child);
    }
    g_hash_table_remove_all(self->game_rows);
}

static void
populate_games_list(GnostrChessGamesBrowser *self)
{
    clear_games_list(self);

    if (!self->games || g_hash_table_size(self->games) == 0) {
        gtk_widget_set_visible(self->scroll, FALSE);
        gtk_widget_set_visible(self->empty_label, TRUE);
        return;
    }

    gtk_widget_set_visible(self->scroll, TRUE);
    gtk_widget_set_visible(self->empty_label, FALSE);

    /* Collect games into array for sorting */
    guint n_games = g_hash_table_size(self->games);
    GPtrArray *games_array = g_ptr_array_new();

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->games);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        g_ptr_array_add(games_array, value);
    }

    /* Sort by date (newest first) */
    g_ptr_array_sort(games_array, compare_games_by_date);

    /* Create rows for each game */
    for (guint i = 0; i < games_array->len; i++) {
        GnostrChessGame *game = g_ptr_array_index(games_array, i);

        /* Create a simple row with game info */
        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_margin_start(row_box, 8);
        gtk_widget_set_margin_end(row_box, 8);
        gtk_widget_set_margin_top(row_box, 8);
        gtk_widget_set_margin_bottom(row_box, 8);

        /* Players line */
        gchar *players = g_strdup_printf("%s vs %s",
            game->white_player ? game->white_player : "Unknown",
            game->black_player ? game->black_player : "Unknown");
        GtkWidget *players_label = gtk_label_new(players);
        gtk_label_set_xalign(GTK_LABEL(players_label), 0);
        gtk_widget_add_css_class(players_label, "heading");
        gtk_box_append(GTK_BOX(row_box), players_label);
        g_free(players);

        /* Result and moves line */
        gchar *info = g_strdup_printf("%s - %zu moves",
            game->result_string ? game->result_string : "*",
            game->moves_count / 2);
        GtkWidget *info_label = gtk_label_new(info);
        gtk_label_set_xalign(GTK_LABEL(info_label), 0);
        gtk_widget_add_css_class(info_label, "dim-label");
        gtk_box_append(GTK_BOX(row_box), info_label);
        g_free(info);

        /* Create row and store event ID */
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_box);
        g_object_set_data_full(G_OBJECT(row), "event-id",
                               g_strdup(game->event_id), g_free);

        gtk_list_box_append(GTK_LIST_BOX(self->games_list), row);
        g_hash_table_insert(self->game_rows, g_strdup(game->event_id), row);
    }

    g_ptr_array_free(games_array, TRUE);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

GnostrChessGamesBrowser *
gnostr_chess_games_browser_new(void)
{
    return g_object_new(GNOSTR_TYPE_CHESS_GAMES_BROWSER, NULL);
}

void
gnostr_chess_games_browser_set_games(GnostrChessGamesBrowser *self,
                                      GHashTable *games)
{
    g_return_if_fail(GNOSTR_IS_CHESS_GAMES_BROWSER(self));

    self->games = games;
    populate_games_list(self);
}

void
gnostr_chess_games_browser_refresh(GnostrChessGamesBrowser *self)
{
    g_return_if_fail(GNOSTR_IS_CHESS_GAMES_BROWSER(self));

    populate_games_list(self);
}

void
gnostr_chess_games_browser_set_loading(GnostrChessGamesBrowser *self,
                                        gboolean loading)
{
    g_return_if_fail(GNOSTR_IS_CHESS_GAMES_BROWSER(self));

    gtk_widget_set_visible(self->loading_spinner, loading);
    if (loading) {
        gtk_spinner_start(GTK_SPINNER(self->loading_spinner));
    } else {
        gtk_spinner_stop(GTK_SPINNER(self->loading_spinner));
    }

    gtk_widget_set_sensitive(self->refresh_button, !loading);
}
