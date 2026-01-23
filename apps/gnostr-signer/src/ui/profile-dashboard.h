/* profile-dashboard.h - Profile Dashboard for gnostr-signer
 *
 * Main screen displayed after a profile is loaded, showing:
 * - Profile header (avatar, display name, truncated npub)
 * - Action button grid for common operations
 */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_PROFILE_DASHBOARD (profile_dashboard_get_type())
G_DECLARE_FINAL_TYPE(ProfileDashboard, profile_dashboard, PROFILE, DASHBOARD, AdwBin)

/* Create a new profile dashboard widget */
ProfileDashboard *profile_dashboard_new(void);

/* Set the profile to display (npub in bech32 format) */
void profile_dashboard_set_npub(ProfileDashboard *self, const gchar *npub);

/* Get the currently displayed npub */
const gchar *profile_dashboard_get_npub(ProfileDashboard *self);

/* Refresh profile data from cache/store */
void profile_dashboard_refresh(ProfileDashboard *self);

/* Signal emitted when user clicks an action button
 * Signature: void handler(ProfileDashboard *dashboard, const gchar *action_name, gpointer user_data)
 * action_name values: "view-events", "manage-relays", "backup-keys", "change-password", "sign-message"
 */

G_END_DECLS
