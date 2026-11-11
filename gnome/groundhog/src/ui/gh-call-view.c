/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gh-call-view.h"

#include <glib/gi18n.h>

struct _GhCallView {
  AdwBreakpointBin parent_instance;
  AdwWindowTitle *window_title;
  GtkPicture *remote_video;
  GtkWidget *info_box;
  AdwAvatar *avatar;
  GtkLabel *name_label;
  GtkLabel *status_label;
  GtkPicture *local_preview;
  GtkWidget *mute_button;
  GtkWidget *camera_button;
  GtkWidget *switch_video_button;
  GtkWidget *devices_button;
  GtkPopover *devices_popover;
  GtkBox *devices_box;
  GtkWidget *decline_button;
  GtkWidget *answer_video_button;
  GtkWidget *answer_button;
  GtkWidget *hang_up_button;
  gboolean camera_on;
};

G_DEFINE_FINAL_TYPE(GhCallView, gh_call_view, ADW_TYPE_BREAKPOINT_BIN)

static void
gh_call_view_dispose(GObject *object)
{
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_CALL_VIEW);
  G_OBJECT_CLASS(gh_call_view_parent_class)->dispose(object);
}

static void
gh_call_view_class_init(GhCallViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  G_OBJECT_CLASS(klass)->dispose = gh_call_view_dispose;
  gtk_widget_class_set_template_from_resource(widget_class, "/org/nostr/Groundhog/ui/gh-call-view.ui");
#define BIND(name) gtk_widget_class_bind_template_child(widget_class, GhCallView, name)
  BIND(window_title); BIND(remote_video); BIND(info_box); BIND(avatar); BIND(name_label);
  BIND(status_label); BIND(local_preview); BIND(mute_button); BIND(camera_button);
  BIND(switch_video_button); BIND(devices_button); BIND(devices_popover); BIND(devices_box);
  BIND(decline_button); BIND(answer_video_button); BIND(answer_button); BIND(hang_up_button);
#undef BIND
}

static void
gh_call_view_init(GhCallView *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->camera_on = TRUE;
}

GtkWidget *
gh_call_view_new(void)
{
  return g_object_new(GH_TYPE_CALL_VIEW, NULL);
}

void
gh_call_view_set_peer(GhCallView *self, const gchar *name)
{
  g_return_if_fail(GH_IS_CALL_VIEW(self));
  adw_avatar_set_text(self->avatar, name);
  gtk_label_set_text(self->name_label, name ? name : "");
  adw_window_title_set_title(self->window_title, name && *name ? name : _("Call"));
}

const gchar *
gh_call_view_get_peer(GhCallView *self)
{
  g_return_val_if_fail(GH_IS_CALL_VIEW(self), NULL);
  return gtk_label_get_text(self->name_label);
}

void
gh_call_view_set_status(GhCallView *self, const gchar *status)
{
  g_return_if_fail(GH_IS_CALL_VIEW(self));
  gtk_label_set_text(self->status_label, status ? status : "");
  adw_window_title_set_subtitle(self->window_title, status ? status : "");
}

const gchar *
gh_call_view_get_status(GhCallView *self)
{
  g_return_val_if_fail(GH_IS_CALL_VIEW(self), NULL);
  return gtk_label_get_text(self->status_label);
}

GtkWidget *
gh_call_view_get_control(GhCallView *self, GhCallViewControls control)
{
  g_return_val_if_fail(GH_IS_CALL_VIEW(self), NULL);
  switch (control) {
  case GH_CALL_VIEW_MUTE: return self->mute_button;
  case GH_CALL_VIEW_CAMERA: return self->camera_button;
  case GH_CALL_VIEW_SWITCH_VIDEO: return self->switch_video_button;
  case GH_CALL_VIEW_DEVICES: return self->devices_button;
  case GH_CALL_VIEW_DECLINE: return self->decline_button;
  case GH_CALL_VIEW_ANSWER: return self->answer_button;
  case GH_CALL_VIEW_ANSWER_VIDEO: return self->answer_video_button;
  case GH_CALL_VIEW_HANG_UP: return self->hang_up_button;
  default: return NULL;
  }
}

void
gh_call_view_set_controls(GhCallView *self, GhCallViewControls controls)
{
  g_return_if_fail(GH_IS_CALL_VIEW(self));
  for (guint bit = GH_CALL_VIEW_MUTE; bit <= GH_CALL_VIEW_HANG_UP; bit <<= 1)
    gtk_widget_set_visible(gh_call_view_get_control(self, bit), (controls & bit) != 0);
}

static void
set_labelled(GtkWidget *button, const gchar *icon, const gchar *label)
{
  gtk_button_set_icon_name(GTK_BUTTON(button), icon);
  gtk_widget_set_tooltip_text(button, label);
  gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

void
gh_call_view_set_muted(GhCallView *self, gboolean muted)
{
  g_return_if_fail(GH_IS_CALL_VIEW(self));
  set_labelled(self->mute_button,
               muted ? "microphone-disabled-symbolic" : "audio-input-microphone-symbolic",
               muted ? _("Unmute Microphone") : _("Mute Microphone"));
}

void
gh_call_view_set_camera(GhCallView *self, gboolean on)
{
  g_return_if_fail(GH_IS_CALL_VIEW(self));
  self->camera_on = !!on;
  set_labelled(self->camera_button, on ? "camera-video-symbolic" : "camera-disabled-symbolic",
               on ? _("Turn Off Camera") : _("Turn On Camera"));
  gtk_widget_set_visible(GTK_WIDGET(self->local_preview),
                         on && gtk_picture_get_paintable(self->local_preview) != NULL);
}

void
gh_call_view_set_video(GhCallView *self, GdkPaintable *remote, GdkPaintable *local)
{
  g_return_if_fail(GH_IS_CALL_VIEW(self));
  gtk_picture_set_paintable(self->remote_video, remote);
  gtk_picture_set_paintable(self->local_preview, local);
  gtk_widget_set_visible(GTK_WIDGET(self->remote_video), remote != NULL);
  /* Over the remote video only the name and state stay, at the top. */
  gtk_widget_set_valign(self->info_box, remote ? GTK_ALIGN_START : GTK_ALIGN_CENTER);
  gtk_widget_set_visible(GTK_WIDGET(self->avatar), remote == NULL);
  gtk_widget_set_visible(GTK_WIDGET(self->local_preview), local != NULL && self->camera_on);
}

GtkBox *
gh_call_view_get_devices_box(GhCallView *self)
{
  g_return_val_if_fail(GH_IS_CALL_VIEW(self), NULL);
  return self->devices_box;
}

GtkPopover *
gh_call_view_get_devices_popover(GhCallView *self)
{
  g_return_val_if_fail(GH_IS_CALL_VIEW(self), NULL);
  return self->devices_popover;
}
