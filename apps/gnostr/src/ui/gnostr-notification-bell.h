/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-notification-bell.h - Notification bell widget for header bar
 *
 * Shows a bell icon with unread notification count badge.
 * Clicking opens a popover with the notification panel.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GNOSTR_NOTIFICATION_BELL_H
#define GNOSTR_NOTIFICATION_BELL_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_NOTIFICATION_BELL (gnostr_notification_bell_get_type())

G_DECLARE_FINAL_TYPE(GnostrNotificationBell, gnostr_notification_bell, GNOSTR, NOTIFICATION_BELL, GtkWidget)

/**
 * gnostr_notification_bell_new:
 *
 * Creates a new notification bell widget.
 * Automatically connects to the badge manager singleton.
 *
 * Returns: (transfer full): A new notification bell widget
 */
GnostrNotificationBell *gnostr_notification_bell_new(void);

/**
 * gnostr_notification_bell_set_count:
 * @self: The notification bell
 * @count: Unread notification count
 *
 * Sets the unread count displayed on the badge.
 * Count of 0 hides the badge.
 */
void gnostr_notification_bell_set_count(GnostrNotificationBell *self, guint count);

/**
 * gnostr_notification_bell_get_count:
 * @self: The notification bell
 *
 * Returns: Current unread count
 */
guint gnostr_notification_bell_get_count(GnostrNotificationBell *self);

/**
 * gnostr_notification_bell_set_panel:
 * @self: The notification bell
 * @panel: The notifications view widget to show in popover
 *
 * Sets the panel widget shown when the bell is clicked.
 */
void gnostr_notification_bell_set_panel(GnostrNotificationBell *self, GtkWidget *panel);

/**
 * gnostr_notification_bell_show_popover:
 * @self: The notification bell
 *
 * Programmatically opens the notification popover.
 */
void gnostr_notification_bell_show_popover(GnostrNotificationBell *self);

/**
 * gnostr_notification_bell_hide_popover:
 * @self: The notification bell
 *
 * Programmatically closes the notification popover.
 */
void gnostr_notification_bell_hide_popover(GnostrNotificationBell *self);

G_END_DECLS

#endif /* GNOSTR_NOTIFICATION_BELL_H */
