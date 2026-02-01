#ifndef GNOSTR_NOTIFICATIONS_VIEW_H
#define GNOSTR_NOTIFICATIONS_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_NOTIFICATIONS_VIEW (gnostr_notifications_view_get_type())

G_DECLARE_FINAL_TYPE(GnostrNotificationsView, gnostr_notifications_view, GNOSTR, NOTIFICATIONS_VIEW, GtkWidget)

/**
 * Signals:
 * "open-note" (gchar* note_id_hex, gpointer user_data)
 *   - Emitted when user clicks to view a note
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user clicks to view a profile
 */

typedef struct _GnostrNotificationsView GnostrNotificationsView;

/**
 * Notification types
 */
typedef enum {
    GNOSTR_NOTIFICATION_TYPE_MENTION,    /* Someone mentioned you in a note */
    GNOSTR_NOTIFICATION_TYPE_REPLY,      /* Someone replied to your note */
    GNOSTR_NOTIFICATION_TYPE_REPOST,     /* Someone reposted your note (kind 6) */
    GNOSTR_NOTIFICATION_TYPE_REACTION,   /* Someone reacted to your note (kind 7) */
    GNOSTR_NOTIFICATION_TYPE_ZAP,        /* Someone zapped your note (kind 9735) */
    GNOSTR_NOTIFICATION_TYPE_FOLLOW,     /* Someone followed you */
    GNOSTR_NOTIFICATION_TYPE_LIST,       /* Someone added you to a NIP-51 list */
} GnostrNotificationType;

/**
 * Notification data structure
 */
typedef struct {
    char *id;                   /* Unique notification ID (event ID) */
    GnostrNotificationType type;/* Type of notification */
    char *actor_pubkey;         /* Pubkey of who triggered the notification */
    char *actor_name;           /* Display name (nullable) */
    char *actor_handle;         /* Handle like @user (nullable) */
    char *actor_avatar_url;     /* Avatar URL (nullable) */
    char *target_note_id;       /* ID of the note being acted upon (nullable) */
    char *content_preview;      /* Preview of content (nullable) */
    gint64 created_at;          /* Timestamp */
    gboolean is_read;           /* Whether this notification has been read */
    guint64 zap_amount_msats;   /* Zap amount in millisats (for ZAP type) */
} GnostrNotification;

GnostrNotificationsView *gnostr_notifications_view_new(void);

/**
 * Add a notification to the view
 * @self: the notifications view
 * @notif: notification data (copied internally)
 */
void gnostr_notifications_view_add_notification(GnostrNotificationsView *self,
                                                  const GnostrNotification *notif);

/**
 * Update an existing notification
 * @self: the notifications view
 * @notif: notification data with matching id
 */
void gnostr_notifications_view_update_notification(GnostrNotificationsView *self,
                                                     const GnostrNotification *notif);

/**
 * Remove a notification
 * @self: the notifications view
 * @notification_id: ID of notification to remove
 */
void gnostr_notifications_view_remove_notification(GnostrNotificationsView *self,
                                                     const char *notification_id);

/**
 * Clear all notifications
 */
void gnostr_notifications_view_clear(GnostrNotificationsView *self);

/**
 * Mark a notification as read
 * @self: the notifications view
 * @notification_id: ID of notification to mark read
 */
void gnostr_notifications_view_mark_read(GnostrNotificationsView *self,
                                          const char *notification_id);

/**
 * Mark all notifications as read
 */
void gnostr_notifications_view_mark_all_read(GnostrNotificationsView *self);

/**
 * Get the count of unread notifications
 */
guint gnostr_notifications_view_get_unread_count(GnostrNotificationsView *self);

/**
 * Set the logged-in user's pubkey (for filtering relevant notifications)
 */
void gnostr_notifications_view_set_user_pubkey(GnostrNotificationsView *self,
                                                const char *pubkey_hex);

/**
 * Show/hide empty state
 */
void gnostr_notifications_view_set_empty(GnostrNotificationsView *self, gboolean is_empty);

/**
 * Show/hide loading state
 */
void gnostr_notifications_view_set_loading(GnostrNotificationsView *self, gboolean is_loading);

/**
 * Set the last checked timestamp (for determining new notifications)
 */
void gnostr_notifications_view_set_last_checked(GnostrNotificationsView *self, gint64 timestamp);

/**
 * Get the last checked timestamp
 */
gint64 gnostr_notifications_view_get_last_checked(GnostrNotificationsView *self);

/* Helper to free notification data */
void gnostr_notification_free(GnostrNotification *notif);

/* Helper to get human-readable type name */
const char *gnostr_notification_type_name(GnostrNotificationType type);

G_END_DECLS

#endif /* GNOSTR_NOTIFICATIONS_VIEW_H */
