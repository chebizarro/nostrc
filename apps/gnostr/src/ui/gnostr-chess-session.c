/**
 * GnostrChessSession - Chess Game Session Management
 *
 * Manages active chess game sessions including:
 * - Player type configuration (human/AI)
 * - Game state transitions
 * - Move history tracking
 * - Async AI move computation via GTask
 * - PGN export
 */

#include "gnostr-chess-session.h"
#include <string.h>
#include <time.h>

/* Property IDs */
enum {
    PROP_0,
    PROP_STATE,
    PROP_WHITE_PLAYER,
    PROP_BLACK_PLAYER,
    PROP_AI_DEPTH,
    PROP_RESULT,
    N_PROPERTIES
};

/* Signal IDs */
enum {
    SIGNAL_STATE_CHANGED,
    SIGNAL_MOVE_MADE,
    SIGNAL_GAME_OVER,
    SIGNAL_TURN_CHANGED,
    SIGNAL_AI_THINKING,
    N_SIGNALS
};

static GParamSpec *properties[N_PROPERTIES] = { NULL, };
static guint signals[N_SIGNALS] = { 0 };

/* Private structure */
struct _GnostrChessSession {
    GObject parent_instance;

    /* Game state */
    GnostrChessState state;
    GnostrChessPlayerType white_player;
    GnostrChessPlayerType black_player;
    gint ai_depth;
    gchar *result;

    /* Chess engine */
    ChessEngine *engine;

    /* Move history (GList of gchar* SAN strings) */
    GList *move_history;
    gint move_count;

    /* AI computation state */
    GCancellable *ai_cancellable;
    gboolean ai_thinking;

    /* Draw offer state */
    gboolean draw_offered;
};

G_DEFINE_TYPE(GnostrChessSession, gnostr_chess_session, G_TYPE_OBJECT)

/* Forward declarations */
static void compute_ai_move_thread(GTask *task,
                                   gpointer source_object,
                                   gpointer task_data,
                                   GCancellable *cancellable);
static void on_ai_move_complete(GObject *source_object,
                                GAsyncResult *res,
                                gpointer user_data);
static void check_game_over(GnostrChessSession *self);
static gchar *move_to_san(GnostrChessSession *self,
                          const gchar *from,
                          const gchar *to,
                          gchar promotion);

/* ============================================================================
 * GObject Implementation
 * ============================================================================ */

static void
gnostr_chess_session_dispose(GObject *object)
{
    GnostrChessSession *self = GNOSTR_CHESS_SESSION(object);

    /* Cancel any pending AI computation */
    if (self->ai_cancellable) {
        g_cancellable_cancel(self->ai_cancellable);
        g_clear_object(&self->ai_cancellable);
    }

    G_OBJECT_CLASS(gnostr_chess_session_parent_class)->dispose(object);
}

static void
gnostr_chess_session_finalize(GObject *object)
{
    GnostrChessSession *self = GNOSTR_CHESS_SESSION(object);

    /* Free engine */
    if (self->engine) {
        chess_engine_free(self->engine);
        self->engine = NULL;
    }

    /* Free move history */
    if (self->move_history) {
        g_list_free_full(self->move_history, g_free);
        self->move_history = NULL;
    }

    /* Free result string */
    g_free(self->result);

    G_OBJECT_CLASS(gnostr_chess_session_parent_class)->finalize(object);
}

static void
gnostr_chess_session_get_property(GObject *object,
                                   guint prop_id,
                                   GValue *value,
                                   GParamSpec *pspec)
{
    GnostrChessSession *self = GNOSTR_CHESS_SESSION(object);

    switch (prop_id) {
    case PROP_STATE:
        g_value_set_int(value, self->state);
        break;
    case PROP_WHITE_PLAYER:
        g_value_set_int(value, self->white_player);
        break;
    case PROP_BLACK_PLAYER:
        g_value_set_int(value, self->black_player);
        break;
    case PROP_AI_DEPTH:
        g_value_set_int(value, self->ai_depth);
        break;
    case PROP_RESULT:
        g_value_set_string(value, self->result);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
gnostr_chess_session_set_property(GObject *object,
                                   guint prop_id,
                                   const GValue *value,
                                   GParamSpec *pspec)
{
    GnostrChessSession *self = GNOSTR_CHESS_SESSION(object);

    switch (prop_id) {
    case PROP_WHITE_PLAYER:
        self->white_player = g_value_get_int(value);
        break;
    case PROP_BLACK_PLAYER:
        self->black_player = g_value_get_int(value);
        break;
    case PROP_AI_DEPTH:
        gnostr_chess_session_set_ai_depth(self, g_value_get_int(value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
gnostr_chess_session_class_init(GnostrChessSessionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = gnostr_chess_session_dispose;
    object_class->finalize = gnostr_chess_session_finalize;
    object_class->get_property = gnostr_chess_session_get_property;
    object_class->set_property = gnostr_chess_session_set_property;

    /* Properties */
    properties[PROP_STATE] =
        g_param_spec_int("state",
                         "State",
                         "Current game state",
                         GNOSTR_CHESS_STATE_SETUP,
                         GNOSTR_CHESS_STATE_FINISHED,
                         GNOSTR_CHESS_STATE_SETUP,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    properties[PROP_WHITE_PLAYER] =
        g_param_spec_int("white-player",
                         "White Player",
                         "Player type for white",
                         GNOSTR_CHESS_PLAYER_HUMAN,
                         GNOSTR_CHESS_PLAYER_AI,
                         GNOSTR_CHESS_PLAYER_HUMAN,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_BLACK_PLAYER] =
        g_param_spec_int("black-player",
                         "Black Player",
                         "Player type for black",
                         GNOSTR_CHESS_PLAYER_HUMAN,
                         GNOSTR_CHESS_PLAYER_AI,
                         GNOSTR_CHESS_PLAYER_HUMAN,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_AI_DEPTH] =
        g_param_spec_int("ai-depth",
                         "AI Depth",
                         "Search depth for AI (2-10)",
                         2, 10, 4,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_RESULT] =
        g_param_spec_string("result",
                            "Result",
                            "Game result (1-0, 0-1, 1/2-1/2)",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPERTIES, properties);

    /* Signals */
    signals[SIGNAL_STATE_CHANGED] =
        g_signal_new("state-changed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 1,
                     G_TYPE_INT);

    signals[SIGNAL_MOVE_MADE] =
        g_signal_new("move-made",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 2,
                     G_TYPE_STRING, G_TYPE_INT);

    signals[SIGNAL_GAME_OVER] =
        g_signal_new("game-over",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 2,
                     G_TYPE_STRING, G_TYPE_STRING);

    signals[SIGNAL_TURN_CHANGED] =
        g_signal_new("turn-changed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 1,
                     G_TYPE_BOOLEAN);

    signals[SIGNAL_AI_THINKING] =
        g_signal_new("ai-thinking",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 1,
                     G_TYPE_BOOLEAN);
}

static void
gnostr_chess_session_init(GnostrChessSession *self)
{
    self->state = GNOSTR_CHESS_STATE_SETUP;
    self->white_player = GNOSTR_CHESS_PLAYER_HUMAN;
    self->black_player = GNOSTR_CHESS_PLAYER_HUMAN;
    self->ai_depth = 4;
    self->result = NULL;
    self->engine = NULL;
    self->move_history = NULL;
    self->move_count = 0;
    self->ai_cancellable = NULL;
    self->ai_thinking = FALSE;
    self->draw_offered = FALSE;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

GnostrChessSession *
gnostr_chess_session_new(void)
{
    return g_object_new(GNOSTR_TYPE_CHESS_SESSION, NULL);
}

void
gnostr_chess_session_set_players(GnostrChessSession *self,
                                  GnostrChessPlayerType white_type,
                                  GnostrChessPlayerType black_type)
{
    g_return_if_fail(GNOSTR_IS_CHESS_SESSION(self));
    g_return_if_fail(self->state == GNOSTR_CHESS_STATE_SETUP);

    self->white_player = white_type;
    self->black_player = black_type;

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_WHITE_PLAYER]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_BLACK_PLAYER]);
}

void
gnostr_chess_session_set_ai_depth(GnostrChessSession *self, gint depth)
{
    g_return_if_fail(GNOSTR_IS_CHESS_SESSION(self));

    /* Clamp to valid range */
    if (depth < 2) depth = 2;
    if (depth > 10) depth = 10;

    if (self->ai_depth != depth) {
        self->ai_depth = depth;
        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_AI_DEPTH]);
    }
}

void
gnostr_chess_session_start(GnostrChessSession *self)
{
    g_return_if_fail(GNOSTR_IS_CHESS_SESSION(self));
    g_return_if_fail(self->state == GNOSTR_CHESS_STATE_SETUP);

    /* Create and initialize the engine */
    if (self->engine) {
        chess_engine_free(self->engine);
    }
    self->engine = chess_engine_new();

    /* Clear move history */
    if (self->move_history) {
        g_list_free_full(self->move_history, g_free);
        self->move_history = NULL;
    }
    self->move_count = 0;

    /* Clear result */
    g_free(self->result);
    self->result = NULL;

    /* Transition to PLAYING state */
    self->state = GNOSTR_CHESS_STATE_PLAYING;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STATE]);
    g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, self->state);

    /* Emit initial turn-changed (white moves first) */
    g_signal_emit(self, signals[SIGNAL_TURN_CHANGED], 0, TRUE);

    /* If white is AI, trigger first move */
    if (self->white_player == GNOSTR_CHESS_PLAYER_AI) {
        gnostr_chess_session_request_ai_move(self);
    }
}

gboolean
gnostr_chess_session_make_move(GnostrChessSession *self,
                                const gchar *from,
                                const gchar *to,
                                gchar promotion)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_SESSION(self), FALSE);
    g_return_val_if_fail(from != NULL, FALSE);
    g_return_val_if_fail(to != NULL, FALSE);
    g_return_val_if_fail(self->state == GNOSTR_CHESS_STATE_PLAYING, FALSE);
    g_return_val_if_fail(self->engine != NULL, FALSE);

    /* Check it's a human's turn */
    if (!gnostr_chess_session_is_human_turn(self)) {
        g_warning("[CHESS_SESSION] make_move called but not human's turn");
        return FALSE;
    }

    /* Generate SAN before making the move (need current position) */
    gchar *san = move_to_san(self, from, to, promotion);

    /* Try to make the move */
    if (!chess_engine_make_move(self->engine, from, to, promotion)) {
        g_free(san);
        return FALSE;
    }

    /* Record the move */
    self->move_count++;
    self->move_history = g_list_append(self->move_history, san);

    /* Clear any draw offer */
    self->draw_offered = FALSE;

    /* Emit move-made signal */
    gint move_number = (self->move_count + 1) / 2;
    g_signal_emit(self, signals[SIGNAL_MOVE_MADE], 0, san, move_number);

    /* Check for game over conditions */
    check_game_over(self);

    if (self->state == GNOSTR_CHESS_STATE_PLAYING) {
        /* Game continues - emit turn changed */
        gboolean is_white = (chess_engine_get_side_to_move(self->engine) == 0);
        g_signal_emit(self, signals[SIGNAL_TURN_CHANGED], 0, is_white);

        /* If opponent is AI, trigger AI move */
        if (!gnostr_chess_session_is_human_turn(self)) {
            gnostr_chess_session_request_ai_move(self);
        }
    }

    return TRUE;
}

void
gnostr_chess_session_request_ai_move(GnostrChessSession *self)
{
    g_return_if_fail(GNOSTR_IS_CHESS_SESSION(self));
    g_return_if_fail(self->state == GNOSTR_CHESS_STATE_PLAYING);
    g_return_if_fail(self->engine != NULL);

    /* Check it's actually an AI's turn */
    gboolean is_white = (chess_engine_get_side_to_move(self->engine) == 0);
    GnostrChessPlayerType current_player = is_white ?
        self->white_player : self->black_player;

    if (current_player != GNOSTR_CHESS_PLAYER_AI) {
        g_warning("[CHESS_SESSION] request_ai_move called but not AI's turn");
        return;
    }

    /* Prevent concurrent AI computations */
    if (self->ai_thinking) {
        g_warning("[CHESS_SESSION] AI already computing");
        return;
    }

    /* Create cancellable */
    if (self->ai_cancellable) {
        g_cancellable_cancel(self->ai_cancellable);
        g_object_unref(self->ai_cancellable);
    }
    self->ai_cancellable = g_cancellable_new();

    /* Set thinking state and emit signal */
    self->ai_thinking = TRUE;
    g_signal_emit(self, signals[SIGNAL_AI_THINKING], 0, TRUE);

    /* Create GTask for async computation */
    GTask *task = g_task_new(self, self->ai_cancellable,
                             on_ai_move_complete, NULL);
    g_task_set_task_data(task, GINT_TO_POINTER(self->ai_depth), NULL);

    /* Run in thread pool */
    g_task_run_in_thread(task, compute_ai_move_thread);
    g_object_unref(task);
}

gboolean
gnostr_chess_session_is_human_turn(GnostrChessSession *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_SESSION(self), FALSE);

    if (self->state != GNOSTR_CHESS_STATE_PLAYING || !self->engine)
        return FALSE;

    gboolean is_white = (chess_engine_get_side_to_move(self->engine) == 0);
    GnostrChessPlayerType current = is_white ?
        self->white_player : self->black_player;

    return current == GNOSTR_CHESS_PLAYER_HUMAN;
}

ChessEngine *
gnostr_chess_session_get_engine(GnostrChessSession *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_SESSION(self), NULL);
    return self->engine;
}

GList *
gnostr_chess_session_get_move_history(GnostrChessSession *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_SESSION(self), NULL);

    /* Return a copy of the list with copied strings */
    GList *copy = NULL;
    for (GList *l = self->move_history; l; l = l->next) {
        copy = g_list_append(copy, g_strdup((gchar *)l->data));
    }
    return copy;
}

gint
gnostr_chess_session_get_move_count(GnostrChessSession *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_SESSION(self), 0);
    return self->move_count;
}

gchar *
gnostr_chess_session_export_pgn(GnostrChessSession *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_SESSION(self), NULL);

    if (!self->move_history && self->state == GNOSTR_CHESS_STATE_SETUP)
        return NULL;

    GString *pgn = g_string_new(NULL);

    /* PGN headers */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    gchar date_buf[16];
    strftime(date_buf, sizeof(date_buf), "%Y.%m.%d", tm);

    g_string_append_printf(pgn, "[Event \"GNostr Chess Game\"]\n");
    g_string_append_printf(pgn, "[Site \"Nostr Network\"]\n");
    g_string_append_printf(pgn, "[Date \"%s\"]\n", date_buf);
    g_string_append_printf(pgn, "[Round \"-\"]\n");
    g_string_append_printf(pgn, "[White \"%s\"]\n",
                           self->white_player == GNOSTR_CHESS_PLAYER_HUMAN ?
                           "Human" : "Engine");
    g_string_append_printf(pgn, "[Black \"%s\"]\n",
                           self->black_player == GNOSTR_CHESS_PLAYER_HUMAN ?
                           "Human" : "Engine");

    /* Result */
    const gchar *result = self->result ? self->result : "*";
    g_string_append_printf(pgn, "[Result \"%s\"]\n", result);
    g_string_append_c(pgn, '\n');

    /* Move text */
    gint move_num = 1;
    gint ply = 0;

    for (GList *l = self->move_history; l; l = l->next) {
        gchar *san = (gchar *)l->data;

        if (ply % 2 == 0) {
            /* White's move */
            g_string_append_printf(pgn, "%d. %s ", move_num, san);
        } else {
            /* Black's move */
            g_string_append_printf(pgn, "%s ", san);
            move_num++;
        }
        ply++;

        /* Line wrap every 6 moves */
        if (ply % 12 == 0)
            g_string_append_c(pgn, '\n');
    }

    g_string_append(pgn, result);
    g_string_append_c(pgn, '\n');

    return g_string_free(pgn, FALSE);
}

GnostrChessState
gnostr_chess_session_get_state(GnostrChessSession *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_SESSION(self),
                         GNOSTR_CHESS_STATE_SETUP);
    return self->state;
}

const gchar *
gnostr_chess_session_get_result(GnostrChessSession *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_SESSION(self), NULL);
    return self->result;
}

gboolean
gnostr_chess_session_is_white_turn(GnostrChessSession *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_SESSION(self), TRUE);

    if (!self->engine)
        return TRUE;

    return chess_engine_get_side_to_move(self->engine) == 0;
}

void
gnostr_chess_session_resign(GnostrChessSession *self)
{
    g_return_if_fail(GNOSTR_IS_CHESS_SESSION(self));
    g_return_if_fail(self->state == GNOSTR_CHESS_STATE_PLAYING);

    /* Determine who resigns based on whose turn it is */
    gboolean is_white = gnostr_chess_session_is_white_turn(self);
    const gchar *result = is_white ? "0-1" : "1-0";
    const gchar *reason = is_white ? "White resigns" : "Black resigns";

    self->result = g_strdup(result);
    self->state = GNOSTR_CHESS_STATE_FINISHED;

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STATE]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_RESULT]);

    g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, self->state);
    g_signal_emit(self, signals[SIGNAL_GAME_OVER], 0, result, reason);
}

void
gnostr_chess_session_offer_draw(GnostrChessSession *self)
{
    g_return_if_fail(GNOSTR_IS_CHESS_SESSION(self));
    g_return_if_fail(self->state == GNOSTR_CHESS_STATE_PLAYING);

    gboolean is_white = gnostr_chess_session_is_white_turn(self);
    GnostrChessPlayerType current = is_white ?
        self->white_player : self->black_player;
    GnostrChessPlayerType opponent = is_white ?
        self->black_player : self->white_player;

    /* If opponent is AI, auto-accept (simplified) */
    if (opponent == GNOSTR_CHESS_PLAYER_AI) {
        self->result = g_strdup("1/2-1/2");
        self->state = GNOSTR_CHESS_STATE_FINISHED;

        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STATE]);
        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_RESULT]);

        g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, self->state);
        g_signal_emit(self, signals[SIGNAL_GAME_OVER], 0,
                      "1/2-1/2", "Draw agreed");
        return;
    }

    /* Human vs Human: toggle draw offer */
    if (self->draw_offered) {
        /* Second player accepts */
        self->result = g_strdup("1/2-1/2");
        self->state = GNOSTR_CHESS_STATE_FINISHED;

        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STATE]);
        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_RESULT]);

        g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, self->state);
        g_signal_emit(self, signals[SIGNAL_GAME_OVER], 0,
                      "1/2-1/2", "Draw agreed");
    } else {
        /* First player offers */
        self->draw_offered = TRUE;
        /* UI should show draw offer indicator */
    }
}

void
gnostr_chess_session_reset(GnostrChessSession *self)
{
    g_return_if_fail(GNOSTR_IS_CHESS_SESSION(self));

    /* Cancel any pending AI computation */
    if (self->ai_cancellable) {
        g_cancellable_cancel(self->ai_cancellable);
        g_clear_object(&self->ai_cancellable);
    }
    self->ai_thinking = FALSE;

    /* Free engine */
    if (self->engine) {
        chess_engine_free(self->engine);
        self->engine = NULL;
    }

    /* Clear move history */
    if (self->move_history) {
        g_list_free_full(self->move_history, g_free);
        self->move_history = NULL;
    }
    self->move_count = 0;

    /* Clear result */
    g_free(self->result);
    self->result = NULL;

    /* Reset to defaults */
    self->state = GNOSTR_CHESS_STATE_SETUP;
    self->white_player = GNOSTR_CHESS_PLAYER_HUMAN;
    self->black_player = GNOSTR_CHESS_PLAYER_HUMAN;
    self->ai_depth = 4;
    self->draw_offered = FALSE;

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STATE]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_WHITE_PLAYER]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_BLACK_PLAYER]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_AI_DEPTH]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_RESULT]);

    g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, self->state);
}

GnostrChessPlayerType
gnostr_chess_session_get_white_player(GnostrChessSession *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_SESSION(self),
                         GNOSTR_CHESS_PLAYER_HUMAN);
    return self->white_player;
}

GnostrChessPlayerType
gnostr_chess_session_get_black_player(GnostrChessSession *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_SESSION(self),
                         GNOSTR_CHESS_PLAYER_HUMAN);
    return self->black_player;
}

gint
gnostr_chess_session_get_ai_depth(GnostrChessSession *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_SESSION(self), 4);
    return self->ai_depth;
}

/* ============================================================================
 * Private Implementation
 * ============================================================================ */

/**
 * Check for game-ending conditions (checkmate, stalemate)
 */
static void
check_game_over(GnostrChessSession *self)
{
    if (!self->engine)
        return;

    if (chess_engine_is_checkmate(self->engine)) {
        /* The side to move is checkmated */
        gboolean white_lost = (chess_engine_get_side_to_move(self->engine) == 0);
        const gchar *result = white_lost ? "0-1" : "1-0";
        const gchar *reason = white_lost ? "White is checkmated" :
                                           "Black is checkmated";

        self->result = g_strdup(result);
        self->state = GNOSTR_CHESS_STATE_FINISHED;

        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STATE]);
        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_RESULT]);

        g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, self->state);
        g_signal_emit(self, signals[SIGNAL_GAME_OVER], 0, result, reason);
    }
    else if (chess_engine_is_stalemate(self->engine)) {
        self->result = g_strdup("1/2-1/2");
        self->state = GNOSTR_CHESS_STATE_FINISHED;

        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STATE]);
        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_RESULT]);

        g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, self->state);
        g_signal_emit(self, signals[SIGNAL_GAME_OVER], 0,
                      "1/2-1/2", "Stalemate");
    }
}

/**
 * Convert a move to SAN notation (simplified)
 * Note: This is a basic implementation. A complete SAN generator
 * would need to handle disambiguation for pieces of the same type
 * that can move to the same square.
 */
static gchar *
move_to_san(GnostrChessSession *self, const gchar *from, const gchar *to,
            gchar promotion)
{
    if (!self->engine || !from || !to || strlen(from) < 2 || strlen(to) < 2)
        return g_strdup("???");

    char piece = chess_engine_get_piece_at(self->engine, from);
    char captured = chess_engine_get_piece_at(self->engine, to);
    int piece_type = 0;

    /* Determine piece type */
    switch (piece) {
    case 'P': case 'p': piece_type = 1; break;  /* Pawn */
    case 'N': case 'n': piece_type = 3; break;  /* Knight */
    case 'K': case 'k': piece_type = 4; break;  /* King */
    case 'B': case 'b': piece_type = 5; break;  /* Bishop */
    case 'R': case 'r': piece_type = 6; break;  /* Rook */
    case 'Q': case 'q': piece_type = 7; break;  /* Queen */
    default: return g_strdup("???");
    }

    GString *san = g_string_new(NULL);

    /* Handle castling */
    if (piece_type == 4) { /* King */
        int from_file = from[0] - 'a';
        int to_file = to[0] - 'a';
        if (to_file - from_file == 2) {
            g_string_assign(san, "O-O");
            return g_string_free(san, FALSE);
        }
        if (from_file - to_file == 2) {
            g_string_assign(san, "O-O-O");
            return g_string_free(san, FALSE);
        }
    }

    /* Piece letter (not for pawns) */
    if (piece_type != 1) {
        switch (piece_type) {
        case 3: g_string_append_c(san, 'N'); break;
        case 4: g_string_append_c(san, 'K'); break;
        case 5: g_string_append_c(san, 'B'); break;
        case 6: g_string_append_c(san, 'R'); break;
        case 7: g_string_append_c(san, 'Q'); break;
        }
    }

    /* For pawns capturing, include source file */
    if (piece_type == 1 && captured != '.') {
        g_string_append_c(san, from[0]);
    }

    /* Capture indicator */
    if (captured != '.') {
        g_string_append_c(san, 'x');
    }

    /* Destination square */
    g_string_append_c(san, to[0]);
    g_string_append_c(san, to[1]);

    /* Promotion */
    if (promotion) {
        g_string_append_c(san, '=');
        g_string_append_c(san, g_ascii_toupper(promotion));
    }

    /* Check for check/checkmate (we need to simulate the move) */
    /* For simplicity, we'll add this after the move is made */

    return g_string_free(san, FALSE);
}

/**
 * AI move data passed through GTask
 */
typedef struct {
    gchar *best_move;  /* e.g., "e2e4" */
    gint depth;
} AiMoveData;

static void
ai_move_data_free(AiMoveData *data)
{
    if (data) {
        g_free(data->best_move);
        g_free(data);
    }
}

/**
 * Thread function for AI move computation
 */
static void
compute_ai_move_thread(GTask *task,
                       gpointer source_object,
                       gpointer task_data,
                       GCancellable *cancellable)
{
    GnostrChessSession *self = GNOSTR_CHESS_SESSION(source_object);
    gint depth = GPOINTER_TO_INT(task_data);

    if (g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "AI computation cancelled");
        return;
    }

    if (!self->engine) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "No chess engine available");
        return;
    }

    /* Compute best move */
    gchar *best = chess_engine_get_best_move(self->engine, depth);

    if (!best) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "No legal moves available");
        return;
    }

    AiMoveData *result = g_new0(AiMoveData, 1);
    result->best_move = best;
    result->depth = depth;

    g_task_return_pointer(task, result, (GDestroyNotify)ai_move_data_free);
}

/**
 * Callback when AI move computation completes
 */
static void
on_ai_move_complete(GObject *source_object,
                    GAsyncResult *res,
                    gpointer user_data)
{
    GnostrChessSession *self = GNOSTR_CHESS_SESSION(source_object);
    GError *error = NULL;

    /* Clear thinking flag (but don't emit signal yet - wait until move is made) */
    self->ai_thinking = FALSE;

    AiMoveData *data = g_task_propagate_pointer(G_TASK(res), &error);

    if (error) {
        g_warning("[CHESS_SESSION] AI computation failed: %s", error->message);
        g_clear_error(&error);
        g_signal_emit(self, signals[SIGNAL_AI_THINKING], 0, FALSE);
        return;
    }

    if (!data || !data->best_move) {
        g_warning("[CHESS_SESSION] AI returned no move");
        ai_move_data_free(data);
        g_signal_emit(self, signals[SIGNAL_AI_THINKING], 0, FALSE);
        return;
    }

    /* Parse the move (format: "e2e4" or "e7e8q" for promotion) */
    gchar *move = data->best_move;
    if (strlen(move) < 4) {
        g_warning("[CHESS_SESSION] Invalid AI move format: %s", move);
        ai_move_data_free(data);
        g_signal_emit(self, signals[SIGNAL_AI_THINKING], 0, FALSE);
        return;
    }

    gchar from[3] = { move[0], move[1], '\0' };
    gchar to[3] = { move[2], move[3], '\0' };
    gchar promotion = (strlen(move) >= 5) ? move[4] : 0;

    /* Generate SAN before making the move */
    gchar *san = move_to_san(self, from, to, promotion);

    /* Make the move on the engine */
    if (!chess_engine_make_move(self->engine, from, to, promotion)) {
        g_warning("[CHESS_SESSION] AI move was illegal: %s", move);
        g_free(san);
        ai_move_data_free(data);
        g_signal_emit(self, signals[SIGNAL_AI_THINKING], 0, FALSE);
        return;
    }

    /* Record the move */
    self->move_count++;
    self->move_history = g_list_append(self->move_history, san);

    /* Now that move is made, emit AI thinking done signal
     * This must be after the move so is_human_turn() returns correct value */
    g_signal_emit(self, signals[SIGNAL_AI_THINKING], 0, FALSE);

    /* Emit move-made signal */
    gint move_number = (self->move_count + 1) / 2;
    g_signal_emit(self, signals[SIGNAL_MOVE_MADE], 0, san, move_number);

    ai_move_data_free(data);

    /* Check for game over */
    check_game_over(self);

    if (self->state == GNOSTR_CHESS_STATE_PLAYING) {
        /* Game continues - emit turn changed */
        gboolean is_white = (chess_engine_get_side_to_move(self->engine) == 0);
        g_signal_emit(self, signals[SIGNAL_TURN_CHANGED], 0, is_white);

        /* If the other player is also AI (AI vs AI), trigger next move */
        if (!gnostr_chess_session_is_human_turn(self)) {
            /* Add a small delay to prevent blocking the main loop */
            g_timeout_add(100, (GSourceFunc)gnostr_chess_session_request_ai_move,
                          self);
        }
    }
}
