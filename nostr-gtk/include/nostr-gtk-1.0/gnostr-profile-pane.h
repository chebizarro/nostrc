#ifndef NOSTR_GTK_PROFILE_PANE_H
#define NOSTR_GTK_PROFILE_PANE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NOSTR_GTK_TYPE_PROFILE_PANE (nostr_gtk_profile_pane_get_type())
G_DECLARE_FINAL_TYPE(NostrGtkProfilePane, nostr_gtk_profile_pane, NOSTR_GTK, PROFILE_PANE, GtkWidget)

NostrGtkProfilePane *nostr_gtk_profile_pane_new(void);
void nostr_gtk_profile_pane_set_pubkey(NostrGtkProfilePane *self, const char *pubkey_hex);
void nostr_gtk_profile_pane_clear(NostrGtkProfilePane *self);
void nostr_gtk_profile_pane_update_from_json(NostrGtkProfilePane *self, const char *profile_json_str);

/**
 * nostr_gtk_profile_pane_get_current_pubkey:
 * @self: the profile pane
 *
 * Get the pubkey of the currently displayed profile.
 *
 * Returns: (transfer none): the current pubkey hex string, or NULL
 */
const char* nostr_gtk_profile_pane_get_current_pubkey(NostrGtkProfilePane *self);

/**
 * nostr_gtk_profile_pane_set_own_pubkey:
 * @self: the profile pane
 * @own_pubkey_hex: the current user's pubkey (64-char hex)
 *
 * Set the current user's pubkey to enable showing the Edit button
 * when viewing their own profile.
 */
void nostr_gtk_profile_pane_set_own_pubkey(NostrGtkProfilePane *self, const char *own_pubkey_hex);

/**
 * nostr_gtk_profile_pane_get_profile_json:
 * @self: the profile pane
 *
 * Get the current profile's raw JSON content.
 *
 * Returns: (transfer none): the profile JSON string, or NULL
 */
const char* nostr_gtk_profile_pane_get_profile_json(NostrGtkProfilePane *self);

/**
 * nostr_gtk_profile_pane_refresh:
 * @self: the profile pane
 *
 * Refresh the current profile by fetching from cache and network.
 * The profile pane automatically fetches on set_pubkey, but this
 * can be used to manually trigger a refresh.
 */
void nostr_gtk_profile_pane_refresh(NostrGtkProfilePane *self);

/**
 * nostr_gtk_profile_pane_is_profile_cached:
 * @self: the profile pane
 *
 * Check if the current profile was loaded from cache.
 *
 * Returns: TRUE if the profile was loaded from nostrdb cache, FALSE otherwise
 */
gboolean nostr_gtk_profile_pane_is_profile_cached(NostrGtkProfilePane *self);

/**
 * nostr_gtk_profile_pane_set_following:
 * @self: the profile pane
 * @is_following: TRUE if the user is following this profile
 *
 * Updates the Follow button label and style to reflect follow state.
 */
void nostr_gtk_profile_pane_set_following(NostrGtkProfilePane *self, gboolean is_following);

/**
 * Signals:
 * - "close-requested": Emitted when the close button is clicked
 * - "note-activated": Emitted when a post is clicked (param: note_id string)
 * - "mute-user-requested": Emitted when the mute button is clicked (param: pubkey_hex string)
 */

G_END_DECLS

#endif /* NOSTR_GTK_PROFILE_PANE_H */
