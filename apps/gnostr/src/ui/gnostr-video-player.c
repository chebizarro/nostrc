/*
 * gnostr-video-player.c
 *
 * Enhanced video player widget with custom controls overlay.
 * Features: fullscreen support, GSettings-based autoplay/loop config,
 * and a controls overlay with play/pause, seek, volume, and fullscreen buttons.
 */

#include "gnostr-video-player.h"
#include <glib/gi18n.h>

/* Controls auto-hide timeout in seconds */
#define CONTROLS_HIDE_TIMEOUT_SEC 3

struct _GnostrVideoPlayer {
  GtkWidget parent_instance;

  /* Main container */
  GtkWidget *overlay;
  GtkWidget *picture;         /* GtkPicture widget (displays media as paintable, no controls) */
  GtkMediaFile *media_file;   /* GtkMediaFile for video playback */
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
  char *uri;
  gboolean autoplay;
  gboolean loop;
  gboolean muted;
  double volume;
  gboolean is_fullscreen;
  gboolean controls_visible;
  gboolean seeking;         /* TRUE while user is dragging seek bar */

  /* Fullscreen window */
  GtkWidget *fullscreen_window;
  GtkWidget *fullscreen_overlay;
  GtkWidget *fullscreen_controls_box;

  /* Timers */
  guint controls_hide_timer_id;
  guint position_update_timer_id;

  /* Settings */
  GSettings *settings;
  gulong settings_changed_handler;   /* Signal handler for GSettings changes */

  /* Motion controller for controls visibility */
  GtkEventController *motion_controller;

  /* Auto-pause when scrolled out of view */
  gboolean was_playing_before_scroll;  /* Track if video was playing before scroll-out */
  gboolean is_visible_in_viewport;     /* Whether currently visible in scrolled parent */
  gulong scroll_adj_changed_handler;   /* Signal handler for scroll adjustment changes */
  GtkAdjustment *scroll_vadjustment;   /* Vertical adjustment of parent scroll */
};

G_DEFINE_TYPE(GnostrVideoPlayer, gnostr_video_player, GTK_TYPE_WIDGET)

/* Forward declarations */
static void create_controls_overlay(GnostrVideoPlayer *self, GtkWidget *parent_overlay, GtkWidget **controls_box_out);
static void update_time_labels(GnostrVideoPlayer *self);
static void update_play_pause_icon(GnostrVideoPlayer *self);
static gboolean hide_controls_timeout(gpointer user_data);
static void show_controls(GnostrVideoPlayer *self);
static void schedule_hide_controls(GnostrVideoPlayer *self);
static gboolean position_update_tick(gpointer user_data);

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
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);
  (void)btn;
  gnostr_video_player_toggle_playback(self);
}

static void on_stop_clicked(GtkButton *btn, gpointer user_data) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);
  (void)btn;
  gnostr_video_player_stop(self);
}

static void on_mute_clicked(GtkButton *btn, gpointer user_data) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);
  (void)btn;
  gnostr_video_player_set_muted(self, !self->muted);
}

static void on_fullscreen_clicked(GtkButton *btn, gpointer user_data) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);
  (void)btn;
  gnostr_video_player_set_fullscreen(self, !self->is_fullscreen);
}

static void on_loop_clicked(GtkButton *btn, gpointer user_data) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);
  (void)btn;
  gnostr_video_player_set_loop(self, !self->loop);
}

static void on_seek_value_changed(GtkRange *range, gpointer user_data) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);

  if (!self->seeking) return;

  GtkMediaStream *stream = GTK_MEDIA_STREAM(self->media_file);
  if (!stream) return;

  double value = gtk_range_get_value(range);
  gint64 duration = gtk_media_stream_get_duration(stream);
  gint64 position = (gint64)(value * duration);

  gtk_media_stream_seek(stream, position);
}

static gboolean on_seek_button_press(GtkGestureClick *gesture, gint n_press, double x, double y, gpointer user_data) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);
  (void)gesture; (void)n_press; (void)x; (void)y;
  self->seeking = TRUE;
  return FALSE;
}

static void on_seek_button_release(GtkGestureClick *gesture, gint n_press, double x, double y, gpointer user_data) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);
  (void)gesture; (void)n_press; (void)x; (void)y;
  self->seeking = FALSE;
}

static void on_volume_value_changed(GtkRange *range, gpointer user_data) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);
  double value = gtk_range_get_value(range);
  gnostr_video_player_set_volume(self, value);
}

static void on_motion_enter(GtkEventControllerMotion *controller, double x, double y, gpointer user_data) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);
  (void)controller; (void)x; (void)y;
  show_controls(self);
}

static void on_motion(GtkEventControllerMotion *controller, double x, double y, gpointer user_data) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);
  (void)controller; (void)x; (void)y;
  show_controls(self);
  schedule_hide_controls(self);
}

static void on_motion_leave(GtkEventControllerMotion *controller, gpointer user_data) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);
  (void)controller;
  schedule_hide_controls(self);
}

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);
  (void)controller; (void)keycode; (void)state;

  switch (keyval) {
    case GDK_KEY_Escape:
      if (self->is_fullscreen) {
        gnostr_video_player_set_fullscreen(self, FALSE);
        return TRUE;
      }
      break;
    case GDK_KEY_space:
    case GDK_KEY_k:
      gnostr_video_player_toggle_playback(self);
      return TRUE;
    case GDK_KEY_f:
      gnostr_video_player_set_fullscreen(self, !self->is_fullscreen);
      return TRUE;
    case GDK_KEY_m:
      gnostr_video_player_set_muted(self, !self->muted);
      return TRUE;
  }
  return FALSE;
}

static void on_fullscreen_window_close_request(GtkWindow *window, gpointer user_data) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);
  (void)window;
  gnostr_video_player_set_fullscreen(self, FALSE);
}

static void show_controls(GnostrVideoPlayer *self) {
  if (self->controls_visible) return;

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
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);

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

static void schedule_hide_controls(GnostrVideoPlayer *self) {
  /* Cancel existing timer */
  if (self->controls_hide_timer_id > 0) {
    g_source_remove(self->controls_hide_timer_id);
    self->controls_hide_timer_id = 0;
  }

  /* Schedule new hide timer */
  self->controls_hide_timer_id = g_timeout_add_seconds(CONTROLS_HIDE_TIMEOUT_SEC,
                                                        hide_controls_timeout, self);
}

static gboolean position_update_tick(gpointer user_data) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);

  if (!self->media_file) {
    self->position_update_timer_id = 0;
    return G_SOURCE_REMOVE;
  }

  GtkMediaStream *stream = GTK_MEDIA_STREAM(self->media_file);
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

static void update_time_labels(GnostrVideoPlayer *self) {
  GtkMediaStream *stream = GTK_MEDIA_STREAM(self->media_file);
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

static void update_play_pause_icon(GnostrVideoPlayer *self) {
  GtkMediaStream *stream = GTK_MEDIA_STREAM(self->media_file);
  gboolean playing = stream && gtk_media_stream_get_playing(stream);

  if (GTK_IS_WIDGET(self->play_icon) && GTK_IS_WIDGET(self->pause_icon)) {
    gtk_widget_set_visible(self->play_icon, !playing);
    gtk_widget_set_visible(self->pause_icon, playing);
  }

  /* Update button tooltip */
  if (GTK_IS_WIDGET(self->btn_play_pause)) {
    gtk_widget_set_tooltip_text(self->btn_play_pause, playing ? _("Pause") : _("Play"));
  }
}

static void update_mute_icon(GnostrVideoPlayer *self) {
  if (!GTK_IS_BUTTON(self->btn_mute)) return;

  const char *icon_name = self->muted ? "audio-volume-muted-symbolic" : "audio-volume-high-symbolic";
  gtk_button_set_icon_name(GTK_BUTTON(self->btn_mute), icon_name);
  gtk_widget_set_tooltip_text(self->btn_mute, self->muted ? _("Unmute") : _("Mute"));
}

static void update_loop_icon(GnostrVideoPlayer *self) {
  if (!GTK_IS_BUTTON(self->btn_loop)) return;

  const char *icon_name = self->loop ? "media-playlist-repeat-symbolic" : "media-playlist-consecutive-symbolic";
  gtk_button_set_icon_name(GTK_BUTTON(self->btn_loop), icon_name);
  gtk_widget_set_tooltip_text(self->btn_loop, self->loop ? _("Loop enabled") : _("Loop disabled"));
}

static void create_controls_overlay(GnostrVideoPlayer *self, GtkWidget *parent_overlay, GtkWidget **controls_box_out) {
  /* Controls container - positioned at bottom */
  GtkWidget *controls_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(controls_box, "video-controls");
  gtk_widget_set_halign(controls_box, GTK_ALIGN_FILL);
  gtk_widget_set_valign(controls_box, GTK_ALIGN_END);
  gtk_widget_set_margin_start(controls_box, 8);
  gtk_widget_set_margin_end(controls_box, 8);
  gtk_widget_set_margin_bottom(controls_box, 8);

  /* Seek bar row */
  GtkWidget *seek_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(seek_row, "video-seek-row");

  /* Time labels */
  GtkWidget *lbl_current = gtk_label_new("00:00");
  gtk_widget_add_css_class(lbl_current, "video-time");
  gtk_widget_add_css_class(lbl_current, "monospace");

  GtkWidget *seek = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.001);
  gtk_widget_set_hexpand(seek, TRUE);
  gtk_widget_add_css_class(seek, "video-seek");
  gtk_scale_set_draw_value(GTK_SCALE(seek), FALSE);

  /* Add gesture for seek drag detection */
  GtkGesture *seek_gesture = gtk_gesture_click_new();
  g_signal_connect(seek_gesture, "pressed", G_CALLBACK(on_seek_button_press), self);
  g_signal_connect(seek_gesture, "released", G_CALLBACK(on_seek_button_release), self);
  gtk_widget_add_controller(seek, GTK_EVENT_CONTROLLER(seek_gesture));
  g_signal_connect(seek, "value-changed", G_CALLBACK(on_seek_value_changed), self);

  GtkWidget *lbl_duration = gtk_label_new("00:00");
  gtk_widget_add_css_class(lbl_duration, "video-time");
  gtk_widget_add_css_class(lbl_duration, "monospace");

  gtk_box_append(GTK_BOX(seek_row), lbl_current);
  gtk_box_append(GTK_BOX(seek_row), seek);
  gtk_box_append(GTK_BOX(seek_row), lbl_duration);

  /* Button row */
  GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(btn_row, "video-button-row");
  gtk_widget_set_halign(btn_row, GTK_ALIGN_CENTER);

  /* Play/Pause button with stacked icons */
  GtkWidget *btn_play = gtk_button_new();
  gtk_widget_add_css_class(btn_play, "video-control-btn");
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

  g_signal_connect(btn_play, "clicked", G_CALLBACK(on_play_pause_clicked), self);

  /* Stop button */
  GtkWidget *btn_stop = gtk_button_new_from_icon_name("media-playback-stop-symbolic");
  gtk_widget_add_css_class(btn_stop, "video-control-btn");
  gtk_button_set_has_frame(GTK_BUTTON(btn_stop), FALSE);
  gtk_widget_set_tooltip_text(btn_stop, _("Stop"));
  g_signal_connect(btn_stop, "clicked", G_CALLBACK(on_stop_clicked), self);

  /* Volume controls */
  GtkWidget *btn_mute = gtk_button_new_from_icon_name("audio-volume-high-symbolic");
  gtk_widget_add_css_class(btn_mute, "video-control-btn");
  gtk_button_set_has_frame(GTK_BUTTON(btn_mute), FALSE);
  g_signal_connect(btn_mute, "clicked", G_CALLBACK(on_mute_clicked), self);

  GtkWidget *vol_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.05);
  gtk_widget_set_size_request(vol_scale, 80, -1);
  gtk_widget_add_css_class(vol_scale, "video-volume");
  gtk_scale_set_draw_value(GTK_SCALE(vol_scale), FALSE);
  gtk_range_set_value(GTK_RANGE(vol_scale), 1.0);
  g_signal_connect(vol_scale, "value-changed", G_CALLBACK(on_volume_value_changed), self);

  /* Loop button */
  const char *loop_icon = self->loop ? "media-playlist-repeat-symbolic" : "media-playlist-consecutive-symbolic";
  GtkWidget *btn_loop = gtk_button_new_from_icon_name(loop_icon);
  gtk_widget_add_css_class(btn_loop, "video-control-btn");
  gtk_button_set_has_frame(GTK_BUTTON(btn_loop), FALSE);
  gtk_widget_set_tooltip_text(btn_loop, self->loop ? _("Loop enabled") : _("Loop disabled"));
  g_signal_connect(btn_loop, "clicked", G_CALLBACK(on_loop_clicked), self);

  /* Fullscreen button */
  GtkWidget *btn_fs = gtk_button_new_from_icon_name("view-fullscreen-symbolic");
  gtk_widget_add_css_class(btn_fs, "video-control-btn");
  gtk_button_set_has_frame(GTK_BUTTON(btn_fs), FALSE);
  gtk_widget_set_tooltip_text(btn_fs, _("Fullscreen"));
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
static gboolean check_visibility_in_viewport(GnostrVideoPlayer *self) {
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
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);
  (void)adjustment;

  if (!GNOSTR_IS_VIDEO_PLAYER(self)) return;

  gboolean is_visible = check_visibility_in_viewport(self);

  if (is_visible != self->is_visible_in_viewport) {
    self->is_visible_in_viewport = is_visible;

    GtkMediaStream *stream = GTK_MEDIA_STREAM(self->media_file);
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

static void setup_scroll_visibility_tracking(GnostrVideoPlayer *self) {
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
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);
  (void)widget;
  setup_scroll_visibility_tracking(self);
}

static void gnostr_video_player_dispose(GObject *obj) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(obj);

  /* Disconnect scroll adjustment handler */
  if (self->scroll_vadjustment && self->scroll_adj_changed_handler > 0) {
    g_signal_handler_disconnect(self->scroll_vadjustment, self->scroll_adj_changed_handler);
    self->scroll_adj_changed_handler = 0;
    self->scroll_vadjustment = NULL;
  }

  /* Disconnect settings changed handler */
  if (self->settings && self->settings_changed_handler > 0) {
    g_signal_handler_disconnect(self->settings, self->settings_changed_handler);
    self->settings_changed_handler = 0;
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

  /* Close fullscreen window if open */
  if (self->fullscreen_window) {
    gtk_window_destroy(GTK_WINDOW(self->fullscreen_window));
    self->fullscreen_window = NULL;
  }

  /* Clear settings */
  g_clear_object(&self->settings);

  /* Clear media file */
  g_clear_object(&self->media_file);

  /* Unparent the overlay (which contains picture and controls) */
  if (self->overlay) {
    gtk_widget_unparent(self->overlay);
    self->overlay = NULL;
  }

  G_OBJECT_CLASS(gnostr_video_player_parent_class)->dispose(obj);
}

static void gnostr_video_player_finalize(GObject *obj) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(obj);
  g_clear_pointer(&self->uri, g_free);
  G_OBJECT_CLASS(gnostr_video_player_parent_class)->finalize(obj);
}

/* Callback for GSettings changes to video-loop and video-autoplay */
static void on_settings_changed(GSettings *settings, const gchar *key, gpointer user_data) {
  GnostrVideoPlayer *self = GNOSTR_VIDEO_PLAYER(user_data);
  if (!GNOSTR_IS_VIDEO_PLAYER(self)) return;

  if (g_strcmp0(key, "video-loop") == 0) {
    gboolean loop = g_settings_get_boolean(settings, "video-loop");
    gnostr_video_player_set_loop(self, loop);
  } else if (g_strcmp0(key, "video-autoplay") == 0) {
    gboolean autoplay = g_settings_get_boolean(settings, "video-autoplay");
    gnostr_video_player_set_autoplay(self, autoplay);
  }
}

static void gnostr_video_player_class_init(GnostrVideoPlayerClass *klass) {
  GObjectClass *gclass = G_OBJECT_CLASS(klass);
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);

  gclass->dispose = gnostr_video_player_dispose;
  gclass->finalize = gnostr_video_player_finalize;

  gtk_widget_class_set_layout_manager_type(wclass, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(wclass, "gnostr-video-player");
}

static void gnostr_video_player_init(GnostrVideoPlayer *self) {
  /* Load settings */
  self->settings = g_settings_new("org.gnostr.Client");
  self->autoplay = g_settings_get_boolean(self->settings, "video-autoplay");
  self->loop = g_settings_get_boolean(self->settings, "video-loop");
  self->volume = 1.0;
  self->muted = FALSE;
  self->controls_visible = TRUE;

  /* Listen for settings changes */
  self->settings_changed_handler = g_signal_connect(self->settings, "changed",
                                                     G_CALLBACK(on_settings_changed), self);

  /* Create overlay container */
  self->overlay = gtk_overlay_new();
  gtk_widget_set_parent(self->overlay, GTK_WIDGET(self));

  /* Create media file for video playback (no built-in controls) */
  self->media_file = GTK_MEDIA_FILE(gtk_media_file_new());
  gtk_media_stream_set_loop(GTK_MEDIA_STREAM(self->media_file), self->loop);

  /* Create picture widget to display the media (no controls, unlike GtkVideo) */
  self->picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(self->media_file));
  gtk_widget_add_css_class(self->picture, "video-content");
  gtk_picture_set_content_fit(GTK_PICTURE(self->picture), GTK_CONTENT_FIT_CONTAIN);
  gtk_overlay_set_child(GTK_OVERLAY(self->overlay), self->picture);

  /* Create controls overlay */
  create_controls_overlay(self, self->overlay, &self->controls_box);

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

  /* Start position update timer */
  self->position_update_timer_id = g_timeout_add(250, position_update_tick, self);

  /* Make focusable for keyboard events */
  gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);

  /* Auto-pause initialization */
  self->is_visible_in_viewport = TRUE;
  self->was_playing_before_scroll = FALSE;

  /* Set up scroll visibility tracking when realized */
  g_signal_connect(GTK_WIDGET(self), "realize", G_CALLBACK(on_video_player_realize), self);
}

GnostrVideoPlayer *gnostr_video_player_new(void) {
  return g_object_new(GNOSTR_TYPE_VIDEO_PLAYER, NULL);
}

void gnostr_video_player_set_uri(GnostrVideoPlayer *self, const char *uri) {
  g_return_if_fail(GNOSTR_IS_VIDEO_PLAYER(self));

  g_free(self->uri);
  self->uri = g_strdup(uri);

  if (self->media_file && uri) {
    GFile *file = g_file_new_for_uri(uri);
    gtk_media_file_set_file(self->media_file, file);
    g_object_unref(file);

    /* Apply settings to media stream */
    GtkMediaStream *stream = GTK_MEDIA_STREAM(self->media_file);
    gtk_media_stream_set_loop(stream, self->loop);
    gtk_media_stream_set_muted(stream, self->muted);
    gtk_media_stream_set_volume(stream, self->volume);

    /* Start playback if autoplay is enabled */
    if (self->autoplay) {
      gtk_media_stream_play(stream);
    }
  }
}

const char *gnostr_video_player_get_uri(GnostrVideoPlayer *self) {
  g_return_val_if_fail(GNOSTR_IS_VIDEO_PLAYER(self), NULL);
  return self->uri;
}

void gnostr_video_player_play(GnostrVideoPlayer *self) {
  g_return_if_fail(GNOSTR_IS_VIDEO_PLAYER(self));

  GtkMediaStream *stream = GTK_MEDIA_STREAM(self->media_file);
  if (stream) {
    gtk_media_stream_play(stream);
  }
  update_play_pause_icon(self);
}

void gnostr_video_player_pause(GnostrVideoPlayer *self) {
  g_return_if_fail(GNOSTR_IS_VIDEO_PLAYER(self));

  GtkMediaStream *stream = GTK_MEDIA_STREAM(self->media_file);
  if (stream) {
    gtk_media_stream_pause(stream);
  }
  update_play_pause_icon(self);
}

void gnostr_video_player_toggle_playback(GnostrVideoPlayer *self) {
  g_return_if_fail(GNOSTR_IS_VIDEO_PLAYER(self));

  GtkMediaStream *stream = GTK_MEDIA_STREAM(self->media_file);
  if (!stream) return;

  if (gtk_media_stream_get_playing(stream)) {
    gtk_media_stream_pause(stream);
  } else {
    gtk_media_stream_play(stream);
  }
  update_play_pause_icon(self);
}

void gnostr_video_player_stop(GnostrVideoPlayer *self) {
  g_return_if_fail(GNOSTR_IS_VIDEO_PLAYER(self));

  GtkMediaStream *stream = GTK_MEDIA_STREAM(self->media_file);
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

void gnostr_video_player_set_fullscreen(GnostrVideoPlayer *self, gboolean fullscreen) {
  g_return_if_fail(GNOSTR_IS_VIDEO_PLAYER(self));

  if (self->is_fullscreen == fullscreen) return;

  self->is_fullscreen = fullscreen;

  if (fullscreen) {
    /* Get parent window */
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    GtkWindow *parent = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;

    /* Create fullscreen window */
    self->fullscreen_window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(self->fullscreen_window), _("Video"));
    gtk_window_set_decorated(GTK_WINDOW(self->fullscreen_window), FALSE);
    if (parent) {
      gtk_window_set_transient_for(GTK_WINDOW(self->fullscreen_window), parent);
    }

    /* Create overlay for fullscreen */
    self->fullscreen_overlay = gtk_overlay_new();

    /* Create a new picture widget for fullscreen that shares the media file (no controls) */
    GtkWidget *fs_picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(self->media_file));
    gtk_widget_add_css_class(fs_picture, "video-content-fullscreen");
    gtk_picture_set_content_fit(GTK_PICTURE(fs_picture), GTK_CONTENT_FIT_CONTAIN);

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

    gtk_window_set_child(GTK_WINDOW(self->fullscreen_window), self->fullscreen_overlay);

    /* Connect close handler */
    g_signal_connect(self->fullscreen_window, "close-request",
                     G_CALLBACK(on_fullscreen_window_close_request), self);

    /* Show fullscreen */
    gtk_window_fullscreen(GTK_WINDOW(self->fullscreen_window));
    gtk_window_present(GTK_WINDOW(self->fullscreen_window));

    /* Update button icon */
    if (GTK_IS_BUTTON(self->btn_fullscreen)) {
      gtk_button_set_icon_name(GTK_BUTTON(self->btn_fullscreen), "view-restore-symbolic");
    }
  } else {
    /* Exit fullscreen */
    if (self->fullscreen_window) {
      gtk_window_destroy(GTK_WINDOW(self->fullscreen_window));
      self->fullscreen_window = NULL;
      self->fullscreen_overlay = NULL;
      self->fullscreen_controls_box = NULL;
    }

    /* Update button icon */
    if (GTK_IS_BUTTON(self->btn_fullscreen)) {
      gtk_button_set_icon_name(GTK_BUTTON(self->btn_fullscreen), "view-fullscreen-symbolic");
    }
  }
}

gboolean gnostr_video_player_get_fullscreen(GnostrVideoPlayer *self) {
  g_return_val_if_fail(GNOSTR_IS_VIDEO_PLAYER(self), FALSE);
  return self->is_fullscreen;
}

void gnostr_video_player_set_autoplay(GnostrVideoPlayer *self, gboolean autoplay) {
  g_return_if_fail(GNOSTR_IS_VIDEO_PLAYER(self));
  self->autoplay = autoplay;
  /* Autoplay is handled in set_uri when media file is set */
}

gboolean gnostr_video_player_get_autoplay(GnostrVideoPlayer *self) {
  g_return_val_if_fail(GNOSTR_IS_VIDEO_PLAYER(self), FALSE);
  return self->autoplay;
}

void gnostr_video_player_set_loop(GnostrVideoPlayer *self, gboolean loop) {
  g_return_if_fail(GNOSTR_IS_VIDEO_PLAYER(self));
  self->loop = loop;
  if (self->media_file) {
    gtk_media_stream_set_loop(GTK_MEDIA_STREAM(self->media_file), loop);
  }
  update_loop_icon(self);
}

gboolean gnostr_video_player_get_loop(GnostrVideoPlayer *self) {
  g_return_val_if_fail(GNOSTR_IS_VIDEO_PLAYER(self), FALSE);
  return self->loop;
}

void gnostr_video_player_set_muted(GnostrVideoPlayer *self, gboolean muted) {
  g_return_if_fail(GNOSTR_IS_VIDEO_PLAYER(self));
  self->muted = muted;

  if (self->media_file) {
    gtk_media_stream_set_muted(GTK_MEDIA_STREAM(self->media_file), muted);
  }
  update_mute_icon(self);
}

gboolean gnostr_video_player_get_muted(GnostrVideoPlayer *self) {
  g_return_val_if_fail(GNOSTR_IS_VIDEO_PLAYER(self), FALSE);
  return self->muted;
}

void gnostr_video_player_set_volume(GnostrVideoPlayer *self, double volume) {
  g_return_if_fail(GNOSTR_IS_VIDEO_PLAYER(self));
  self->volume = CLAMP(volume, 0.0, 1.0);

  if (self->media_file) {
    gtk_media_stream_set_volume(GTK_MEDIA_STREAM(self->media_file), self->volume);
  }

  /* Update volume slider */
  if (GTK_IS_RANGE(self->volume_scale)) {
    g_signal_handlers_block_by_func(self->volume_scale, on_volume_value_changed, self);
    gtk_range_set_value(GTK_RANGE(self->volume_scale), self->volume);
    g_signal_handlers_unblock_by_func(self->volume_scale, on_volume_value_changed, self);
  }

  /* Auto-unmute when adjusting volume */
  if (self->muted && volume > 0) {
    gnostr_video_player_set_muted(self, FALSE);
  }
}

double gnostr_video_player_get_volume(GnostrVideoPlayer *self) {
  g_return_val_if_fail(GNOSTR_IS_VIDEO_PLAYER(self), 1.0);
  return self->volume;
}
