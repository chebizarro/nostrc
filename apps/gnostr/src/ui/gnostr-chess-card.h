/*
 * gnostr-chess-card.h - NIP-64 Chess Game Card Widget
 *
 * GTK4 widget for displaying NIP-64 kind 64 chess game events.
 * Renders a chess board with the current position and provides
 * navigation controls to step through the game.
 *
 * Features:
 * - Visual chess board with piece rendering
 * - Move navigation (first, prev, next, last)
 * - Last move highlighting
 * - Game metadata display (players, result, event, date)
 * - Move list display
 * - Optional animated move playback
 * - Author info with NIP-05 verification
 */

#ifndef GNOSTR_CHESS_CARD_H
#define GNOSTR_CHESS_CARD_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_CHESS_CARD (gnostr_chess_card_get_type())

G_DECLARE_FINAL_TYPE(GnostrChessCard, gnostr_chess_card, GNOSTR, CHESS_CARD, GtkWidget)

/*
 * Signals:
 * "open-profile" (gchar* pubkey_hex, gpointer user_data) - view author profile
 * "open-game" (gchar* event_id_hex, gpointer user_data) - view game details
 * "share-game" (gchar* nostr_uri, gpointer user_data) - share game
 * "copy-pgn" (gchar* pgn_text, gpointer user_data) - PGN copied to clipboard
 * "zap-requested" (gchar* event_id, gchar* pubkey_hex, gchar* lud16, gpointer user_data)
 * "bookmark-toggled" (gchar* event_id, gboolean is_bookmarked, gpointer user_data)
 */

typedef struct _GnostrChessCard GnostrChessCard;

/**
 * gnostr_chess_card_new:
 *
 * Creates a new chess card widget.
 *
 * Returns: (transfer full): A new GnostrChessCard.
 */
GnostrChessCard *gnostr_chess_card_new(void);

/**
 * gnostr_chess_card_set_pgn:
 * @self: The chess card.
 * @pgn_text: PGN text of the chess game.
 *
 * Sets the chess game by parsing PGN text.
 * This will update the board display and metadata.
 *
 * Returns: TRUE if PGN was parsed successfully.
 */
gboolean gnostr_chess_card_set_pgn(GnostrChessCard *self, const char *pgn_text);

/**
 * gnostr_chess_card_set_event:
 * @self: The chess card.
 * @event_id: Event ID (hex).
 * @pubkey: Author pubkey (hex).
 * @created_at: Event creation timestamp.
 *
 * Sets the Nostr event metadata.
 */
void gnostr_chess_card_set_event(GnostrChessCard *self,
                                  const char *event_id,
                                  const char *pubkey,
                                  gint64 created_at);

/**
 * gnostr_chess_card_set_author:
 * @self: The chess card.
 * @display_name: Author display name.
 * @handle: Author handle/username.
 * @avatar_url: Avatar image URL (may be NULL).
 * @pubkey_hex: Author's public key (hex).
 *
 * Sets the author information for the game.
 */
void gnostr_chess_card_set_author(GnostrChessCard *self,
                                   const char *display_name,
                                   const char *handle,
                                   const char *avatar_url,
                                   const char *pubkey_hex);

/**
 * gnostr_chess_card_set_nip05:
 * @self: The chess card.
 * @nip05: NIP-05 identifier.
 * @pubkey_hex: Expected public key.
 *
 * Initiates NIP-05 verification for the author.
 */
void gnostr_chess_card_set_nip05(GnostrChessCard *self,
                                  const char *nip05,
                                  const char *pubkey_hex);

/**
 * gnostr_chess_card_set_author_lud16:
 * @self: The chess card.
 * @lud16: Lightning address.
 *
 * Sets author's lightning address for zapping.
 */
void gnostr_chess_card_set_author_lud16(GnostrChessCard *self,
                                         const char *lud16);

/**
 * gnostr_chess_card_set_bookmarked:
 * @self: The chess card.
 * @is_bookmarked: Whether game is bookmarked.
 *
 * Updates the bookmark button state.
 */
void gnostr_chess_card_set_bookmarked(GnostrChessCard *self,
                                       gboolean is_bookmarked);

/**
 * gnostr_chess_card_set_logged_in:
 * @self: The chess card.
 * @logged_in: Whether user is logged in.
 *
 * Updates button sensitivity based on login state.
 */
void gnostr_chess_card_set_logged_in(GnostrChessCard *self,
                                      gboolean logged_in);

/**
 * gnostr_chess_card_go_first:
 * @self: The chess card.
 *
 * Navigates to the starting position.
 */
void gnostr_chess_card_go_first(GnostrChessCard *self);

/**
 * gnostr_chess_card_go_prev:
 * @self: The chess card.
 *
 * Navigates to the previous move.
 */
void gnostr_chess_card_go_prev(GnostrChessCard *self);

/**
 * gnostr_chess_card_go_next:
 * @self: The chess card.
 *
 * Navigates to the next move.
 */
void gnostr_chess_card_go_next(GnostrChessCard *self);

/**
 * gnostr_chess_card_go_last:
 * @self: The chess card.
 *
 * Navigates to the final position.
 */
void gnostr_chess_card_go_last(GnostrChessCard *self);

/**
 * gnostr_chess_card_start_autoplay:
 * @self: The chess card.
 * @interval_ms: Interval between moves in milliseconds.
 *
 * Starts automatic move playback.
 */
void gnostr_chess_card_start_autoplay(GnostrChessCard *self, guint interval_ms);

/**
 * gnostr_chess_card_stop_autoplay:
 * @self: The chess card.
 *
 * Stops automatic move playback.
 */
void gnostr_chess_card_stop_autoplay(GnostrChessCard *self);

/**
 * gnostr_chess_card_is_playing:
 * @self: The chess card.
 *
 * Returns: TRUE if autoplay is running.
 */
gboolean gnostr_chess_card_is_playing(GnostrChessCard *self);

/**
 * gnostr_chess_card_get_event_id:
 * @self: The chess card.
 *
 * Returns: (transfer none): Event ID string or NULL.
 */
const char *gnostr_chess_card_get_event_id(GnostrChessCard *self);

/**
 * gnostr_chess_card_get_pgn:
 * @self: The chess card.
 *
 * Returns: (transfer full) (nullable): PGN text or NULL.
 */
gchar *gnostr_chess_card_get_pgn(GnostrChessCard *self);

/**
 * gnostr_chess_card_set_board_size:
 * @self: The chess card.
 * @size: Size in pixels for the board (square).
 *
 * Sets the chess board display size.
 */
void gnostr_chess_card_set_board_size(GnostrChessCard *self, gint size);

/**
 * gnostr_chess_card_set_flipped:
 * @self: The chess card.
 * @flipped: Whether to show from Black's perspective.
 *
 * Sets the board orientation.
 */
void gnostr_chess_card_set_flipped(GnostrChessCard *self, gboolean flipped);

/**
 * gnostr_chess_card_is_flipped:
 * @self: The chess card.
 *
 * Returns: TRUE if board is shown from Black's perspective.
 */
gboolean gnostr_chess_card_is_flipped(GnostrChessCard *self);

G_END_DECLS

#endif /* GNOSTR_CHESS_CARD_H */
