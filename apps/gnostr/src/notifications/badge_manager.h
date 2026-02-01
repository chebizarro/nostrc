/*
 * badge_manager.h - Notification badge system for GNostr
 *
 * Tracks unread notifications (DMs, mentions, replies, zaps) and
 * updates system tray badges on Linux (AppIndicator) and macOS (NSStatusItem).
 *
 * Badge sources are configurable per notification type.
 */

#ifndef BADGE_MANAGER_H
#define BADGE_MANAGER_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_BADGE_MANAGER (gnostr_badge_manager_get_type())

G_DECLARE_FINAL_TYPE(GnostrBadgeManager, gnostr_badge_manager, GNOSTR, BADGE_MANAGER, GObject)

/**
 * GnostrNotificationType:
 * @GNOSTR_NOTIFICATION_DM: Direct messages (kind:4 legacy, kind:14 NIP-17)
 * @GNOSTR_NOTIFICATION_MENTION: Mentions in notes (p-tag)
 * @GNOSTR_NOTIFICATION_REPLY: Replies to own notes (e-tag with reply marker)
 * @GNOSTR_NOTIFICATION_ZAP: Zap receipts (kind:9735)
 * @GNOSTR_NOTIFICATION_REPOST: Reposts of user's notes (kind:6)
 *
 * Types of notifications that can generate badges.
 */
typedef enum {
  GNOSTR_NOTIFICATION_DM = 0,
  GNOSTR_NOTIFICATION_MENTION,
  GNOSTR_NOTIFICATION_REPLY,
  GNOSTR_NOTIFICATION_ZAP,
  GNOSTR_NOTIFICATION_REPOST,
  GNOSTR_NOTIFICATION_TYPE_COUNT
} GnostrNotificationType;

/**
 * GnostrBadgeDisplayMode:
 * @GNOSTR_BADGE_DISPLAY_NONE: No badge shown
 * @GNOSTR_BADGE_DISPLAY_DOT: Simple dot indicator
 * @GNOSTR_BADGE_DISPLAY_COUNT: Numeric count (1, 2, 99+)
 *
 * How to display the badge on the system tray icon.
 */
typedef enum {
  GNOSTR_BADGE_DISPLAY_NONE = 0,
  GNOSTR_BADGE_DISPLAY_DOT,
  GNOSTR_BADGE_DISPLAY_COUNT
} GnostrBadgeDisplayMode;

/**
 * GnostrBadgeChangedCallback:
 * @manager: The badge manager
 * @total_count: Total unread count across all enabled types
 * @user_data: User data
 *
 * Callback invoked when badge count changes.
 */
typedef void (*GnostrBadgeChangedCallback)(GnostrBadgeManager *manager,
                                            guint total_count,
                                            gpointer user_data);

/* ============== Lifecycle ============== */

/**
 * gnostr_badge_manager_get_default:
 *
 * Gets the default badge manager singleton.
 * The manager is created on first call and persists for app lifetime.
 *
 * Returns: (transfer none): The default badge manager
 */
GnostrBadgeManager *gnostr_badge_manager_get_default(void);

/**
 * gnostr_badge_manager_new:
 *
 * Creates a new badge manager instance.
 * Typically you should use gnostr_badge_manager_get_default() instead.
 *
 * Returns: (transfer full): A new badge manager
 */
GnostrBadgeManager *gnostr_badge_manager_new(void);

/* ============== Configuration ============== */

/**
 * gnostr_badge_manager_set_user_pubkey:
 * @self: The badge manager
 * @pubkey_hex: User's public key (64 hex chars) or NULL to disable
 *
 * Sets the current user's public key. This is required to track
 * mentions, replies, and zaps directed at the user.
 */
void gnostr_badge_manager_set_user_pubkey(GnostrBadgeManager *self,
                                           const char *pubkey_hex);

/**
 * gnostr_badge_manager_set_notification_enabled:
 * @self: The badge manager
 * @type: Notification type
 * @enabled: Whether to include this type in badge count
 *
 * Enables or disables a notification type for badge counting.
 * Changes are persisted to GSettings.
 */
void gnostr_badge_manager_set_notification_enabled(GnostrBadgeManager *self,
                                                    GnostrNotificationType type,
                                                    gboolean enabled);

/**
 * gnostr_badge_manager_get_notification_enabled:
 * @self: The badge manager
 * @type: Notification type
 *
 * Returns: Whether this notification type is enabled
 */
gboolean gnostr_badge_manager_get_notification_enabled(GnostrBadgeManager *self,
                                                        GnostrNotificationType type);

/**
 * gnostr_badge_manager_set_display_mode:
 * @self: The badge manager
 * @mode: Badge display mode
 *
 * Sets how the badge should be displayed on the tray icon.
 */
void gnostr_badge_manager_set_display_mode(GnostrBadgeManager *self,
                                            GnostrBadgeDisplayMode mode);

/**
 * gnostr_badge_manager_get_display_mode:
 * @self: The badge manager
 *
 * Returns: Current badge display mode
 */
GnostrBadgeDisplayMode gnostr_badge_manager_get_display_mode(GnostrBadgeManager *self);

/* ============== Count Management ============== */

/**
 * gnostr_badge_manager_increment:
 * @self: The badge manager
 * @type: Notification type
 * @count: Number to add (usually 1)
 *
 * Increments the unread count for a notification type.
 */
void gnostr_badge_manager_increment(GnostrBadgeManager *self,
                                     GnostrNotificationType type,
                                     guint count);

/**
 * gnostr_badge_manager_clear:
 * @self: The badge manager
 * @type: Notification type
 *
 * Clears the unread count for a notification type.
 */
void gnostr_badge_manager_clear(GnostrBadgeManager *self,
                                 GnostrNotificationType type);

/**
 * gnostr_badge_manager_clear_all:
 * @self: The badge manager
 *
 * Clears all unread counts.
 */
void gnostr_badge_manager_clear_all(GnostrBadgeManager *self);

/**
 * gnostr_badge_manager_get_count:
 * @self: The badge manager
 * @type: Notification type
 *
 * Returns: Unread count for this notification type
 */
guint gnostr_badge_manager_get_count(GnostrBadgeManager *self,
                                      GnostrNotificationType type);

/**
 * gnostr_badge_manager_get_total_count:
 * @self: The badge manager
 *
 * Returns: Total unread count across all enabled notification types
 */
guint gnostr_badge_manager_get_total_count(GnostrBadgeManager *self);

/* ============== Timestamp Tracking ============== */

/**
 * gnostr_badge_manager_set_last_read:
 * @self: The badge manager
 * @type: Notification type
 * @timestamp: Unix timestamp of last read event
 *
 * Sets the last-read timestamp for a notification type.
 * Events older than this are considered read.
 */
void gnostr_badge_manager_set_last_read(GnostrBadgeManager *self,
                                         GnostrNotificationType type,
                                         gint64 timestamp);

/**
 * gnostr_badge_manager_get_last_read:
 * @self: The badge manager
 * @type: Notification type
 *
 * Returns: Last-read timestamp for this notification type
 */
gint64 gnostr_badge_manager_get_last_read(GnostrBadgeManager *self,
                                           GnostrNotificationType type);

/* ============== Callbacks ============== */

/**
 * gnostr_badge_manager_set_changed_callback:
 * @self: The badge manager
 * @callback: Callback function or NULL to remove
 * @user_data: User data for callback
 * @destroy: Destroy notify for user_data or NULL
 *
 * Sets a callback to be invoked when the total badge count changes.
 * Only one callback can be registered at a time.
 */
void gnostr_badge_manager_set_changed_callback(GnostrBadgeManager *self,
                                                GnostrBadgeChangedCallback callback,
                                                gpointer user_data,
                                                GDestroyNotify destroy);

/**
 * GnostrNotificationEventCallback:
 * @manager: The badge manager
 * @type: Type of notification event
 * @sender_pubkey: Sender's public key (hex, 64 chars)
 * @sender_name: Sender's display name (may be NULL)
 * @content: Event content/preview (may be NULL)
 * @event_id: Event ID (hex, 64 chars)
 * @amount_sats: For zaps, the amount in satoshis (0 for other types)
 * @user_data: User data
 *
 * Callback invoked when a new notification event is detected.
 * This is used by the desktop notification system to send popups.
 */
typedef void (*GnostrNotificationEventCallback)(GnostrBadgeManager *manager,
                                                  GnostrNotificationType type,
                                                  const char *sender_pubkey,
                                                  const char *sender_name,
                                                  const char *content,
                                                  const char *event_id,
                                                  guint64 amount_sats,
                                                  gpointer user_data);

/**
 * gnostr_badge_manager_set_event_callback:
 * @self: The badge manager
 * @callback: Callback function or NULL to remove
 * @user_data: User data for callback
 * @destroy: Destroy notify for user_data or NULL
 *
 * Sets a callback to be invoked when a new notification event is detected.
 * This is separate from the badge changed callback and provides full event
 * details for sending desktop notifications.
 * Only one callback can be registered at a time.
 */
void gnostr_badge_manager_set_event_callback(GnostrBadgeManager *self,
                                              GnostrNotificationEventCallback callback,
                                              gpointer user_data,
                                              GDestroyNotify destroy);

/* ============== Relay Subscription Integration ============== */

/**
 * gnostr_badge_manager_start_subscriptions:
 * @self: The badge manager
 *
 * Starts local database subscriptions to detect new notification events.
 * Requires storage_ndb to be initialized and user pubkey to be set.
 */
void gnostr_badge_manager_start_subscriptions(GnostrBadgeManager *self);

/**
 * gnostr_badge_manager_stop_subscriptions:
 * @self: The badge manager
 *
 * Stops notification subscriptions.
 */
void gnostr_badge_manager_stop_subscriptions(GnostrBadgeManager *self);

/* ============== Badge Formatting ============== */

/**
 * gnostr_badge_manager_format_count:
 * @count: Unread count
 *
 * Formats a count for badge display (e.g., "99+" for counts > 99).
 *
 * Returns: (transfer full): Formatted string
 */
gchar *gnostr_badge_manager_format_count(guint count);

G_END_DECLS

#endif /* BADGE_MANAGER_H */
