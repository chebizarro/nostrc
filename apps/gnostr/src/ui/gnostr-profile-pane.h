#ifndef GNOSTR_PROFILE_PANE_H
#define GNOSTR_PROFILE_PANE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_PROFILE_PANE (gnostr_profile_pane_get_type())
G_DECLARE_FINAL_TYPE(GnostrProfilePane, gnostr_profile_pane, GNOSTR, PROFILE_PANE, GtkWidget)

GnostrProfilePane *gnostr_profile_pane_new(void);
void gnostr_profile_pane_set_pubkey(GnostrProfilePane *self, const char *pubkey_hex);
void gnostr_profile_pane_clear(GnostrProfilePane *self);
void gnostr_profile_pane_update_from_json(GnostrProfilePane *self, const char *profile_json_str);

/**
 * gnostr_profile_pane_get_current_pubkey:
 * @self: the profile pane
 *
 * Get the pubkey of the currently displayed profile.
 *
 * Returns: (transfer none): the current pubkey hex string, or NULL
 */
const char* gnostr_profile_pane_get_current_pubkey(GnostrProfilePane *self);

/**
 * gnostr_profile_pane_set_own_pubkey:
 * @self: the profile pane
 * @own_pubkey_hex: the current user's pubkey (64-char hex)
 *
 * Set the current user's pubkey to enable showing the Edit button
 * when viewing their own profile.
 */
void gnostr_profile_pane_set_own_pubkey(GnostrProfilePane *self, const char *own_pubkey_hex);

/**
 * gnostr_profile_pane_get_profile_json:
 * @self: the profile pane
 *
 * Get the current profile's raw JSON content.
 *
 * Returns: (transfer none): the profile JSON string, or NULL
 */
const char* gnostr_profile_pane_get_profile_json(GnostrProfilePane *self);

G_END_DECLS

#endif /* GNOSTR_PROFILE_PANE_H */
