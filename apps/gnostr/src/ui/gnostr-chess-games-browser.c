/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-chess-games-browser.c - Chess Games Browser Widget
 *
 * Displays a list of chess games from Nostr relays (NIP-64).
 * Uses GListStore + GtkListView for scalable list rendering.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gnostr-chess-games-browser.h"
#include "gnostr-chess-card.h"
#include "../model/gnostr-chess-game-item.h"
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
    GtkWidget *list_view;
    GtkWidget *empty_label;

    /* Model pipeline */
    GListStore *store;
    GtkSortListModel *sort_model;
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
static void populate_games_list(GnostrChessGamesBrowser *self);

/* ============================================================================
 * Factory Callbacks
 * ============================================================================ */

static void
factory_setup(GtkSignalListItemFactory *factory,
              GtkListItem              *list_item,
              gpointer                  user_data)
{
    (void)factory;
    (void)user_data;

    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(row_box, 8);
    gtk_widget_set_margin_end(row_box, 8);
    gtk_widget_set_margin_top(row_box, 8);
    gtk_widget_set_margin_bottom(row_box, 8);

    GtkWidget *players_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(players_label), 0);
    gtk_widget_add_css_class(players_label, "heading");
    gtk_box_append(GTK_BOX(row_box), players_label);

    GtkWidget *info_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(info_label), 0);
    gtk_widget_add_css_class(info_label, "dim-label");
    gtk_box_append(GTK_BOX(row_box), info_label);

    gtk_list_item_set_child(list_item, row_box);
}

static void
factory_bind(GtkSignalListItemFactory *factory,
             GtkListItem              *list_item,
             gpointer                  user_data)
{
    (void)factory;
    (void)user_data;

    GnostrChessGameItem *item = gtk_list_item_get_item(list_item);
    GnostrChessGame *game = gnostr_chess_game_item_get_game(item);
    if (!game)
        return;

    GtkWidget *row_box = gtk_list_item_get_child(list_item);
    GtkWidget *players_label = gtk_widget_get_first_child(row_box);
    GtkWidget *info_label = gtk_widget_get_next_sibling(players_label);

    g_autofree gchar *players = g_strdup_printf("%s vs %s",
        game->white_player ? game->white_player : "Unknown",
        game->black_player ? game->black_player : "Unknown");
    gtk_label_set_text(GTK_LABEL(players_label), players);

    g_autofree gchar *info = g_strdup_printf("%s - %zu moves",
        game->result_string ? game->result_string : "*",
        game->moves_count / 2);
    gtk_label_set_text(GTK_LABEL(info_label), info);
}

static void
factory_unbind(GtkSignalListItemFactory *factory,
               GtkListItem              *list_item,
               gpointer                  user_data)
{
    (void)factory;
    (void)list_item;
    (void)user_data;
}

/* ============================================================================
 * Sort + Selection
 * ============================================================================ */

static int
sort_by_date_desc(gconstpointer a, gconstpointer b, gpointer user_data)
{
    (void)user_data;

    GnostrChessGameItem *item_a = (GnostrChessGameItem *)a;
    GnostrChessGameItem *item_b = (GnostrChessGameItem *)b;

    gint64 ta = gnostr_chess_game_item_get_created_at(item_a);
    gint64 tb = gnostr_chess_game_item_get_created_at(item_b);

    if (ta > tb) return -1;
    if (ta < tb) return  1;
    return 0;
}

static void
on_selection_activated(GtkListView *view, guint position, gpointer user_data)
{
    GnostrChessGamesBrowser *self = GNOSTR_CHESS_GAMES_BROWSER(user_data);
    (void)view;

    GtkSelectionModel *sel = gtk_list_view_get_model(GTK_LIST_VIEW(self->list_view));
    GnostrChessGameItem *item = g_list_model_get_item(G_LIST_MODEL(sel), position);
    if (!item)
        return;

    const char *event_id = gnostr_chess_game_item_get_event_id(item);
    if (event_id)
        g_signal_emit(self, signals[SIGNAL_GAME_SELECTED], 0, event_id);

    g_object_unref(item);
}

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

    g_clear_object(&self->store);
    g_clear_object(&self->sort_model);

    G_OBJECT_CLASS(gnostr_chess_games_browser_parent_class)->dispose(object);
}

static void
gnostr_chess_games_browser_class_init(GnostrChessGamesBrowserClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = gnostr_chess_games_browser_dispose;

    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);

    signals[SIGNAL_GAME_SELECTED] = g_signal_new(
        "game-selected",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0,
        NULL, NULL,
        NULL,
        G_TYPE_NONE, 1,
        G_TYPE_STRING);

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
    /* Model pipeline: GListStore → GtkSortListModel → GtkNoSelection → ListView */
    self->store = g_list_store_new(GNOSTR_TYPE_CHESS_GAME_ITEM);

    GtkCustomSorter *sorter = gtk_custom_sorter_new(sort_by_date_desc, NULL, NULL);
    self->sort_model = gtk_sort_list_model_new(
        G_LIST_MODEL(g_object_ref(self->store)),
        GTK_SORTER(sorter));

    GtkNoSelection *selection = gtk_no_selection_new(
        G_LIST_MODEL(g_object_ref(self->sort_model)));

    /* Factory */
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup",  G_CALLBACK(factory_setup),  self);
    g_signal_connect(factory, "bind",   G_CALLBACK(factory_bind),   self);
    g_signal_connect(factory, "unbind", G_CALLBACK(factory_unbind), self);

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

    self->list_view = gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory);
    gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(self->list_view), FALSE);
    g_signal_connect(self->list_view, "activate",
                     G_CALLBACK(on_selection_activated), self);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scroll),
                                   self->list_view);

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

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static void
populate_games_list(GnostrChessGamesBrowser *self)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->store));

    if (n == 0) {
        gtk_widget_set_visible(self->scroll, FALSE);
        gtk_widget_set_visible(self->empty_label, TRUE);
    } else {
        gtk_widget_set_visible(self->scroll, TRUE);
        gtk_widget_set_visible(self->empty_label, FALSE);
    }
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

    g_list_store_remove_all(self->store);

    if (games) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, games);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            GnostrChessGame *game = value;
            GnostrChessGameItem *item = gnostr_chess_game_item_new(game);
            g_list_store_append(self->store, item);
            g_object_unref(item);
        }
    }

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
