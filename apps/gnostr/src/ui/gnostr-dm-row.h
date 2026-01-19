#ifndef GNOSTR_DM_ROW_H
#define GNOSTR_DM_ROW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_DM_ROW (gnostr_dm_row_get_type())

G_DECLARE_FINAL_TYPE(GnostrDmRow, gnostr_dm_row, GNOSTR, DM_ROW, GtkWidget)

/**
 * Signals:
 * "open-conversation" (gchar* peer_pubkey_hex, gpointer user_data)
 *   - Emitted when user clicks to open the conversation
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user clicks avatar to view profile
 */

typedef struct _GnostrDmRow GnostrDmRow;

GnostrDmRow *gnostr_dm_row_new(void);

/**
 * Set the peer (conversation partner) information
 * @self: the row widget
 * @pubkey_hex: peer's public key (64 hex chars)
 * @display_name: peer's display name (nullable)
 * @handle: peer's handle like @user or npub... (nullable)
 * @avatar_url: URL to avatar image (nullable)
 */
void gnostr_dm_row_set_peer(GnostrDmRow *self,
                            const char *pubkey_hex,
                            const char *display_name,
                            const char *handle,
                            const char *avatar_url);

/**
 * Set the last message preview
 * @self: the row widget
 * @preview: truncated preview of last message
 * @is_outgoing: TRUE if last message was sent by us
 */
void gnostr_dm_row_set_preview(GnostrDmRow *self,
                               const char *preview,
                               gboolean is_outgoing);

/**
 * Set the timestamp of last message
 * @self: the row widget
 * @created_at: unix timestamp
 * @fallback_ts: fallback string if timestamp formatting fails (nullable)
 */
void gnostr_dm_row_set_timestamp(GnostrDmRow *self,
                                 gint64 created_at,
                                 const char *fallback_ts);

/**
 * Set unread status
 * @self: the row widget
 * @unread_count: number of unread messages (0 = none)
 */
void gnostr_dm_row_set_unread(GnostrDmRow *self, guint unread_count);

/**
 * Get the peer's pubkey for this conversation
 */
const char *gnostr_dm_row_get_peer_pubkey(GnostrDmRow *self);

G_END_DECLS

#endif /* GNOSTR_DM_ROW_H */
