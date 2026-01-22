#ifndef GNOSTR_PROFILE_ROW_H
#define GNOSTR_PROFILE_ROW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_PROFILE_ROW (gnostr_profile_row_get_type())

G_DECLARE_FINAL_TYPE(GnostrProfileRow, gnostr_profile_row, GNOSTR, PROFILE_ROW, GtkWidget)

/**
 * Signals:
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user clicks on the profile row
 * "follow-requested" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user requests to follow this profile
 * "unfollow-requested" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user requests to unfollow this profile
 * "mute-requested" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user requests to mute this profile
 * "copy-npub-requested" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user requests to copy npub to clipboard
 */

/**
 * gnostr_profile_row_new:
 *
 * Creates a new profile row widget.
 *
 * Returns: (transfer full): A new #GnostrProfileRow
 */
GnostrProfileRow *gnostr_profile_row_new(void);

/**
 * gnostr_profile_row_set_profile:
 * @self: the profile row
 * @pubkey_hex: the profile's public key in hex
 * @display_name: display name (can be NULL)
 * @name: username/handle (can be NULL)
 * @nip05: NIP-05 identifier (can be NULL)
 * @bio: about/bio text (can be NULL)
 * @avatar_url: avatar URL (can be NULL)
 *
 * Set profile data for display.
 */
void gnostr_profile_row_set_profile(GnostrProfileRow *self,
                                    const char *pubkey_hex,
                                    const char *display_name,
                                    const char *name,
                                    const char *nip05,
                                    const char *bio,
                                    const char *avatar_url);

/**
 * gnostr_profile_row_set_following:
 * @self: the profile row
 * @is_following: whether the current user follows this profile
 *
 * Update the follow status indicator.
 */
void gnostr_profile_row_set_following(GnostrProfileRow *self, gboolean is_following);

/**
 * gnostr_profile_row_get_pubkey:
 * @self: the profile row
 *
 * Get the pubkey of the displayed profile.
 *
 * Returns: (transfer none): The pubkey hex string
 */
const char *gnostr_profile_row_get_pubkey(GnostrProfileRow *self);

/**
 * gnostr_profile_row_get_is_following:
 * @self: the profile row
 *
 * Get whether the current user is following this profile.
 *
 * Returns: TRUE if following
 */
gboolean gnostr_profile_row_get_is_following(GnostrProfileRow *self);

G_END_DECLS

#endif /* GNOSTR_PROFILE_ROW_H */
