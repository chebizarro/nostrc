/*
 * gn-video-player.c
 *
 * Video player widget with custom controls overlay, ported from Gnostr's
 * gnostr-video-player.c (nostrc-8xfib.4). Gnostr's settings and remote-media
 * check moved to the host's GnMediaSource; remote URLs never reach the media
 * backend from here, and no GtkMediaFile is created until there is media.
 */

#include <nostr-gtk-1.0/gn-video-player.h>
#include <adwaita.h>
#include "gn-portable-i18n-private.h"
#include "gn-media-decode-private.h"

/* nostrc-8xfib.4: the look Gnostr shipped in gnostr.css (.image-viewer-*,
 * .video-*), renamed gn-media-* and gn-video-*, installed once by the
 * library so every host gets it. */
static const char *media_css =
  "window.gn-media-viewer { background-color: alpha(black, 0.92); }\n"
  "window.gn-media-viewer scrolledwindow, window.gn-media-viewer picture,"
  " window.gn-media-viewer viewport { background: transparent; }\n"
  ".gn-media-viewer .gn-media-osd { background-color: alpha(black, 0.5); color: white;"
  " border-radius: 24px; padding: 8px 16px; }\n"
  ".gn-media-viewer .gn-media-osd label { color: white; font-weight: 500; }\n"
  ".gn-media-viewer .gn-media-zoom { background-color: alpha(black, 0.5); color: white;"
  " padding: 6px 12px; border-radius: 6px; font-size: 13px; font-weight: 500; }\n"
  ".gn-media-viewer .gn-media-message { color: white; }\n"
  ".gn-media-viewer button.gn-media-button { color: white; background: transparent;"
  " min-width: 32px; min-height: 32px; border: none; box-shadow: none; }\n"
  ".gn-media-viewer button.gn-media-button:hover { background: alpha(white, 0.15); }\n"
  ".gn-media-viewer button.gn-media-button:disabled { opacity: 0.3; }\n"
  ".gn-media-viewer button.gn-media-close, .gn-media-viewer button.gn-media-tool {"
  " background: alpha(black, 0.5); color: white; min-width: 36px; min-height: 36px;"
  " border: none; box-shadow: none; }\n"
  ".gn-media-viewer button.gn-media-close:hover, .gn-media-viewer button.gn-media-tool:hover {"
  " background: alpha(black, 0.7); }\n"
  "gn-video-player { border-radius: 8px; background: #000; }\n"
  ".gn-video-content, .gn-video-content-fullscreen { background: #000; }\n"
  ".gn-video-controls { background: linear-gradient(to top, alpha(black, 0.8) 0%,"
  " alpha(black, 0.6) 50%, alpha(black, 0) 100%); padding: 12px 16px;"
  " border-radius: 0 0 8px 8px; opacity: 1; transition: opacity 300ms ease; }\n"
  ".gn-video-controls:not(.controls-visible) { opacity: 0; }\n"
  ".gn-video-controls.controls-visible { opacity: 1; }\n"
  ".gn-video-error { padding: 24px; color: @error_color; }\n"
  ".gn-video-error image { opacity: 0.7; margin-bottom: 8px; }\n"
  ".gn-video-error .gn-video-error-title { font-size: 16px; font-weight: 600; color: white; }\n"
  ".gn-video-error .gn-video-error-detail { font-size: 12px; color: alpha(white, 0.6);"
  " margin-top: 4px; }\n"
  ".gn-video-load label { color: white; }\n"
  ".gn-video-seek-row { margin-bottom: 8px; }\n"
  ".gn-video-time { font-size: 12px; color: white; min-width: 45px; }\n"
  ".gn-video-seek { min-height: 6px; }\n"
  ".gn-video-seek trough { background: alpha(white, 0.3); border-radius: 3px; min-height: 6px; }\n"
  ".gn-video-seek highlight { background: @accent_bg_color; border-radius: 3px; }\n"
  ".gn-video-seek slider, .gn-video-volume slider { background: white; border-radius: 50%;"
  " min-width: 14px; min-height: 14px; margin: -4px; border: none; }\n"
  ".gn-video-button-row { margin-top: 4px; }\n"
  ".gn-video-control-btn { color: white; min-width: 36px; min-height: 36px; padding: 8px;"
  " border-radius: 50%; transition: background 150ms ease; }\n"
  ".gn-video-control-btn:hover { background: alpha(white, 0.15); }\n"
  ".gn-video-control-btn:active { background: alpha(white, 0.25); }\n"
  ".gn-video-volume { min-height: 4px; }\n"
  ".gn-video-volume trough { background: alpha(white, 0.3); border-radius: 2px; min-height: 4px; }\n"
  ".gn-video-volume highlight { background: white; border-radius: 2px; }\n"
  ".gn-video-content-fullscreen + .gn-video-controls { border-radius: 0; padding: 16px 24px; }\n";

void
gn_media_install_css(void)
{
  static gboolean installed;
  GdkDisplay *display = gdk_display_get_default();
  if (installed || !display) return;
  installed = TRUE;
  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(provider, media_css, -1);
  gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

/* nostrc-sykf: One-time check for GTK media backend availability.
 * Cached to avoid repeated "could not find a media module" warnings. */
static int s_media_backend_status = -1; /* -1 = unchecked, 0 = unavailable, 1 = available */

/* Controls auto-hide timeout in seconds */
#define CONTROLS_HIDE_TIMEOUT_SEC 3



struct _GnVideoPlayer {
  GtkWidget parent_instance;

  /* Main container */
  GtkWidget *overlay;
  GtkWidget *picture;         /* GtkPicture widget (displays media as paintable, no controls) */
  GtkMediaStream *stream;     /* what plays, or NULL */
  gboolean owns_stream;       /* a GtkMediaFile made by set_file/the source */
  GnMediaSource *source;      /* host plumbing, or NULL */
  GtkWidget *load_box;        /* Load action / blocked message */
  GtkWidget *load_label;
  GtkWidget *load_button;
  GtkWidget *controls_box;    /* Controls overlay */

  /* Control buttons */
  GtkWidget *btn_play_pause;
  GtkWidget *play_icon;
  GtkWidget *pause_icon;
  GtkWidget *btn_stop;        /* Stop button */
  GtkWidget *seek_scale;
  GtkWidget *lbl_time_current;
  GtkWidget *lbl_time_duration;
  GtkWidget *btn_mute;
  GtkWidget *volume_scale;
  GtkWidget *btn_loop;
  GtkWidget *btn_fullscreen;

  /* State */
  char *url;
  gboolean autoplay;
  gboolean loop;
  gboolean muted;
  double volume;
  gboolean is_fullscreen;
  gboolean controls_visible;
  gboolean seeking;         /* TRUE while user is dragging seek bar */

  /* Fullscreen window */
  GtkWidget *fullscreen_window;
  GtkWidget *fullscreen_toast_overlay;  /* AdwToastOverlay for fullscreen toast */
  GtkWidget *fullscreen_overlay;
  GtkWidget *fullscreen_controls_box;

  /* Timers */
  guint controls_hide_timer_id;
  guint position_update_timer_id;
  guint loading_timeout_id;        /* Timeout for loading state (shows error if exceeded) */


  /* Motion controller for controls visibility */
  GtkEventController *motion_controller;

  /* Auto-pause when scrolled out of view */
  gboolean was_playing_before_scroll;  /* Track if video was playing before scroll-out */
  gboolean is_visible_in_viewport;     /* Whether currently visible in scrolled parent */
  gulong scroll_adj_changed_handler;   /* Signal handler for scroll adjustment changes */
  GtkAdjustment *scroll_vadjustment;   /* Vertical adjustment of parent scroll */

  /* Error state */
  GtkWidget *error_box;              /* Error overlay shown when video fails */
  GtkWidget *loading_spinner;        /* Loading spinner shown while preparing */
  gboolean has_error;                /* TRUE if video failed to load */

  /* Media stream signal handlers */
  gulong media_error_handler;
  gulong media_prepared_handler;

  /* Disposal flag */
  gboolean disposed;
};

G_DEFINE_TYPE(GnVideoPlayer, gn_video_player, GTK_TYPE_WIDGET)

/* Forward declarations */
static void create_controls_overlay(GnVideoPlayer *self, GtkWidget *parent_overlay, GtkWidget **controls_box_out);
static void update_time_labels(GnVideoPlayer *self);
static void update_play_pause_icon(GnVideoPlayer *self);
static gboolean hide_controls_timeout(gpointer user_data);
static void show_controls(GnVideoPlayer *self);
static void schedule_hide_controls(GnVideoPlayer *self);
static gboolean position_update_tick(gpointer user_data);
static void show_error_state(GnVideoPlayer *self, const char *message);
static void show_loading_state(GnVideoPlayer *self, gboolean loading);
static void cancel_loading_timeout(GnVideoPlayer *self);

/* Icon-only buttons: the name is the tooltip and what screen readers say
 * (nostrc-8xfib.4, a11y from the alpha-6 viewer). */
static void
set_button_name(GtkWidget *button, const char *name)
{
  gtk_widget_set_tooltip_text(button, name);
  gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
}

/* Media stream error callback - called when video fails to load or play */
static void on_media_error(GtkMediaStream *stream, GParamSpec *pspec, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  (void)pspec;

  if (!GN_IS_VIDEO_PLAYER(self) || self->disposed) return;

  /* Cancel loading timeout since we got an error */
  cancel_loading_timeout(self);

  const GError *error = gtk_media_stream_get_error(stream);
  if (error) {
    g_warning("Video playback error: %s", error->message);
    show_error_state(self, error->message);
  }
}

/* Media stream prepared callback - called when video metadata is available and ready to play */
static void on_media_prepared(GtkMediaStream *stream, GParamSpec *pspec, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  (void)pspec;

  if (!GN_IS_VIDEO_PLAYER(self) || self->disposed) return;

  gboolean prepared = gtk_media_stream_is_prepared(stream);
  if (prepared) {
    /* Cancel loading timeout since video is ready */
    cancel_loading_timeout(self);

    g_debug("Video prepared: %s", self->url ? self->url : "(null)");
    show_loading_state(self, FALSE);
    /* If autoplay is enabled and no error, start playing */
    if (self->autoplay && !self->has_error) {
      gtk_media_stream_play(stream);
    }
    update_time_labels(self);
  }
}

/* Show error overlay when video fails to load */
static void show_error_state(GnVideoPlayer *self, const char *message) {
  self->has_error = TRUE;
  show_loading_state(self, FALSE);

  /* Hide the picture and controls */
  if (GTK_IS_WIDGET(self->picture)) {
    gtk_widget_set_visible(self->picture, FALSE);
  }
  if (GTK_IS_WIDGET(self->controls_box)) {
    gtk_widget_set_visible(self->controls_box, FALSE);
  }

  /* Create error box if not exists */
  if (!self->error_box) {
    self->error_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(self->error_box, "gn-video-error");
    gtk_widget_set_halign(self->error_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(self->error_box, GTK_ALIGN_CENTER);

    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-error-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 48);
    gtk_box_append(GTK_BOX(self->error_box), icon);

    GtkWidget *label = gtk_label_new(_("Video unavailable"));
    gtk_widget_add_css_class(label, "gn-video-error-title");
    gtk_box_append(GTK_BOX(self->error_box), label);

    GtkWidget *detail = gtk_label_new(NULL);
    gtk_widget_add_css_class(detail, "gn-video-error-detail");
    gtk_label_set_wrap(GTK_LABEL(detail), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(detail), 40);
    g_object_set_data(G_OBJECT(self->error_box), "detail-label", detail);
    gtk_box_append(GTK_BOX(self->error_box), detail);

    gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->error_box);
  }

  /* Update error detail */
  GtkWidget *detail = g_object_get_data(G_OBJECT(self->error_box), "detail-label");
  if (GTK_IS_LABEL(detail) && message) {
    gtk_label_set_text(GTK_LABEL(detail), message);
  }

  gtk_widget_set_visible(self->error_box, TRUE);
}

/* Show/hide loading spinner */
static void show_loading_state(GnVideoPlayer *self, gboolean loading) {
  if (loading) {
    if (!self->loading_spinner) {
      self->loading_spinner = gtk_spinner_new();
      gtk_widget_set_size_request(self->loading_spinner, 48, 48);
      gtk_widget_set_halign(self->loading_spinner, GTK_ALIGN_CENTER);
      gtk_widget_set_valign(self->loading_spinner, GTK_ALIGN_CENTER);
      gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->loading_spinner);
    }
    gtk_spinner_start(GTK_SPINNER(self->loading_spinner));
    gtk_widget_set_visible(self->loading_spinner, TRUE);
  } else {
    if (GTK_IS_SPINNER(self->loading_spinner)) {
      gtk_spinner_stop(GTK_SPINNER(self->loading_spinner));
      gtk_widget_set_visible(self->loading_spinner, FALSE);
    }
  }
}

/* Cancel the loading timeout timer */
static void cancel_loading_timeout(GnVideoPlayer *self) {
  if (self->loading_timeout_id > 0) {
    g_source_remove(self->loading_timeout_id);
    self->loading_timeout_id = 0;
  }
}

/* Format time in MM:SS or HH:MM:SS format */
static char *format_time(gint64 microseconds) {
  gint64 seconds = microseconds / 1000000;
  if (seconds < 0) seconds = 0;

  gint64 hours = seconds / 3600;
  gint64 minutes = (seconds % 3600) / 60;
  gint64 secs = seconds % 60;

  if (hours > 0) {
    return g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT,
                           hours, minutes, secs);
  } else {
    return g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, minutes, secs);
  }
}

static void on_play_pause_clicked(GtkButton *btn, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  (void)btn;
  gn_video_player_toggle_playback(self);
}

static void on_stop_clicked(GtkButton *btn, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  (void)btn;
  gn_video_player_stop(self);
}

static void on_mute_clicked(GtkButton *btn, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  (void)btn;
  gn_video_player_set_muted(self, !self->muted);
}

static void on_fullscreen_clicked(GtkButton *btn, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  (void)btn;
  gn_video_player_set_fullscreen(self, !self->is_fullscreen);
}

static void on_loop_clicked(GtkButton *btn, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  (void)btn;
  gn_video_player_set_loop(self, !self->loop);
}

static void on_seek_value_changed(GtkRange *range, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);

  if (!self->seeking) return;

  GtkMediaStream *stream = self->stream;
  if (!stream) return;

  double value = gtk_range_get_value(range);
  gint64 duration = gtk_media_stream_get_duration(stream);
  gint64 position = (gint64)(value * duration);

  gtk_media_stream_seek(stream, position);
}

static gboolean on_seek_button_press(GtkGestureClick *gesture, gint n_press, double x, double y, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  (void)gesture; (void)n_press; (void)x; (void)y;
  self->seeking = TRUE;
  return FALSE;
}

static void on_seek_button_release(GtkGestureClick *gesture, gint n_press, double x, double y, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  (void)gesture; (void)n_press; (void)x; (void)y;
  self->seeking = FALSE;
}

static void on_volume_value_changed(GtkRange *range, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  double value = gtk_range_get_value(range);
  gn_video_player_set_volume(self, value);
}

static void on_motion_enter(GtkEventControllerMotion *controller, double x, double y, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  (void)controller; (void)x; (void)y;
  show_controls(self);
}

static void on_motion(GtkEventControllerMotion *controller, double x, double y, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  (void)controller; (void)x; (void)y;
  show_controls(self);
  schedule_hide_controls(self);
}

static void on_motion_leave(GtkEventControllerMotion *controller, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  (void)controller;
  schedule_hide_controls(self);
}

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  (void)controller; (void)keycode; (void)state;

  switch (keyval) {
    case GDK_KEY_Escape:
      if (self->is_fullscreen) {
        gn_video_player_set_fullscreen(self, FALSE);
        return TRUE;
      }
      break;
    case GDK_KEY_space:
    case GDK_KEY_k:
      gn_video_player_toggle_playback(self);
      return TRUE;
    case GDK_KEY_f:
      gn_video_player_set_fullscreen(self, !self->is_fullscreen);
      return TRUE;
    case GDK_KEY_m:
      gn_video_player_set_muted(self, !self->muted);
      return TRUE;
  }
  return FALSE;
}

static void on_fullscreen_window_close_request(GtkWindow *window, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  (void)window;
  gn_video_player_set_fullscreen(self, FALSE);
}

static void show_controls(GnVideoPlayer *self) {
  if (self->disposed || self->controls_visible) return;

  self->controls_visible = TRUE;

  /* Show controls in current mode */
  if (self->is_fullscreen && self->fullscreen_controls_box) {
    gtk_widget_set_visible(self->fullscreen_controls_box, TRUE);
    gtk_widget_add_css_class(self->fullscreen_controls_box, "controls-visible");
  } else if (self->controls_box) {
    gtk_widget_set_visible(self->controls_box, TRUE);
    gtk_widget_add_css_class(self->controls_box, "controls-visible");
  }
}

static gboolean hide_controls_timeout(gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);

  if (self->disposed) return G_SOURCE_REMOVE;

  self->controls_hide_timer_id = 0;
  self->controls_visible = FALSE;

  /* Hide controls in current mode */
  if (self->is_fullscreen && self->fullscreen_controls_box) {
    gtk_widget_remove_css_class(self->fullscreen_controls_box, "controls-visible");
    /* Don't completely hide, just fade out via CSS */
  } else if (self->controls_box) {
    gtk_widget_remove_css_class(self->controls_box, "controls-visible");
  }

  return G_SOURCE_REMOVE;
}

static void schedule_hide_controls(GnVideoPlayer *self) {
  if (self->disposed) return;

  /* Cancel existing timer */
  if (self->controls_hide_timer_id > 0) {
    g_source_remove(self->controls_hide_timer_id);
    self->controls_hide_timer_id = 0;
  }

  /* LEGITIMATE TIMEOUT - Auto-hide controls after inactivity.
   * nostrc-b0h: Audited - standard video player UX.
   * Ref-hold self to prevent UAF if widget is destroyed before timer fires. */
  self->controls_hide_timer_id = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
                                                             CONTROLS_HIDE_TIMEOUT_SEC,
                                                             hide_controls_timeout,
                                                             g_object_ref(self),
                                                             g_object_unref);
}

static gboolean position_update_tick(gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);

  if (self->disposed) return G_SOURCE_REMOVE;

  GtkMediaStream *stream = self->stream;
  if (!stream) return G_SOURCE_CONTINUE;

  update_time_labels(self);

  /* Update seek bar if not currently seeking */
  if (!self->seeking && GTK_IS_RANGE(self->seek_scale)) {
    gint64 position = gtk_media_stream_get_timestamp(stream);
    gint64 duration = gtk_media_stream_get_duration(stream);
    if (duration > 0) {
      double fraction = (double)position / (double)duration;
      g_signal_handlers_block_by_func(self->seek_scale, on_seek_value_changed, self);
      gtk_range_set_value(GTK_RANGE(self->seek_scale), fraction);
      g_signal_handlers_unblock_by_func(self->seek_scale, on_seek_value_changed, self);
    }
  }

  /* Update play/pause icon based on playing state */
  update_play_pause_icon(self);

  return G_SOURCE_CONTINUE;
}

static void update_time_labels(GnVideoPlayer *self) {
  if (self->disposed) return;
  
  GtkMediaStream *stream = self->stream;
  if (!stream) return;

  gint64 position = gtk_media_stream_get_timestamp(stream);
  gint64 duration = gtk_media_stream_get_duration(stream);

  char *pos_str = format_time(position);
  char *dur_str = format_time(duration);

  if (GTK_IS_LABEL(self->lbl_time_current)) {
    gtk_label_set_text(GTK_LABEL(self->lbl_time_current), pos_str);
  }
  if (GTK_IS_LABEL(self->lbl_time_duration)) {
    gtk_label_set_text(GTK_LABEL(self->lbl_time_duration), dur_str);
  }

  g_free(pos_str);
  g_free(dur_str);
}

static void update_play_pause_icon(GnVideoPlayer *self) {
  if (self->disposed) return;
  
  GtkMediaStream *stream = self->stream;
  gboolean playing = stream && gtk_media_stream_get_playing(stream);

  if (GTK_IS_WIDGET(self->play_icon) && GTK_IS_WIDGET(self->pause_icon)) {
    gtk_widget_set_visible(self->play_icon, !playing);
    gtk_widget_set_visible(self->pause_icon, playing);
  }

  /* Update button tooltip */
  if (GTK_IS_WIDGET(self->btn_play_pause)) {
    set_button_name(self->btn_play_pause, playing ? _("Pause") : _("Play"));
  }
}

static void update_mute_icon(GnVideoPlayer *self) {
  if (!GTK_IS_BUTTON(self->btn_mute)) return;

  const char *icon_name = self->muted ? "audio-volume-muted-symbolic" : "audio-volume-high-symbolic";
  gtk_button_set_icon_name(GTK_BUTTON(self->btn_mute), icon_name);
  set_button_name(self->btn_mute, self->muted ? _("Unmute") : _("Mute"));
}

static void update_loop_icon(GnVideoPlayer *self) {
  if (!GTK_IS_BUTTON(self->btn_loop)) return;

  const char *icon_name = self->loop ? "media-playlist-repeat-symbolic" : "media-playlist-consecutive-symbolic";
  gtk_button_set_icon_name(GTK_BUTTON(self->btn_loop), icon_name);
  set_button_name(self->btn_loop, self->loop ? _("Loop enabled") : _("Loop disabled"));
}

static void create_controls_overlay(GnVideoPlayer *self, GtkWidget *parent_overlay, GtkWidget **controls_box_out) {
  /* Controls container - positioned at bottom */
  GtkWidget *controls_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(controls_box, "gn-video-controls");
  gtk_widget_set_halign(controls_box, GTK_ALIGN_FILL);
  gtk_widget_set_valign(controls_box, GTK_ALIGN_END);
  gtk_widget_set_margin_start(controls_box, 8);
  gtk_widget_set_margin_end(controls_box, 8);
  gtk_widget_set_margin_bottom(controls_box, 8);

  /* Seek bar row */
  GtkWidget *seek_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(seek_row, "gn-video-seek-row");

  /* Time labels */
  GtkWidget *lbl_current = gtk_label_new("00:00");
  gtk_widget_add_css_class(lbl_current, "gn-video-time");
  gtk_widget_add_css_class(lbl_current, "monospace");

  GtkWidget *seek = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.001);
  gtk_widget_set_hexpand(seek, TRUE);
  gtk_widget_add_css_class(seek, "gn-video-seek");
  gtk_scale_set_draw_value(GTK_SCALE(seek), FALSE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(seek), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Position"), -1);

  /* Add gesture for seek drag detection */
  GtkGesture *seek_gesture = gtk_gesture_click_new();
  g_signal_connect(seek_gesture, "pressed", G_CALLBACK(on_seek_button_press), self);
  g_signal_connect(seek_gesture, "released", G_CALLBACK(on_seek_button_release), self);
  gtk_widget_add_controller(seek, GTK_EVENT_CONTROLLER(seek_gesture));
  g_signal_connect(seek, "value-changed", G_CALLBACK(on_seek_value_changed), self);

  GtkWidget *lbl_duration = gtk_label_new("00:00");
  gtk_widget_add_css_class(lbl_duration, "gn-video-time");
  gtk_widget_add_css_class(lbl_duration, "monospace");

  gtk_box_append(GTK_BOX(seek_row), lbl_current);
  gtk_box_append(GTK_BOX(seek_row), seek);
  gtk_box_append(GTK_BOX(seek_row), lbl_duration);

  /* Button row */
  GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(btn_row, "gn-video-button-row");
  gtk_widget_set_halign(btn_row, GTK_ALIGN_CENTER);

  /* Play/Pause button with stacked icons */
  GtkWidget *btn_play = gtk_button_new();
  gtk_widget_add_css_class(btn_play, "gn-video-control-btn");
  gtk_widget_add_css_class(btn_play, "circular");
  gtk_button_set_has_frame(GTK_BUTTON(btn_play), FALSE);

  GtkWidget *play_icon = gtk_image_new_from_icon_name("media-playback-start-symbolic");
  GtkWidget *pause_icon = gtk_image_new_from_icon_name("media-playback-pause-symbolic");

  /* Stack for play/pause icons */
  GtkWidget *icon_stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(icon_stack), play_icon, "play");
  gtk_stack_add_named(GTK_STACK(icon_stack), pause_icon, "pause");
  gtk_stack_set_visible_child_name(GTK_STACK(icon_stack), "play");
  gtk_button_set_child(GTK_BUTTON(btn_play), icon_stack);

  set_button_name(btn_play, _("Play"));
  g_signal_connect(btn_play, "clicked", G_CALLBACK(on_play_pause_clicked), self);

  /* Stop button */
  GtkWidget *btn_stop = gtk_button_new_from_icon_name("media-playback-stop-symbolic");
  gtk_widget_add_css_class(btn_stop, "gn-video-control-btn");
  gtk_button_set_has_frame(GTK_BUTTON(btn_stop), FALSE);
  set_button_name(btn_stop, _("Stop"));
  g_signal_connect(btn_stop, "clicked", G_CALLBACK(on_stop_clicked), self);

  /* Volume controls */
  GtkWidget *btn_mute = gtk_button_new_from_icon_name("audio-volume-high-symbolic");
  gtk_widget_add_css_class(btn_mute, "gn-video-control-btn");
  gtk_button_set_has_frame(GTK_BUTTON(btn_mute), FALSE);
  set_button_name(btn_mute, self->muted ? _("Unmute") : _("Mute"));
  g_signal_connect(btn_mute, "clicked", G_CALLBACK(on_mute_clicked), self);

  GtkWidget *vol_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.05);
  gtk_widget_set_size_request(vol_scale, 80, -1);
  gtk_widget_add_css_class(vol_scale, "gn-video-volume");
  gtk_scale_set_draw_value(GTK_SCALE(vol_scale), FALSE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(vol_scale), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Volume"), -1);
  gtk_range_set_value(GTK_RANGE(vol_scale), 1.0);
  g_signal_connect(vol_scale, "value-changed", G_CALLBACK(on_volume_value_changed), self);

  /* Loop button */
  const char *loop_icon = self->loop ? "media-playlist-repeat-symbolic" : "media-playlist-consecutive-symbolic";
  GtkWidget *btn_loop = gtk_button_new_from_icon_name(loop_icon);
  gtk_widget_add_css_class(btn_loop, "gn-video-control-btn");
  gtk_button_set_has_frame(GTK_BUTTON(btn_loop), FALSE);
  set_button_name(btn_loop, self->loop ? _("Loop enabled") : _("Loop disabled"));
  g_signal_connect(btn_loop, "clicked", G_CALLBACK(on_loop_clicked), self);

  /* Fullscreen button */
  GtkWidget *btn_fs = gtk_button_new_from_icon_name("view-fullscreen-symbolic");
  gtk_widget_add_css_class(btn_fs, "gn-video-control-btn");
  gtk_button_set_has_frame(GTK_BUTTON(btn_fs), FALSE);
  set_button_name(btn_fs, _("Fullscreen"));
  g_signal_connect(btn_fs, "clicked", G_CALLBACK(on_fullscreen_clicked), self);

  gtk_box_append(GTK_BOX(btn_row), btn_play);
  gtk_box_append(GTK_BOX(btn_row), btn_stop);
  gtk_box_append(GTK_BOX(btn_row), btn_mute);
  gtk_box_append(GTK_BOX(btn_row), vol_scale);
  gtk_box_append(GTK_BOX(btn_row), btn_loop);
  gtk_box_append(GTK_BOX(btn_row), btn_fs);

  gtk_box_append(GTK_BOX(controls_box), seek_row);
  gtk_box_append(GTK_BOX(controls_box), btn_row);

  gtk_overlay_add_overlay(GTK_OVERLAY(parent_overlay), controls_box);

  /* Store references (use first set created for main widget) */
  if (!self->btn_play_pause) {
    self->btn_play_pause = btn_play;
    self->play_icon = play_icon;
    self->pause_icon = pause_icon;
    self->btn_stop = btn_stop;
    self->seek_scale = seek;
    self->lbl_time_current = lbl_current;
    self->lbl_time_duration = lbl_duration;
    self->btn_mute = btn_mute;
    self->volume_scale = vol_scale;
    self->btn_loop = btn_loop;
    self->btn_fullscreen = btn_fs;
  }

  *controls_box_out = controls_box;
}

/* Check if the video player is visible within its scrolled parent viewport */
static gboolean check_visibility_in_viewport(GnVideoPlayer *self) {
  if (!GTK_IS_WIDGET(self)) return FALSE;

  GtkWidget *widget = GTK_WIDGET(self);
  if (!gtk_widget_get_realized(widget)) return FALSE;

  /* Find the nearest GtkScrolledWindow ancestor */
  GtkWidget *scrolled = gtk_widget_get_ancestor(widget, GTK_TYPE_SCROLLED_WINDOW);
  if (!scrolled) return TRUE;  /* No scrolled parent, consider visible */

  /* Get the vertical adjustment */
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
  if (!vadj) return TRUE;

  /* Get scroll position and viewport size */
  double scroll_pos = gtk_adjustment_get_value(vadj);
  double viewport_height = gtk_adjustment_get_page_size(vadj);

  /* Get widget position relative to scrolled window content */
  graphene_point_t point = GRAPHENE_POINT_INIT(0, 0);
  graphene_point_t result;
  GtkWidget *viewport_child = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scrolled));
  if (!viewport_child) return TRUE;

  /* Compute the widget's position relative to the scrollable content */
  if (!gtk_widget_compute_point(widget, viewport_child, &point, &result)) {
    return TRUE;  /* Can't compute, assume visible */
  }

  double widget_top = result.y;
  double widget_height = gtk_widget_get_height(widget);
  double widget_bottom = widget_top + widget_height;

  /* Check if widget overlaps with visible viewport area */
  double viewport_top = scroll_pos;
  double viewport_bottom = scroll_pos + viewport_height;

  /* Consider visible if at least 30% of the video is in view */
  double visible_top = MAX(widget_top, viewport_top);
  double visible_bottom = MIN(widget_bottom, viewport_bottom);
  double visible_height = visible_bottom - visible_top;

  if (visible_height <= 0) return FALSE;  /* Completely out of view */

  double visible_fraction = visible_height / widget_height;
  return visible_fraction >= 0.3;  /* At least 30% visible */
}

static void on_scroll_value_changed(GtkAdjustment *adjustment, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  (void)adjustment;

  if (!GN_IS_VIDEO_PLAYER(self) || self->disposed) return;

  gboolean is_visible = check_visibility_in_viewport(self);

  if (is_visible != self->is_visible_in_viewport) {
    self->is_visible_in_viewport = is_visible;

    GtkMediaStream *stream = self->stream;
    if (!stream) return;

    if (!is_visible) {
      /* Scrolled out of view - pause if playing */
      if (gtk_media_stream_get_playing(stream)) {
        self->was_playing_before_scroll = TRUE;
        gtk_media_stream_pause(stream);
        update_play_pause_icon(self);
      }
    } else {
      /* Scrolled back into view - resume if was playing */
      if (self->was_playing_before_scroll) {
        self->was_playing_before_scroll = FALSE;
        gtk_media_stream_play(stream);
        update_play_pause_icon(self);
      }
    }
  }
}

static void setup_scroll_visibility_tracking(GnVideoPlayer *self) {
  /* Already set up? */
  if (self->scroll_vadjustment) return;

  /* Find the nearest GtkScrolledWindow ancestor */
  GtkWidget *scrolled = gtk_widget_get_ancestor(GTK_WIDGET(self), GTK_TYPE_SCROLLED_WINDOW);
  if (!scrolled) return;

  /* Get the vertical adjustment */
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
  if (!vadj) return;

  /* Store reference and connect signal */
  self->scroll_vadjustment = vadj;
  self->scroll_adj_changed_handler = g_signal_connect(vadj, "value-changed",
                                                       G_CALLBACK(on_scroll_value_changed), self);
  self->is_visible_in_viewport = TRUE;
  self->was_playing_before_scroll = FALSE;
}

static void on_video_player_realize(GtkWidget *widget, gpointer user_data) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(user_data);
  (void)widget;
  setup_scroll_visibility_tracking(self);
}

/* nostrc-8xfib.4: Gnostr's teardown order (disconnect, pause, rewind, clear
 * the file so backend audio threads stop before the last unref), applied in
 * full only to a stream the player made; a host's stream is only paused. */
static void
release_stream(GnVideoPlayer *self)
{
  if (self->is_fullscreen) gn_video_player_set_fullscreen(self, FALSE);
  if (self->position_update_timer_id > 0) {
    g_source_remove(self->position_update_timer_id);
    self->position_update_timer_id = 0;
  }
  if (!self->stream) return;
  GtkMediaStream *stream = self->stream;
  /* nostrc-sigf: a failed GStreamer stream may already have dropped them. */
  if (self->media_error_handler > 0 &&
      g_signal_handler_is_connected(G_OBJECT(stream), self->media_error_handler))
    g_signal_handler_disconnect(stream, self->media_error_handler);
  self->media_error_handler = 0;
  if (self->media_prepared_handler > 0 &&
      g_signal_handler_is_connected(G_OBJECT(stream), self->media_prepared_handler))
    g_signal_handler_disconnect(stream, self->media_prepared_handler);
  self->media_prepared_handler = 0;
  gtk_media_stream_pause(stream);
  if (self->owns_stream) {
    gtk_media_stream_seek(stream, 0);
    if (GTK_IS_MEDIA_FILE(stream)) gtk_media_file_clear(GTK_MEDIA_FILE(stream));
  }
  if (GTK_IS_PICTURE(self->picture)) gtk_picture_set_paintable(GTK_PICTURE(self->picture), NULL);
  g_clear_object(&self->stream);
  self->owns_stream = FALSE;
  self->was_playing_before_scroll = FALSE;
}

static void
hide_states(GnVideoPlayer *self)
{
  self->has_error = FALSE;
  cancel_loading_timeout(self);
  show_loading_state(self, FALSE);
  if (GTK_IS_WIDGET(self->error_box)) gtk_widget_set_visible(self->error_box, FALSE);
  if (GTK_IS_WIDGET(self->load_box)) gtk_widget_set_visible(self->load_box, FALSE);
  if (GTK_IS_WIDGET(self->picture)) gtk_widget_set_visible(self->picture, TRUE);
  if (GTK_IS_WIDGET(self->controls_box)) gtk_widget_set_visible(self->controls_box, TRUE);
}

static void
attach_stream(GnVideoPlayer *self, GtkMediaStream *stream, gboolean owned)
{
  if (stream && stream == self->stream) return;
  release_stream(self);
  hide_states(self);
  if (!stream) return;
  self->stream = g_object_ref(stream);
  self->owns_stream = owned;
  gtk_picture_set_paintable(GTK_PICTURE(self->picture), GDK_PAINTABLE(stream));
  self->media_error_handler = g_signal_connect(stream, "notify::error",
                                               G_CALLBACK(on_media_error), self);
  self->media_prepared_handler = g_signal_connect(stream, "notify::prepared",
                                                  G_CALLBACK(on_media_prepared), self);
  gtk_media_stream_set_loop(stream, self->loop);
  gtk_media_stream_set_muted(stream, self->muted);
  gtk_media_stream_set_volume(stream, self->volume);
  /* LEGITIMATE TIMEOUT - Position slider update during playback (250ms).
   * nostrc-b0h: Audited - polling playback position is standard. Runs only
   * while there is a stream (nostrc-8xfib.4: no timer per idle card) and
   * holds no reference: a strong one kept every unparented player alive;
   * dispose (via release_stream) removes it. */
  self->position_update_timer_id = g_timeout_add(250, position_update_tick, self);
  if (gtk_media_stream_get_error(stream))
    on_media_error(stream, NULL, self);
  else if (gtk_media_stream_is_prepared(stream))
    on_media_prepared(stream, NULL, self);
  else
    show_loading_state(self, TRUE);
}

static void gn_video_player_dispose(GObject *obj) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(obj);

  /* Mark as disposed FIRST to prevent callbacks from accessing widget state */
  self->disposed = TRUE;

  /* Disconnect scroll adjustment handler */
  if (self->scroll_vadjustment && self->scroll_adj_changed_handler > 0) {
    g_signal_handler_disconnect(self->scroll_vadjustment, self->scroll_adj_changed_handler);
    self->scroll_adj_changed_handler = 0;
    self->scroll_vadjustment = NULL;
  }

  /* Cancel timers */
  if (self->controls_hide_timer_id > 0) {
    g_source_remove(self->controls_hide_timer_id);
    self->controls_hide_timer_id = 0;
  }
  if (self->position_update_timer_id > 0) {
    g_source_remove(self->position_update_timer_id);
    self->position_update_timer_id = 0;
  }
  cancel_loading_timeout(self);

  /* Close fullscreen window if open */
  if (self->fullscreen_window) {
    gtk_window_destroy(GTK_WINDOW(self->fullscreen_window));
    self->fullscreen_window = NULL;
  }

  release_stream(self);
  g_clear_object(&self->source);

  /* Clear widget pointers before unparenting to prevent dangling references */
  self->btn_play_pause = NULL;
  self->play_icon = NULL;
  self->pause_icon = NULL;
  self->btn_stop = NULL;
  self->seek_scale = NULL;
  self->lbl_time_current = NULL;
  self->lbl_time_duration = NULL;
  self->btn_mute = NULL;
  self->volume_scale = NULL;
  self->btn_loop = NULL;
  self->btn_fullscreen = NULL;
  self->controls_box = NULL;
  self->picture = NULL;
  self->error_box = NULL;
  self->loading_spinner = NULL;

  /* Unparent the overlay (which contains picture and controls) */
  if (self->overlay) {
    gtk_widget_unparent(self->overlay);
    self->overlay = NULL;
  }

  G_OBJECT_CLASS(gn_video_player_parent_class)->dispose(obj);
}

static void gn_video_player_finalize(GObject *obj) {
  GnVideoPlayer *self = GN_VIDEO_PLAYER(obj);
  g_clear_pointer(&self->url, g_free);
  G_OBJECT_CLASS(gn_video_player_parent_class)->finalize(obj);
}

/* Clamp horizontal minimum/natural to zero so video players never force the
 * timeline to expand beyond its allocated width.  GtkMediaFile reports the
 * video's intrinsic pixel dimensions (e.g. 640x360) which propagates through
 * GtkPicture → overlay → BinLayout → GtkListView → window. */
static void
gn_video_player_measure(GtkWidget      *widget,
                             GtkOrientation  orientation,
                             int             for_size,
                             int            *minimum,
                             int            *natural,
                             int            *minimum_baseline,
                             int            *natural_baseline)
{
  GnVideoPlayer *self = GN_VIDEO_PLAYER(widget);

  if (self->disposed) {
    *minimum = 0;
    *natural = 0;
    *minimum_baseline = -1;
    *natural_baseline = -1;
    return;
  }

  GTK_WIDGET_CLASS(gn_video_player_parent_class)->measure(
      widget, orientation, for_size,
      minimum, natural, minimum_baseline, natural_baseline);

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    *minimum = 0;
    *natural = 0;
  }
}

enum { PROP_0, PROP_AUTOPLAY, PROP_LOOP, PROP_MUTED, PROP_VOLUME, PROP_URL, N_PROPS };
static GParamSpec *props[N_PROPS];
static guint load_requested_signal;

static void
gn_video_player_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GnVideoPlayer *self = GN_VIDEO_PLAYER(object);
  switch (id) {
  case PROP_AUTOPLAY: g_value_set_boolean(value, self->autoplay); break;
  case PROP_LOOP: g_value_set_boolean(value, self->loop); break;
  case PROP_MUTED: g_value_set_boolean(value, self->muted); break;
  case PROP_VOLUME: g_value_set_double(value, self->volume); break;
  case PROP_URL: g_value_set_string(value, self->url); break;
  default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gn_video_player_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  GnVideoPlayer *self = GN_VIDEO_PLAYER(object);
  switch (id) {
  case PROP_AUTOPLAY: gn_video_player_set_autoplay(self, g_value_get_boolean(value)); break;
  case PROP_LOOP: gn_video_player_set_loop(self, g_value_get_boolean(value)); break;
  case PROP_MUTED: gn_video_player_set_muted(self, g_value_get_boolean(value)); break;
  case PROP_VOLUME: gn_video_player_set_volume(self, g_value_get_double(value)); break;
  case PROP_URL: gn_video_player_set_url(self, g_value_get_string(value)); break;
  default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

/* Taken off screen (scrolled away, recycled, dialog closed): never keep
 * playing unseen (nostrc-8xfib.4, from the alpha-6 viewer). */
static void
gn_video_player_unmap(GtkWidget *widget)
{
  GnVideoPlayer *self = GN_VIDEO_PLAYER(widget);
  if (self->stream && !self->is_fullscreen) gtk_media_stream_pause(self->stream);
  GTK_WIDGET_CLASS(gn_video_player_parent_class)->unmap(widget);
}

static void gn_video_player_class_init(GnVideoPlayerClass *klass) {
  GObjectClass *gclass = G_OBJECT_CLASS(klass);
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);

  gclass->dispose = gn_video_player_dispose;
  gclass->finalize = gn_video_player_finalize;
  gclass->get_property = gn_video_player_get_property;
  gclass->set_property = gn_video_player_set_property;
  wclass->measure = gn_video_player_measure;
  wclass->unmap = gn_video_player_unmap;
  gn_portable_gettext_domain();

  props[PROP_AUTOPLAY] = g_param_spec_boolean("autoplay", NULL, NULL, FALSE,
    G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_LOOP] = g_param_spec_boolean("loop", NULL, NULL, FALSE,
    G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_MUTED] = g_param_spec_boolean("muted", NULL, NULL, FALSE,
    G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_VOLUME] = g_param_spec_double("volume", NULL, NULL, 0.0, 1.0, 1.0,
    G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_URL] = g_param_spec_string("url", NULL, NULL, NULL,
    G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(gclass, N_PROPS, props);
  /* The user asked to load url; nothing was fetched by the player. */
  load_requested_signal = g_signal_new("load-requested", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

  gtk_widget_class_set_layout_manager_type(wclass, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(wclass, "gn-video-player");
}

static void on_load_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  gn_video_player_request_load(GN_VIDEO_PLAYER(user_data));
}

static void gn_video_player_init(GnVideoPlayer *self) {
  gn_media_install_css();
  self->autoplay = FALSE;
  self->loop = FALSE;
  self->volume = 1.0;
  self->muted = FALSE;
  self->controls_visible = FALSE;

  /* Create overlay container */
  self->overlay = gtk_overlay_new();
  gtk_widget_set_hexpand(self->overlay, FALSE);
  gtk_widget_set_vexpand(self->overlay, FALSE);
  gtk_widget_set_parent(self->overlay, GTK_WIDGET(self));

  /* Clip rendering to allocated bounds — prevents HD video frames from
   * painting beyond the constrained card width */
  gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);

  /* Displays the stream as a paintable (no built-in controls, unlike
   * GtkVideo). nostrc-8xfib.4: no GtkMediaFile until there is media, so a
   * card that binds a player costs no backend pipeline; GtkPicture keeps the
   * aspect ratio by default (content-fit needs GTK 4.8). */
  self->picture = gtk_picture_new();
  gtk_widget_add_css_class(self->picture, "gn-video-content");
  /* Allow picture to shrink below its natural size - critical for preventing
   * HD video frames from expanding the timeline beyond card width */
  gtk_picture_set_can_shrink(GTK_PICTURE(self->picture), TRUE);
  /* Prevent the picture from expanding the timeline to full screen width
   * when the video's natural size exceeds the card width */
  gtk_widget_set_hexpand(self->picture, FALSE);
  gtk_widget_set_vexpand(self->picture, FALSE);
  gtk_overlay_set_child(GTK_OVERLAY(self->overlay), self->picture);

  /* Create controls overlay */
  create_controls_overlay(self, self->overlay, &self->controls_box);

  /* Not loaded (consent pending) or blocked: a message and a Load action. */
  self->load_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(self->load_box, "gn-video-load");
  gtk_widget_set_halign(self->load_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->load_box, GTK_ALIGN_CENTER);
  self->load_label = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(self->load_label), TRUE);
  gtk_box_append(GTK_BOX(self->load_box), self->load_label);
  self->load_button = gtk_button_new_with_label(_("Load video"));
  gtk_widget_add_css_class(self->load_button, "pill");
  g_signal_connect(self->load_button, "clicked", G_CALLBACK(on_load_clicked), self);
  gtk_box_append(GTK_BOX(self->load_box), self->load_button);
  gtk_widget_set_visible(self->load_box, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->load_box);

  /* Controls start hidden, shown on mouse hover (CSS handles opacity transition) */

  /* Motion controller for showing/hiding controls */
  self->motion_controller = gtk_event_controller_motion_new();
  g_signal_connect(self->motion_controller, "enter", G_CALLBACK(on_motion_enter), self);
  g_signal_connect(self->motion_controller, "motion", G_CALLBACK(on_motion), self);
  g_signal_connect(self->motion_controller, "leave", G_CALLBACK(on_motion_leave), self);
  gtk_widget_add_controller(self->overlay, self->motion_controller);

  /* Key controller for keyboard shortcuts */
  GtkEventController *key_controller = gtk_event_controller_key_new();
  g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), key_controller);

  /* Make focusable for keyboard events */
  gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);

  /* Auto-pause initialization */
  self->is_visible_in_viewport = TRUE;
  self->was_playing_before_scroll = FALSE;

  /* Set up scroll visibility tracking when realized */
  g_signal_connect(GTK_WIDGET(self), "realize", G_CALLBACK(on_video_player_realize), self);
}

GnVideoPlayer *gn_video_player_new(void) {
  return g_object_new(GN_TYPE_VIDEO_PLAYER, NULL);
}

static void
show_load_state(GnVideoPlayer *self, const char *message, gboolean can_load)
{
  release_stream(self);
  hide_states(self);
  gtk_widget_set_visible(self->picture, FALSE);
  gtk_widget_set_visible(self->controls_box, FALSE);
  gtk_label_set_text(GTK_LABEL(self->load_label), message ? message : "");
  gtk_widget_set_visible(self->load_label, message != NULL);
  gtk_widget_set_visible(self->load_button, can_load);
  gtk_widget_set_visible(self->load_box, TRUE);
}

static void
load_from_source(GnVideoPlayer *self)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GtkMediaStream) stream = gn_media_source_open_stream(self->source, self->url, &error);
  if (stream) {
    attach_stream(self, stream, TRUE);
  } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
    show_error_state(self, error ? error->message : NULL);
  }
  /* NOT_SUPPORTED: the host answers "load-requested" itself. */
}

void
gn_video_player_set_source(GnVideoPlayer *self, GnMediaSource *source)
{
  g_return_if_fail(GN_IS_VIDEO_PLAYER(self));
  g_return_if_fail(!source || GN_IS_MEDIA_SOURCE(source));
  if (!g_set_object(&self->source, source)) return;
  if (source) gn_media_source_adopt(source, GTK_WIDGET(self));
}

GnMediaSource *
gn_video_player_get_source(GnVideoPlayer *self)
{
  g_return_val_if_fail(GN_IS_VIDEO_PLAYER(self), NULL);
  return self->source;
}

void
gn_video_player_set_url(GnVideoPlayer *self, const char *url)
{
  g_return_if_fail(GN_IS_VIDEO_PLAYER(self));
  if (g_strcmp0(self->url, url) == 0) return;
  g_free(self->url);
  self->url = g_strdup(url);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_URL]);
  if (!url || !*url) {
    attach_stream(self, NULL, FALSE);
    return;
  }
  GnMediaPolicy policy = GN_MEDIA_POLICY_ASK;
  if (self->source) policy = gn_media_source_get_policy(self->source, url, GN_MEDIA_KIND_VIDEO);
  if (policy == GN_MEDIA_POLICY_ALLOW) {
    /* Stop the old video first so its late prepared or error signals
     * cannot land on the new one (as Gnostr did). */
    attach_stream(self, NULL, FALSE);
    load_from_source(self);
  } else if (policy == GN_MEDIA_POLICY_ASK) {
    show_load_state(self, NULL, TRUE);
  } else {
    show_load_state(self, _("Remote media loading is disabled"), FALSE);
  }
}

const char *
gn_video_player_get_url(GnVideoPlayer *self)
{
  g_return_val_if_fail(GN_IS_VIDEO_PLAYER(self), NULL);
  return self->url;
}

void
gn_video_player_request_load(GnVideoPlayer *self)
{
  g_return_if_fail(GN_IS_VIDEO_PLAYER(self));
  if (!self->url || !*self->url || self->stream) return;
  g_signal_emit(self, load_requested_signal, 0, self->url);
  if (self->stream || !self->source) return; /* a handler supplied it */
  if (gn_media_source_get_policy(self->source, self->url, GN_MEDIA_KIND_VIDEO) ==
      GN_MEDIA_POLICY_BLOCKED)
    return;
  load_from_source(self);
}

void
gn_video_player_set_stream(GnVideoPlayer *self, GtkMediaStream *stream)
{
  g_return_if_fail(GN_IS_VIDEO_PLAYER(self));
  g_return_if_fail(!stream || GTK_IS_MEDIA_STREAM(stream));
  attach_stream(self, stream, FALSE);
}

GtkMediaStream *
gn_video_player_get_stream(GnVideoPlayer *self)
{
  g_return_val_if_fail(GN_IS_VIDEO_PLAYER(self), NULL);
  return self->stream;
}

void
gn_video_player_set_error(GnVideoPlayer *self, const char *message)
{
  g_return_if_fail(GN_IS_VIDEO_PLAYER(self));
  show_error_state(self, message);
}

/* nostrc-sykf: whether GTK has a media backend, probed once on first use to
 * avoid repeated "could not find a media module" warnings. */
static gboolean
media_backend_available(void)
{
  if (s_media_backend_status < 0) {
    GtkMediaStream *probe = gtk_media_file_new();
    const GError *probe_err = gtk_media_stream_get_error(probe);
    s_media_backend_status = probe_err ? 0 : 1;
    if (probe_err) g_info("Video playback disabled: %s", probe_err->message);
    g_object_unref(probe);
  }
  return s_media_backend_status == 1;
}

void
gn_video_player_set_file(GnVideoPlayer *self, GFile *file)
{
  g_return_if_fail(GN_IS_VIDEO_PLAYER(self));
  g_return_if_fail(!file || G_IS_FILE(file));
  if (!file) {
    attach_stream(self, NULL, FALSE);
    return;
  }
  if (!media_backend_available()) {
    attach_stream(self, NULL, FALSE);
    show_error_state(self, _("No video playback module installed"));
    return;
  }
  g_autoptr(GtkMediaStream) stream = gtk_media_file_new_for_file(file);
  attach_stream(self, stream, TRUE);
}

void gn_video_player_play(GnVideoPlayer *self) {
  g_return_if_fail(GN_IS_VIDEO_PLAYER(self));

  GtkMediaStream *stream = self->stream;
  if (stream) {
    gtk_media_stream_play(stream);
  }
  update_play_pause_icon(self);
}

void gn_video_player_pause(GnVideoPlayer *self) {
  g_return_if_fail(GN_IS_VIDEO_PLAYER(self));

  GtkMediaStream *stream = self->stream;
  if (stream) {
    gtk_media_stream_pause(stream);
  }
  update_play_pause_icon(self);
}

void gn_video_player_toggle_playback(GnVideoPlayer *self) {
  g_return_if_fail(GN_IS_VIDEO_PLAYER(self));

  GtkMediaStream *stream = self->stream;
  if (!stream) return;

  if (gtk_media_stream_get_playing(stream)) {
    gtk_media_stream_pause(stream);
  } else {
    gtk_media_stream_play(stream);
  }
  update_play_pause_icon(self);
}

void gn_video_player_stop(GnVideoPlayer *self) {
  g_return_if_fail(GN_IS_VIDEO_PLAYER(self));

  GtkMediaStream *stream = self->stream;
  if (!stream) return;

  /* Pause playback */
  gtk_media_stream_pause(stream);

  /* Seek to beginning to show thumbnail/poster */
  gtk_media_stream_seek(stream, 0);

  update_play_pause_icon(self);
  update_time_labels(self);

  /* Reset seek bar to beginning */
  if (GTK_IS_RANGE(self->seek_scale)) {
    g_signal_handlers_block_by_func(self->seek_scale, on_seek_value_changed, self);
    gtk_range_set_value(GTK_RANGE(self->seek_scale), 0.0);
    g_signal_handlers_unblock_by_func(self->seek_scale, on_seek_value_changed, self);
  }
}

void gn_video_player_set_fullscreen(GnVideoPlayer *self, gboolean fullscreen) {
  g_return_if_fail(GN_IS_VIDEO_PLAYER(self));

  if (self->is_fullscreen == fullscreen) return;
  if (fullscreen && !self->stream) return;

  self->is_fullscreen = fullscreen;

  if (fullscreen) {
    /* Create fullscreen window - don't set transient_for as it can constrain
     * the fullscreen window to the parent's monitor/size on some WMs */
    self->fullscreen_window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(self->fullscreen_window), _("Video"));
    gtk_window_set_decorated(GTK_WINDOW(self->fullscreen_window), FALSE);

    /* Create toast overlay for fullscreen (shows escape hint toast) */
    self->fullscreen_toast_overlay = adw_toast_overlay_new();

    /* Create overlay for fullscreen video and controls */
    self->fullscreen_overlay = gtk_overlay_new();

    /* Create a new picture widget for fullscreen that shares the media file (no controls) */
    GtkWidget *fs_picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(self->stream));
    gtk_widget_add_css_class(fs_picture, "gn-video-content-fullscreen");

    gtk_overlay_set_child(GTK_OVERLAY(self->fullscreen_overlay), fs_picture);

    /* Create controls for fullscreen */
    create_controls_overlay(self, self->fullscreen_overlay, &self->fullscreen_controls_box);

    /* Add motion controller for fullscreen */
    GtkEventController *fs_motion = gtk_event_controller_motion_new();
    g_signal_connect(fs_motion, "enter", G_CALLBACK(on_motion_enter), self);
    g_signal_connect(fs_motion, "motion", G_CALLBACK(on_motion), self);
    g_signal_connect(fs_motion, "leave", G_CALLBACK(on_motion_leave), self);
    gtk_widget_add_controller(self->fullscreen_overlay, fs_motion);

    /* Add key controller for fullscreen */
    GtkEventController *fs_key = gtk_event_controller_key_new();
    g_signal_connect(fs_key, "key-pressed", G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self->fullscreen_window), fs_key);

    /* Set up widget hierarchy: window -> toast_overlay -> video_overlay */
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(self->fullscreen_toast_overlay), self->fullscreen_overlay);
    gtk_window_set_child(GTK_WINDOW(self->fullscreen_window), self->fullscreen_toast_overlay);

    /* Connect close handler */
    g_signal_connect(self->fullscreen_window, "close-request",
                     G_CALLBACK(on_fullscreen_window_close_request), self);

    /* Show fullscreen - present first to ensure window is realized, then fullscreen */
    gtk_window_present(GTK_WINDOW(self->fullscreen_window));
    gtk_window_fullscreen(GTK_WINDOW(self->fullscreen_window));

    /* Update button icon */
    if (GTK_IS_BUTTON(self->btn_fullscreen)) {
      gtk_button_set_icon_name(GTK_BUTTON(self->btn_fullscreen), "view-restore-symbolic");
    }

    /* Show toast with Escape key hint directly in fullscreen window */
    AdwToast *toast = adw_toast_new(_("Press Esc to exit fullscreen"));
    adw_toast_set_timeout(toast, 3);  /* Auto-dismiss after 3 seconds */
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->fullscreen_toast_overlay), toast);
  } else {
    /* Exit fullscreen */
    if (self->fullscreen_window) {
      gtk_window_destroy(GTK_WINDOW(self->fullscreen_window));
      self->fullscreen_window = NULL;
      self->fullscreen_toast_overlay = NULL;
      self->fullscreen_overlay = NULL;
      self->fullscreen_controls_box = NULL;
    }

    /* Update button icon */
    if (GTK_IS_BUTTON(self->btn_fullscreen)) {
      gtk_button_set_icon_name(GTK_BUTTON(self->btn_fullscreen), "view-fullscreen-symbolic");
    }
  }
}

gboolean gn_video_player_get_fullscreen(GnVideoPlayer *self) {
  g_return_val_if_fail(GN_IS_VIDEO_PLAYER(self), FALSE);
  return self->is_fullscreen;
}

void gn_video_player_set_autoplay(GnVideoPlayer *self, gboolean autoplay) {
  g_return_if_fail(GN_IS_VIDEO_PLAYER(self));
  self->autoplay = autoplay;
  /* Applied when a stream becomes prepared. */
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_AUTOPLAY]);
}

gboolean gn_video_player_get_autoplay(GnVideoPlayer *self) {
  g_return_val_if_fail(GN_IS_VIDEO_PLAYER(self), FALSE);
  return self->autoplay;
}

void gn_video_player_set_loop(GnVideoPlayer *self, gboolean loop) {
  g_return_if_fail(GN_IS_VIDEO_PLAYER(self));
  self->loop = loop;
  if (self->stream) {
    gtk_media_stream_set_loop(self->stream, loop);
  }
  update_loop_icon(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_LOOP]);
}

gboolean gn_video_player_get_loop(GnVideoPlayer *self) {
  g_return_val_if_fail(GN_IS_VIDEO_PLAYER(self), FALSE);
  return self->loop;
}

void gn_video_player_set_muted(GnVideoPlayer *self, gboolean muted) {
  g_return_if_fail(GN_IS_VIDEO_PLAYER(self));
  self->muted = muted;

  if (self->stream) {
    gtk_media_stream_set_muted(self->stream, muted);
  }
  update_mute_icon(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_MUTED]);
}

gboolean gn_video_player_get_muted(GnVideoPlayer *self) {
  g_return_val_if_fail(GN_IS_VIDEO_PLAYER(self), FALSE);
  return self->muted;
}

void gn_video_player_set_volume(GnVideoPlayer *self, double volume) {
  g_return_if_fail(GN_IS_VIDEO_PLAYER(self));
  self->volume = CLAMP(volume, 0.0, 1.0);

  if (self->stream) {
    gtk_media_stream_set_volume(self->stream, self->volume);
  }

  /* Update volume slider */
  if (GTK_IS_RANGE(self->volume_scale)) {
    g_signal_handlers_block_by_func(self->volume_scale, on_volume_value_changed, self);
    gtk_range_set_value(GTK_RANGE(self->volume_scale), self->volume);
    g_signal_handlers_unblock_by_func(self->volume_scale, on_volume_value_changed, self);
  }

  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_VOLUME]);

  /* Auto-unmute when adjusting volume */
  if (self->muted && volume > 0) {
    gn_video_player_set_muted(self, FALSE);
  }
}

double gn_video_player_get_volume(GnVideoPlayer *self) {
  g_return_val_if_fail(GN_IS_VIDEO_PLAYER(self), 1.0);
  return self->volume;
}
