/**
 * Chess Engine Test
 *
 * Simple test to verify the Micro-Max chess engine port works correctly.
 */

#include <stdio.h>
#include <string.h>
#include <glib.h>
#include "../src/util/chess_engine.h"

static void test_basic_creation(void)
{
    printf("=== Test: Basic Creation ===\n");

    ChessEngine *e = chess_engine_new();
    g_assert_nonnull(e);

    /* Check starting position */
    g_assert_cmpint(chess_engine_get_side_to_move(e), ==, 0);  /* White */
    g_assert_false(chess_engine_is_check(e));
    g_assert_false(chess_engine_is_checkmate(e));
    g_assert_false(chess_engine_is_stalemate(e));

    /* Check some pieces */
    g_assert_cmpint(chess_engine_get_piece_at(e, "e1"), ==, 'K');
    g_assert_cmpint(chess_engine_get_piece_at(e, "d8"), ==, 'q');
    g_assert_cmpint(chess_engine_get_piece_at(e, "e2"), ==, 'P');
    g_assert_cmpint(chess_engine_get_piece_at(e, "e4"), ==, '.');

    chess_engine_free(e);
    printf("PASSED\n\n");
}

static void test_move_execution(void)
{
    printf("=== Test: Move Execution ===\n");

    ChessEngine *e = chess_engine_new();

    /* Make e2e4 */
    printf("Making move e2-e4...\n");
    g_assert_true(chess_engine_make_move(e, "e2", "e4", 0));
    g_assert_cmpint(chess_engine_get_piece_at(e, "e2"), ==, '.');
    g_assert_cmpint(chess_engine_get_piece_at(e, "e4"), ==, 'P');
    g_assert_cmpint(chess_engine_get_side_to_move(e), ==, 1);  /* Black */

    /* Make e7e5 */
    printf("Making move e7-e5...\n");
    g_assert_true(chess_engine_make_move(e, "e7", "e5", 0));
    g_assert_cmpint(chess_engine_get_piece_at(e, "e7"), ==, '.');
    g_assert_cmpint(chess_engine_get_piece_at(e, "e5"), ==, 'p');
    g_assert_cmpint(chess_engine_get_side_to_move(e), ==, 0);  /* White */

    /* Make Nf3 */
    printf("Making move g1-f3 (Nf3)...\n");
    g_assert_true(chess_engine_make_move(e, "g1", "f3", 0));
    g_assert_cmpint(chess_engine_get_piece_at(e, "g1"), ==, '.');
    g_assert_cmpint(chess_engine_get_piece_at(e, "f3"), ==, 'N');

    chess_engine_print_board(e);
    chess_engine_free(e);
    printf("PASSED\n\n");
}

static void test_move_san(void)
{
    printf("=== Test: SAN Move Parsing ===\n");

    ChessEngine *e = chess_engine_new();

    /* Test various SAN moves */
    printf("Making move e4...\n");
    g_assert_true(chess_engine_make_move_san(e, "e4"));
    g_assert_cmpint(chess_engine_get_piece_at(e, "e4"), ==, 'P');

    printf("Making move e5...\n");
    g_assert_true(chess_engine_make_move_san(e, "e5"));
    g_assert_cmpint(chess_engine_get_piece_at(e, "e5"), ==, 'p');

    printf("Making move Nf3...\n");
    g_assert_true(chess_engine_make_move_san(e, "Nf3"));
    g_assert_cmpint(chess_engine_get_piece_at(e, "f3"), ==, 'N');

    printf("Making move Nc6...\n");
    g_assert_true(chess_engine_make_move_san(e, "Nc6"));
    g_assert_cmpint(chess_engine_get_piece_at(e, "c6"), ==, 'n');

    printf("Making move Bb5...\n");
    g_assert_true(chess_engine_make_move_san(e, "Bb5"));
    g_assert_cmpint(chess_engine_get_piece_at(e, "b5"), ==, 'B');

    chess_engine_print_board(e);
    chess_engine_free(e);
    printf("PASSED\n\n");
}

static void test_legal_moves(void)
{
    printf("=== Test: Legal Move Generation ===\n");

    ChessEngine *e = chess_engine_new();

    /* Get legal moves for e2 pawn */
    GList *moves = chess_engine_get_legal_moves(e, "e2");
    g_assert_nonnull(moves);

    printf("Legal moves from e2: ");
    int count = 0;
    for (GList *l = moves; l; l = l->next) {
        printf("%s ", (char *)l->data);
        count++;
    }
    printf("(count=%d)\n", count);

    /* e2 pawn should have 2 moves: e3 and e4 */
    g_assert_cmpint(count, ==, 2);

    g_list_free_full(moves, g_free);

    /* Get legal moves for knight on g1 */
    moves = chess_engine_get_legal_moves(e, "g1");
    g_assert_nonnull(moves);

    printf("Legal moves from g1: ");
    count = 0;
    for (GList *l = moves; l; l = l->next) {
        printf("%s ", (char *)l->data);
        count++;
    }
    printf("(count=%d)\n", count);

    /* g1 knight should have 2 moves: f3 and h3 */
    g_assert_cmpint(count, ==, 2);

    g_list_free_full(moves, g_free);
    chess_engine_free(e);
    printf("PASSED\n\n");
}

static void test_best_move(void)
{
    printf("=== Test: Best Move Calculation ===\n");

    ChessEngine *e = chess_engine_new();

    /* Make opening moves */
    chess_engine_make_move(e, "e2", "e4", 0);
    chess_engine_make_move(e, "e7", "e5", 0);
    chess_engine_make_move(e, "g1", "f3", 0);

    chess_engine_print_board(e);

    /* Get best move at depth 4 */
    printf("Calculating best move at depth 4...\n");
    char *best = chess_engine_get_best_move(e, 4);

    if (best) {
        printf("Best move: %s\n", best);
        g_free(best);
    } else {
        printf("No best move found (this is unexpected!)\n");
    }

    g_assert_nonnull(best);

    chess_engine_free(e);
    printf("PASSED\n\n");
}

static void test_fen(void)
{
    printf("=== Test: FEN Import/Export ===\n");

    ChessEngine *e = chess_engine_new();

    /* Get starting position FEN */
    char *fen = chess_engine_get_fen(e);
    printf("Starting FEN: %s\n", fen);
    g_assert_nonnull(fen);
    g_free(fen);

    /* Make some moves */
    chess_engine_make_move(e, "e2", "e4", 0);
    chess_engine_make_move(e, "e7", "e5", 0);

    fen = chess_engine_get_fen(e);
    printf("After 1.e4 e5 FEN: %s\n", fen);
    g_free(fen);

    /* Test setting a FEN position */
    const char *test_fen = "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3";
    printf("Setting FEN: %s\n", test_fen);
    g_assert_true(chess_engine_set_fen(e, test_fen));

    chess_engine_print_board(e);

    /* Verify position */
    g_assert_cmpint(chess_engine_get_piece_at(e, "f3"), ==, 'N');
    g_assert_cmpint(chess_engine_get_piece_at(e, "c6"), ==, 'n');
    g_assert_cmpint(chess_engine_get_side_to_move(e), ==, 0);  /* White */

    chess_engine_free(e);
    printf("PASSED\n\n");
}

static void test_check_detection(void)
{
    printf("=== Test: Check Detection ===\n");

    ChessEngine *e = chess_engine_new();

    /* Scholar's mate position */
    const char *check_fen = "r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4";
    g_assert_true(chess_engine_set_fen(e, check_fen));

    chess_engine_print_board(e);

    /* White to move, can deliver checkmate with Qxf7# */
    printf("Making Qxf7# (checkmate)...\n");
    g_assert_true(chess_engine_make_move(e, "h5", "f7", 0));

    chess_engine_print_board(e);

    g_assert_true(chess_engine_is_check(e));
    g_assert_true(chess_engine_is_checkmate(e));
    g_assert_false(chess_engine_is_stalemate(e));

    printf("Checkmate confirmed!\n");

    chess_engine_free(e);
    printf("PASSED\n\n");
}

static void test_castling(void)
{
    printf("=== Test: Castling ===\n");

    ChessEngine *e = chess_engine_new();

    /* Position ready for kingside castling */
    const char *castle_fen = "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4";
    g_assert_true(chess_engine_set_fen(e, castle_fen));

    chess_engine_print_board(e);

    /* Castle kingside */
    printf("Castling kingside (O-O)...\n");
    g_assert_true(chess_engine_make_move_san(e, "O-O"));

    chess_engine_print_board(e);

    g_assert_cmpint(chess_engine_get_piece_at(e, "g1"), ==, 'K');
    g_assert_cmpint(chess_engine_get_piece_at(e, "f1"), ==, 'R');
    g_assert_cmpint(chess_engine_get_piece_at(e, "e1"), ==, '.');
    g_assert_cmpint(chess_engine_get_piece_at(e, "h1"), ==, '.');

    printf("Castling verified!\n");

    chess_engine_free(e);
    printf("PASSED\n\n");
}

int main(int argc, char *argv[])
{
    printf("GNostr Chess Engine Test Suite\n");
    printf("==============================\n\n");

    test_basic_creation();
    test_move_execution();
    test_move_san();
    test_legal_moves();
    test_best_move();
    test_fen();
    test_check_detection();
    test_castling();

    printf("==============================\n");
    printf("All tests PASSED!\n");

    return 0;
}
