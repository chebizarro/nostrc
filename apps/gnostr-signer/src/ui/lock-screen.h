/* lock-screen.h - Lock screen widget for gnostr-signer
 *
 * Provides a password entry UI for unlocking the session.
 * Integrates with GnSessionManager for authentication.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APPS_GNOSTR_SIGNER_UI_LOCK_SCREEN_H
#define APPS_GNOSTR_SIGNER_UI_LOCK_SCREEN_H

#include <adwaita.h>
#include <gtk/gtk.h>
#include "../session-manager.h"

G_BEGIN_DECLS

#define GN_TYPE_LOCK_SCREEN (gn_lock_screen_get_type())

G_DECLARE_FINAL_TYPE(GnLockScreen, gn_lock_screen, GN, LOCK_SCREEN, GtkBox)

/* Note: GnLockReason is defined in session-manager.h */

/**
 * gn_lock_screen_new:
 *
 * Creates a new lock screen widget.
 *
 * Returns: (transfer full): A new #GnLockScreen
 */
GnLockScreen *gn_lock_screen_new(void);

/**
 * gn_lock_screen_set_lock_reason:
 * @self: A #GnLockScreen
 * @reason: The reason for locking
 *
 * Sets the displayed lock reason.
 */
void gn_lock_screen_set_lock_reason(GnLockScreen *self, GnLockReason reason);

/**
 * gn_lock_screen_set_locked_at:
 * @self: A #GnLockScreen
 * @timestamp: Unix timestamp when locked
 *
 * Sets the displayed lock timestamp.
 */
void gn_lock_screen_set_locked_at(GnLockScreen *self, gint64 timestamp);

/**
 * gn_lock_screen_clear_error:
 * @self: A #GnLockScreen
 *
 * Clears any displayed error message.
 */
void gn_lock_screen_clear_error(GnLockScreen *self);

/**
 * gn_lock_screen_show_error:
 * @self: A #GnLockScreen
 * @message: Error message to display
 *
 * Shows an error message below the password entry.
 */
void gn_lock_screen_show_error(GnLockScreen *self, const gchar *message);

/**
 * gn_lock_screen_focus_password:
 * @self: A #GnLockScreen
 *
 * Sets focus to the password entry field.
 */
void gn_lock_screen_focus_password(GnLockScreen *self);

/**
 * gn_lock_screen_clear_password:
 * @self: A #GnLockScreen
 *
 * Clears the password entry field.
 */
void gn_lock_screen_clear_password(GnLockScreen *self);

/**
 * gn_lock_screen_set_busy:
 * @self: A #GnLockScreen
 * @busy: Whether authentication is in progress
 *
 * Sets the busy state, disabling input during authentication.
 */
void gn_lock_screen_set_busy(GnLockScreen *self, gboolean busy);

/**
 * gn_lock_screen_get_password_configured:
 * @self: A #GnLockScreen
 *
 * Checks if a password has been configured.
 *
 * Returns: %TRUE if a password is required
 */
gboolean gn_lock_screen_get_password_configured(GnLockScreen *self);

/**
 * gn_lock_screen_is_rate_limited:
 * @self: A #GnLockScreen
 *
 * Checks if the lock screen is currently rate limited due to
 * too many failed authentication attempts (nostrc-1g1).
 *
 * Returns: %TRUE if rate limited
 */
gboolean gn_lock_screen_is_rate_limited(GnLockScreen *self);

/**
 * gn_lock_screen_get_rate_limit_remaining:
 * @self: A #GnLockScreen
 *
 * Gets the number of seconds remaining in the rate limit lockout.
 *
 * Returns: Seconds until retry allowed, or 0 if not rate limited
 */
guint gn_lock_screen_get_rate_limit_remaining(GnLockScreen *self);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_LOCK_SCREEN_H */
