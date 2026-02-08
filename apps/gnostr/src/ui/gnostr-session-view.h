#ifndef GNOSTR_SESSION_VIEW_H
#define GNOSTR_SESSION_VIEW_H

#include <adwaita.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_SESSION_VIEW (gnostr_session_view_get_type())

G_DECLARE_FINAL_TYPE(GnostrSessionView, gnostr_session_view, GNOSTR, SESSION_VIEW, AdwBin)

/* Constructor */
GnostrSessionView *gnostr_session_view_new(void);

/* Responsive mode (drives template bindings) */
gboolean gnostr_session_view_get_compact(GnostrSessionView *self);
void gnostr_session_view_set_compact(GnostrSessionView *self, gboolean compact);

/* Guest-mode gating for Notifications / Messages */
gboolean gnostr_session_view_get_authenticated(GnostrSessionView *self);
void gnostr_session_view_set_authenticated(GnostrSessionView *self, gboolean authenticated);

/* Navigation */
void gnostr_session_view_show_page(GnostrSessionView *self, const char *page_name);

/* Side panel (profile/thread) controls */
void gnostr_session_view_show_profile_panel(GnostrSessionView *self);
void gnostr_session_view_show_thread_panel(GnostrSessionView *self);
void gnostr_session_view_hide_side_panel(GnostrSessionView *self);
gboolean gnostr_session_view_is_side_panel_visible(GnostrSessionView *self);

/* Toast forwarding (main window owns the overlay; session view can be given a weak reference) */
void gnostr_session_view_set_toast_overlay(GnostrSessionView *self, AdwToastOverlay *overlay);
void gnostr_session_view_show_toast(GnostrSessionView *self, const char *message);

/* Access to child widgets for main window to wire up models/signals */
GtkWidget *gnostr_session_view_get_timeline(GnostrSessionView *self);
GtkWidget *gnostr_session_view_get_notifications_view(GnostrSessionView *self);
GtkWidget *gnostr_session_view_get_dm_inbox(GnostrSessionView *self);
GtkStack  *gnostr_session_view_get_dm_stack(GnostrSessionView *self);
GtkWidget *gnostr_session_view_get_dm_conversation(GnostrSessionView *self);
GtkWidget *gnostr_session_view_get_discover_page(GnostrSessionView *self);
GtkWidget *gnostr_session_view_get_search_results_view(GnostrSessionView *self);
GtkWidget *gnostr_session_view_get_classifieds_view(GnostrSessionView *self);
GtkWidget *gnostr_session_view_get_repo_browser(GnostrSessionView *self);
GtkWidget *gnostr_session_view_get_profile_pane(GnostrSessionView *self);
GtkWidget *gnostr_session_view_get_thread_view(GnostrSessionView *self);

/* Panel state queries */
gboolean gnostr_session_view_is_showing_profile(GnostrSessionView *self);

/* New notes indicator */
void gnostr_session_view_set_new_notes_count(GnostrSessionView *self, guint count);

/* Relay connection status indicator */
void gnostr_session_view_set_relay_status(GnostrSessionView *self,
                                          guint connected_count,
                                          guint total_count);

/* Search bar control */
void gnostr_session_view_set_search_mode(GnostrSessionView *self, gboolean enabled);
gboolean gnostr_session_view_get_search_mode(GnostrSessionView *self);
const char *gnostr_session_view_get_search_text(GnostrSessionView *self);

/**
 * Signals:
 * - "page-selected" (const char *page_name): Emitted when sidebar navigation changes
 * - "settings-requested": Emitted when settings button clicked
 * - "relays-requested": Emitted when manage relays clicked
 * - "reconnect-requested": Emitted when reconnect clicked
 * - "login-requested": Emitted when sign in requested
 * - "logout-requested": Emitted when sign out clicked
 * - "account-switch-requested" (const char *npub): Emitted when user wants to switch accounts
 * - "new-notes-clicked": Emitted when new notes toast clicked
 * - "compose-requested": Emitted when compose button clicked
 * - "search-changed" (const char *text): Emitted when search text changes
 */

/**
 * gnostr_session_view_refresh_account_list:
 * @self: a #GnostrSessionView
 *
 * Refreshes the account list in the avatar popover.
 * Call this after adding or removing accounts.
 */
void gnostr_session_view_refresh_account_list(GnostrSessionView *self);

/**
 * gnostr_session_view_set_user_profile:
 * @self: a #GnostrSessionView
 * @pubkey_hex: The user's public key in hex format (64 chars)
 * @display_name: (nullable): The user's display name
 * @avatar_url: (nullable): URL to the user's avatar image
 *
 * Updates the account menu with the current user's profile information.
 * This shows the user's avatar and display name in the popover header.
 * Call this when the user logs in or when their profile is updated.
 */
void gnostr_session_view_set_user_profile(GnostrSessionView *self,
                                          const char *pubkey_hex,
                                          const char *display_name,
                                          const char *avatar_url);

/**
 * gnostr_session_view_add_plugin_sidebar_item:
 * @self: a #GnostrSessionView
 * @panel_id: Unique identifier for the plugin panel
 * @label: Display label for the sidebar item
 * @icon_name: Icon name for the sidebar item
 * @requires_auth: Whether the item requires authentication
 * @position: Position in sidebar (0 = after fixed items, -1 = end)
 * @extension: (nullable): GnostrUIExtension instance for creating panel widget
 * @context: (nullable): GnostrPluginContext for the extension
 *
 * Adds a plugin-provided sidebar item and associated panel to the session view.
 * When the item is clicked, the extension's create_panel_widget will be called
 * to lazily create the panel content.
 */
void gnostr_session_view_add_plugin_sidebar_item(GnostrSessionView *self,
                                                  const char *panel_id,
                                                  const char *label,
                                                  const char *icon_name,
                                                  gboolean requires_auth,
                                                  int position,
                                                  gpointer extension,
                                                  gpointer context);

/**
 * gnostr_session_view_remove_plugin_sidebar_item:
 * @self: a #GnostrSessionView
 * @panel_id: Unique identifier for the plugin panel to remove
 *
 * Removes a plugin-provided sidebar item and its associated panel.
 */
void gnostr_session_view_remove_plugin_sidebar_item(GnostrSessionView *self,
                                                     const char *panel_id);

G_END_DECLS

#endif /* GNOSTR_SESSION_VIEW_H */