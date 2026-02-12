#include "gnostr-youtube-embed.h"
#include "../util/youtube_url.h"

#ifdef HAVE_WEBKITGTK

#include <webkit/webkit.h>

struct _GnostrYoutubeEmbed {
  GtkWidget parent_instance;

  GtkWidget *aspect_frame;
  GtkWidget *overlay;
  WebKitWebView *webview;
  GtkWidget *close_button;

  char *video_id;
  gboolean disposed;
};

G_DEFINE_TYPE(GnostrYoutubeEmbed, gnostr_youtube_embed, GTK_TYPE_WIDGET)

static void on_close_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  GnostrYoutubeEmbed *self = GNOSTR_YOUTUBE_EMBED(user_data);
  if (self->disposed) return;

  /* Stop playback */
  if (self->webview && WEBKIT_IS_WEB_VIEW(self->webview)) {
    webkit_web_view_load_uri(self->webview, "about:blank");
  }

  /* Hide self — parent (OgPreviewWidget) will show OG card instead */
  gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
}

/* Restrict navigation to only the embed URL */
static gboolean on_decide_policy(WebKitWebView *webview,
                                  WebKitPolicyDecision *decision,
                                  WebKitPolicyDecisionType type,
                                  gpointer user_data) {
  (void)webview;
  (void)user_data;

  if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
    WebKitNavigationPolicyDecision *nav = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
    WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(nav);
    WebKitURIRequest *request = webkit_navigation_action_get_request(action);
    const char *uri = webkit_uri_request_get_uri(request);

    /* Allow YouTube embed URLs and about:blank */
    if (g_str_has_prefix(uri, "https://www.youtube.com/embed/") ||
        g_str_has_prefix(uri, "about:blank")) {
      webkit_policy_decision_use(decision);
    } else {
      webkit_policy_decision_ignore(decision);
    }
    return TRUE;
  }

  return FALSE;
}

static void gnostr_youtube_embed_dispose(GObject *object) {
  GnostrYoutubeEmbed *self = GNOSTR_YOUTUBE_EMBED(object);
  self->disposed = TRUE;

  gtk_widget_set_layout_manager(GTK_WIDGET(self), NULL);
  g_clear_pointer(&self->aspect_frame, gtk_widget_unparent);
  self->overlay = NULL;
  self->webview = NULL;
  self->close_button = NULL;

  G_OBJECT_CLASS(gnostr_youtube_embed_parent_class)->dispose(object);
}

static void gnostr_youtube_embed_finalize(GObject *object) {
  GnostrYoutubeEmbed *self = GNOSTR_YOUTUBE_EMBED(object);
  g_free(self->video_id);
  G_OBJECT_CLASS(gnostr_youtube_embed_parent_class)->finalize(object);
}

/* Clamp horizontal minimum/natural to zero so the YouTube embed never forces
 * the timeline to expand beyond its allocated width. */
static void
gnostr_youtube_embed_measure(GtkWidget      *widget,
                              GtkOrientation  orientation,
                              int             for_size,
                              int            *minimum,
                              int            *natural,
                              int            *minimum_baseline,
                              int            *natural_baseline)
{
  GnostrYoutubeEmbed *self = GNOSTR_YOUTUBE_EMBED(widget);

  if (self->disposed) {
    *minimum = 0;
    *natural = 0;
    *minimum_baseline = -1;
    *natural_baseline = -1;
    return;
  }

  GTK_WIDGET_CLASS(gnostr_youtube_embed_parent_class)->measure(
      widget, orientation, for_size,
      minimum, natural, minimum_baseline, natural_baseline);

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    *minimum = 0;
    *natural = 0;
  }
}

static void gnostr_youtube_embed_class_init(GnostrYoutubeEmbedClass *klass) {
  GObjectClass *obj_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  obj_class->dispose = gnostr_youtube_embed_dispose;
  obj_class->finalize = gnostr_youtube_embed_finalize;
  widget_class->measure = gnostr_youtube_embed_measure;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "youtube-embed");
}

static void gnostr_youtube_embed_init(GnostrYoutubeEmbed *self) {
  /* 16:9 aspect frame */
  self->aspect_frame = gtk_aspect_frame_new(0.5, 0.5, 16.0 / 9.0, FALSE);
  gtk_widget_set_parent(self->aspect_frame, GTK_WIDGET(self));
  gtk_widget_set_size_request(self->aspect_frame, -1, 240);

  /* Overlay for close button on top of webview */
  self->overlay = gtk_overlay_new();
  gtk_aspect_frame_set_child(GTK_ASPECT_FRAME(self->aspect_frame), self->overlay);

  /* WebKitWebView */
  WebKitSettings *settings = webkit_settings_new();
  webkit_settings_set_enable_javascript(settings, TRUE);
  webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);

  self->webview = WEBKIT_WEB_VIEW(webkit_web_view_new_with_settings(settings));
  gtk_overlay_set_child(GTK_OVERLAY(self->overlay), GTK_WIDGET(self->webview));
  g_object_unref(settings);

  /* Restrict navigation */
  g_signal_connect(self->webview, "decide-policy",
                   G_CALLBACK(on_decide_policy), self);

  /* Close button overlay (top-right) */
  self->close_button = gtk_button_new_from_icon_name("window-close-symbolic");
  gtk_widget_set_halign(self->close_button, GTK_ALIGN_END);
  gtk_widget_set_valign(self->close_button, GTK_ALIGN_START);
  gtk_widget_set_margin_top(self->close_button, 8);
  gtk_widget_set_margin_end(self->close_button, 8);
  gtk_widget_add_css_class(self->close_button, "osd");
  gtk_widget_add_css_class(self->close_button, "circular");
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->close_button);
  g_signal_connect(self->close_button, "clicked",
                   G_CALLBACK(on_close_clicked), self);
}

GtkWidget *gnostr_youtube_embed_new(const char *video_id) {
  GnostrYoutubeEmbed *self = g_object_new(GNOSTR_TYPE_YOUTUBE_EMBED, NULL);

  self->video_id = g_strdup(video_id);

  g_autofree char *embed_url = gnostr_youtube_url_build_embed(video_id);
  if (embed_url && self->webview) {
    webkit_web_view_load_uri(self->webview, embed_url);
  }

  return GTK_WIDGET(self);
}

void gnostr_youtube_embed_stop(GnostrYoutubeEmbed *self) {
  g_return_if_fail(GNOSTR_IS_YOUTUBE_EMBED(self));
  if (self->disposed) return;

  if (self->webview && WEBKIT_IS_WEB_VIEW(self->webview)) {
    webkit_web_view_load_uri(self->webview, "about:blank");
  }
}

gboolean gnostr_youtube_embed_is_available(void) {
  return TRUE;
}

#else /* !HAVE_WEBKITGTK */

gboolean gnostr_youtube_embed_is_available(void) {
  return FALSE;
}

#endif /* HAVE_WEBKITGTK */
