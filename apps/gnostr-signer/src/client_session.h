/* client_session.h - NIP-46 client session management for gnostr-signer
 *
 * Manages authenticated NIP-46 client sessions (remote applications) with:
 * - Per-client session tracking with activity timestamps
 * - Configurable session timeout (default 15 minutes of inactivity)
 * - Session persistence for remembered/approved clients
 * - Re-authentication prompts after session timeout
 * - Session revocation (ability to end individual sessions)
 * - Secure storage of session data via libsecret
 *
 * This is distinct from GnSessionManager which handles user authentication
 * to unlock the signer itself. ClientSession tracks which remote applications
 * have been granted access and for how long.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GN_TYPE_CLIENT_SESSION (gn_client_session_get_type())
#define GN_TYPE_CLIENT_SESSION_MANAGER (gn_client_session_manager_get_type())

G_DECLARE_FINAL_TYPE(GnClientSession, gn_client_session, GN, CLIENT_SESSION, GObject)
G_DECLARE_FINAL_TYPE(GnClientSessionManager, gn_client_session_manager, GN, CLIENT_SESSION_MANAGER, GObject)

/**
 * GnClientSessionState:
 * @GN_CLIENT_SESSION_ACTIVE: Session is active and valid
 * @GN_CLIENT_SESSION_EXPIRED: Session expired due to inactivity timeout
 * @GN_CLIENT_SESSION_REVOKED: Session was manually revoked by user
 * @GN_CLIENT_SESSION_PENDING: Session is pending user approval
 *
 * The current state of a client session.
 */
typedef enum {
  GN_CLIENT_SESSION_PENDING,
  GN_CLIENT_SESSION_ACTIVE,
  GN_CLIENT_SESSION_EXPIRED,
  GN_CLIENT_SESSION_REVOKED
} GnClientSessionState;

/**
 * GnClientSessionPermission:
 * @GN_PERM_SIGN_EVENT: Permission to sign events
 * @GN_PERM_GET_PUBLIC_KEY: Permission to retrieve public key
 * @GN_PERM_ENCRYPT: Permission to encrypt messages (NIP-04/NIP-44)
 * @GN_PERM_DECRYPT: Permission to decrypt messages
 * @GN_PERM_CONNECT: Basic connection permission
 *
 * Permissions that can be granted to a client session.
 */
typedef enum {
  GN_PERM_NONE         = 0,
  GN_PERM_CONNECT      = (1 << 0),
  GN_PERM_GET_PUBLIC_KEY = (1 << 1),
  GN_PERM_SIGN_EVENT   = (1 << 2),
  GN_PERM_ENCRYPT      = (1 << 3),
  GN_PERM_DECRYPT      = (1 << 4),
  GN_PERM_ALL          = 0x1F
} GnClientSessionPermission;

/* ============================================================================
 * GnClientSession - Individual client session
 * ============================================================================ */

/**
 * gn_client_session_get_client_pubkey:
 * @self: A #GnClientSession
 *
 * Gets the client's public key (hex format).
 *
 * Returns: (transfer none): The client's public key
 */
const gchar *gn_client_session_get_client_pubkey(GnClientSession *self);

/**
 * gn_client_session_get_app_name:
 * @self: A #GnClientSession
 *
 * Gets the application name (if provided during connection).
 *
 * Returns: (transfer none) (nullable): The app name or NULL
 */
const gchar *gn_client_session_get_app_name(GnClientSession *self);

/**
 * gn_client_session_get_identity:
 * @self: A #GnClientSession
 *
 * Gets the identity (npub) this session is associated with.
 *
 * Returns: (transfer none): The identity npub
 */
const gchar *gn_client_session_get_identity(GnClientSession *self);

/**
 * gn_client_session_get_state:
 * @self: A #GnClientSession
 *
 * Gets the current session state.
 *
 * Returns: The #GnClientSessionState
 */
GnClientSessionState gn_client_session_get_state(GnClientSession *self);

/**
 * gn_client_session_get_permissions:
 * @self: A #GnClientSession
 *
 * Gets the granted permissions bitmask.
 *
 * Returns: Bitmask of #GnClientSessionPermission
 */
guint gn_client_session_get_permissions(GnClientSession *self);

/**
 * gn_client_session_has_permission:
 * @self: A #GnClientSession
 * @perm: The permission to check
 *
 * Checks if session has a specific permission.
 *
 * Returns: %TRUE if permission is granted
 */
gboolean gn_client_session_has_permission(GnClientSession *self,
                                          GnClientSessionPermission perm);

/**
 * gn_client_session_get_created_at:
 * @self: A #GnClientSession
 *
 * Gets the session creation timestamp.
 *
 * Returns: Unix timestamp of session creation
 */
gint64 gn_client_session_get_created_at(GnClientSession *self);

/**
 * gn_client_session_get_last_activity:
 * @self: A #GnClientSession
 *
 * Gets the timestamp of last activity.
 *
 * Returns: Unix timestamp of last activity
 */
gint64 gn_client_session_get_last_activity(GnClientSession *self);

/**
 * gn_client_session_get_request_count:
 * @self: A #GnClientSession
 *
 * Gets the total number of requests made in this session.
 *
 * Returns: Request count
 */
guint gn_client_session_get_request_count(GnClientSession *self);

/**
 * gn_client_session_get_expires_at:
 * @self: A #GnClientSession
 *
 * Gets the session expiration timestamp.
 *
 * Returns: Unix timestamp when session expires, or 0 if no expiration
 */
gint64 gn_client_session_get_expires_at(GnClientSession *self);

/**
 * gn_client_session_is_persistent:
 * @self: A #GnClientSession
 *
 * Checks if session is persisted (remembered across app restarts).
 *
 * Returns: %TRUE if session is persistent
 */
gboolean gn_client_session_is_persistent(GnClientSession *self);

/**
 * gn_client_session_get_remaining_time:
 * @self: A #GnClientSession
 *
 * Gets remaining time before session timeout (in seconds).
 *
 * Returns: Seconds until timeout, or 0 if expired/revoked
 */
guint gn_client_session_get_remaining_time(GnClientSession *self);

/* ============================================================================
 * GnClientSessionManager - Manages all client sessions
 * ============================================================================ */

/**
 * gn_client_session_manager_new:
 *
 * Creates a new client session manager.
 *
 * Returns: (transfer full): A new #GnClientSessionManager
 */
GnClientSessionManager *gn_client_session_manager_new(void);

/**
 * gn_client_session_manager_get_default:
 *
 * Gets the singleton client session manager instance.
 *
 * Returns: (transfer none): The default #GnClientSessionManager
 */
GnClientSessionManager *gn_client_session_manager_get_default(void);

/**
 * gn_client_session_manager_get_timeout:
 * @self: A #GnClientSessionManager
 *
 * Gets the default session timeout in seconds.
 *
 * Returns: Timeout in seconds (0 means no timeout)
 */
guint gn_client_session_manager_get_timeout(GnClientSessionManager *self);

/**
 * gn_client_session_manager_set_timeout:
 * @self: A #GnClientSessionManager
 * @seconds: Timeout in seconds (0 to disable)
 *
 * Sets the default session timeout for new sessions.
 */
void gn_client_session_manager_set_timeout(GnClientSessionManager *self,
                                           guint seconds);

/**
 * gn_client_session_manager_create_session:
 * @self: A #GnClientSessionManager
 * @client_pubkey: Client's public key (hex)
 * @identity: Identity npub to associate with
 * @app_name: (nullable): Application name
 * @permissions: Bitmask of granted permissions
 * @persistent: Whether to persist across restarts
 * @ttl_seconds: Session TTL (0 for default timeout, -1 for never)
 *
 * Creates a new active client session.
 *
 * Returns: (transfer none): The new #GnClientSession
 */
GnClientSession *gn_client_session_manager_create_session(
    GnClientSessionManager *self,
    const gchar *client_pubkey,
    const gchar *identity,
    const gchar *app_name,
    guint permissions,
    gboolean persistent,
    gint64 ttl_seconds);

/**
 * gn_client_session_manager_get_session:
 * @self: A #GnClientSessionManager
 * @client_pubkey: Client's public key
 * @identity: (nullable): Identity to match (NULL for any)
 *
 * Gets an existing session for a client.
 *
 * Returns: (transfer none) (nullable): The session or NULL
 */
GnClientSession *gn_client_session_manager_get_session(
    GnClientSessionManager *self,
    const gchar *client_pubkey,
    const gchar *identity);

/**
 * gn_client_session_manager_has_active_session:
 * @self: A #GnClientSessionManager
 * @client_pubkey: Client's public key
 * @identity: (nullable): Identity to match
 *
 * Checks if client has an active (non-expired) session.
 *
 * Returns: %TRUE if active session exists
 */
gboolean gn_client_session_manager_has_active_session(
    GnClientSessionManager *self,
    const gchar *client_pubkey,
    const gchar *identity);

/**
 * gn_client_session_manager_touch_session:
 * @self: A #GnClientSessionManager
 * @client_pubkey: Client's public key
 * @identity: (nullable): Identity to match
 *
 * Updates session activity timestamp (extends timeout).
 *
 * Returns: %TRUE if session was found and updated
 */
gboolean gn_client_session_manager_touch_session(
    GnClientSessionManager *self,
    const gchar *client_pubkey,
    const gchar *identity);

/**
 * gn_client_session_manager_revoke_session:
 * @self: A #GnClientSessionManager
 * @client_pubkey: Client's public key
 * @identity: (nullable): Identity to match
 *
 * Revokes a client session (marks as revoked).
 *
 * Returns: %TRUE if session was revoked
 */
gboolean gn_client_session_manager_revoke_session(
    GnClientSessionManager *self,
    const gchar *client_pubkey,
    const gchar *identity);

/**
 * gn_client_session_manager_revoke_all_for_client:
 * @self: A #GnClientSessionManager
 * @client_pubkey: Client's public key
 *
 * Revokes all sessions for a client.
 *
 * Returns: Number of sessions revoked
 */
guint gn_client_session_manager_revoke_all_for_client(
    GnClientSessionManager *self,
    const gchar *client_pubkey);

/**
 * gn_client_session_manager_revoke_all:
 * @self: A #GnClientSessionManager
 *
 * Revokes all active sessions.
 *
 * Returns: Number of sessions revoked
 */
guint gn_client_session_manager_revoke_all(GnClientSessionManager *self);

/**
 * gn_client_session_manager_list_sessions:
 * @self: A #GnClientSessionManager
 *
 * Lists all sessions (active and expired/revoked).
 *
 * Returns: (transfer container) (element-type GnClientSession):
 *          Array of sessions. Free with g_ptr_array_unref().
 */
GPtrArray *gn_client_session_manager_list_sessions(GnClientSessionManager *self);

/**
 * gn_client_session_manager_list_active_sessions:
 * @self: A #GnClientSessionManager
 *
 * Lists only active sessions.
 *
 * Returns: (transfer container) (element-type GnClientSession):
 *          Array of active sessions. Free with g_ptr_array_unref().
 */
GPtrArray *gn_client_session_manager_list_active_sessions(GnClientSessionManager *self);

/**
 * gn_client_session_manager_cleanup_expired:
 * @self: A #GnClientSessionManager
 *
 * Removes expired/revoked sessions from memory.
 *
 * Returns: Number of sessions cleaned up
 */
guint gn_client_session_manager_cleanup_expired(GnClientSessionManager *self);

/**
 * gn_client_session_manager_load_persistent:
 * @self: A #GnClientSessionManager
 *
 * Loads persistent sessions from secure storage.
 *
 * Returns: Number of sessions loaded
 */
guint gn_client_session_manager_load_persistent(GnClientSessionManager *self);

/**
 * gn_client_session_manager_save_persistent:
 * @self: A #GnClientSessionManager
 *
 * Saves persistent sessions to secure storage.
 *
 * Returns: %TRUE if save was successful
 */
gboolean gn_client_session_manager_save_persistent(GnClientSessionManager *self);

/**
 * gn_client_session_manager_get_session_count:
 * @self: A #GnClientSessionManager
 *
 * Gets total number of tracked sessions.
 *
 * Returns: Session count
 */
guint gn_client_session_manager_get_session_count(GnClientSessionManager *self);

/**
 * gn_client_session_manager_get_active_count:
 * @self: A #GnClientSessionManager
 *
 * Gets number of active sessions.
 *
 * Returns: Active session count
 */
guint gn_client_session_manager_get_active_count(GnClientSessionManager *self);

G_END_DECLS
