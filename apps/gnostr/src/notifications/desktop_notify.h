/*
 * desktop_notify.h - Desktop notification system for GNostr
 *
 * Cross-platform desktop notifications:
 *
 * Linux:
 *   - GNotification (GLib/GIO) for desktop notifications
 *   - Integrates with GNOME, KDE, and other FreeDesktop-compliant desktops
 *
 * macOS:
 *   - UNUserNotificationCenter (UserNotifications.framework)
 *   - Native macOS notification center integration
 *
 * Notification types match GnostrNotificationType from badge_manager.h:
 *   - DM: Direct messages
 *   - Mention: Mentions in notes
 *   - Reply: Replies to your notes
 *   - Zap: Zap receipts
 *   - Repost: Reposts of your notes
 *
 * Settings are read from GSettings (org.gnostr.Notifications schema).
 */

#ifndef DESKTOP_NOTIFY_H
#define DESKTOP_NOTIFY_H

#include <glib-object.h>
#include <gio/gio.h>
#include "badge_manager.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_DESKTOP_NOTIFY (gnostr_desktop_notify_get_type())

G_DECLARE_FINAL_TYPE(GnostrDesktopNotify, gnostr_desktop_notify, GNOSTR, DESKTOP_NOTIFY, GObject)

/**
 * GnostrDesktopNotifyPrivacy:
 * @GNOSTR_NOTIFY_PRIVACY_FULL: Show full message content in notification
 * @GNOSTR_NOTIFY_PRIVACY_SENDER_ONLY: Show only sender name, no message preview
 * @GNOSTR_NOTIFY_PRIVACY_HIDDEN: Generic "New notification" message
 *
 * Privacy levels for notification content.
 */
typedef enum {
  GNOSTR_NOTIFY_PRIVACY_FULL = 0,
  GNOSTR_NOTIFY_PRIVACY_SENDER_ONLY,
  GNOSTR_NOTIFY_PRIVACY_HIDDEN
} GnostrDesktopNotifyPrivacy;

/* ============== Lifecycle ============== */

/**
 * gnostr_desktop_notify_get_default:
 *
 * Gets the default desktop notification manager singleton.
 * The manager is created on first call and persists for app lifetime.
 *
 * Returns: (transfer none): The default desktop notification manager
 */
GnostrDesktopNotify *gnostr_desktop_notify_get_default(void);

/**
 * gnostr_desktop_notify_new:
 * @app: The GApplication instance (for GNotification on Linux)
 *
 * Creates a new desktop notification manager instance.
 * Typically you should use gnostr_desktop_notify_get_default() instead.
 *
 * Returns: (transfer full): A new desktop notification manager
 */
GnostrDesktopNotify *gnostr_desktop_notify_new(GApplication *app);

/* ============== Initialization ============== */

/**
 * gnostr_desktop_notify_set_app:
 * @self: The desktop notification manager
 * @app: The GApplication instance
 *
 * Sets the GApplication instance. Required for GNotification on Linux.
 * On macOS, this is used for app activation handling.
 */
void gnostr_desktop_notify_set_app(GnostrDesktopNotify *self,
                                    GApplication *app);

/**
 * gnostr_desktop_notify_request_permission:
 * @self: The desktop notification manager
 *
 * Requests notification permission from the OS.
 * On macOS, this shows the system permission dialog.
 * On Linux, this is a no-op (permissions are granted by default).
 *
 * Call this early in app startup to ensure notifications work.
 */
void gnostr_desktop_notify_request_permission(GnostrDesktopNotify *self);

/**
 * gnostr_desktop_notify_is_available:
 *
 * Checks if desktop notifications are available at runtime.
 * This checks both compile-time and runtime availability.
 *
 * Returns: TRUE if desktop notifications are available
 */
gboolean gnostr_desktop_notify_is_available(void);

/**
 * gnostr_desktop_notify_has_permission:
 * @self: The desktop notification manager
 *
 * Checks if the app has permission to show notifications.
 *
 * Returns: TRUE if notifications are permitted
 */
gboolean gnostr_desktop_notify_has_permission(GnostrDesktopNotify *self);

/* ============== Configuration ============== */

/**
 * gnostr_desktop_notify_set_enabled:
 * @self: The desktop notification manager
 * @type: Notification type
 * @enabled: Whether to send desktop notifications for this type
 *
 * Enables or disables desktop notifications for a specific type.
 * Changes are persisted to GSettings.
 */
void gnostr_desktop_notify_set_enabled(GnostrDesktopNotify *self,
                                        GnostrNotificationType type,
                                        gboolean enabled);

/**
 * gnostr_desktop_notify_get_enabled:
 * @self: The desktop notification manager
 * @type: Notification type
 *
 * Returns: Whether desktop notifications are enabled for this type
 */
gboolean gnostr_desktop_notify_get_enabled(GnostrDesktopNotify *self,
                                            GnostrNotificationType type);

/**
 * gnostr_desktop_notify_set_privacy:
 * @self: The desktop notification manager
 * @privacy: Privacy level for notification content
 *
 * Sets the privacy level for notification content display.
 */
void gnostr_desktop_notify_set_privacy(GnostrDesktopNotify *self,
                                        GnostrDesktopNotifyPrivacy privacy);

/**
 * gnostr_desktop_notify_get_privacy:
 * @self: The desktop notification manager
 *
 * Returns: Current privacy level for notification content
 */
GnostrDesktopNotifyPrivacy gnostr_desktop_notify_get_privacy(GnostrDesktopNotify *self);

/**
 * gnostr_desktop_notify_set_sound_enabled:
 * @self: The desktop notification manager
 * @enabled: Whether to play notification sounds
 *
 * Enables or disables notification sounds.
 */
void gnostr_desktop_notify_set_sound_enabled(GnostrDesktopNotify *self,
                                              gboolean enabled);

/**
 * gnostr_desktop_notify_get_sound_enabled:
 * @self: The desktop notification manager
 *
 * Returns: Whether notification sounds are enabled
 */
gboolean gnostr_desktop_notify_get_sound_enabled(GnostrDesktopNotify *self);

/* ============== Send Notifications ============== */

/**
 * gnostr_desktop_notify_send_dm:
 * @self: The desktop notification manager
 * @sender_name: Display name of the sender
 * @sender_pubkey: Sender's public key (hex, for action handling)
 * @message_preview: Optional message preview text (may be NULL)
 * @event_id: Event ID (hex, for action handling)
 *
 * Sends a DM notification if DM notifications are enabled.
 */
void gnostr_desktop_notify_send_dm(GnostrDesktopNotify *self,
                                    const char *sender_name,
                                    const char *sender_pubkey,
                                    const char *message_preview,
                                    const char *event_id);

/**
 * gnostr_desktop_notify_send_mention:
 * @self: The desktop notification manager
 * @sender_name: Display name of the person who mentioned you
 * @sender_pubkey: Sender's public key (hex)
 * @note_preview: Optional note preview text (may be NULL)
 * @event_id: Event ID (hex)
 *
 * Sends a mention notification if mention notifications are enabled.
 */
void gnostr_desktop_notify_send_mention(GnostrDesktopNotify *self,
                                         const char *sender_name,
                                         const char *sender_pubkey,
                                         const char *note_preview,
                                         const char *event_id);

/**
 * gnostr_desktop_notify_send_reply:
 * @self: The desktop notification manager
 * @sender_name: Display name of the person who replied
 * @sender_pubkey: Sender's public key (hex)
 * @reply_preview: Optional reply preview text (may be NULL)
 * @event_id: Event ID (hex)
 *
 * Sends a reply notification if reply notifications are enabled.
 */
void gnostr_desktop_notify_send_reply(GnostrDesktopNotify *self,
                                       const char *sender_name,
                                       const char *sender_pubkey,
                                       const char *reply_preview,
                                       const char *event_id);

/**
 * gnostr_desktop_notify_send_zap:
 * @self: The desktop notification manager
 * @sender_name: Display name of the person who zapped
 * @sender_pubkey: Sender's public key (hex)
 * @amount_sats: Amount of the zap in satoshis
 * @message: Optional zap message (may be NULL)
 * @event_id: Event ID (hex)
 *
 * Sends a zap notification if zap notifications are enabled.
 */
void gnostr_desktop_notify_send_zap(GnostrDesktopNotify *self,
                                     const char *sender_name,
                                     const char *sender_pubkey,
                                     guint64 amount_sats,
                                     const char *message,
                                     const char *event_id);

/**
 * gnostr_desktop_notify_send_repost:
 * @self: The desktop notification manager
 * @reposter_name: Display name of the person who reposted
 * @reposter_pubkey: Reposter's public key (hex)
 * @event_id: Event ID (hex)
 *
 * Sends a repost notification if repost notifications are enabled.
 */
void gnostr_desktop_notify_send_repost(GnostrDesktopNotify *self,
                                        const char *reposter_name,
                                        const char *reposter_pubkey,
                                        const char *event_id);

/**
 * gnostr_desktop_notify_send:
 * @self: The desktop notification manager
 * @type: Notification type
 * @title: Notification title
 * @body: Notification body text
 * @event_id: Optional event ID for action handling (may be NULL)
 *
 * Sends a generic notification if the specified type is enabled.
 * Prefer the type-specific methods (send_dm, send_mention, etc.) when possible.
 */
void gnostr_desktop_notify_send(GnostrDesktopNotify *self,
                                 GnostrNotificationType type,
                                 const char *title,
                                 const char *body,
                                 const char *event_id);

/* ============== Notification Actions ============== */

/**
 * GnostrNotifyActionCallback:
 * @self: The desktop notification manager
 * @action: Action identifier (e.g., "open", "mark-read", "reply")
 * @event_id: Event ID associated with the notification
 * @user_data: User data
 *
 * Callback invoked when a notification action is triggered.
 */
typedef void (*GnostrNotifyActionCallback)(GnostrDesktopNotify *self,
                                            const char *action,
                                            const char *event_id,
                                            gpointer user_data);

/**
 * gnostr_desktop_notify_set_action_callback:
 * @self: The desktop notification manager
 * @callback: Callback function or NULL to remove
 * @user_data: User data for callback
 * @destroy: Destroy notify for user_data or NULL
 *
 * Sets a callback to be invoked when notification actions are triggered.
 * Only one callback can be registered at a time.
 */
void gnostr_desktop_notify_set_action_callback(GnostrDesktopNotify *self,
                                                GnostrNotifyActionCallback callback,
                                                gpointer user_data,
                                                GDestroyNotify destroy);

/* ============== Grouping ============== */

/**
 * gnostr_desktop_notify_withdraw:
 * @self: The desktop notification manager
 * @event_id: Event ID of the notification to withdraw
 *
 * Withdraws (removes) a previously sent notification by event ID.
 */
void gnostr_desktop_notify_withdraw(GnostrDesktopNotify *self,
                                     const char *event_id);

/**
 * gnostr_desktop_notify_withdraw_type:
 * @self: The desktop notification manager
 * @type: Notification type to withdraw all notifications for
 *
 * Withdraws all notifications of a specific type.
 */
void gnostr_desktop_notify_withdraw_type(GnostrDesktopNotify *self,
                                          GnostrNotificationType type);

/**
 * gnostr_desktop_notify_withdraw_all:
 * @self: The desktop notification manager
 *
 * Withdraws all notifications sent by this app.
 */
void gnostr_desktop_notify_withdraw_all(GnostrDesktopNotify *self);

G_END_DECLS

#endif /* DESKTOP_NOTIFY_H */
