/*
 * gnostr-tray-icon.h - System tray/menu bar icon support
 *
 * Cross-platform status icon functionality:
 *
 * Linux:
 *   - libayatana-appindicator3 (preferred, modern)
 *   - libappindicator3 (fallback, legacy)
 *
 * macOS:
 *   - NSStatusItem (native menu bar icon)
 *
 * The tray/menu bar icon provides:
 *   - Click: show dropdown menu
 *   - Menu: Show/Hide Window, New Note, Check DMs, Settings, Quit
 *   - Optional notification badge/count display
 */

#ifndef GNOSTR_TRAY_ICON_H
#define GNOSTR_TRAY_ICON_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_TRAY_ICON (gnostr_tray_icon_get_type())

G_DECLARE_FINAL_TYPE(GnostrTrayIcon, gnostr_tray_icon, GNOSTR, TRAY_ICON, GObject)

/**
 * gnostr_tray_icon_new:
 * @app: The GtkApplication instance
 *
 * Creates a new tray/menu bar icon instance. Returns NULL if
 * support is not available on this system.
 *
 * On Linux, this creates a system tray icon using AppIndicator.
 * On macOS, this creates a menu bar status item using NSStatusItem.
 *
 * Returns: (nullable) (transfer full): A new #GnostrTrayIcon or NULL
 */
GnostrTrayIcon *gnostr_tray_icon_new(GtkApplication *app);

/**
 * gnostr_tray_icon_set_window:
 * @self: The tray icon
 * @window: The main window to show/hide
 *
 * Associates the main window with the tray icon for show/hide functionality.
 */
void gnostr_tray_icon_set_window(GnostrTrayIcon *self, GtkWindow *window);

/**
 * gnostr_tray_icon_is_available:
 *
 * Checks if system tray/menu bar support is available at runtime.
 * This checks both compile-time and runtime availability.
 *
 * Returns: TRUE if tray/menu bar icon support is available
 */
gboolean gnostr_tray_icon_is_available(void);

/**
 * gnostr_tray_icon_set_unread_count:
 * @self: The tray icon
 * @count: Number of unread notifications/DMs
 *
 * Updates the notification badge/count displayed on the tray icon.
 * On macOS, this updates the menu item labels and tooltip.
 * On Linux with AppIndicator, this could update the icon or label.
 *
 * A count of 0 clears the badge.
 */
void gnostr_tray_icon_set_unread_count(GnostrTrayIcon *self, int count);

/**
 * GnostrTrayRelayState:
 * @GNOSTR_TRAY_RELAY_DISCONNECTED: All relays are disconnected
 * @GNOSTR_TRAY_RELAY_CONNECTING: Some relays are connecting
 * @GNOSTR_TRAY_RELAY_CONNECTED: At least one relay is connected
 *
 * Connection state for tray icon relay status indicator.
 */
typedef enum {
  GNOSTR_TRAY_RELAY_DISCONNECTED = 0,
  GNOSTR_TRAY_RELAY_CONNECTING   = 1,
  GNOSTR_TRAY_RELAY_CONNECTED    = 2,
} GnostrTrayRelayState;

/**
 * gnostr_tray_icon_set_relay_status:
 * @self: The tray icon
 * @connected_count: Number of currently connected relays
 * @total_count: Total number of configured relays
 * @state: Overall connection state
 *
 * Updates the relay connection status displayed in the tray menu.
 * Shows number of connected relays and overall connection state.
 */
void gnostr_tray_icon_set_relay_status(GnostrTrayIcon *self,
                                        int connected_count,
                                        int total_count,
                                        GnostrTrayRelayState state);

/**
 * gnostr_app_update_relay_status:
 * @connected_count: Number of currently connected relays
 * @total_count: Total number of configured relays
 *
 * Updates the global tray icon with relay connection status.
 * Called by main window when relay status changes.
 * Safe to call even if tray icon is not available.
 */
void gnostr_app_update_relay_status(int connected_count, int total_count);

G_END_DECLS

#endif /* GNOSTR_TRAY_ICON_H */
