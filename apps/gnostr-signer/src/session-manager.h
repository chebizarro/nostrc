/* session-manager.h - Session management for gnostr-signer
 *
 * Manages authenticated sessions with:
 * - Password-based authentication
 * - Auto-lock after configurable inactivity timeout
 * - Session extension on activity
 * - Lock/unlock signals for UI integration
 *
 * The session manager tracks when the user has authenticated and
 * automatically locks the session after a period of inactivity.
 * This ensures private key operations are protected when the user
 * is away from their device.
 */
#pragma once

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GN_TYPE_SESSION_MANAGER (gn_session_manager_get_type())

G_DECLARE_FINAL_TYPE(GnSessionManager, gn_session_manager, GN, SESSION_MANAGER, GObject)

/**
 * GnSessionState:
 * @GN_SESSION_STATE_LOCKED: Session is locked, authentication required
 * @GN_SESSION_STATE_AUTHENTICATED: Session is active and authenticated
 * @GN_SESSION_STATE_EXPIRED: Session expired due to timeout
 *
 * The current state of the session.
 */
typedef enum {
  GN_SESSION_STATE_LOCKED,
  GN_SESSION_STATE_AUTHENTICATED,
  GN_SESSION_STATE_EXPIRED
} GnSessionState;

/**
 * gn_session_manager_new:
 *
 * Creates a new session manager instance.
 *
 * The session manager will load timeout settings from GSettings
 * and start in the locked state.
 *
 * Returns: (transfer full): A new #GnSessionManager
 */
GnSessionManager *gn_session_manager_new(void);

/**
 * gn_session_manager_get_default:
 *
 * Gets the singleton session manager instance.
 *
 * Returns: (transfer none): The default #GnSessionManager
 */
GnSessionManager *gn_session_manager_get_default(void);

/**
 * gn_session_manager_authenticate:
 * @self: A #GnSessionManager
 * @password: The password to authenticate with
 * @error: (nullable): Return location for error
 *
 * Attempts to authenticate and start a new session.
 *
 * On success, the session state changes to %GN_SESSION_STATE_AUTHENTICATED
 * and the "session-unlocked" signal is emitted. The inactivity timer
 * is started based on the configured timeout.
 *
 * Returns: %TRUE if authentication succeeded, %FALSE otherwise
 */
gboolean gn_session_manager_authenticate(GnSessionManager *self,
                                         const gchar *password,
                                         GError **error);

/**
 * gn_session_manager_is_authenticated:
 * @self: A #GnSessionManager
 *
 * Checks if the session is currently authenticated.
 *
 * Returns: %TRUE if session is active and authenticated
 */
gboolean gn_session_manager_is_authenticated(GnSessionManager *self);

/**
 * gn_session_manager_get_state:
 * @self: A #GnSessionManager
 *
 * Gets the current session state.
 *
 * Returns: The current #GnSessionState
 */
GnSessionState gn_session_manager_get_state(GnSessionManager *self);

/**
 * gn_session_manager_get_timeout:
 * @self: A #GnSessionManager
 *
 * Gets the current auto-lock timeout in seconds.
 *
 * A value of 0 means auto-lock is disabled.
 *
 * Returns: Timeout in seconds, or 0 if disabled
 */
guint gn_session_manager_get_timeout(GnSessionManager *self);

/**
 * gn_session_manager_set_timeout:
 * @self: A #GnSessionManager
 * @seconds: Timeout in seconds, or 0 to disable
 *
 * Sets the auto-lock timeout.
 *
 * Changes are persisted to GSettings. If a session is active,
 * the timer is reset with the new timeout value.
 */
void gn_session_manager_set_timeout(GnSessionManager *self, guint seconds);

/**
 * gn_session_manager_lock:
 * @self: A #GnSessionManager
 *
 * Manually locks the session.
 *
 * This clears any cached credentials and emits the "session-locked"
 * signal. Re-authentication will be required for further operations.
 */
void gn_session_manager_lock(GnSessionManager *self);

/**
 * gn_session_manager_extend:
 * @self: A #GnSessionManager
 *
 * Extends the session on user activity.
 *
 * Call this when user activity is detected to reset the inactivity
 * timer. If the session is not authenticated, this has no effect.
 */
void gn_session_manager_extend(GnSessionManager *self);

/**
 * gn_session_manager_get_remaining_time:
 * @self: A #GnSessionManager
 *
 * Gets the remaining time before auto-lock in seconds.
 *
 * Returns: Seconds until auto-lock, or 0 if session is locked
 *          or auto-lock is disabled
 */
guint gn_session_manager_get_remaining_time(GnSessionManager *self);

/**
 * gn_session_manager_set_password:
 * @self: A #GnSessionManager
 * @current_password: The current password for verification
 * @new_password: The new password to set
 * @error: (nullable): Return location for error
 *
 * Changes the session password.
 *
 * The current password must be provided for verification.
 * The new password is stored securely using the secret store.
 *
 * Returns: %TRUE if password was changed, %FALSE on error
 */
gboolean gn_session_manager_set_password(GnSessionManager *self,
                                         const gchar *current_password,
                                         const gchar *new_password,
                                         GError **error);

/**
 * gn_session_manager_has_password:
 * @self: A #GnSessionManager
 *
 * Checks if a session password has been set.
 *
 * Returns: %TRUE if a password is configured
 */
gboolean gn_session_manager_has_password(GnSessionManager *self);

/**
 * gn_session_manager_clear_password:
 * @self: A #GnSessionManager
 * @current_password: The current password for verification
 * @error: (nullable): Return location for error
 *
 * Removes the session password.
 *
 * After clearing, the session can be unlocked without a password.
 *
 * Returns: %TRUE if password was cleared, %FALSE on error
 */
gboolean gn_session_manager_clear_password(GnSessionManager *self,
                                           const gchar *current_password,
                                           GError **error);

G_END_DECLS
