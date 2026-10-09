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
  self->title = gtk_label_new(NULL);
  self->description = gtk_label_new(NULL);
  self->site = gtk_label_new(NULL);
  self->status = gtk_label_new(NULL);
  self->image = gtk_picture_new();
  self->load = gtk_button_new_with_label(_("Load preview"));
  self->load_image = gtk_button_new_with_label(_("Load image"));
  gtk_label_set_wrap(GTK_LABEL(self->description), TRUE);
  gtk_widget_set_halign(self->title, GTK_ALIGN_START);
  gtk_widget_set_halign(self->description, GTK_ALIGN_START);
  gtk_widget_set_halign(self->site, GTK_ALIGN_START);
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
  gtk_label_set_text(GTK_LABEL(self->title), "");
  gtk_label_set_text(GTK_LABEL(self->description), "");
  gtk_label_set_text(GTK_LABEL(self->site), "");
  gtk_label_set_text(GTK_LABEL(self->status), "");
  gtk_picture_set_paintable(GTK_PICTURE(self->image), NULL);
  gtk_widget_set_visible(self->image, FALSE);
  gtk_widget_set_visible(self->load_image, FALSE);
  gtk_widget_set_visible(self->load, FALSE);
}
void gn_og_preview_card_set_url(GnOgPreviewCard *self, const gchar *url) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  gn_og_preview_card_clear(self);
  self->url = g_strdup(url);
  gtk_label_set_text(GTK_LABEL(self->site), url ? url : "");
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
  gtk_label_set_text(GTK_LABEL(self->title), title ? title : "");
  gtk_label_set_text(GTK_LABEL(self->description), description ? description : "");
  gtk_label_set_text(GTK_LABEL(self->site), site ? site : (self->url ? self->url : ""));
  gtk_label_set_text(GTK_LABEL(self->status), "");
  gtk_widget_set_visible(self->load, FALSE);
  g_free(self->image_url);
  self->image_url = g_strdup(image_url);
  gtk_widget_set_visible(self->load_image, image_url && *image_url);
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
  gtk_label_set_text(GTK_LABEL(self->status), message ? message : _("Preview unavailable"));
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
  if (texture) gtk_widget_set_visible(self->load_image, FALSE);
}
