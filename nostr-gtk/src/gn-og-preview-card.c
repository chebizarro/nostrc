/* GnOgPreviewCard: the one Open Graph link-preview widget for gnostr and
 * Groundhog (nostrc-8xfib.3). Ported from gnostr's og-preview-widget.c
 * (spinner, compact media-card skin, parent-cancellable chaining, weak
 * stale-request guard, unbind quiesce, domain fallback) onto the portable
 * card (no I/O of its own, explicit image consent, i18n, hidden empty
 * labels). Fetching is the application's GnOgPreviewProvider. */
#include <nostr-gtk-1.0/gn-og-preview-card.h>
#include "gn-portable-i18n-private.h"

struct _GnOgPreviewCard {
  GtkBox parent_instance;
  GtkWidget *title, *description, *site, *image, *load, *load_image, *status, *spinner;
  GtkWidget *compact_box, *compact_text, *compact_overlay, *badge;
  gchar *url, *image_url;
  GCancellable *cancellable;
  GCancellable *parent_cancellable;
  gulong parent_cancelled_id;
  GnOgPreviewProvider *provider;
  GnOgPreviewLayout layout;
  guint64 generation;
  gboolean auto_load, auto_load_image, has_result;
};
G_DEFINE_TYPE(GnOgPreviewCard, gn_og_preview_card, GTK_TYPE_BOX)
enum { LOAD_REQUESTED, IMAGE_LOAD_REQUESTED, ACTIVATE, N_SIGNALS };
static guint signals[N_SIGNALS];

/* A label shows only with text (nostrc-p15n5.7: an empty title or site
 * line left blank rows in the card). Text is always valid UTF-8. */
static void set_text(GtkWidget *label, const gchar *text) {
  g_autofree gchar *valid = text ? g_utf8_make_valid(text, -1) : NULL;
  gtk_label_set_text(GTK_LABEL(label), valid ? valid : "");
  gtk_widget_set_visible(label, valid && *valid);
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
/* GtkContentFit is GTK 4.8; the 4.6 floor keeps the aspect ratio (contain). */
static void set_fit(GtkWidget *picture, gboolean cover) {
#if GTK_CHECK_VERSION(4, 8, 0)
  if (gtk_get_minor_version() >= 8) {
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_picture_set_content_fit(GTK_PICTURE(picture),
                                cover ? GTK_CONTENT_FIT_COVER : GTK_CONTENT_FIT_CONTAIN);
    G_GNUC_END_IGNORE_DEPRECATIONS
    return;
  }
#endif
  (void)cover;
  gtk_picture_set_keep_aspect_ratio(GTK_PICTURE(picture), TRUE);
}
static gchar *url_host(const gchar *url) {
  if (!url) return NULL;
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
  const gchar *host = uri ? g_uri_get_host(uri) : NULL;
  return host && *host ? g_strdup(host) : NULL;
}

/* ── stale-request guard (weak card + generation + URL) ─────────────── */
typedef struct {
  GWeakRef card;
  guint64 generation;
  gchar *url;
} Request;
static Request *request_new(GnOgPreviewCard *self) {
  Request *r = g_new0(Request, 1);
  g_weak_ref_init(&r->card, self);
  r->generation = self->generation;
  r->url = g_strdup(self->url);
  return r;
}
static void request_free(Request *r) {
  g_weak_ref_clear(&r->card);
  g_free(r->url);
  g_free(r);
}
static GnOgPreviewCard *request_card(Request *r) {
  GnOgPreviewCard *self = g_weak_ref_get(&r->card);
  if (self && (self->generation != r->generation || g_strcmp0(self->url, r->url) != 0))
    g_clear_object(&self);
  return self;
}

/* ── cancellables ───────────────────────────────────────────────────── */
static void on_parent_cancelled(GCancellable *parent, gpointer data) {
  (void)parent;
  GnOgPreviewCard *self = data;
  if (self->cancellable) g_cancellable_cancel(self->cancellable);
}
static void disconnect_parent(GnOgPreviewCard *self) {
  if (self->parent_cancellable && self->parent_cancelled_id)
    g_cancellable_disconnect(self->parent_cancellable, self->parent_cancelled_id);
  self->parent_cancelled_id = 0;
}
static void connect_parent(GnOgPreviewCard *self) {
  disconnect_parent(self);
  if (!self->parent_cancellable) return;
  self->parent_cancelled_id = g_cancellable_connect(self->parent_cancellable,
    G_CALLBACK(on_parent_cancelled), self, NULL);
}
static void restart_cancellable(GnOgPreviewCard *self) {
  if (self->cancellable) g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  self->cancellable = g_cancellable_new();
  connect_parent(self);
}

static void stop_spinner(GnOgPreviewCard *self) {
  gtk_spinner_stop(GTK_SPINNER(self->spinner));
  gtk_widget_set_visible(self->spinner, FALSE);
}
static void set_paintable(GnOgPreviewCard *self, GdkPaintable *paintable) {
  gtk_picture_set_paintable(GTK_PICTURE(self->image), paintable);
  gtk_widget_set_visible(self->image, paintable != NULL);
  if (self->compact_overlay) gtk_widget_set_visible(self->compact_overlay, paintable != NULL);
  gtk_widget_set_visible(self->load_image,
                         !paintable && self->image_url && *self->image_url);
}

/* ── provider flow ──────────────────────────────────────────────────── */
static void image_loaded(GObject *source, GAsyncResult *result, gpointer data) {
  Request *r = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkPaintable) paintable =
    gn_og_preview_provider_load_image_finish(GN_OG_PREVIEW_PROVIDER(source), result, &error);
  g_autoptr(GnOgPreviewCard) self = request_card(r);
  if (self && paintable && !g_cancellable_is_cancelled(self->cancellable))
    set_paintable(self, paintable);
  else if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_debug("OG preview image unavailable for %s: %s", r->url, error->message);
  request_free(r);
}
static void metadata_loaded(GObject *source, GAsyncResult *result, gpointer data) {
  Request *r = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GnOgMetadata) metadata =
    gn_og_preview_provider_load_metadata_finish(GN_OG_PREVIEW_PROVIDER(source), result, &error);
  g_autoptr(GnOgPreviewCard) self = request_card(r);
  request_free(r);
  if (!self) return;
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return;
  stop_spinner(self);
  if (!metadata) {
    if (error) g_debug("OG metadata unavailable for %s: %s", self->url, error->message);
    gn_og_preview_card_set_error(self, NULL);
    return;
  }
  /* gnostr's fallbacks: the page's host stands in for a missing title, and
   * names the site when og:site_name is absent. */
  const gchar *source_url = gn_og_metadata_get_source_url(metadata);
  g_autofree gchar *host = url_host(source_url && *source_url ? source_url : self->url);
  const gchar *title = gn_og_metadata_get_title(metadata);
  const gchar *site = gn_og_metadata_get_site_name(metadata);
  gn_og_preview_card_set_result(self, title && *title ? title : host,
                                gn_og_metadata_get_description(metadata),
                                site && *site ? site : host,
                                gn_og_metadata_get_image_url(metadata));
  if (self->auto_load_image) gn_og_preview_card_request_image(self);
}

/* ── activation ─────────────────────────────────────────────────────── */
static void on_released(GtkGestureClick *gesture, int n_press, double x, double y,
                        gpointer data) {
  (void)n_press;
  GnOgPreviewCard *self = data;
  if (!self->has_result || !self->url || !*self->url) return;
  GtkWidget *picked = gtk_widget_pick(GTK_WIDGET(self), x, y, GTK_PICK_DEFAULT);
  if (picked && (GTK_IS_BUTTON(picked) || gtk_widget_get_ancestor(picked, GTK_TYPE_BUTTON)))
    return;
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  g_signal_emit(self, signals[ACTIVATE], 0, self->url);
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
  self->generation++;
  disconnect_parent(self);
  g_clear_object(&self->parent_cancellable);
  if (self->cancellable) g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  g_clear_object(&self->provider);
  g_clear_object(&self->badge);
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
  signals[ACTIVATE] = g_signal_new("activate", G_TYPE_FROM_CLASS(klass),
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
  self->spinner = gtk_spinner_new();
  gtk_widget_set_name(self->spinner, "og_spinner");
  gtk_widget_set_halign(self->spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->spinner, FALSE);
  self->image = gtk_picture_new();
  gtk_widget_set_name(self->image, "og_image");
  gtk_picture_set_can_shrink(GTK_PICTURE(self->image), TRUE);
  set_fit(self->image, FALSE);
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
  gtk_box_append(GTK_BOX(self), self->spinner);
  gtk_box_append(GTK_BOX(self), self->status);
  gtk_box_append(GTK_BOX(self), self->load);
  gtk_box_append(GTK_BOX(self), self->load_image);
  g_signal_connect(self->load, "clicked", G_CALLBACK(on_load), self);
  g_signal_connect(self->load_image, "clicked", G_CALLBACK(on_load_image), self);
  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "released", G_CALLBACK(on_released), self);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));
  self->cancellable = g_cancellable_new();
  gtk_widget_set_visible(self->image, FALSE);
  gtk_widget_set_visible(self->load_image, FALSE);
}
GnOgPreviewCard *gn_og_preview_card_new(void) { return g_object_new(GN_TYPE_OG_PREVIEW_CARD, NULL); }

static void reset_view(GnOgPreviewCard *self) {
  self->has_result = FALSE;
  set_text(self->title, NULL);
  set_text(self->description, NULL);
  set_text(self->site, NULL);
  set_text(self->status, NULL);
  stop_spinner(self);
  gtk_picture_set_paintable(GTK_PICTURE(self->image), NULL);
  gtk_widget_set_visible(self->image, FALSE);
  if (self->compact_box) gtk_widget_set_visible(self->compact_overlay, FALSE);
  gtk_widget_set_visible(self->load_image, FALSE);
  gtk_widget_set_visible(self->load, FALSE);
}
void gn_og_preview_card_clear(GnOgPreviewCard *self) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  self->generation++;
  restart_cancellable(self);
  g_clear_pointer(&self->url, g_free);
  g_clear_pointer(&self->image_url, g_free);
  reset_view(self);
}
void gn_og_preview_card_set_url(GnOgPreviewCard *self, const gchar *url) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  gn_og_preview_card_clear(self);
  self->url = g_strdup(url);
  set_text(self->site, url);
  gtk_widget_set_visible(self->load, url && *url);
  g_autofree gchar *host = url_host(url);
  if (host) {
    g_autofree gchar *tip = g_strdup_printf(
      _("Load preview from %s (reveals your network address)"), host);
    gtk_widget_set_tooltip_text(self->load, tip);
  } else {
    gtk_widget_set_tooltip_text(self->load, NULL);
  }
  if (self->auto_load && url && *url) gn_og_preview_card_request_load(self);
}
void gn_og_preview_card_set_url_with_cancellable(GnOgPreviewCard *self, const gchar *url,
                                                 GCancellable *parent) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  g_return_if_fail(parent == NULL || G_IS_CANCELLABLE(parent));
  disconnect_parent(self);
  g_set_object(&self->parent_cancellable, parent);
  if (url && *url && g_strcmp0(self->url, url) == 0) {
    /* Same URL on rebind: keep the card, adopt the new parent. */
    connect_parent(self);
    if (parent && g_cancellable_is_cancelled(parent)) g_cancellable_cancel(self->cancellable);
    return;
  }
  if (!url || !*url) {
    gn_og_preview_card_clear(self);
    return;
  }
  gn_og_preview_card_set_url(self, url);
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
  if (!self->url || !*self->url) return;
  g_signal_emit(self, signals[LOAD_REQUESTED], 0, self->url);
  if (!self->provider) return;
  self->generation++;
  restart_cancellable(self);
  if (self->parent_cancellable && g_cancellable_is_cancelled(self->parent_cancellable)) return;
  gtk_widget_set_visible(self->load, FALSE);
  set_text(self->status, NULL);
  gtk_widget_set_visible(self->spinner, TRUE);
  gtk_spinner_start(GTK_SPINNER(self->spinner));
  gn_og_preview_provider_load_metadata_async(self->provider, self->url, self->cancellable,
                                             metadata_loaded, request_new(self));
}
void gn_og_preview_card_set_result(GnOgPreviewCard *self, const gchar *title,
                                   const gchar *description, const gchar *site,
                                   const gchar *image_url) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  if (g_cancellable_is_cancelled(self->cancellable)) return;
  self->has_result = TRUE;
  stop_spinner(self);
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
    if (self->compact_box) gtk_widget_set_visible(self->compact_overlay, FALSE);
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
  stop_spinner(self);
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
  if (!self->image_url || !*self->image_url) return;
  g_signal_emit(self, signals[IMAGE_LOAD_REQUESTED], 0, self->image_url);
  if (!self->provider || !gn_og_preview_provider_can_load_image(self->provider)) return;
  if (g_cancellable_is_cancelled(self->cancellable)) return;
  gn_og_preview_provider_load_image_async(self->provider, self->image_url, 240, 160,
                                          self->cancellable, image_loaded, request_new(self));
}
void gn_og_preview_card_set_image_texture(GnOgPreviewCard *self, GdkTexture *texture) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  set_paintable(self, texture ? GDK_PAINTABLE(texture) : NULL);
}

/* ── 1.2 API ────────────────────────────────────────────────────────── */
static void move_to(GtkWidget *child, GtkWidget *box) {
  g_object_ref(child);
  GtkWidget *parent = gtk_widget_get_parent(child);
  if (GTK_IS_OVERLAY(parent)) gtk_overlay_set_child(GTK_OVERLAY(parent), NULL);
  else if (parent) gtk_box_remove(GTK_BOX(parent), child);
  gtk_box_append(GTK_BOX(box), child);
  g_object_unref(child);
}
void gn_og_preview_card_set_layout(GnOgPreviewCard *self, GnOgPreviewLayout layout) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  if (layout == self->layout) return;
  self->layout = layout;
  if (layout == GN_OG_PREVIEW_LAYOUT_COMPACT) {
    /* gnostr's media card: CSS names og-preview-card/-image/-title/... */
    gtk_widget_add_css_class(GTK_WIDGET(self), "og-preview");
    self->compact_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(self->compact_box, "og_card");
    gtk_widget_add_css_class(self->compact_box, "og-preview-card");
    gtk_widget_set_hexpand(self->compact_box, TRUE);
    self->compact_overlay = gtk_overlay_new();
    gtk_widget_set_size_request(self->compact_overlay, 120, -1);
    self->compact_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(self->compact_text, TRUE);
    gtk_widget_set_valign(self->compact_text, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(self->compact_text, 10);
    gtk_widget_set_margin_end(self->compact_text, 10);
    gtk_widget_set_margin_top(self->compact_text, 6);
    gtk_widget_set_margin_bottom(self->compact_text, 6);
    gtk_box_append(GTK_BOX(self->compact_box), self->compact_overlay);
    gtk_box_append(GTK_BOX(self->compact_box), self->compact_text);
    gtk_box_prepend(GTK_BOX(self), self->compact_box);
    g_object_ref(self->image);
    gtk_box_remove(GTK_BOX(self), self->image);
    gtk_overlay_set_child(GTK_OVERLAY(self->compact_overlay), self->image);
    g_object_unref(self->image);
    set_fit(self->image, TRUE);
    gtk_widget_set_size_request(self->image, 120, -1);
    gtk_widget_add_css_class(self->image, "og-preview-image");
    move_to(self->title, self->compact_text);
    move_to(self->description, self->compact_text);
    move_to(self->site, self->compact_text);
    gtk_label_set_lines(GTK_LABEL(self->title), 1);
    gtk_label_set_lines(GTK_LABEL(self->description), 2);
    gtk_widget_add_css_class(self->title, "og-preview-title");
    gtk_widget_add_css_class(self->description, "og-preview-description");
    gtk_widget_add_css_class(self->site, "og-preview-site");
    gtk_widget_set_visible(self->compact_overlay, gtk_widget_get_visible(self->image));
    if (self->badge) {
      gtk_overlay_add_overlay(GTK_OVERLAY(self->compact_overlay), self->badge);
    }
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self), "og-preview");
    if (self->badge) {
      g_object_ref(self->badge);
      gtk_overlay_remove_overlay(GTK_OVERLAY(self->compact_overlay), self->badge);
    }
    g_object_ref(self->image);
    gtk_overlay_set_child(GTK_OVERLAY(self->compact_overlay), NULL);
    gtk_box_prepend(GTK_BOX(self), self->image);
    g_object_unref(self->image);
    GtkWidget *labels[] = { self->site, self->description, self->title };
    for (guint i = 0; i < G_N_ELEMENTS(labels); i++) {
      g_object_ref(labels[i]);
      gtk_box_remove(GTK_BOX(self->compact_text), labels[i]);
      gtk_box_prepend(GTK_BOX(self), labels[i]);
      g_object_unref(labels[i]);
    }
    set_fit(self->image, FALSE);
    gtk_widget_set_size_request(self->image, -1, 120);
    gtk_label_set_lines(GTK_LABEL(self->title), 2);
    gtk_label_set_lines(GTK_LABEL(self->description), 3);
    gtk_box_remove(GTK_BOX(self), self->compact_box);
    self->compact_box = self->compact_text = self->compact_overlay = NULL;
    if (self->badge) {
      /* Re-attached when the compact skin returns. */
      g_object_unref(self->badge);
    }
  }
}
GnOgPreviewLayout gn_og_preview_card_get_layout(GnOgPreviewCard *self) {
  g_return_val_if_fail(GN_IS_OG_PREVIEW_CARD(self), GN_OG_PREVIEW_LAYOUT_STACKED);
  return self->layout;
}
void gn_og_preview_card_set_provider(GnOgPreviewCard *self, GnOgPreviewProvider *provider) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  g_return_if_fail(provider == NULL || GN_IS_OG_PREVIEW_PROVIDER(provider));
  g_set_object(&self->provider, provider);
}
GnOgPreviewProvider *gn_og_preview_card_get_provider(GnOgPreviewCard *self) {
  g_return_val_if_fail(GN_IS_OG_PREVIEW_CARD(self), NULL);
  return self->provider;
}
void gn_og_preview_card_set_auto_load(GnOgPreviewCard *self, gboolean auto_load) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  self->auto_load = !!auto_load;
}
void gn_og_preview_card_set_auto_load_image(GnOgPreviewCard *self, gboolean auto_load) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  self->auto_load_image = !!auto_load;
}
void gn_og_preview_card_set_media_badge(GnOgPreviewCard *self, const gchar *icon_name) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  if (!icon_name || !*icon_name) {
    if (self->badge) gtk_widget_set_visible(self->badge, FALSE);
    return;
  }
  if (!self->badge) {
    /* Owned by the card; parented only in the compact skin. */
    self->badge = gtk_image_new();
    g_object_ref_sink(self->badge);
    gtk_widget_set_name(self->badge, "og_badge");
    gtk_widget_add_css_class(self->badge, "osd");
    gtk_widget_add_css_class(self->badge, "circular");
    gtk_image_set_pixel_size(GTK_IMAGE(self->badge), 32);
    gtk_widget_set_halign(self->badge, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(self->badge, GTK_ALIGN_CENTER);
    gtk_widget_set_can_target(self->badge, FALSE);
    if (self->compact_overlay)
      gtk_overlay_add_overlay(GTK_OVERLAY(self->compact_overlay), self->badge);
  }
  gtk_image_set_from_icon_name(GTK_IMAGE(self->badge), icon_name);
  gtk_widget_set_visible(self->badge, TRUE);
}
void gn_og_preview_card_prepare_for_unbind(GnOgPreviewCard *self) {
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(self));
  gn_og_preview_card_clear(self);
  disconnect_parent(self);
  g_clear_object(&self->parent_cancellable);
}
