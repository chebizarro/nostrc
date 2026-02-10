#include "og-preview-widget.h"
#include "gnostr-youtube-embed.h"
#include "../util/utils.h"
#include "../util/youtube_url.h"
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
  GtkWidget *error_label;   /* "Preview Not Available" message */

  /* State */
  char *current_url;
  /* Note: Uses gnostr_get_shared_soup_session() instead of per-widget session */
  GCancellable *cancellable;
  GHashTable *cache; /* URL -> OgMetadata */

  /* Image loading */
  GCancellable *image_cancellable;

  /* External cancellable from parent widget (not owned, just referenced) */
  GCancellable *external_cancellable;

  /* YouTube inline playback (nostrc-1du) */
  GtkWidget *play_overlay;        /* Play button overlay on card image */
#ifdef HAVE_WEBKITGTK
  GtkWidget *youtube_embed;       /* GnostrYoutubeEmbed widget (lazily created) */
#endif

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

/* HTML parsing: Extract content from a meta tag.
 * tag_start should point to '<meta' and tag_end to the closing '>'.
 * This properly handles both attribute orders:
 *   <meta property="og:title" content="...">
 *   <meta content="..." property="og:title">
 */
static char *extract_content_from_meta_range(const char *tag_start, const char *tag_end) {
  if (!tag_start || !tag_end || tag_end <= tag_start) return NULL;

  /* Search for content= within the tag bounds only */
  const char *pos = tag_start;
  const char *content_attr = NULL;

  while (pos < tag_end) {
    content_attr = stristr(pos, "content=");
    if (!content_attr || content_attr >= tag_end) return NULL;
    break;
  }

  if (!content_attr) return NULL;

  const char *content_start = content_attr + 8; /* Skip "content=" */
  if (content_start >= tag_end) return NULL;

  /* Determine quote type */
  char quote = *content_start;
  if (quote != '"' && quote != '\'') return NULL;

  content_start++; /* Skip opening quote */
  if (content_start >= tag_end) return NULL;

  /* Find closing quote (must be before tag_end) */
  const char *content_end = strchr(content_start, quote);
  if (!content_end || content_end >= tag_end) return NULL;

  return g_strndup(content_start, content_end - content_start);
}

/* HTML parsing: Find a meta tag containing the given property and extract content.
 * Handles both attribute orders: property before content and content before property.
 * nostrc-24m: Fixed to properly handle <meta content="..." property="og:..."> format.
 */
static char *extract_meta_tag(const char *html, const char *property) {
  if (!html || !property) return NULL;

  /* Build search patterns for property="..." or property='...' */
  char *prop_pattern1 = g_strdup_printf("property=\"%s\"", property);
  char *prop_pattern2 = g_strdup_printf("property='%s'", property);
  char *name_pattern1 = g_strdup_printf("name=\"%s\"", property);
  char *name_pattern2 = g_strdup_printf("name='%s'", property);

  const char *pos = html;
  char *result = NULL;

  /* Scan through all <meta tags */
  while ((pos = stristr(pos, "<meta")) != NULL) {
    const char *tag_start = pos;

    /* Find the end of this tag */
    const char *tag_end = strchr(tag_start, '>');
    if (!tag_end) break;

    /* Check if this meta tag contains our target property/name */
    gboolean found = FALSE;

    /* Check within tag bounds for the property patterns */
    const char *prop_match = stristr(tag_start, prop_pattern1);
    if (prop_match && prop_match < tag_end) found = TRUE;

    if (!found) {
      prop_match = stristr(tag_start, prop_pattern2);
      if (prop_match && prop_match < tag_end) found = TRUE;
    }

    /* Try name= patterns (Twitter cards) */
    if (!found) {
      prop_match = stristr(tag_start, name_pattern1);
      if (prop_match && prop_match < tag_end) found = TRUE;
    }

    if (!found) {
      prop_match = stristr(tag_start, name_pattern2);
      if (prop_match && prop_match < tag_end) found = TRUE;
    }

    if (found) {
      /* Extract content from this tag (handles both attribute orders) */
      result = extract_content_from_meta_range(tag_start, tag_end);
      if (result) break;
    }

    /* Move past this tag */
    pos = tag_end + 1;
  }

  g_free(prop_pattern1);
  g_free(prop_pattern2);
  g_free(name_pattern1);
  g_free(name_pattern2);

  return result;
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

  /* Twitter card fallbacks (Twitter/X uses twitter:* meta tags) */
  if (!meta->title) {
    meta->title = extract_meta_tag(html, "twitter:title");
  }
  if (!meta->description) {
    meta->description = extract_meta_tag(html, "twitter:description");
  }
  if (!meta->image_url) {
    meta->image_url = extract_meta_tag(html, "twitter:image");
  }
  if (!meta->site_name) {
    meta->site_name = extract_meta_tag(html, "twitter:site");
  }

  /* Generic fallbacks */
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

/* hq-869ko: Worker thread: decode OG preview image off main thread.
 * GdkTexture is immutable and thread-safe to create from any thread. */
static void og_image_decode_thread(GTask *task, gpointer source_object,
                                    gpointer task_data, GCancellable *cancellable) {
  (void)source_object; (void)cancellable;
  GBytes *bytes = (GBytes *)task_data;
  GError *error = NULL;

  GdkTexture *texture = gdk_texture_new_from_bytes(bytes, &error);
  if (texture)
    g_task_return_pointer(task, texture, g_object_unref);
  else
    g_task_return_error(task, error);
}

/* hq-869ko: Main-thread completion: apply decoded OG texture to widget */
static void on_og_image_decode_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  OgPreviewWidget *self = OG_PREVIEW_WIDGET(user_data);
  GError *error = NULL;

  GdkTexture *texture = g_task_propagate_pointer(G_TASK(res), &error);
  if (texture && !self->disposed && self->image_widget && GTK_IS_PICTURE(self->image_widget)) {
    gtk_picture_set_paintable(GTK_PICTURE(self->image_widget), GDK_PAINTABLE(texture));
    gtk_widget_set_visible(self->image_widget, TRUE);
  }

  if (texture) g_object_unref(texture);
  if (error) {
    g_debug("OG: Failed to create texture: %s", error->message);
    g_error_free(error);
  }
  g_object_unref(self);
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

  /* Re-check disposed flag before decoding */
  if (self->disposed) {
    g_bytes_unref(bytes);
    return;
  }

  /* hq-869ko: Decode image in worker thread — large OG images can take
   * 50-200ms to decompress, which would drop frames on the main thread. */
  GTask *task = g_task_new(NULL, NULL, on_og_image_decode_done, g_object_ref(self));
  g_task_set_task_data(task, bytes, (GDestroyNotify)g_bytes_unref);
  g_task_run_in_thread(task, og_image_decode_thread);
  g_object_unref(task);
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
    /* Hide spinner and card, show "Preview Not Available" */
    if (self->spinner) gtk_widget_set_visible(self->spinner, FALSE);
    if (self->card_box) gtk_widget_set_visible(self->card_box, FALSE);
    if (self->error_label) gtk_widget_set_visible(self->error_label, TRUE);
    return;
  }

  /* Hide spinner and error, show card - check pointers before use */
  if (self->spinner) gtk_widget_set_visible(self->spinner, FALSE);
  if (self->error_label) gtk_widget_set_visible(self->error_label, FALSE);
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

  /* Show play button overlay for YouTube URLs (nostrc-1du) */
  if (self->current_url && gnostr_youtube_url_is_youtube(self->current_url)) {
    if (!self->play_overlay) {
      self->play_overlay = gtk_button_new_from_icon_name("media-playback-start-symbolic");
      gtk_widget_add_css_class(self->play_overlay, "youtube-play-overlay");
      gtk_widget_add_css_class(self->play_overlay, "osd");
      gtk_widget_add_css_class(self->play_overlay, "circular");
      gtk_widget_set_halign(self->play_overlay, GTK_ALIGN_CENTER);
      gtk_widget_set_valign(self->play_overlay, GTK_ALIGN_CENTER);
      /* The play button is purely visual — clicks are handled by card_box gesture */
      gtk_widget_set_can_target(self->play_overlay, FALSE);
      gtk_widget_set_parent(self->play_overlay, GTK_WIDGET(self));
    }
    gtk_widget_set_visible(self->play_overlay, TRUE);
  } else if (self->play_overlay) {
    gtk_widget_set_visible(self->play_overlay, FALSE);
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
      /* Show error message instead of spinner */
      if (!self->disposed) {
        if (self->spinner) gtk_widget_set_visible(self->spinner, FALSE);
        if (self->error_label) gtk_widget_set_visible(self->error_label, TRUE);
      }
    }
    g_error_free(error);
    if (!self->disposed && self->spinner) gtk_widget_set_visible(self->spinner, FALSE);
    return;
  }

  if (!bytes || g_bytes_get_size(bytes) == 0) {
    if (bytes) g_bytes_unref(bytes);
    if (!self->disposed) {
      if (self->spinner) gtk_widget_set_visible(self->spinner, FALSE);
      if (self->error_label) gtk_widget_set_visible(self->error_label, TRUE);
    }
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
  gtk_widget_set_visible(self->error_label, FALSE);
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

  /* GTK4 widget lifecycle: unparent children in dispose(), BEFORE calling
   * parent dispose. This is the correct pattern - dispose handles widget
   * hierarchy cleanup, finalize handles non-widget resources.
   *
   * Previous code (nostrc-oa9) tried to unparent in finalize() which caused
   * crashes because parent dispose() had already run, leaving the widget
   * hierarchy in an inconsistent state where gtk_box_dispose would try to
   * measure partially destroyed children (nostrc-14wu).
   *
   * The key insight: unparent children BEFORE parent dispose runs, not after.
   * Use g_clear_pointer pattern which safely handles NULL and only unparents
   * if the pointer is still valid. */

  /* CRITICAL FIX (nostrc-14wu): Clear the layout manager BEFORE unparenting
   * children. The box layout manager may try to measure children during
   * destruction, causing NULL pointer dereference in pango when labels
   * have already been partially destroyed. Setting layout manager to NULL
   * prevents any measurement attempts during the disposal process.
   *
   * CRITICAL FIX: Also clear layout managers of nested containers (card_box,
   * text_box) to prevent them from measuring their children during disposal.
   * This happens when many list items are disposed at once during bulk removal. */
  gtk_widget_set_layout_manager(GTK_WIDGET(self), NULL);
  if (self->text_box && GTK_IS_WIDGET(self->text_box)) {
    gtk_widget_set_layout_manager(self->text_box, NULL);
  }
  if (self->card_box && GTK_IS_WIDGET(self->card_box)) {
    gtk_widget_set_layout_manager(self->card_box, NULL);
  }

  /* Clear label text BEFORE unparenting to prevent Pango layout crashes.
   * During cascade disposal, Pango tries to finalize layouts and can crash
   * if the layout data is corrupted or widgets are disposed in wrong order.
   * nostrc-pgo5: MUST check gtk_widget_get_native() before gtk_label_set_text.
   * When the widget tree is already being torn down (e.g., gtk_box_remove in
   * prepare_for_bind), the PangoContext is gone and gtk_label_set_text will
   * SEGV trying to unref the old PangoLayout with a NULL context. */
#define OG_LABEL_SAFE(lbl) \
  ((lbl) != NULL && GTK_IS_LABEL(lbl) && \
   gtk_widget_get_native(GTK_WIDGET(lbl)) != NULL)
  if (OG_LABEL_SAFE(self->title_label)) {
    gtk_label_set_text(GTK_LABEL(self->title_label), "");
  }
  if (OG_LABEL_SAFE(self->description_label)) {
    gtk_label_set_text(GTK_LABEL(self->description_label), "");
  }
  if (OG_LABEL_SAFE(self->site_label)) {
    gtk_label_set_text(GTK_LABEL(self->site_label), "");
  }
  if (OG_LABEL_SAFE(self->error_label)) {
    gtk_label_set_text(GTK_LABEL(self->error_label), "");
  }
#undef OG_LABEL_SAFE

  g_clear_pointer(&self->play_overlay, gtk_widget_unparent);
#ifdef HAVE_WEBKITGTK
  g_clear_pointer(&self->youtube_embed, gtk_widget_unparent);
#endif
  g_clear_pointer(&self->spinner, gtk_widget_unparent);
  g_clear_pointer(&self->error_label, gtk_widget_unparent);
  g_clear_pointer(&self->card_box, gtk_widget_unparent);

  /* These are children of card_box/text_box, not direct children of self.
   * They were already unparented when card_box was unparented above.
   * Just clear the pointers to prevent stale access. */
  self->title_label = NULL;
  self->description_label = NULL;
  self->site_label = NULL;
  self->image_widget = NULL;
  self->text_box = NULL;

  G_OBJECT_CLASS(og_preview_widget_parent_class)->dispose(object);

  /* Shared session is managed globally - do not clear here */
}

static void og_preview_widget_finalize(GObject *object) {
  OgPreviewWidget *self = OG_PREVIEW_WIDGET(object);

  /* Clean up cancellables that were only cancelled in dispose */
  g_clear_object(&self->cancellable);
  g_clear_object(&self->image_cancellable);

  /* Free non-widget resources only - child widgets were already unparented
   * in dispose() which is the correct GTK4 pattern. Trying to unparent here
   * caused crashes (nostrc-14wu) because the parent class dispose had already
   * run, leaving the widget in an inconsistent state. */
  g_free(self->current_url);

  G_OBJECT_CLASS(og_preview_widget_parent_class)->finalize(object);
}

/* Clamp natural width so OG preview never forces the timeline to expand.
 * GtkPicture reports its image's intrinsic dimensions as natural size
 * (often 1200×630 for OG images).  Without clamping, the timeline expands
 * to the image width and the window can't be shrunk back. */
static void
og_preview_widget_measure(GtkWidget      *widget,
                          GtkOrientation  orientation,
                          int             for_size,
                          int            *minimum,
                          int            *natural,
                          int            *minimum_baseline,
                          int            *natural_baseline)
{
  GTK_WIDGET_CLASS(og_preview_widget_parent_class)->measure(
      widget, orientation, for_size,
      minimum, natural, minimum_baseline, natural_baseline);

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    /* Natural = minimum: the widget is happy with whatever width the
     * parent allocates.  The GtkPicture (can_shrink=TRUE, content_fit=COVER)
     * scales down to fit the allocation. */
    *natural = *minimum;
  }
}

static void og_preview_widget_class_init(OgPreviewWidgetClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = og_preview_widget_dispose;
  object_class->finalize = og_preview_widget_finalize;
  widget_class->measure = og_preview_widget_measure;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "og-preview");
}

static void on_card_clicked(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;

  OgPreviewWidget *self = user_data;

  /* Check disposed flag before accessing widget state (nostrc-oa9) */
  if (self->disposed) return;

  if (!self->current_url || !*self->current_url) return;

  /* YouTube: inline embed if WebKit available (nostrc-1du) */
  if (gnostr_youtube_url_is_youtube(self->current_url)) {
#ifdef HAVE_WEBKITGTK
    g_autofree char *vid = gnostr_youtube_url_extract_video_id(self->current_url);
    if (vid) {
      /* Hide OG card, show YouTube embed */
      gtk_widget_set_visible(self->card_box, FALSE);
      if (self->play_overlay)
        gtk_widget_set_visible(self->play_overlay, FALSE);

      if (!self->youtube_embed) {
        self->youtube_embed = gnostr_youtube_embed_new(vid);
        gtk_widget_set_parent(self->youtube_embed, GTK_WIDGET(self));
      } else {
        /* Re-load with new video */
        gnostr_youtube_embed_stop(GNOSTR_YOUTUBE_EMBED(self->youtube_embed));
        g_clear_pointer(&self->youtube_embed, gtk_widget_unparent);
        self->youtube_embed = gnostr_youtube_embed_new(vid);
        gtk_widget_set_parent(self->youtube_embed, GTK_WIDGET(self));
      }
      gtk_widget_set_visible(self->youtube_embed, TRUE);
      return;
    }
#endif
    /* Fallthrough: no WebKit or no video ID — open in browser */
  }

  /* Default: open in browser */
  GtkUriLauncher *launcher = gtk_uri_launcher_new(self->current_url);
  gtk_uri_launcher_launch(launcher, NULL, NULL, NULL, NULL);
  g_object_unref(launcher);
}

static void og_preview_widget_init(OgPreviewWidget *self) {
  /* Uses shared session from gnostr_get_shared_soup_session() */
  
  self->cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)og_metadata_free);
  
  /* Create spinner */
  self->spinner = gtk_spinner_new();
  gtk_spinner_start(GTK_SPINNER(self->spinner));
  gtk_widget_set_visible(self->spinner, FALSE);
  gtk_widget_set_parent(self->spinner, GTK_WIDGET(self));

  /* Create error label for "Preview Not Available" */
  self->error_label = gtk_label_new("Preview Not Available");
  gtk_widget_add_css_class(self->error_label, "dim-label");
  gtk_widget_set_visible(self->error_label, FALSE);
  gtk_widget_set_parent(self->error_label, GTK_WIDGET(self));
  
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
  /* nostrc-1tn9: Ensure image can shrink and doesn't force window expansion */
  gtk_picture_set_can_shrink(GTK_PICTURE(self->image_widget), TRUE);
  gtk_widget_set_halign(self->image_widget, GTK_ALIGN_FILL);
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
  gtk_widget_set_visible(self->error_label, FALSE);
  gtk_widget_set_visible(self->card_box, FALSE);
}

void og_preview_widget_prepare_for_unbind(OgPreviewWidget *self) {
  /* NOTE: Do NOT use type-checking macros (OG_IS_PREVIEW_WIDGET) here - they
   * dereference the pointer which crashes if it contains garbage. The pointer
   * may be stale if the widget was destroyed or the row is being recycled.
   * Just check for NULL and trust the caller (note_card_row) checked validity.
   * See nostrc-ofq crash fix for this pattern. */
  g_return_if_fail(self != NULL);

  /* Mark as disposed FIRST to prevent any async callbacks from running.
   * This is the same pattern as note_card_row_prepare_for_unbind. */
  self->disposed = TRUE;

  /* Cancel all async operations - this prevents callbacks from trying to
   * access widget memory during disposal. Cancel but don't clear the objects
   * here - finalize will handle that. This matches the dispose() behavior. */
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
  }

  if (self->image_cancellable) {
    g_cancellable_cancel(self->image_cancellable);
  }

  /* Clear external cancellable reference - it's owned by the parent and
   * will be cancelled by the parent's prepare_for_unbind */
  self->external_cancellable = NULL;
}
