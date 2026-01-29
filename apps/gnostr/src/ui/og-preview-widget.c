#include "og-preview-widget.h"
#include "../util/utils.h"
#include <libsoup/soup.h>
#include <string.h>
#include <ctype.h>

/* Maximum OG metadata entries to cache per widget to prevent unbounded memory growth */
#define OG_CACHE_MAX 100

/* Open Graph metadata structure */
typedef struct {
  char *title;
  char *description;
  char *image_url;
  char *url;
  char *site_name;
} OgMetadata;

struct _OgPreviewWidget {
  GtkWidget parent_instance;
  
  /* UI elements */
  GtkWidget *card_box;
  GtkWidget *image_widget;
  GtkWidget *text_box;
  GtkWidget *title_label;
  GtkWidget *description_label;
  GtkWidget *site_label;
  GtkWidget *spinner;
  
  /* State */
  char *current_url;
  /* Note: Uses gnostr_get_shared_soup_session() instead of per-widget session */
  GCancellable *cancellable;
  GHashTable *cache; /* URL -> OgMetadata */
  
  /* Image loading */
  GCancellable *image_cancellable;
  
  /* External cancellable from parent widget (not owned, just referenced) */
  GCancellable *external_cancellable;
  
  /* Disposal flag - set during dispose to prevent callbacks from accessing widget */
  gboolean disposed;
};

G_DEFINE_TYPE(OgPreviewWidget, og_preview_widget, GTK_TYPE_WIDGET)

/* Forward declarations */
static void og_metadata_free(OgMetadata *meta);
static void fetch_og_metadata_async(OgPreviewWidget *self, const char *url);
static void load_image_async(OgPreviewWidget *self, const char *url);
static OgMetadata *parse_og_metadata(const char *html, const char *url);
static char *extract_meta_tag(const char *html, const char *property);
static char *extract_title_tag(const char *html);
static void update_ui_with_metadata(OgPreviewWidget *self, OgMetadata *meta);

/* Utility: Free OgMetadata */
static void og_metadata_free(OgMetadata *meta) {
  if (!meta) return;
  g_free(meta->title);
  g_free(meta->description);
  g_free(meta->image_url);
  g_free(meta->url);
  g_free(meta->site_name);
  g_free(meta);
}

/* Utility: Extract domain from URL */
static char *extract_domain(const char *url) {
  if (!url) return NULL;
  
  const char *start = strstr(url, "://");
  if (start) start += 3;
  else start = url;
  
  const char *end = strchr(start, '/');
  if (!end) end = start + strlen(start);
  
  return g_strndup(start, end - start);
}

/* Utility: Case-insensitive substring search */
static const char *stristr(const char *haystack, const char *needle) {
  if (!haystack || !needle) return NULL;
  
  size_t needle_len = strlen(needle);
  if (needle_len == 0) return haystack;
  
  size_t haystack_len = strlen(haystack);
  if (haystack_len < needle_len) return NULL;
  
  /* Only iterate while there are enough bytes remaining for a match */
  size_t max_start = haystack_len - needle_len;
  for (size_t i = 0; i <= max_start; i++) {
    if (g_ascii_strncasecmp(haystack + i, needle, needle_len) == 0) {
      return haystack + i;
    }
  }
  return NULL;
}

/* HTML parsing: Extract meta tag content */
static char *extract_meta_tag(const char *html, const char *property) {
  if (!html || !property) return NULL;
  
  /* Build search patterns */
  char *pattern1 = g_strdup_printf("<meta property=\"%s\"", property);
  char *pattern2 = g_strdup_printf("<meta property='%s'", property);
  
  const char *meta_start = stristr(html, pattern1);
  if (!meta_start) meta_start = stristr(html, pattern2);
  
  g_free(pattern1);
  g_free(pattern2);
  
  if (!meta_start) return NULL;
  
  /* Find content attribute */
  const char *content_start = stristr(meta_start, "content=");
  if (!content_start) return NULL;
  
  content_start += 8; /* Skip "content=" */
  
  /* Determine quote type */
  char quote = *content_start;
  if (quote != '"' && quote != '\'') return NULL;
  
  content_start++; /* Skip opening quote */
  
  /* Find closing quote */
  const char *content_end = strchr(content_start, quote);
  if (!content_end) return NULL;
  
  return g_strndup(content_start, content_end - content_start);
}

/* HTML parsing: Extract <title> tag */
static char *extract_title_tag(const char *html) {
  if (!html) return NULL;
  
  const char *title_start = stristr(html, "<title>");
  if (!title_start) return NULL;
  
  title_start += 7; /* Skip "<title>" */
  
  const char *title_end = stristr(title_start, "</title>");
  if (!title_end) return NULL;
  
  /* Trim whitespace */
  while (title_start < title_end && g_ascii_isspace(*title_start)) title_start++;
  while (title_end > title_start && g_ascii_isspace(*(title_end - 1))) title_end--;
  
  return g_strndup(title_start, title_end - title_start);
}

/* Parse Open Graph metadata from HTML */
static OgMetadata *parse_og_metadata(const char *html, const char *url) {
  if (!html) return NULL;
  
  OgMetadata *meta = g_new0(OgMetadata, 1);
  
  /* Extract OG tags */
  meta->title = extract_meta_tag(html, "og:title");
  meta->description = extract_meta_tag(html, "og:description");
  meta->image_url = extract_meta_tag(html, "og:image");
  meta->url = extract_meta_tag(html, "og:url");
  meta->site_name = extract_meta_tag(html, "og:site_name");
  
  /* Fallbacks */
  if (!meta->title) {
    meta->title = extract_title_tag(html);
  }
  
  if (!meta->description) {
    meta->description = extract_meta_tag(html, "description");
  }
  
  if (!meta->url) {
    meta->url = g_strdup(url);
  }
  
  if (!meta->site_name) {
    meta->site_name = extract_domain(url);
  }
  
  /* Validate we have at least a title */
  if (!meta->title || !*meta->title) {
    og_metadata_free(meta);
    return NULL;
  }
  
  return meta;
}

/* Async image loading callback.
 * user_data is a weak ref pointer that will be NULL if widget was destroyed. */
static void on_image_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
  SoupSession *session = SOUP_SESSION(source);
  GObject **weak_ref = (GObject **)user_data;
  OgPreviewWidget *self = NULL;
  GError *error = NULL;
  
  /* Check weak reference - if NULL, widget was destroyed */
  if (weak_ref && *weak_ref) {
    self = OG_PREVIEW_WIDGET(*weak_ref);
    /* Remove weak pointer before freeing container - widget is still alive */
    g_object_remove_weak_pointer(G_OBJECT(self), (gpointer *)weak_ref);
  }
  
  /* Clean up weak ref container */
  g_free(weak_ref);
  
  /* Exit early if widget was destroyed or is being disposed */
  if (!self || self->disposed) {
    return;
  }
  
  GBytes *bytes = soup_session_send_and_read_finish(session, res, &error);
  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("OG: Failed to load image: %s", error->message);
    }
    g_error_free(error);
    return;
  }
  
  if (!bytes || g_bytes_get_size(bytes) == 0) {
    if (bytes) g_bytes_unref(bytes);
    return;
  }
  
  /* Re-check disposed flag before creating texture */
  if (self->disposed) {
    g_bytes_unref(bytes);
    return;
  }
  
  /* Create texture from bytes */
  GdkTexture *texture = gdk_texture_new_from_bytes(bytes, &error);
  g_bytes_unref(bytes);
  
  if (error) {
    g_debug("OG: Failed to create texture: %s", error->message);
    g_error_free(error);
    return;
  }
  
  /* Update UI - verify widget is still valid and not disposed */
  if (!self->disposed && self->image_widget && GTK_IS_PICTURE(self->image_widget)) {
    gtk_picture_set_paintable(GTK_PICTURE(self->image_widget), GDK_PAINTABLE(texture));
    gtk_widget_set_visible(self->image_widget, TRUE);
  }
  
  g_object_unref(texture);
}

/* Load image asynchronously */
static void load_image_async(OgPreviewWidget *self, const char *url) {
  if (!url || !*url) return;
  
  /* Cancel previous load */
  if (self->image_cancellable) {
    g_cancellable_cancel(self->image_cancellable);
    g_clear_object(&self->image_cancellable);
  }
  
  self->image_cancellable = g_cancellable_new();
  
  SoupMessage *msg = soup_message_new("GET", url);
  if (!msg) {
    g_debug("OG: Invalid image URL: %s", url);
    return;
  }
  
  /* Start async fetch - use weak reference to safely handle widget destruction */
  GObject **weak_ref = g_new0(GObject *, 1);
  *weak_ref = G_OBJECT(self);
  g_object_add_weak_pointer(G_OBJECT(self), (gpointer *)weak_ref);
  
  soup_session_send_and_read_async(
    gnostr_get_shared_soup_session(),
    msg,
    G_PRIORITY_LOW,
    self->image_cancellable,
    on_image_loaded,
    weak_ref
  );
  
  g_object_unref(msg);
}

/* Update UI with parsed metadata */
static void update_ui_with_metadata(OgPreviewWidget *self, OgMetadata *meta) {
  /* Guard against disposed widget */
  if (self->disposed) return;
  
  if (!meta) {
    if (self->card_box) gtk_widget_set_visible(self->card_box, FALSE);
    return;
  }
  
  /* Hide spinner, show card - check pointers before use */
  if (self->spinner) gtk_widget_set_visible(self->spinner, FALSE);
  if (self->card_box) gtk_widget_set_visible(self->card_box, TRUE);
  
  /* Update labels - verify each widget is valid before use */
  if (meta->title && self->title_label && GTK_IS_LABEL(self->title_label)) {
    gtk_label_set_text(GTK_LABEL(self->title_label), meta->title);
  }
  
  if (self->description_label && GTK_IS_LABEL(self->description_label)) {
    if (meta->description) {
      gtk_label_set_text(GTK_LABEL(self->description_label), meta->description);
      gtk_widget_set_visible(self->description_label, TRUE);
    } else {
      gtk_widget_set_visible(self->description_label, FALSE);
    }
  }
  
  if (meta->site_name && self->site_label && GTK_IS_LABEL(self->site_label)) {
    gtk_label_set_text(GTK_LABEL(self->site_label), meta->site_name);
  }
  
  /* Load image if available */
  if (meta->image_url && *meta->image_url) {
    load_image_async(self, meta->image_url);
  } else if (self->image_widget) {
    gtk_widget_set_visible(self->image_widget, FALSE);
  }
}

/* Async HTML fetch callback.
 * user_data is a weak ref pointer that will be NULL if widget was destroyed. */
static void on_html_fetched(GObject *source, GAsyncResult *res, gpointer user_data) {
  SoupSession *session = SOUP_SESSION(source);
  GObject **weak_ref = (GObject **)user_data;
  OgPreviewWidget *self = NULL;
  GError *error = NULL;
  
  /* Check weak reference - if NULL, widget was destroyed */
  if (weak_ref && *weak_ref) {
    self = OG_PREVIEW_WIDGET(*weak_ref);
    /* Remove weak pointer before freeing container - widget is still alive */
    g_object_remove_weak_pointer(G_OBJECT(self), (gpointer *)weak_ref);
  }
  
  /* Clean up weak ref container */
  g_free(weak_ref);
  
  /* Exit early if widget was destroyed or is being disposed */
  if (!self || self->disposed) {
    return;
  }
  
  GBytes *bytes = soup_session_send_and_read_finish(session, res, &error);
  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("OG: Failed to fetch URL: %s", error->message);
    }
    g_error_free(error);
    if (!self->disposed && self->spinner) gtk_widget_set_visible(self->spinner, FALSE);
    return;
  }
  
  if (!bytes || g_bytes_get_size(bytes) == 0) {
    if (bytes) g_bytes_unref(bytes);
    if (!self->disposed && self->spinner) gtk_widget_set_visible(self->spinner, FALSE);
    return;
  }
  
  /* Re-check disposed flag before accessing widget members */
  if (self->disposed) {
    g_bytes_unref(bytes);
    return;
  }
  
  /* Parse HTML - g_bytes_get_data returns non-null-terminated data,
   * so we must create a null-terminated copy for string functions */
  gsize size;
  const char *data = g_bytes_get_data(bytes, &size);
  char *html = g_strndup(data, size);
  
  OgMetadata *meta = parse_og_metadata(html, self->current_url);
  g_free(html);
  
  /* Cache result with size limit to prevent unbounded memory growth */
  if (meta && self->current_url && !self->disposed) {
    /* Clear cache if it exceeds limit */
    if (g_hash_table_size(self->cache) >= OG_CACHE_MAX) {
      g_hash_table_remove_all(self->cache);
    }
    g_hash_table_insert(self->cache, g_strdup(self->current_url), meta);
  }
  
  /* Update UI - only if not disposed */
  if (!self->disposed) {
    update_ui_with_metadata(self, meta);
  }
  
  g_bytes_unref(bytes);
}

/* Fetch Open Graph metadata asynchronously */
static void fetch_og_metadata_async(OgPreviewWidget *self, const char *url) {
  if (!url || !*url) return;
  
  /* Check cache first */
  OgMetadata *cached = g_hash_table_lookup(self->cache, url);
  if (cached) {
    update_ui_with_metadata(self, cached);
    return;
  }
  
  /* Cancel previous fetch */
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  
  /* Use external cancellable from parent if available, otherwise create our own */
  GCancellable *effective_cancellable;
  if (self->external_cancellable) {
    effective_cancellable = self->external_cancellable;
  } else {
    self->cancellable = g_cancellable_new();
    effective_cancellable = self->cancellable;
  }
  
  /* Show loading state */
  gtk_widget_set_visible(self->card_box, FALSE);
  gtk_widget_set_visible(self->spinner, TRUE);
  
  /* Create request */
  SoupMessage *msg = soup_message_new("GET", url);
  if (!msg) {
    g_debug("OG: Invalid URL: %s", url);
    gtk_widget_set_visible(self->spinner, FALSE);
    return;
  }
  
  /* Set timeout and size limits */
  soup_message_set_priority(msg, SOUP_MESSAGE_PRIORITY_LOW);
  
  /* Start async fetch - use weak reference to safely handle widget destruction */
  GObject **weak_ref = g_new0(GObject *, 1);
  *weak_ref = G_OBJECT(self);
  g_object_add_weak_pointer(G_OBJECT(self), (gpointer *)weak_ref);
  
  soup_session_send_and_read_async(
    gnostr_get_shared_soup_session(),
    msg,
    G_PRIORITY_LOW,
    effective_cancellable,
    on_html_fetched,
    weak_ref
  );
  
  g_object_unref(msg);
}

/* Widget lifecycle */
static void og_preview_widget_dispose(GObject *object) {
  OgPreviewWidget *self = OG_PREVIEW_WIDGET(object);
  
  /* Mark as disposed FIRST - this prevents callbacks from accessing widget state */
  self->disposed = TRUE;
  
  /* Cancel any in-flight requests - this will trigger cleanup in libsoup.
   * Do NOT call g_clear_object on cancellables immediately - let them be
   * cleaned up naturally to avoid file descriptor corruption in GLib main loop.
   * The cancellables will be freed when the widget is finalized. */
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
  }
  
  if (self->image_cancellable) {
    g_cancellable_cancel(self->image_cancellable);
  }
  
  g_clear_pointer(&self->cache, g_hash_table_unref);
  
  /* Do NOT manipulate labels during disposal - any calls to gtk_label_set_text() or
   * gtk_label_set_attributes() can trigger Pango layout recalculation which crashes
   * when widgets are being disposed. GTK will handle all label and Pango cleanup
   * automatically during finalization. */
  
  /* Clear widget pointers AFTER unparenting to prevent race conditions.
   * The unparent will trigger child disposal, so we need the pointers valid
   * until that completes. */
  
  /* For programmatic widgets (not template-based), we must unparent children.
   * Do this BEFORE calling parent dispose. */
  GtkWidget *card = self->card_box;
  GtkWidget *spin = self->spinner;
  
  /* Clear pointers first to prevent any callbacks from accessing them */
  self->card_box = NULL;
  self->spinner = NULL;
  self->title_label = NULL;
  self->description_label = NULL;
  self->site_label = NULL;
  self->image_widget = NULL;
  self->text_box = NULL;
  
  /* Now unparent - this triggers child disposal */
  if (card && GTK_IS_WIDGET(card)) {
    gtk_widget_unparent(card);
  }
  
  if (spin && GTK_IS_WIDGET(spin)) {
    gtk_widget_unparent(spin);
  }
  
  G_OBJECT_CLASS(og_preview_widget_parent_class)->dispose(object);
  
  /* Shared session is managed globally - do not clear here */
}

static void og_preview_widget_finalize(GObject *object) {
  OgPreviewWidget *self = OG_PREVIEW_WIDGET(object);
  
  /* Clean up cancellables that were only cancelled in dispose */
  g_clear_object(&self->cancellable);
  g_clear_object(&self->image_cancellable);
  
  g_free(self->current_url);
  
  G_OBJECT_CLASS(og_preview_widget_parent_class)->finalize(object);
}

static void og_preview_widget_class_init(OgPreviewWidgetClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  
  object_class->dispose = og_preview_widget_dispose;
  object_class->finalize = og_preview_widget_finalize;
  
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "og-preview");
}

static void on_card_clicked(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;
  
  OgPreviewWidget *self = user_data;
  
  if (self->current_url && *self->current_url) {
    GtkUriLauncher *launcher = gtk_uri_launcher_new(self->current_url);
    gtk_uri_launcher_launch(launcher, NULL, NULL, NULL, NULL);
    g_object_unref(launcher);
  }
}

static void og_preview_widget_init(OgPreviewWidget *self) {
  /* Uses shared session from gnostr_get_shared_soup_session() */
  
  self->cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)og_metadata_free);
  
  /* Create spinner */
  self->spinner = gtk_spinner_new();
  gtk_spinner_start(GTK_SPINNER(self->spinner));
  gtk_widget_set_visible(self->spinner, FALSE);
  gtk_widget_set_parent(self->spinner, GTK_WIDGET(self));
  
  /* Create card container */
  self->card_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(self->card_box, "og-preview-card");
  gtk_widget_set_visible(self->card_box, FALSE);
  gtk_widget_set_parent(self->card_box, GTK_WIDGET(self));
  
  /* Create image */
  self->image_widget = gtk_picture_new();
  gtk_widget_add_css_class(self->image_widget, "og-preview-image");
  gtk_widget_set_size_request(self->image_widget, -1, 200);
  gtk_picture_set_content_fit(GTK_PICTURE(self->image_widget), GTK_CONTENT_FIT_COVER);
  gtk_widget_set_visible(self->image_widget, FALSE);
  gtk_box_append(GTK_BOX(self->card_box), self->image_widget);
  
  /* Create text container */
  self->text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start(self->text_box, 12);
  gtk_widget_set_margin_end(self->text_box, 12);
  gtk_widget_set_margin_top(self->text_box, 8);
  gtk_widget_set_margin_bottom(self->text_box, 8);
  gtk_box_append(GTK_BOX(self->card_box), self->text_box);
  
  /* Create title */
  self->title_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(self->title_label), 0.0);
  gtk_label_set_wrap(GTK_LABEL(self->title_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(self->title_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(self->title_label), 50);
  gtk_widget_add_css_class(self->title_label, "og-preview-title");
  gtk_box_append(GTK_BOX(self->text_box), self->title_label);
  
  /* Create description */
  self->description_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(self->description_label), 0.0);
  gtk_label_set_wrap(GTK_LABEL(self->description_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(self->description_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(self->description_label), 50);
  gtk_label_set_lines(GTK_LABEL(self->description_label), 2);
  gtk_label_set_ellipsize(GTK_LABEL(self->description_label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(self->description_label, "og-preview-description");
  gtk_box_append(GTK_BOX(self->text_box), self->description_label);
  
  /* Create site name */
  self->site_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(self->site_label), 0.0);
  gtk_widget_add_css_class(self->site_label, "og-preview-site");
  gtk_box_append(GTK_BOX(self->text_box), self->site_label);
  
  /* Make card clickable */
  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "pressed", G_CALLBACK(on_card_clicked), self);
  gtk_widget_add_controller(self->card_box, GTK_EVENT_CONTROLLER(click));
}

/* Public API */
OgPreviewWidget *og_preview_widget_new(void) {
  return g_object_new(OG_TYPE_PREVIEW_WIDGET, NULL);
}

void og_preview_widget_set_url(OgPreviewWidget *self, const char *url) {
  g_return_if_fail(OG_IS_PREVIEW_WIDGET(self));
  
  if (!url || !*url) {
    og_preview_widget_clear(self);
    return;
  }
  
  /* Check if same URL */
  if (self->current_url && strcmp(self->current_url, url) == 0) {
    return;
  }
  
  /* Update current URL */
  g_free(self->current_url);
  self->current_url = g_strdup(url);
  
  /* Fetch metadata */
  fetch_og_metadata_async(self, url);
}

void og_preview_widget_set_url_with_cancellable(OgPreviewWidget *self, 
                                                 const char *url,
                                                 GCancellable *cancellable) {
  g_return_if_fail(OG_IS_PREVIEW_WIDGET(self));
  
  /* Store external cancellable (not owned, just referenced) */
  self->external_cancellable = cancellable;
  
  /* Delegate to set_url which will use the external cancellable */
  og_preview_widget_set_url(self, url);
}

void og_preview_widget_clear(OgPreviewWidget *self) {
  g_return_if_fail(OG_IS_PREVIEW_WIDGET(self));
  
  /* Cancel requests */
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  
  if (self->image_cancellable) {
    g_cancellable_cancel(self->image_cancellable);
    g_clear_object(&self->image_cancellable);
  }
  
  /* Clear external cancellable reference (not owned) */
  self->external_cancellable = NULL;
  
  /* Clear URL */
  g_free(self->current_url);
  self->current_url = NULL;
  
  /* Hide UI */
  gtk_widget_set_visible(self->spinner, FALSE);
  gtk_widget_set_visible(self->card_box, FALSE);
}
