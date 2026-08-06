#include "og-preview-widget.h"
#include "gnostr-label-guard.h"
#include "gnostr-youtube-embed.h"
#include "../services/gnostr-media-service.h"
#include "../util/utils.h"
#include "../util/youtube_url.h"
#include <nostr-gtk-1.0/content_renderer.h>
#include <string.h>

struct _OgPreviewWidget {
  GtkWidget parent_instance;

  GtkWidget *card_box;
  GtkWidget *image_widget;
  GtkWidget *text_box;
  GtkWidget *title_label;
  GtkWidget *description_label;
  GtkWidget *site_label;
  GtkWidget *spinner;
  GtkWidget *error_label;

  char *current_url;
  GCancellable *cancellable;
  GCancellable *external_cancellable;
  gulong external_cancelled_id;
  guint64 request_generation;

  GtkWidget *image_overlay_widget;
  GtkWidget *play_overlay;
#ifdef HAVE_WEBKITGTK
  GtkWidget *youtube_embed;
#endif

  gboolean disposed;
};

G_DEFINE_TYPE(OgPreviewWidget, og_preview_widget, GTK_TYPE_WIDGET)

typedef struct {
  GWeakRef widget_ref;
  guint64 generation;
} OgRequestContext;

static OgRequestContext *
og_request_context_new(OgPreviewWidget *self)
{
  OgRequestContext *ctx = g_new0(OgRequestContext, 1);
  g_weak_ref_init(&ctx->widget_ref, self);
  ctx->generation = self->request_generation;
  return ctx;
}

static void
og_request_context_free(gpointer data)
{
  OgRequestContext *ctx = data;
  if (!ctx)
    return;
  g_weak_ref_clear(&ctx->widget_ref);
  g_free(ctx);
}

static OgPreviewWidget *
og_request_context_get_widget(OgRequestContext *ctx)
{
  OgPreviewWidget *self = g_weak_ref_get(&ctx->widget_ref);
  if (!self)
    return NULL;
  if (self->disposed || self->request_generation != ctx->generation) {
    g_object_unref(self);
    return NULL;
  }
  return self;
}

static char *
extract_domain(const char *url)
{
  if (!url)
    return NULL;

  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
  const char *host = uri ? g_uri_get_host(uri) : NULL;
  return host ? g_strdup(host) : g_strdup(url);
}

static void
set_loading_state(OgPreviewWidget *self)
{
  gtk_widget_set_visible(self->card_box, FALSE);
  gtk_widget_set_visible(self->error_label, FALSE);
  gtk_widget_set_visible(self->spinner, TRUE);
  gtk_spinner_start(GTK_SPINNER(self->spinner));
}

static void
set_error_state(OgPreviewWidget *self)
{
  gtk_spinner_stop(GTK_SPINNER(self->spinner));
  gtk_widget_set_visible(self->spinner, FALSE);
  gtk_widget_set_visible(self->card_box, FALSE);
  gtk_widget_set_visible(self->error_label, TRUE);
}

static void
on_preview_texture_ready(GnostrMediaService *service,
                         const char *url,
                         GdkTexture *texture,
                         const GError *error,
                         gpointer user_data)
{
  (void)service;
  OgRequestContext *ctx = user_data;
  OgPreviewWidget *self = og_request_context_get_widget(ctx);
  if (!self)
    return;

  if (texture && GTK_IS_PICTURE(self->image_widget)) {
    gtk_picture_set_paintable(GTK_PICTURE(self->image_widget),
                              GDK_PAINTABLE(texture));
    gtk_widget_set_visible(self->image_widget, TRUE);
    gtk_widget_set_visible(self->image_overlay_widget, TRUE);
  } else {
    gtk_widget_set_visible(self->image_overlay_widget, FALSE);
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_debug("OG preview image unavailable for %s: %s", url, error->message);
  }

  g_object_unref(self);
}

static void
update_ui_with_metadata(OgPreviewWidget *self,
                        GnostrOgMetadata *metadata)
{
  const char *title = gnostr_og_metadata_get_title(metadata);
  const char *description = gnostr_og_metadata_get_description(metadata);
  const char *source_url = gnostr_og_metadata_get_source_url(metadata);
  const char *image_url = gnostr_og_metadata_get_image_url(metadata);
  g_autofree char *domain = extract_domain(
      source_url && *source_url ? source_url : self->current_url);

  gtk_spinner_stop(GTK_SPINNER(self->spinner));
  gtk_widget_set_visible(self->spinner, FALSE);
  gtk_widget_set_visible(self->error_label, FALSE);
  gtk_widget_set_visible(self->card_box, TRUE);

  const char *display_title =
      title && *title ? title :
      (domain && *domain ? domain : self->current_url);
  g_autofree char *safe_title = gnostr_sanitize_utf8(display_title);
  g_autofree char *safe_description = gnostr_sanitize_utf8(description);
  g_autofree char *safe_domain = gnostr_sanitize_utf8(domain);

  gtk_label_set_text(GTK_LABEL(self->title_label), safe_title);
  if (safe_description && *safe_description) {
    gtk_label_set_text(GTK_LABEL(self->description_label), safe_description);
    gtk_widget_set_visible(self->description_label, TRUE);
  } else {
    gtk_label_set_text(GTK_LABEL(self->description_label), "");
    gtk_widget_set_visible(self->description_label, FALSE);
  }
  gtk_label_set_text(GTK_LABEL(self->site_label), safe_domain);

  gtk_picture_set_paintable(GTK_PICTURE(self->image_widget), NULL);
  gtk_widget_set_visible(self->image_widget, FALSE);
  gtk_widget_set_visible(self->image_overlay_widget, FALSE);

  if (image_url && *image_url) {
    gnostr_media_service_request_texture(
        gnostr_media_service_get_default(),
        image_url,
        GNOSTR_MEDIA_RESOURCE_OG_IMAGE,
        240,
        160,
        self->cancellable,
        on_preview_texture_ready,
        og_request_context_new(self),
        og_request_context_free);
  }

  if (self->current_url && gnostr_youtube_url_is_youtube(self->current_url)) {
    if (!self->play_overlay) {
      self->play_overlay =
          gtk_button_new_from_icon_name("media-playback-start-symbolic");
      gtk_widget_add_css_class(self->play_overlay, "youtube-play-overlay");
      gtk_widget_add_css_class(self->play_overlay, "osd");
      gtk_widget_add_css_class(self->play_overlay, "circular");
      gtk_widget_set_halign(self->play_overlay, GTK_ALIGN_CENTER);
      gtk_widget_set_valign(self->play_overlay, GTK_ALIGN_CENTER);
      gtk_widget_set_can_target(self->play_overlay, FALSE);
      gtk_overlay_add_overlay(GTK_OVERLAY(self->image_overlay_widget),
                              self->play_overlay);
    }
    gtk_widget_set_visible(self->play_overlay, TRUE);
  } else if (self->play_overlay) {
    gtk_widget_set_visible(self->play_overlay, FALSE);
  }
}

static void
on_metadata_ready(GnostrMediaService *service,
                  const char *url,
                  GnostrOgMetadata *metadata,
                  const GError *error,
                  gpointer user_data)
{
  (void)service;
  OgRequestContext *ctx = user_data;
  OgPreviewWidget *self = og_request_context_get_widget(ctx);
  if (!self)
    return;
  if (g_strcmp0(self->current_url, url) != 0) {
    g_object_unref(self);
    return;
  }

  if (metadata) {
    update_ui_with_metadata(self, metadata);
  } else {
    set_error_state(self);
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_debug("OG metadata unavailable for %s: %s", url, error->message);
  }

  g_object_unref(self);
}

static void
on_parent_cancelled(GCancellable *parent, gpointer user_data)
{
  (void)parent;
  OgPreviewWidget *self = user_data;
  if (self->cancellable)
    g_cancellable_cancel(self->cancellable);
}

static void
disconnect_external_cancellable(OgPreviewWidget *self)
{
  if (self->external_cancellable && self->external_cancelled_id) {
    g_cancellable_disconnect(self->external_cancellable,
                             self->external_cancelled_id);
    self->external_cancelled_id = 0;
  }
  g_clear_object(&self->external_cancellable);
}

static void
restart_cancellable(OgPreviewWidget *self)
{
  if (self->external_cancellable && self->external_cancelled_id) {
    g_cancellable_disconnect(self->external_cancellable,
                             self->external_cancelled_id);
    self->external_cancelled_id = 0;
  }
  if (self->cancellable)
    g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  self->cancellable = g_cancellable_new();

  if (self->external_cancellable) {
    self->external_cancelled_id = g_cancellable_connect(
        self->external_cancellable,
        G_CALLBACK(on_parent_cancelled),
        self,
        NULL);
    if (g_cancellable_is_cancelled(self->external_cancellable))
      g_cancellable_cancel(self->cancellable);
  }
}

static void
on_card_clicked(GtkGestureClick *gesture,
                int n_press,
                double x,
                double y,
                gpointer user_data)
{
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;
  OgPreviewWidget *self = user_data;

  if (self->disposed || !self->current_url || !*self->current_url)
    return;

  if (gnostr_youtube_url_is_youtube(self->current_url)) {
#ifdef HAVE_WEBKITGTK
    g_autofree char *video_id =
        gnostr_youtube_url_extract_video_id(self->current_url);
    if (video_id) {
      gtk_widget_set_visible(self->card_box, FALSE);
      if (!self->youtube_embed) {
        self->youtube_embed = gnostr_youtube_embed_new(video_id);
        gtk_widget_set_parent(self->youtube_embed, GTK_WIDGET(self));
      }
      gtk_widget_set_visible(self->youtube_embed, TRUE);
      return;
    }
#endif
  }

  g_autoptr(GtkUriLauncher) launcher =
      gtk_uri_launcher_new(self->current_url);
  gtk_uri_launcher_launch(launcher, NULL, NULL, NULL, NULL);
}

static void
og_preview_widget_dispose(GObject *object)
{
  OgPreviewWidget *self = OG_PREVIEW_WIDGET(object);
  self->disposed = TRUE;
  self->request_generation++;

  if (self->cancellable)
    g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  disconnect_external_cancellable(self);

  gtk_widget_set_layout_manager(GTK_WIDGET(self), NULL);
  if (self->text_box && GTK_IS_WIDGET(self->text_box))
    gtk_widget_set_layout_manager(self->text_box, NULL);
  if (self->card_box && GTK_IS_WIDGET(self->card_box))
    gtk_widget_set_layout_manager(self->card_box, NULL);
  if (self->image_overlay_widget && GTK_IS_WIDGET(self->image_overlay_widget))
    gtk_widget_set_layout_manager(self->image_overlay_widget, NULL);

  /* Labels are cleared while rooted by prepare_for_unbind() from the parent
   * quiesce path. Do not retain them here: normal unparenting must release
   * the widget tree. */
  self->play_overlay = NULL;
#ifdef HAVE_WEBKITGTK
  g_clear_pointer(&self->youtube_embed, gtk_widget_unparent);
#endif
  g_clear_pointer(&self->spinner, gtk_widget_unparent);
  g_clear_pointer(&self->error_label, gtk_widget_unparent);
  g_clear_pointer(&self->card_box, gtk_widget_unparent);

  self->title_label = NULL;
  self->description_label = NULL;
  self->site_label = NULL;
  self->image_widget = NULL;
  self->image_overlay_widget = NULL;
  self->text_box = NULL;

  G_OBJECT_CLASS(og_preview_widget_parent_class)->dispose(object);
}

static void
og_preview_widget_finalize(GObject *object)
{
  OgPreviewWidget *self = OG_PREVIEW_WIDGET(object);
  g_free(self->current_url);
  G_OBJECT_CLASS(og_preview_widget_parent_class)->finalize(object);
}

static void
og_preview_widget_measure(GtkWidget *widget,
                          GtkOrientation orientation,
                          int for_size,
                          int *minimum,
                          int *natural,
                          int *minimum_baseline,
                          int *natural_baseline)
{
  OgPreviewWidget *self = OG_PREVIEW_WIDGET(widget);
  if (self->disposed) {
    *minimum = 0;
    *natural = 0;
    *minimum_baseline = -1;
    *natural_baseline = -1;
    return;
  }

  GTK_WIDGET_CLASS(og_preview_widget_parent_class)->measure(
      widget, orientation, for_size, minimum, natural,
      minimum_baseline, natural_baseline);
  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    *minimum = 0;
    *natural = 0;
  }
}

static void
og_preview_widget_class_init(OgPreviewWidgetClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->dispose = og_preview_widget_dispose;
  object_class->finalize = og_preview_widget_finalize;
  widget_class->measure = og_preview_widget_measure;
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "og-preview");
}

static void
og_preview_widget_init(OgPreviewWidget *self)
{
  GtkLayoutManager *layout =
      gtk_widget_get_layout_manager(GTK_WIDGET(self));
  gtk_orientable_set_orientation(GTK_ORIENTABLE(layout),
                                 GTK_ORIENTATION_VERTICAL);

  self->cancellable = g_cancellable_new();

  self->spinner = gtk_spinner_new();
  gtk_widget_set_halign(self->spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->spinner, FALSE);
  gtk_widget_set_parent(self->spinner, GTK_WIDGET(self));

  self->error_label = gtk_label_new("Preview Not Available");
  gtk_label_set_ellipsize(GTK_LABEL(self->error_label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(self->error_label, "dim-label");
  gtk_widget_set_visible(self->error_label, FALSE);
  gtk_widget_set_parent(self->error_label, GTK_WIDGET(self));

  self->card_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(self->card_box, "og-preview-card");
  gtk_widget_set_hexpand(self->card_box, TRUE);
  gtk_widget_set_vexpand(self->card_box, TRUE);
  gtk_widget_set_visible(self->card_box, FALSE);
  gtk_widget_set_parent(self->card_box, GTK_WIDGET(self));

  self->image_widget = gtk_picture_new();
  gtk_widget_add_css_class(self->image_widget, "og-preview-image");
  gtk_widget_set_size_request(self->image_widget, 120, -1);
  gtk_picture_set_content_fit(GTK_PICTURE(self->image_widget),
                              GTK_CONTENT_FIT_COVER);
  gtk_picture_set_can_shrink(GTK_PICTURE(self->image_widget), TRUE);
  gtk_widget_set_visible(self->image_widget, FALSE);

  self->image_overlay_widget = gtk_overlay_new();
  gtk_widget_set_size_request(self->image_overlay_widget, 120, -1);
  gtk_overlay_set_child(GTK_OVERLAY(self->image_overlay_widget),
                        self->image_widget);
  gtk_widget_set_visible(self->image_overlay_widget, FALSE);
  gtk_box_append(GTK_BOX(self->card_box), self->image_overlay_widget);

  self->text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(self->text_box, TRUE);
  gtk_widget_set_valign(self->text_box, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start(self->text_box, 10);
  gtk_widget_set_margin_end(self->text_box, 10);
  gtk_widget_set_margin_top(self->text_box, 6);
  gtk_widget_set_margin_bottom(self->text_box, 6);
  gtk_box_append(GTK_BOX(self->card_box), self->text_box);

  self->title_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(self->title_label), 0.0);
  gtk_label_set_lines(GTK_LABEL(self->title_label), 1);
  gtk_label_set_ellipsize(GTK_LABEL(self->title_label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(self->title_label, "og-preview-title");
  gtk_box_append(GTK_BOX(self->text_box), self->title_label);

  self->description_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(self->description_label), 0.0);
  gtk_label_set_lines(GTK_LABEL(self->description_label), 2);
  gtk_label_set_wrap(GTK_LABEL(self->description_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(self->description_label),
                          PANGO_WRAP_WORD_CHAR);
  gtk_label_set_ellipsize(GTK_LABEL(self->description_label),
                          PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(self->description_label,
                           "og-preview-description");
  gtk_box_append(GTK_BOX(self->text_box), self->description_label);

  self->site_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(self->site_label), 0.0);
  gtk_label_set_lines(GTK_LABEL(self->site_label), 1);
  gtk_label_set_ellipsize(GTK_LABEL(self->site_label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(self->site_label, "og-preview-site");
  gtk_box_append(GTK_BOX(self->text_box), self->site_label);

  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "released", G_CALLBACK(on_card_clicked), self);
  gtk_widget_add_controller(self->card_box, GTK_EVENT_CONTROLLER(click));
}

OgPreviewWidget *
og_preview_widget_new(void)
{
  return g_object_new(OG_TYPE_PREVIEW_WIDGET, NULL);
}

void
og_preview_widget_set_url(OgPreviewWidget *self, const char *url)
{
  g_return_if_fail(OG_IS_PREVIEW_WIDGET(self));

  if (!url || !*url || !gnostr_is_remote_media_allowed()) {
    og_preview_widget_clear(self);
    return;
  }

  if (!self->disposed && g_strcmp0(self->current_url, url) == 0)
    return;

  self->disposed = FALSE;
  self->request_generation++;
  g_free(self->current_url);
  self->current_url = g_strdup(url);
  restart_cancellable(self);
  set_loading_state(self);

  gnostr_media_service_request_og_metadata(
      gnostr_media_service_get_default(),
      url,
      self->cancellable,
      on_metadata_ready,
      og_request_context_new(self),
      og_request_context_free);
}

void
og_preview_widget_set_url_with_cancellable(OgPreviewWidget *self,
                                            const char *url,
                                            GCancellable *cancellable)
{
  g_return_if_fail(OG_IS_PREVIEW_WIDGET(self));

  disconnect_external_cancellable(self);
  if (cancellable)
    self->external_cancellable = g_object_ref(cancellable);
  og_preview_widget_set_url(self, url);
}

void
og_preview_widget_clear(OgPreviewWidget *self)
{
  g_return_if_fail(OG_IS_PREVIEW_WIDGET(self));

  self->request_generation++;
  if (self->cancellable)
    g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  self->cancellable = g_cancellable_new();

  disconnect_external_cancellable(self);
  g_clear_pointer(&self->current_url, g_free);
  if (GTK_IS_PICTURE(self->image_widget))
    gtk_picture_set_paintable(GTK_PICTURE(self->image_widget), NULL);
  if (self->spinner)
    gtk_spinner_stop(GTK_SPINNER(self->spinner));
  if (self->spinner)
    gtk_widget_set_visible(self->spinner, FALSE);
  if (self->error_label)
    gtk_widget_set_visible(self->error_label, FALSE);
  if (self->card_box)
    gtk_widget_set_visible(self->card_box, FALSE);
#ifdef HAVE_WEBKITGTK
  if (self->youtube_embed)
    gtk_widget_set_visible(self->youtube_embed, FALSE);
#endif
}

void
og_preview_widget_prepare_for_unbind(OgPreviewWidget *self)
{
  g_return_if_fail(self != NULL);

  if (GNOSTR_LABEL_SAFE(self->title_label))
    gtk_label_set_text(GTK_LABEL(self->title_label), "");
  if (GNOSTR_LABEL_SAFE(self->description_label))
    gtk_label_set_text(GTK_LABEL(self->description_label), "");
  if (GNOSTR_LABEL_SAFE(self->site_label))
    gtk_label_set_text(GTK_LABEL(self->site_label), "");
  if (GNOSTR_LABEL_SAFE(self->error_label))
    gtk_label_set_text(GTK_LABEL(self->error_label), "");

  self->disposed = TRUE;
  self->request_generation++;
  if (self->cancellable)
    g_cancellable_cancel(self->cancellable);
  disconnect_external_cancellable(self);
}
