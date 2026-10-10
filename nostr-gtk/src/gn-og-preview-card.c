#include <nostr-gtk-1.0/gn-og-preview-card.h>
#include "gn-portable-i18n-private.h"

struct _GnOgPreviewCard {
  GtkBox parent_instance;
  GtkWidget *title, *description, *site, *image, *load, *load_image, *status;
  gchar *url, *image_url;
  GCancellable *cancellable;
};
G_DEFINE_TYPE(GnOgPreviewCard, gn_og_preview_card, GTK_TYPE_BOX)
enum { LOAD_REQUESTED, IMAGE_LOAD_REQUESTED, N_SIGNALS };
static guint signals[N_SIGNALS];
/* A label shows only with text (nostrc-p15n5.7: an empty title or site
 * line left blank rows in the card). */
static void set_text(GtkWidget *label, const gchar *text) {
  gtk_label_set_text(GTK_LABEL(label), text ? text : "");
  gtk_widget_set_visible(label, text && *text);
}
static GtkWidget *new_label(const gchar *name, const gchar *css, gint lines) {
  GtkWidget *label = gtk_label_new(NULL);
  gtk_widget_set_name(label, name);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
  if (lines > 0) {
    gtk_label_set_lines(GTK_LABEL(label), lines);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  }
  gtk_label_set_max_width_chars(GTK_LABEL(label), 48);
  gtk_widget_set_halign(label, GTK_ALIGN_FILL);
  if (css) gtk_widget_add_css_class(label, css);
  gtk_widget_set_visible(label, FALSE);
  return label;
}
static void on_load(GtkButton *button, gpointer user_data) {
  (void)button;
  gn_og_preview_card_request_load(GN_OG_PREVIEW_CARD(user_data));
}
static void on_load_image(GtkButton *button, gpointer user_data) {
  (void)button;
  gn_og_preview_card_request_image(GN_OG_PREVIEW_CARD(user_data));
}
static void dispose(GObject *object) {
  GnOgPreviewCard *self = GN_OG_PREVIEW_CARD(object);
  if (self->cancellable) g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  G_OBJECT_CLASS(gn_og_preview_card_parent_class)->dispose(object);
}
static void finalize(GObject *object) {
  GnOgPreviewCard *self = GN_OG_PREVIEW_CARD(object);
  g_free(self->url);
  g_free(self->image_url);
  G_OBJECT_CLASS(gn_og_preview_card_parent_class)->finalize(object);
}
static void gn_og_preview_card_class_init(GnOgPreviewCardClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = dispose;
  object_class->finalize = finalize;
  gn_portable_gettext_domain();
  signals[LOAD_REQUESTED] = g_signal_new("load-requested", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[IMAGE_LOAD_REQUESTED] = g_signal_new("image-load-requested", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}
static void gn_og_preview_card_init(GnOgPreviewCard *self) {
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing(GTK_BOX(self), 4);
  gtk_widget_add_css_class(GTK_WIDGET(self), "gn-og-preview-card");
  self->title = new_label("og_title", "heading", 2);
  self->description = new_label("og_description", NULL, 3);
  self->site = new_label("og_site", "dim-label", 1);
  gtk_widget_add_css_class(self->site, "caption");
  self->status = new_label("og_status", "dim-label", 0);
  self->image = gtk_picture_new();
  gtk_widget_set_name(self->image, "og_image");
  gtk_picture_set_can_shrink(GTK_PICTURE(self->image), TRUE);
  gtk_picture_set_content_fit(GTK_PICTURE(self->image), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_size_request(self->image, -1, 120);
  self->load = gtk_button_new_with_label(_("Load preview"));
  gtk_widget_set_name(self->load, "og_load");
  self->load_image = gtk_button_new_with_label(_("Load image"));
  gtk_widget_set_name(self->load_image, "og_load_image");
  gtk_widget_set_tooltip_text(self->load_image,
    _("Fetch this page's image. Nothing is loaded until you ask."));
  for (GtkWidget *b = self->load; b; b = b == self->load ? self->load_image : NULL) {
    gtk_widget_set_halign(b, GTK_ALIGN_START);
    gtk_widget_add_css_class(b, "flat");
  }
  gtk_box_append(GTK_BOX(self), self->title);
  gtk_box_append(GTK_BOX(self), self->description);
  gtk_box_append(GTK_BOX(self), self->site);
  gtk_box_append(GTK_BOX(self), self->image);
  gtk_box_append(GTK_BOX(self), self->status);
  gtk_box_append(GTK_BOX(self), self->load);
  gtk_box_append(GTK_BOX(self), self->load_image);
  g_signal_connect(self->load, "clicked", G_CALLBACK(on_load), self);
  g_signal_connect(self->load_image, "clicked", G_CALLBACK(on_load_image), self);
  self->cancellable = g_cancellable_new();
  gtk_widget_set_visible(self->image, FALSE);
  gtk_widget_set_visible(self->load_image, FALSE);
}
GnOgPreviewCard *gn_og_preview_card_new(void) { return g_object_new(GN_TYPE_OG_PREVIEW_CARD, NULL); }
void gn_og_preview_card_clear(GnOgPreviewCard *self) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  self->cancellable = g_cancellable_new();
  g_clear_pointer(&self->url, g_free);
  g_clear_pointer(&self->image_url, g_free);
  set_text(self->title, NULL);
  set_text(self->description, NULL);
  set_text(self->site, NULL);
  set_text(self->status, NULL);
  gtk_picture_set_paintable(GTK_PICTURE(self->image), NULL);
  gtk_widget_set_visible(self->image, FALSE);
  gtk_widget_set_visible(self->load_image, FALSE);
  gtk_widget_set_visible(self->load, FALSE);
}
void gn_og_preview_card_set_url(GnOgPreviewCard *self, const gchar *url) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  gn_og_preview_card_clear(self);
  self->url = g_strdup(url);
  set_text(self->site, url);
  gtk_widget_set_visible(self->load, url && *url);
}
const gchar *gn_og_preview_card_get_url(GnOgPreviewCard *self) {
  g_return_val_if_fail(GN_IS_OG_PREVIEW_CARD(self), NULL);
  return self->url;
}
GCancellable *gn_og_preview_card_get_cancellable(GnOgPreviewCard *self) {
  g_return_val_if_fail(GN_IS_OG_PREVIEW_CARD(self), NULL);
  return self->cancellable;
}
void gn_og_preview_card_request_load(GnOgPreviewCard *self) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  if (self->url && *self->url) g_signal_emit(self, signals[LOAD_REQUESTED], 0, self->url);
}
void gn_og_preview_card_set_result(GnOgPreviewCard *self, const gchar *title,
                                   const gchar *description, const gchar *site,
                                   const gchar *image_url) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  if (g_cancellable_is_cancelled(self->cancellable)) return;
  set_text(self->title, title);
  set_text(self->description, description);
  set_text(self->site, site && *site ? site : self->url);
  set_text(self->status, NULL);
  gtk_widget_set_visible(self->load, FALSE);
  /* The same result again (a row rebinding) keeps loaded artwork. */
  gboolean same_image = g_strcmp0(self->image_url, image_url) == 0;
  g_free(self->image_url);
  self->image_url = g_strdup(image_url);
  if (!same_image) {
    gtk_picture_set_paintable(GTK_PICTURE(self->image), NULL);
    gtk_widget_set_visible(self->image, FALSE);
  }
  gtk_widget_set_visible(self->load_image, image_url && *image_url &&
                                           !gtk_widget_get_visible(self->image));
}
void gn_og_preview_card_set_result_for_url(GnOgPreviewCard *self, const gchar *url,
                                           const gchar *title, const gchar *description,
                                           const gchar *site, const gchar *image_url) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  if (g_strcmp0(self->url, url) != 0) return;
  gn_og_preview_card_set_result(self, title, description, site, image_url);
}
void gn_og_preview_card_set_error(GnOgPreviewCard *self, const gchar *message) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  set_text(self->status, message ? message : _("Preview unavailable"));
  gtk_widget_set_visible(self->load, self->url != NULL);
}
void gn_og_preview_card_set_error_for_url(GnOgPreviewCard *self, const gchar *url,
                                          const gchar *message) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  if (g_strcmp0(self->url, url) != 0) return;
  gn_og_preview_card_set_error(self, message);
}
void gn_og_preview_card_request_image(GnOgPreviewCard *self) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  if (self->image_url && *self->image_url)
    g_signal_emit(self, signals[IMAGE_LOAD_REQUESTED], 0, self->image_url);
}
void gn_og_preview_card_set_image_texture(GnOgPreviewCard *self, GdkTexture *texture) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  gtk_picture_set_paintable(GTK_PICTURE(self->image), texture ? GDK_PAINTABLE(texture) : NULL);
  gtk_widget_set_visible(self->image, texture != NULL);
  gtk_widget_set_visible(self->load_image,
                         !texture && self->image_url && *self->image_url);
}
