/**
 * GnostrChessGameView - Complete Chess Game View
 *
 * Container widget that combines the chess board with game controls for
 * playing chess against an AI opponent. Wires together:
 * - GnostrChessBoard for interactive piece movement
 * - GnostrChessSession for game state and AI computation
 *
 * Signal flow:
 * 1. Board "move-made" -> Session make_move()
 * 2. Session "ai-thinking" -> View shows/hides spinner
 * 3. Session "move-made" -> View updates move list, Board updates position
 * 4. Session "turn-changed" -> View updates status label
 * 5. Session "game-over" -> View shows result dialog
 */

#include "gnostr-chess-game-view.h"
#include "gnostr-chess-games-browser.h"
#include "../util/nip64_chess.h"
#include <string.h>

/* Use the types defined in the header */

/* Private structure */
struct _GnostrChessGameView {
    GtkWidget parent_instance;

    /* Core components */
    GnostrChessBoard *board;
    GnostrChessSession *session;

    /* UI elements */
    GtkWidget *main_box;           /* Horizontal container */
    GtkWidget *board_container;    /* Board + status area */
    GtkWidget *status_box;         /* Status label + spinner */
    GtkWidget *status_label;       /* "Your turn" / "AI thinking..." */
    GtkWidget *thinking_spinner;   /* Shows during AI computation */
    GtkWidget *side_panel;         /* Stack switcher + stack container */
    GtkWidget *stack_switcher;     /* Tabs: Game | Browse */
    GtkWidget *stack;              /* GtkStack for game/browse pages */
    GtkWidget *game_page;          /* Move list + controls */
    GtkWidget *move_list_scroll;   /* Scrollable move history */
    GtkWidget *move_list;          /* ListBox for moves */
    GtkWidget *controls_box;       /* Control buttons */
    GtkWidget *resign_button;
    GtkWidget *new_game_button;
    GtkWidget *flip_button;
    GtkWidget *draw_button;
    GnostrChessGamesBrowser *games_browser; /* Browse tab content */

    /* Plugin reference and function pointers */
    Nip64ChessPlugin *plugin;
    GnostrChessGetGamesFunc get_games_func;
    GnostrChessRequestGamesFunc request_games_func;

    /* State */
    gboolean show_move_list;
    gboolean human_plays_white;
    gboolean viewing_game;         /* TRUE when viewing a loaded game */

    /* Signal handler IDs */
    gulong board_move_made_id;
    gulong session_move_made_id;
    gulong session_turn_changed_id;
    gulong session_ai_thinking_id;
    gulong session_game_over_id;
    gulong session_state_changed_id;
    gulong games_updated_id;
    gulong game_selected_id;
    gulong refresh_requested_id;
};

G_DEFINE_TYPE(GnostrChessGameView, gnostr_chess_game_view, GTK_TYPE_WIDGET)

/* Signal IDs */
enum {
    SIGNAL_GAME_STARTED,
    SIGNAL_GAME_ENDED,
    SIGNAL_MOVE_PLAYED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_board_move_made(GnostrChessBoard *board,
                                const gchar *san,
                                const gchar *uci,
                                gpointer user_data);
static void on_session_move_made(GnostrChessSession *session,
                                  const gchar *san,
                                  gint move_number,
                                  gpointer user_data);
static void on_session_turn_changed(GnostrChessSession *session,
                                     gboolean is_white_turn,
                                     gpointer user_data);
static void on_session_ai_thinking(GnostrChessSession *session,
                                    gboolean is_thinking,
                                    gpointer user_data);
static void on_session_game_over(GnostrChessSession *session,
                                  const gchar *result,
                                  const gchar *reason,
                                  gpointer user_data);
static void on_session_state_changed(GnostrChessSession *session,
                                      gint new_state,
                                      gpointer user_data);
static void on_resign_clicked(GtkButton *button, gpointer user_data);
static void on_new_game_clicked(GtkButton *button, gpointer user_data);
static void on_flip_clicked(GtkButton *button, gpointer user_data);
static void on_draw_clicked(GtkButton *button, gpointer user_data);
static void update_status_label(GnostrChessGameView *self);
static void add_move_to_list(GnostrChessGameView *self, const gchar *san, gint move_number);
static void clear_move_list(GnostrChessGameView *self);
static void sync_board_with_session(GnostrChessGameView *self);
static void show_game_over_dialog(GnostrChessGameView *self,
                                   const gchar *result,
                                   const gchar *reason);
static void on_game_selected(GnostrChessGamesBrowser *browser,
                              const gchar *event_id,
                              gpointer user_data);
static void on_refresh_requested(GnostrChessGamesBrowser *browser,
                                  gpointer user_data);
static void on_games_updated(GObject *plugin, guint count, gpointer user_data);

/* ============================================================================
 * Widget Lifecycle
 * ============================================================================ */

static void
gnostr_chess_game_view_dispose(GObject *object)
{
    GnostrChessGameView *self = GNOSTR_CHESS_GAME_VIEW(object);

    /* Disconnect signal handlers */
    if (self->board && self->board_move_made_id > 0) {
        g_signal_handler_disconnect(self->board, self->board_move_made_id);
        self->board_move_made_id = 0;
    }

    if (self->session) {
        if (self->session_move_made_id > 0) {
            g_signal_handler_disconnect(self->session, self->session_move_made_id);
            self->session_move_made_id = 0;
        }
        if (self->session_turn_changed_id > 0) {
            g_signal_handler_disconnect(self->session, self->session_turn_changed_id);
            self->session_turn_changed_id = 0;
        }
        if (self->session_ai_thinking_id > 0) {
            g_signal_handler_disconnect(self->session, self->session_ai_thinking_id);
            self->session_ai_thinking_id = 0;
        }
        if (self->session_game_over_id > 0) {
            g_signal_handler_disconnect(self->session, self->session_game_over_id);
            self->session_game_over_id = 0;
        }
        if (self->session_state_changed_id > 0) {
            g_signal_handler_disconnect(self->session, self->session_state_changed_id);
            self->session_state_changed_id = 0;
        }
    }

    /* Disconnect games browser signals */
    if (self->games_browser) {
        if (self->game_selected_id > 0) {
            g_signal_handler_disconnect(self->games_browser, self->game_selected_id);
            self->game_selected_id = 0;
        }
        if (self->refresh_requested_id > 0) {
            g_signal_handler_disconnect(self->games_browser, self->refresh_requested_id);
            self->refresh_requested_id = 0;
        }
    }

    /* Disconnect plugin signal */
    if (self->plugin && self->games_updated_id > 0) {
        g_signal_handler_disconnect(self->plugin, self->games_updated_id);
        self->games_updated_id = 0;
    }

    /* Unparent main container */
    if (self->main_box) {
        gtk_widget_unparent(self->main_box);
        self->main_box = NULL;
    }

    /* Clear references */
    g_clear_object(&self->session);
    self->plugin = NULL;

    G_OBJECT_CLASS(gnostr_chess_game_view_parent_class)->dispose(object);
}

static void
gnostr_chess_game_view_class_init(GnostrChessGameViewClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = gnostr_chess_game_view_dispose;

    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);

    /* Signals */
    signals[SIGNAL_GAME_STARTED] = g_signal_new("game-started",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    signals[SIGNAL_GAME_ENDED] = g_signal_new("game-ended",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

    signals[SIGNAL_MOVE_PLAYED] = g_signal_new("move-played",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);

    /* CSS */
    gtk_widget_class_set_css_name(widget_class, "chess-game-view");
}

static void
gnostr_chess_game_view_init(GnostrChessGameView *self)
{
    self->show_move_list = TRUE;
    self->human_plays_white = TRUE;

    /* Create main horizontal container */
    self->main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_parent(self->main_box, GTK_WIDGET(self));

    /* ========== Board container (left side) ========== */
    self->board_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(self->board_container, TRUE);
    gtk_widget_set_vexpand(self->board_container, TRUE);
    gtk_box_append(GTK_BOX(self->main_box), self->board_container);

    /* Status box with label and spinner */
    self->status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(self->status_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(self->board_container), self->status_box);

    self->status_label = gtk_label_new("White to move");
    gtk_widget_add_css_class(self->status_label, "chess-status");
    gtk_box_append(GTK_BOX(self->status_box), self->status_label);

    self->thinking_spinner = gtk_spinner_new();
    gtk_widget_set_visible(self->thinking_spinner, FALSE);
    gtk_box_append(GTK_BOX(self->status_box), self->thinking_spinner);

    /* Chess board */
    self->board = gnostr_chess_board_new();
    gnostr_chess_board_set_interactive(self->board, FALSE);
    gnostr_chess_board_set_size(self->board, 400);
    gtk_widget_set_halign(GTK_WIDGET(self->board), GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(GTK_WIDGET(self->board), TRUE);
    gtk_box_append(GTK_BOX(self->board_container), GTK_WIDGET(self->board));

    /* Connect board signals */
    self->board_move_made_id = g_signal_connect(self->board, "move-made",
        G_CALLBACK(on_board_move_made), self);

    /* ========== Side panel (right side) ========== */
    self->side_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(self->side_panel, 220, -1);
    gtk_box_append(GTK_BOX(self->main_box), self->side_panel);

    /* Stack switcher (Game | Browse tabs) */
    self->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(self->stack),
                                   GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_vexpand(self->stack, TRUE);

    self->stack_switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(self->stack_switcher),
                                  GTK_STACK(self->stack));
    gtk_widget_set_halign(self->stack_switcher, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(self->side_panel), self->stack_switcher);
    gtk_box_append(GTK_BOX(self->side_panel), self->stack);

    /* ========== Game page ========== */
    self->game_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_stack_add_titled(GTK_STACK(self->stack), self->game_page, "game", "Game");

    /* Move list header */
    GtkWidget *move_header = gtk_label_new("Moves");
    gtk_widget_add_css_class(move_header, "heading");
    gtk_widget_set_halign(move_header, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(self->game_page), move_header);

    /* Scrolled move list */
    self->move_list_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->move_list_scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(self->move_list_scroll, TRUE);
    gtk_box_append(GTK_BOX(self->game_page), self->move_list_scroll);

    self->move_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->move_list),
                                     GTK_SELECTION_NONE);
    gtk_widget_add_css_class(self->move_list, "move-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->move_list_scroll),
                                   self->move_list);

    /* Control buttons */
    self->controls_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(self->game_page), self->controls_box);

    /* New Game button */
    self->new_game_button = gtk_button_new_with_label("New Game");
    gtk_widget_add_css_class(self->new_game_button, "suggested-action");
    g_signal_connect(self->new_game_button, "clicked",
                     G_CALLBACK(on_new_game_clicked), self);
    gtk_box_append(GTK_BOX(self->controls_box), self->new_game_button);

    /* Button row: Flip | Resign */
    GtkWidget *button_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_set_homogeneous(GTK_BOX(button_row), TRUE);
    gtk_box_append(GTK_BOX(self->controls_box), button_row);

    self->flip_button = gtk_button_new_with_label("Flip Board");
    g_signal_connect(self->flip_button, "clicked",
                     G_CALLBACK(on_flip_clicked), self);
    gtk_box_append(GTK_BOX(button_row), self->flip_button);

    self->resign_button = gtk_button_new_with_label("Resign");
    gtk_widget_add_css_class(self->resign_button, "destructive-action");
    gtk_widget_set_sensitive(self->resign_button, FALSE);
    g_signal_connect(self->resign_button, "clicked",
                     G_CALLBACK(on_resign_clicked), self);
    gtk_box_append(GTK_BOX(button_row), self->resign_button);

    /* Draw button */
    self->draw_button = gtk_button_new_with_label("Offer Draw");
    gtk_widget_set_sensitive(self->draw_button, FALSE);
    g_signal_connect(self->draw_button, "clicked",
                     G_CALLBACK(on_draw_clicked), self);
    gtk_box_append(GTK_BOX(self->controls_box), self->draw_button);

    /* ========== Browse page ========== */
    self->games_browser = gnostr_chess_games_browser_new();
    gtk_stack_add_titled(GTK_STACK(self->stack),
                          GTK_WIDGET(self->games_browser), "browse", "Browse");

    /* Connect browser signals */
    self->game_selected_id = g_signal_connect(self->games_browser, "game-selected",
        G_CALLBACK(on_game_selected), self);
    self->refresh_requested_id = g_signal_connect(self->games_browser, "refresh-requested",
        G_CALLBACK(on_refresh_requested), self);

    /* Create session */
    self->session = gnostr_chess_session_new();

    /* Connect session signals */
    self->session_move_made_id = g_signal_connect(self->session, "move-made",
        G_CALLBACK(on_session_move_made), self);
    self->session_turn_changed_id = g_signal_connect(self->session, "turn-changed",
        G_CALLBACK(on_session_turn_changed), self);
    self->session_ai_thinking_id = g_signal_connect(self->session, "ai-thinking",
        G_CALLBACK(on_session_ai_thinking), self);
    self->session_game_over_id = g_signal_connect(self->session, "game-over",
        G_CALLBACK(on_session_game_over), self);
    self->session_state_changed_id = g_signal_connect(self->session, "state-changed",
        G_CALLBACK(on_session_state_changed), self);
}

/* ============================================================================
 * Signal Handlers
 * ============================================================================ */

/**
 * Handle move made on the board (by human player clicking)
 */
static void
on_board_move_made(GnostrChessBoard *board,
                   const gchar *san,
                   const gchar *uci,
                   gpointer user_data)
{
    GnostrChessGameView *self = GNOSTR_CHESS_GAME_VIEW(user_data);
    (void)board;
    (void)san;

    if (!self->session || !uci || strlen(uci) < 4) return;

    /* Parse UCI move and forward to session */
    gchar from[3] = { uci[0], uci[1], '\0' };
    gchar to[3] = { uci[2], uci[3], '\0' };
    gchar promotion = (strlen(uci) >= 5) ? uci[4] : 0;

    gnostr_chess_session_make_move(self->session, from, to, promotion);
}

/**
 * Handle move made in session (by human or AI)
 */
static void
on_session_move_made(GnostrChessSession *session,
                     const gchar *san,
                     gint move_number,
                     gpointer user_data)
{
    GnostrChessGameView *self = GNOSTR_CHESS_GAME_VIEW(user_data);
    (void)session;

    /* Add move to the list */
    add_move_to_list(self, san, move_number);

    /* Sync board position with session */
    sync_board_with_session(self);

    /* Emit our own signal */
    g_signal_emit(self, signals[SIGNAL_MOVE_PLAYED], 0, san, move_number);
}

/**
 * Handle turn change
 */
static void
on_session_turn_changed(GnostrChessSession *session,
                        gboolean is_white_turn,
                        gpointer user_data)
{
    GnostrChessGameView *self = GNOSTR_CHESS_GAME_VIEW(user_data);
    (void)session;
    (void)is_white_turn;

    update_status_label(self);

    /* Check if it's AI's turn and trigger move if needed */
    if (!gnostr_chess_session_is_human_turn(self->session)) {
        /* AI move is triggered automatically by session after make_move */
    }
}

/**
 * Handle AI thinking state change
 */
static void
on_session_ai_thinking(GnostrChessSession *session,
                       gboolean is_thinking,
                       gpointer user_data)
{
    GnostrChessGameView *self = GNOSTR_CHESS_GAME_VIEW(user_data);
    (void)session;

    gtk_widget_set_visible(self->thinking_spinner, is_thinking);

    if (is_thinking) {
        gtk_spinner_start(GTK_SPINNER(self->thinking_spinner));
    } else {
        gtk_spinner_stop(GTK_SPINNER(self->thinking_spinner));
    }

    update_status_label(self);

    /* Disable interaction while AI is thinking */
    gnostr_chess_board_set_interactive(self->board, !is_thinking &&
        gnostr_chess_session_is_human_turn(self->session));
}

/**
 * Handle game over
 */
static void
on_session_game_over(GnostrChessSession *session,
                     const gchar *result,
                     const gchar *reason,
                     gpointer user_data)
{
    GnostrChessGameView *self = GNOSTR_CHESS_GAME_VIEW(user_data);
    (void)session;

    /* Disable board interaction */
    gnostr_chess_board_set_interactive(self->board, FALSE);

    /* Update button states */
    gtk_widget_set_sensitive(self->resign_button, FALSE);
    gtk_widget_set_sensitive(self->draw_button, FALSE);

    /* Update status */
    update_status_label(self);

    /* Show result dialog */
    show_game_over_dialog(self, result, reason);

    /* Emit signal */
    g_signal_emit(self, signals[SIGNAL_GAME_ENDED], 0, result, reason);
}

/**
 * Handle session state change
 */
static void
on_session_state_changed(GnostrChessSession *session,
                         gint new_state,
                         gpointer user_data)
{
    GnostrChessGameView *self = GNOSTR_CHESS_GAME_VIEW(user_data);
    (void)session;

    gboolean is_playing = (new_state == GNOSTR_CHESS_STATE_PLAYING);

    gtk_widget_set_sensitive(self->resign_button, is_playing);
    gtk_widget_set_sensitive(self->draw_button, is_playing);

    if (is_playing) {
        gnostr_chess_board_set_interactive(self->board,
            gnostr_chess_session_is_human_turn(self->session));
    }
}

/**
 * Resign button clicked
 */
static void
on_resign_clicked(GtkButton *button, gpointer user_data)
{
    GnostrChessGameView *self = GNOSTR_CHESS_GAME_VIEW(user_data);
    (void)button;

    gnostr_chess_session_resign(self->session);
}

/**
 * New game button clicked
 */
static void
on_new_game_clicked(GtkButton *button, gpointer user_data)
{
    GnostrChessGameView *self = GNOSTR_CHESS_GAME_VIEW(user_data);
    (void)button;

    /* Default: human plays white, intermediate difficulty */
    gnostr_chess_game_view_new_game(self, TRUE, GNOSTR_CHESS_DIFFICULTY_INTERMEDIATE);
}

/**
 * Flip board button clicked
 */
static void
on_flip_clicked(GtkButton *button, gpointer user_data)
{
    GnostrChessGameView *self = GNOSTR_CHESS_GAME_VIEW(user_data);
    (void)button;

    gnostr_chess_game_view_flip_board(self);
}

/**
 * Draw button clicked
 */
static void
on_draw_clicked(GtkButton *button, gpointer user_data)
{
    GnostrChessGameView *self = GNOSTR_CHESS_GAME_VIEW(user_data);
    (void)button;

    gnostr_chess_session_offer_draw(self->session);
}

/**
 * Handle game selected from browser
 */
static void
on_game_selected(GnostrChessGamesBrowser *browser,
                  const gchar *event_id,
                  gpointer user_data)
{
    GnostrChessGameView *self = GNOSTR_CHESS_GAME_VIEW(user_data);
    (void)browser;

    if (!self->plugin || !self->get_games_func || !event_id) return;

    GHashTable *games = self->get_games_func(self->plugin);
    if (!games) return;

    GnostrChessGame *game = g_hash_table_lookup(games, event_id);
    if (game) {
        gnostr_chess_game_view_load_game(self, game);
        /* Switch to game tab to show the loaded game */
        gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "game");
    }
}

/**
 * Handle refresh requested from browser
 */
static void
on_refresh_requested(GnostrChessGamesBrowser *browser,
                      gpointer user_data)
{
    GnostrChessGameView *self = GNOSTR_CHESS_GAME_VIEW(user_data);
    (void)browser;

    if (!self->plugin || !self->request_games_func) return;

    /* Show loading state */
    gnostr_chess_games_browser_set_loading(self->games_browser, TRUE);

    /* Request fresh games from relays */
    self->request_games_func(self->plugin);
}

/**
 * Handle games-updated signal from plugin
 */
static void
on_games_updated(GObject *plugin, guint count, gpointer user_data)
{
    GnostrChessGameView *self = GNOSTR_CHESS_GAME_VIEW(user_data);
    (void)plugin;
    (void)count;

    /* Hide loading state */
    gnostr_chess_games_browser_set_loading(self->games_browser, FALSE);

    /* Refresh the browser list */
    if (self->plugin && self->get_games_func) {
        GHashTable *games = self->get_games_func(self->plugin);
        gnostr_chess_games_browser_set_games(self->games_browser, games);
    }
}

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static void
update_status_label(GnostrChessGameView *self)
{
    if (!self->session) return;

    GnostrChessState state = gnostr_chess_session_get_state(self->session);

    if (state == GNOSTR_CHESS_STATE_SETUP) {
        gtk_label_set_text(GTK_LABEL(self->status_label), "Ready to play");
        return;
    }

    if (state == GNOSTR_CHESS_STATE_FINISHED) {
        const gchar *result = gnostr_chess_session_get_result(self->session);
        gchar *status;

        if (g_strcmp0(result, "1-0") == 0) {
            status = g_strdup("Checkmate! White wins");
        } else if (g_strcmp0(result, "0-1") == 0) {
            status = g_strdup("Checkmate! Black wins");
        } else if (g_strcmp0(result, "1/2-1/2") == 0) {
            status = g_strdup("Game drawn");
        } else {
            status = g_strdup("Game over");
        }

        gtk_label_set_text(GTK_LABEL(self->status_label), status);
        g_free(status);
        return;
    }

    /* Game in progress */
    gboolean is_white = gnostr_chess_session_is_white_turn(self->session);
    gboolean is_human = gnostr_chess_session_is_human_turn(self->session);

    ChessEngine *engine = gnostr_chess_session_get_engine(self->session);
    gboolean is_check = engine ? chess_engine_is_check(engine) : FALSE;

    const gchar *turn = is_white ? "White" : "Black";
    gchar *status;

    if (!is_human) {
        /* AI's turn */
        status = g_strdup_printf("%s thinking...", turn);
    } else if (is_check) {
        status = g_strdup_printf("%s to move (in check)", turn);
    } else {
        status = g_strdup_printf("%s to move", turn);
    }

    gtk_label_set_text(GTK_LABEL(self->status_label), status);
    g_free(status);
}

static void
add_move_to_list(GnostrChessGameView *self, const gchar *san, gint move_number)
{
    gint ply = gnostr_chess_session_get_move_count(self->session);
    gboolean is_white_move = ((ply % 2) == 1);

    GtkWidget *row;

    if (is_white_move) {
        /* White's move - create new row */
        gchar *text = g_strdup_printf("%d. %s", move_number, san);
        row = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(row), 0.0);
        gtk_widget_add_css_class(row, "move-row");
        g_free(text);

        gtk_list_box_append(GTK_LIST_BOX(self->move_list), row);
    } else {
        /* Black's move - update last row */
        GtkListBoxRow *last_row = gtk_list_box_get_row_at_index(
            GTK_LIST_BOX(self->move_list),
            move_number - 1);

        if (last_row) {
            GtkWidget *label = gtk_list_box_row_get_child(last_row);
            if (GTK_IS_LABEL(label)) {
                const gchar *current = gtk_label_get_text(GTK_LABEL(label));
                gchar *text = g_strdup_printf("%s  %s", current, san);
                gtk_label_set_text(GTK_LABEL(label), text);
                g_free(text);
            }
        }
    }

    /* Scroll to bottom */
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(self->move_list_scroll));
    gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
}

static void
clear_move_list(GnostrChessGameView *self)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->move_list)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(self->move_list), child);
    }
}

static void
sync_board_with_session(GnostrChessGameView *self)
{
    if (!self->session || !self->board) return;

    ChessEngine *engine = gnostr_chess_session_get_engine(self->session);
    if (!engine) return;

    /* Get FEN from session's engine and set it on board */
    gchar *fen = chess_engine_get_fen(engine);
    if (fen) {
        gnostr_chess_board_set_fen(self->board, fen);
        g_free(fen);
    }
}

static void
show_game_over_dialog(GnostrChessGameView *self,
                      const gchar *result,
                      const gchar *reason)
{
    GtkWidget *toplevel = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(self)));
    if (!GTK_IS_WINDOW(toplevel)) return;

    /* Build message */
    gchar *title;
    gchar *message;

    if (g_strcmp0(result, "1-0") == 0) {
        title = g_strdup("White Wins!");
        message = g_strdup(reason ? reason : "Checkmate");
    } else if (g_strcmp0(result, "0-1") == 0) {
        title = g_strdup("Black Wins!");
        message = g_strdup(reason ? reason : "Checkmate");
    } else if (g_strcmp0(result, "1/2-1/2") == 0) {
        title = g_strdup("Draw");
        message = g_strdup(reason ? reason : "Game drawn");
    } else {
        title = g_strdup("Game Over");
        message = g_strdup(reason ? reason : "");
    }

    /* Create and show dialog */
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(dialog, message);
    gtk_alert_dialog_set_buttons(dialog, (const char * const[]){"OK", NULL});

    gtk_alert_dialog_show(dialog, GTK_WINDOW(toplevel));

    g_object_unref(dialog);
    g_free(title);
    g_free(message);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

GnostrChessGameView *
gnostr_chess_game_view_new(void)
{
    return g_object_new(GNOSTR_TYPE_CHESS_GAME_VIEW, NULL);
}

void
gnostr_chess_game_view_new_game(GnostrChessGameView *self,
                                 gboolean play_as_white,
                                 GnostrChessDifficulty difficulty)
{
    g_return_if_fail(GNOSTR_IS_CHESS_GAME_VIEW(self));

    self->human_plays_white = play_as_white;

    /* Reset session */
    gnostr_chess_session_reset(self->session);

    /* Configure players */
    if (play_as_white) {
        gnostr_chess_session_set_players(self->session,
            GNOSTR_CHESS_PLAYER_HUMAN,
            GNOSTR_CHESS_PLAYER_AI);
        gnostr_chess_board_set_player_color(self->board, GNOSTR_CHESS_COLOR_WHITE);
        gnostr_chess_board_set_flipped(self->board, FALSE);
    } else {
        gnostr_chess_session_set_players(self->session,
            GNOSTR_CHESS_PLAYER_AI,
            GNOSTR_CHESS_PLAYER_HUMAN);
        gnostr_chess_board_set_player_color(self->board, GNOSTR_CHESS_COLOR_BLACK);
        gnostr_chess_board_set_flipped(self->board, TRUE);
    }

    /* Set AI difficulty */
    gnostr_chess_session_set_ai_depth(self->session, (gint)difficulty);

    /* Reset board */
    gnostr_chess_board_reset(self->board);
    clear_move_list(self);

    /* Start the game */
    gnostr_chess_session_start(self->session);

    /* Enable interaction if it's human's turn */
    gnostr_chess_board_set_interactive(self->board,
        gnostr_chess_session_is_human_turn(self->session));

    update_status_label(self);

    /* Emit signal */
    g_signal_emit(self, signals[SIGNAL_GAME_STARTED], 0);
}

void
gnostr_chess_game_view_new_game_human_vs_human(GnostrChessGameView *self)
{
    g_return_if_fail(GNOSTR_IS_CHESS_GAME_VIEW(self));

    self->human_plays_white = TRUE;

    /* Reset session */
    gnostr_chess_session_reset(self->session);

    /* Both players are human */
    gnostr_chess_session_set_players(self->session,
        GNOSTR_CHESS_PLAYER_HUMAN,
        GNOSTR_CHESS_PLAYER_HUMAN);

    /* Allow moving any color */
    gnostr_chess_board_set_player_color(self->board, GNOSTR_CHESS_COLOR_NONE);
    gnostr_chess_board_set_flipped(self->board, FALSE);

    /* Reset board */
    gnostr_chess_board_reset(self->board);
    clear_move_list(self);

    /* Start the game */
    gnostr_chess_session_start(self->session);
    gnostr_chess_board_set_interactive(self->board, TRUE);

    update_status_label(self);
    g_signal_emit(self, signals[SIGNAL_GAME_STARTED], 0);
}

void
gnostr_chess_game_view_new_game_ai_vs_ai(GnostrChessGameView *self,
                                          GnostrChessDifficulty difficulty)
{
    g_return_if_fail(GNOSTR_IS_CHESS_GAME_VIEW(self));

    /* Reset session */
    gnostr_chess_session_reset(self->session);

    /* Both players are AI */
    gnostr_chess_session_set_players(self->session,
        GNOSTR_CHESS_PLAYER_AI,
        GNOSTR_CHESS_PLAYER_AI);

    gnostr_chess_session_set_ai_depth(self->session, (gint)difficulty);

    /* Disable interaction */
    gnostr_chess_board_set_player_color(self->board, GNOSTR_CHESS_COLOR_NONE);
    gnostr_chess_board_set_interactive(self->board, FALSE);
    gnostr_chess_board_set_flipped(self->board, FALSE);

    /* Reset board */
    gnostr_chess_board_reset(self->board);
    clear_move_list(self);

    /* Start the game - AI will begin computing */
    gnostr_chess_session_start(self->session);

    update_status_label(self);
    g_signal_emit(self, signals[SIGNAL_GAME_STARTED], 0);
}

void
gnostr_chess_game_view_resign(GnostrChessGameView *self)
{
    g_return_if_fail(GNOSTR_IS_CHESS_GAME_VIEW(self));

    if (gnostr_chess_session_get_state(self->session) == GNOSTR_CHESS_STATE_PLAYING) {
        gnostr_chess_session_resign(self->session);
    }
}

void
gnostr_chess_game_view_offer_draw(GnostrChessGameView *self)
{
    g_return_if_fail(GNOSTR_IS_CHESS_GAME_VIEW(self));

    if (gnostr_chess_session_get_state(self->session) == GNOSTR_CHESS_STATE_PLAYING) {
        gnostr_chess_session_offer_draw(self->session);
    }
}

void
gnostr_chess_game_view_flip_board(GnostrChessGameView *self)
{
    g_return_if_fail(GNOSTR_IS_CHESS_GAME_VIEW(self));

    gboolean currently_flipped = gnostr_chess_board_is_flipped(self->board);
    gnostr_chess_board_set_flipped(self->board, !currently_flipped);
}

gboolean
gnostr_chess_game_view_is_game_active(GnostrChessGameView *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_GAME_VIEW(self), FALSE);

    return gnostr_chess_session_get_state(self->session) == GNOSTR_CHESS_STATE_PLAYING;
}

gboolean
gnostr_chess_game_view_is_thinking(GnostrChessGameView *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_GAME_VIEW(self), FALSE);

    return gtk_widget_get_visible(self->thinking_spinner);
}

GnostrChessSession *
gnostr_chess_game_view_get_session(GnostrChessGameView *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_GAME_VIEW(self), NULL);
    return self->session;
}

GnostrChessBoard *
gnostr_chess_game_view_get_board(GnostrChessGameView *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_GAME_VIEW(self), NULL);
    return self->board;
}

gchar *
gnostr_chess_game_view_export_pgn(GnostrChessGameView *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_GAME_VIEW(self), NULL);
    return gnostr_chess_session_export_pgn(self->session);
}

void
gnostr_chess_game_view_set_board_size(GnostrChessGameView *self, gint size)
{
    g_return_if_fail(GNOSTR_IS_CHESS_GAME_VIEW(self));
    gnostr_chess_board_set_size(self->board, size);
}

void
gnostr_chess_game_view_set_show_move_list(GnostrChessGameView *self,
                                           gboolean show)
{
    g_return_if_fail(GNOSTR_IS_CHESS_GAME_VIEW(self));

    self->show_move_list = show;
    gtk_widget_set_visible(self->side_panel, show);
}

void
gnostr_chess_game_view_set_plugin(GnostrChessGameView *self,
                                   Nip64ChessPlugin *plugin)
{
    g_return_if_fail(GNOSTR_IS_CHESS_GAME_VIEW(self));

    /* Disconnect from old plugin */
    if (self->plugin && self->games_updated_id > 0) {
        g_signal_handler_disconnect(self->plugin, self->games_updated_id);
        self->games_updated_id = 0;
    }

    self->plugin = plugin;
    self->get_games_func = NULL;
    self->request_games_func = NULL;

    if (plugin) {
        /* Connect to games-updated signal */
        self->games_updated_id = g_signal_connect(plugin, "games-updated",
            G_CALLBACK(on_games_updated), self);
    }
}

void
gnostr_chess_game_view_set_plugin_callbacks(GnostrChessGameView *self,
                                             GnostrChessGetGamesFunc get_games,
                                             GnostrChessRequestGamesFunc request_games)
{
    g_return_if_fail(GNOSTR_IS_CHESS_GAME_VIEW(self));

    self->get_games_func = get_games;
    self->request_games_func = request_games;

    /* Initialize browser with current games if plugin is set */
    if (self->plugin && self->get_games_func) {
        GHashTable *games = self->get_games_func(self->plugin);
        gnostr_chess_games_browser_set_games(self->games_browser, games);
    }
}

void
gnostr_chess_game_view_load_game(GnostrChessGameView *self,
                                  GnostrChessGame *game)
{
    g_return_if_fail(GNOSTR_IS_CHESS_GAME_VIEW(self));
    g_return_if_fail(game != NULL);

    self->viewing_game = TRUE;

    /* Disable interactive mode - this is view only */
    gnostr_chess_board_set_interactive(self->board, FALSE);

    /* Clear existing move list */
    clear_move_list(self);

    /* Populate move list from game */
    if (game->moves && game->moves_count > 0) {
        gint move_num = 1;
        for (gsize i = 0; i < game->moves_count; i++) {
            GnostrChessMove *move = game->moves[i];
            if (move && move->san) {
                gboolean is_white = (i % 2 == 0);
                add_move_to_list(self, move->san, is_white ? move_num : move_num);
                if (!is_white) move_num++;
            }
        }
    }

    /* Navigate to final position and render board */
    gnostr_chess_game_last(game);

    /* Build FEN from the final position or reset to starting position */
    if (game->moves_count > 0) {
        /* Render board from game's current board state */
        gchar fen[128];
        gint empty_count = 0;
        gint fen_pos = 0;

        for (gint rank = 7; rank >= 0; rank--) {
            for (gint file = 0; file < 8; file++) {
                gint idx = rank * 8 + file;
                const GnostrChessSquare *sq = &game->board[idx];

                if (sq->piece == GNOSTR_CHESS_PIECE_NONE) {
                    empty_count++;
                } else {
                    if (empty_count > 0) {
                        fen[fen_pos++] = '0' + empty_count;
                        empty_count = 0;
                    }
                    gchar c = gnostr_chess_piece_char(sq->piece);
                    if (sq->color == GNOSTR_CHESS_COLOR_BLACK) {
                        c = g_ascii_tolower(c);
                    }
                    fen[fen_pos++] = c;
                }
            }
            if (empty_count > 0) {
                fen[fen_pos++] = '0' + empty_count;
                empty_count = 0;
            }
            if (rank > 0) {
                fen[fen_pos++] = '/';
            }
        }
        /* Add minimal FEN suffix */
        g_strlcpy(fen + fen_pos, " w - - 0 1", sizeof(fen) - fen_pos);
        gnostr_chess_board_set_fen(self->board, fen);
    } else {
        /* No moves, reset to starting position */
        gnostr_chess_board_reset(self->board);
    }

    /* Update status label with game info */
    gchar *status;
    if (game->result_string && game->result_string[0] != '*') {
        if (g_strcmp0(game->result_string, "1-0") == 0) {
            status = g_strdup_printf("White wins - %s vs %s",
                game->white_player ? game->white_player : "?",
                game->black_player ? game->black_player : "?");
        } else if (g_strcmp0(game->result_string, "0-1") == 0) {
            status = g_strdup_printf("Black wins - %s vs %s",
                game->white_player ? game->white_player : "?",
                game->black_player ? game->black_player : "?");
        } else if (g_strcmp0(game->result_string, "1/2-1/2") == 0) {
            status = g_strdup_printf("Draw - %s vs %s",
                game->white_player ? game->white_player : "?",
                game->black_player ? game->black_player : "?");
        } else {
            status = g_strdup_printf("%s vs %s - %s",
                game->white_player ? game->white_player : "?",
                game->black_player ? game->black_player : "?",
                game->result_string);
        }
    } else {
        status = g_strdup_printf("%s vs %s (in progress)",
            game->white_player ? game->white_player : "?",
            game->black_player ? game->black_player : "?");
    }

    gtk_label_set_text(GTK_LABEL(self->status_label), status);
    g_free(status);

    /* Disable game controls when viewing */
    gtk_widget_set_sensitive(self->resign_button, FALSE);
    gtk_widget_set_sensitive(self->draw_button, FALSE);
}
