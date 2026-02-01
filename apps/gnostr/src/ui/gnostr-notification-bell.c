/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-notification-bell.c - Notification bell widget for header bar
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#define G_LOG_DOMAIN "gnostr-notification-bell"

#include "gnostr-notification-bell.h"
#include "../notifications/badge_manager.h"
#include <adwaita.h>

struct _GnostrNotificationBell
{
  GtkWidget parent_instance;

  /* Widgets */
  GtkMenuButton *button;
  GtkImage *icon;
  GtkLabel *badge_label;
  GtkWidget *badge_box;
  GtkPopover *popover;
  GtkWidget *panel;

  /* State */
  guint unread_count;

  /* Badge manager connection */
  gulong badge_changed_handler;
};

G_DEFINE_TYPE(GnostrNotificationBell, gnostr_notification_bell, GTK_TYPE_WIDGET)

enum {
  PROP_0,
  PROP_COUNT,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
update_badge_visibility(GnostrNotificationBell *self)
{
  if (self->unread_count > 0)
    {
      gchar *text;
      if (self->unread_count > 99)
        text = g_strdup("99+");
      else
        text = g_strdup_printf("%u", self->unread_count);

      gtk_label_set_text(self->badge_label, text);
      gtk_widget_set_visible(self->badge_box, TRUE);
      g_free(text);
    }
  else
    {
      gtk_widget_set_visible(self->badge_box, FALSE);
    }
}

static void
on_badge_changed(GnostrBadgeManager *manager,
                 guint               total_count,
                 gpointer            user_data)
{
  GnostrNotificationBell *self = GNOSTR_NOTIFICATION_BELL(user_data);
  gnostr_notification_bell_set_count(self, total_count);
}

static void
gnostr_notification_bell_dispose(GObject *object)
{
  GnostrNotificationBell *self = GNOSTR_NOTIFICATION_BELL(object);

  /* Clear the badge manager callback */
  if (self->badge_changed_handler > 0)
    {
      GnostrBadgeManager *manager = gnostr_badge_manager_get_default();
      gnostr_badge_manager_set_changed_callback(manager, NULL, NULL, NULL);
      self->badge_changed_handler = 0;
    }

  /* Clear the popover child */
  if (self->popover)
    {
      gtk_popover_set_child(self->popover, NULL);
      self->panel = NULL;
    }

  /* Unparent child widgets */
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
  if (child)
    gtk_widget_unparent(child);

  G_OBJECT_CLASS(gnostr_notification_bell_parent_class)->dispose(object);
}

static void
gnostr_notification_bell_get_property(GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  GnostrNotificationBell *self = GNOSTR_NOTIFICATION_BELL(object);

  switch (prop_id)
    {
    case PROP_COUNT:
      g_value_set_uint(value, self->unread_count);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
gnostr_notification_bell_set_property(GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  GnostrNotificationBell *self = GNOSTR_NOTIFICATION_BELL(object);

  switch (prop_id)
    {
    case PROP_COUNT:
      gnostr_notification_bell_set_count(self, g_value_get_uint(value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
gnostr_notification_bell_class_init(GnostrNotificationBellClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_notification_bell_dispose;
  object_class->get_property = gnostr_notification_bell_get_property;
  object_class->set_property = gnostr_notification_bell_set_property;

  properties[PROP_COUNT] =
    g_param_spec_uint("count",
                      "Count",
                      "Unread notification count",
                      0, G_MAXUINT, 0,
                      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties(object_class, N_PROPS, properties);

  /* Use BoxLayout for the widget */
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "notification-bell");
}

static void
gnostr_notification_bell_init(GnostrNotificationBell *self)
{
  self->unread_count = 0;

  /* Create an overlay to stack the badge on the button */
  GtkWidget *overlay = gtk_overlay_new();
  gtk_widget_set_parent(overlay, GTK_WIDGET(self));

  /* Create the menu button */
  self->button = GTK_MENU_BUTTON(gtk_menu_button_new());
  gtk_menu_button_set_icon_name(self->button, "preferences-system-notifications-symbolic");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->button), "Notifications");
  gtk_widget_add_css_class(GTK_WIDGET(self->button), "flat");
  gtk_overlay_set_child(GTK_OVERLAY(overlay), GTK_WIDGET(self->button));

  /* Create the badge (overlay on top-right) */
  self->badge_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_halign(self->badge_box, GTK_ALIGN_END);
  gtk_widget_set_valign(self->badge_box, GTK_ALIGN_START);
  gtk_widget_add_css_class(self->badge_box, "notification-badge");
  gtk_widget_set_visible(self->badge_box, FALSE);

  self->badge_label = GTK_LABEL(gtk_label_new("0"));
  gtk_widget_add_css_class(GTK_WIDGET(self->badge_label), "caption");
  gtk_box_append(GTK_BOX(self->badge_box), GTK_WIDGET(self->badge_label));

  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), self->badge_box);

  /* Create the popover for the notification panel */
  self->popover = GTK_POPOVER(gtk_popover_new());
  gtk_popover_set_has_arrow(self->popover, TRUE);
  gtk_widget_set_size_request(GTK_WIDGET(self->popover), 380, 450);
  gtk_menu_button_set_popover(self->button, GTK_WIDGET(self->popover));

  /* Connect to the badge manager */
  GnostrBadgeManager *manager = gnostr_badge_manager_get_default();
  gnostr_badge_manager_set_changed_callback(manager, on_badge_changed, self, NULL);
  self->badge_changed_handler = 1; /* Mark as connected */

  /* Get initial count */
  guint initial_count = gnostr_badge_manager_get_total_count(manager);
  if (initial_count > 0)
    gnostr_notification_bell_set_count(self, initial_count);
}

GnostrNotificationBell *
gnostr_notification_bell_new(void)
{
  return g_object_new(GNOSTR_TYPE_NOTIFICATION_BELL, NULL);
}

void
gnostr_notification_bell_set_count(GnostrNotificationBell *self, guint count)
{
  g_return_if_fail(GNOSTR_IS_NOTIFICATION_BELL(self));

  if (self->unread_count == count)
    return;

  self->unread_count = count;
  update_badge_visibility(self);

  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_COUNT]);
}

guint
gnostr_notification_bell_get_count(GnostrNotificationBell *self)
{
  g_return_val_if_fail(GNOSTR_IS_NOTIFICATION_BELL(self), 0);
  return self->unread_count;
}

void
gnostr_notification_bell_set_panel(GnostrNotificationBell *self, GtkWidget *panel)
{
  g_return_if_fail(GNOSTR_IS_NOTIFICATION_BELL(self));

  self->panel = panel;
  if (self->popover)
    gtk_popover_set_child(self->popover, panel);
}

void
gnostr_notification_bell_show_popover(GnostrNotificationBell *self)
{
  g_return_if_fail(GNOSTR_IS_NOTIFICATION_BELL(self));
  if (self->popover)
    gtk_popover_popup(self->popover);
}

void
gnostr_notification_bell_hide_popover(GnostrNotificationBell *self)
{
  g_return_if_fail(GNOSTR_IS_NOTIFICATION_BELL(self));
  if (self->popover)
    gtk_popover_popdown(self->popover);
}
