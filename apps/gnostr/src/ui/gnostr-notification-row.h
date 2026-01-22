#ifndef GNOSTR_NOTIFICATION_ROW_H
#define GNOSTR_NOTIFICATION_ROW_H

#include <gtk/gtk.h>
#include "gnostr-notifications-view.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_NOTIFICATION_ROW (gnostr_notification_row_get_type())

G_DECLARE_FINAL_TYPE(GnostrNotificationRow, gnostr_notification_row, GNOSTR, NOTIFICATION_ROW, GtkWidget)

/**
 * Signals:
 * "open-note" (gchar* note_id_hex, gpointer user_data)
 *   - Emitted when user clicks to open the related note
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user clicks avatar to view profile
 * "mark-read" (gchar* notification_id, gpointer user_data)
 *   - Emitted when notification should be marked as read
 */

typedef struct _GnostrNotificationRow GnostrNotificationRow;

GnostrNotificationRow *gnostr_notification_row_new(void);

/**
 * Set the notification data for this row
 * @self: the row widget
 * @notif: notification data
 */
void gnostr_notification_row_set_notification(GnostrNotificationRow *self,
                                                const GnostrNotification *notif);

/**
 * Get the notification ID for this row
 */
const char *gnostr_notification_row_get_id(GnostrNotificationRow *self);

/**
 * Get the target note ID for this row (may be NULL)
 */
const char *gnostr_notification_row_get_target_note_id(GnostrNotificationRow *self);

/**
 * Get the actor pubkey for this row
 */
const char *gnostr_notification_row_get_actor_pubkey(GnostrNotificationRow *self);

/**
 * Set the read state
 */
void gnostr_notification_row_set_read(GnostrNotificationRow *self, gboolean is_read);

/**
 * Check if notification is read
 */
gboolean gnostr_notification_row_is_read(GnostrNotificationRow *self);

G_END_DECLS

#endif /* GNOSTR_NOTIFICATION_ROW_H */
