/**
 * GnostrChessPublishDialog - Publish Chess Game to Nostr Dialog
 *
 * A dialog for publishing completed chess games to Nostr as NIP-64 events.
 * Displays game summary and handles event signing and publishing.
 */

#ifndef GNOSTR_CHESS_PUBLISH_DIALOG_H
#define GNOSTR_CHESS_PUBLISH_DIALOG_H

#include <gtk/gtk.h>
#include <adwaita.h>
#include "gnostr-chess-session.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_CHESS_PUBLISH_DIALOG (gnostr_chess_publish_dialog_get_type())

G_DECLARE_FINAL_TYPE(GnostrChessPublishDialog, gnostr_chess_publish_dialog, GNOSTR, CHESS_PUBLISH_DIALOG, AdwDialog)

/**
 * gnostr_chess_publish_dialog_new:
 *
 * Creates a new chess publish dialog.
 *
 * Returns: (transfer full): A new #GnostrChessPublishDialog
 */
GnostrChessPublishDialog *gnostr_chess_publish_dialog_new(void);

/**
 * gnostr_chess_publish_dialog_present:
 * @self: The dialog
 * @parent: (nullable): Parent widget
 *
 * Presents the dialog to the user.
 */
void gnostr_chess_publish_dialog_present(GnostrChessPublishDialog *self, GtkWidget *parent);

/**
 * gnostr_chess_publish_dialog_set_session:
 * @self: The dialog
 * @session: The chess session containing the game to publish
 *
 * Sets the chess session whose game will be published.
 * The session is not owned by the dialog.
 */
void gnostr_chess_publish_dialog_set_session(GnostrChessPublishDialog *self,
                                              GnostrChessSession *session);

/**
 * gnostr_chess_publish_dialog_set_result_info:
 * @self: The dialog
 * @result: Result string (e.g., "1-0", "0-1", "1/2-1/2")
 * @reason: Reason for the result (e.g., "White wins by checkmate")
 * @white_name: Display name for white player
 * @black_name: Display name for black player
 *
 * Sets the game result information to display in the dialog.
 */
void gnostr_chess_publish_dialog_set_result_info(GnostrChessPublishDialog *self,
                                                  const gchar *result,
                                                  const gchar *reason,
                                                  const gchar *white_name,
                                                  const gchar *black_name);

/**
 * Signals:
 * - "published": Emitted when game is successfully published
 *   Callback signature: void callback(GnostrChessPublishDialog *dialog,
 *                                      const gchar *event_id,
 *                                      gpointer user_data)
 *
 * - "publish-failed": Emitted when publishing fails
 *   Callback signature: void callback(GnostrChessPublishDialog *dialog,
 *                                      const gchar *error_message,
 *                                      gpointer user_data)
 */

G_END_DECLS

#endif /* GNOSTR_CHESS_PUBLISH_DIALOG_H */
