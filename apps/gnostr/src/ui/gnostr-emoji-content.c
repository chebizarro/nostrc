/**
 * @file gnostr-emoji-content.c
 * @brief NIP-30 Custom Emoji-aware content widget implementation
 *
 * Renders text content with inline custom emoji images using GtkFlowBox.
 * Segments content into text and emoji parts, displaying emojis as GtkPicture
 * widgets mixed with GtkLabel text segments.
 */

#include "gnostr-emoji-content.h"
#include "../util/utils.h"
#include <string.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

struct _GnostrEmojiContent {
  GtkWidget parent_instance;

  GtkWidget *flow_box;      /* GtkFlowBox container for content */
  gchar *plain_text;        /* Plain text content for clipboard */
  gboolean wrap;
  gboolean selectable;
};

G_DEFINE_TYPE(GnostrEmojiContent, gnostr_emoji_content, GTK_TYPE_WIDGET)

/* Forward declarations */
static void gnostr_emoji_content_dispose(GObject *obj);
static void gnostr_emoji_content_finalize(GObject *obj);
static void rebuild_content(GnostrEmojiContent *self, const char *content, GnostrEmojiList *emoji_list);

/* Callback for when emoji texture is loaded */
typedef struct {
  GtkPicture *picture;
  gchar *url;
} EmojiLoadCtx;

static void emoji_load_ctx_free(EmojiLoadCtx *ctx) {
  if (!ctx) return;
  g_free(ctx->url);
  g_free(ctx);
}

#ifdef HAVE_SOUP3
static void on_emoji_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
#endif

static void gnostr_emoji_content_class_init(GnostrEmojiContentClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_emoji_content_dispose;
  object_class->finalize = gnostr_emoji_content_finalize;

  /* Set up widget layout */
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "emoji-content");
}

static void gnostr_emoji_content_init(GnostrEmojiContent *self) {
  self->wrap = TRUE;
  self->selectable = FALSE;
  self->plain_text = NULL;

  /* Create the flow box container */
  self->flow_box = gtk_flow_box_new();
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(self->flow_box), FALSE);
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->flow_box), GTK_SELECTION_NONE);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->flow_box), 1);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->flow_box), 100);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->flow_box), 0);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->flow_box), 0);
  gtk_widget_set_valign(self->flow_box, GTK_ALIGN_START);
  gtk_widget_set_halign(self->flow_box, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand(self->flow_box, TRUE);

  gtk_widget_set_parent(self->flow_box, GTK_WIDGET(self));
}

static void gnostr_emoji_content_dispose(GObject *obj) {
  GnostrEmojiContent *self = GNOSTR_EMOJI_CONTENT(obj);

  g_clear_pointer(&self->flow_box, gtk_widget_unparent);

  G_OBJECT_CLASS(gnostr_emoji_content_parent_class)->dispose(obj);
}

static void gnostr_emoji_content_finalize(GObject *obj) {
  GnostrEmojiContent *self = GNOSTR_EMOJI_CONTENT(obj);

  g_clear_pointer(&self->plain_text, g_free);

  G_OBJECT_CLASS(gnostr_emoji_content_parent_class)->finalize(obj);
}

GnostrEmojiContent *gnostr_emoji_content_new(void) {
  return g_object_new(GNOSTR_TYPE_EMOJI_CONTENT, NULL);
}

/**
 * Find :shortcode: pattern starting at position p.
 * Returns the shortcode (without colons) if found, NULL otherwise.
 * Sets *end_pos to point after the closing colon.
 */
static gchar *find_shortcode(const char *p, const char **end_pos) {
  if (!p || *p != ':') return NULL;

  const char *start = p + 1;
  const char *end = start;

  /* Shortcode must be alphanumeric, underscore, or hyphen only */
  while (*end && *end != ':' && *end != ' ' && *end != '\n' && *end != '\t') {
    char c = *end;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) {
      return NULL;
    }
    end++;
  }

  if (*end != ':' || end == start) return NULL;

  *end_pos = end + 1;
  return g_strndup(start, end - start);
}

/**
 * Add a text segment to the flow box.
 */
static void add_text_segment(GnostrEmojiContent *self, const char *text, gsize len) {
  if (!text || len == 0) return;

  g_autofree gchar *segment = g_strndup(text, len);
  GtkWidget *label = gtk_label_new(segment);
  gtk_label_set_wrap(GTK_LABEL(label), self->wrap);
  gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_selectable(GTK_LABEL(label), self->selectable);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_add_css_class(label, "emoji-content-text");

  gtk_flow_box_append(GTK_FLOW_BOX(self->flow_box), label);
}

/**
 * Add an emoji image to the flow box.
 */
static void add_emoji_image(GnostrEmojiContent *self, GnostrCustomEmoji *emoji) {
  if (!emoji || !emoji->url) return;

  /* Create a picture widget for the emoji */
  GtkWidget *picture = gtk_picture_new();
  gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_size_request(picture, 24, 24);  /* Standard emoji size */
  gtk_widget_add_css_class(picture, "custom-emoji");
  gtk_widget_set_tooltip_text(picture, emoji->shortcode);

  /* Try to load from cache first */
  GdkTexture *cached = gnostr_emoji_try_load_cached(emoji->url);
  if (cached) {
    gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(cached));
    g_object_unref(cached);
  } else {
    /* Load asynchronously - use shared session to reduce memory overhead */
#ifdef HAVE_SOUP3
    SoupSession *session = gnostr_get_shared_soup_session();
    if (session) {
      SoupMessage *msg = soup_message_new("GET", emoji->url);
      if (msg) {
        EmojiLoadCtx *ctx = g_new0(EmojiLoadCtx, 1);
        ctx->picture = GTK_PICTURE(g_object_ref(picture));
        ctx->url = g_strdup(emoji->url);
        soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT, NULL, on_emoji_loaded, ctx);
        g_object_unref(msg);
      }
    }
#endif
    /* Prefetch to cache */
    gnostr_emoji_cache_prefetch(emoji->url);
  }

  gtk_flow_box_append(GTK_FLOW_BOX(self->flow_box), picture);
}

#ifdef HAVE_SOUP3
static void on_emoji_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
  EmojiLoadCtx *ctx = (EmojiLoadCtx *)user_data;
  if (!ctx) return;

  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  /* Check for errors first - don't proceed if fetch failed */
  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("Emoji: failed to load %s: %s", ctx->url ? ctx->url : "(null)", error->message);
    }
    g_error_free(error);
    if (ctx->picture) g_object_unref(ctx->picture);
    emoji_load_ctx_free(ctx);
    return;
  }

  if (bytes && ctx->picture && GTK_IS_PICTURE(ctx->picture)) {
    GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream_at_scale(stream, 24, 24, TRUE, NULL, NULL);
    g_object_unref(stream);

    if (pixbuf) {
      /* Create texture from pixbuf */
      int width = gdk_pixbuf_get_width(pixbuf);
      int height = gdk_pixbuf_get_height(pixbuf);
      int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
      gboolean has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
      GBytes *pix_bytes = gdk_pixbuf_read_pixel_bytes(pixbuf);

      GdkMemoryFormat format = has_alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8;
      GdkTexture *texture = gdk_memory_texture_new(width, height, format, pix_bytes, rowstride);

      gtk_picture_set_paintable(ctx->picture, GDK_PAINTABLE(texture));

      g_bytes_unref(pix_bytes);
      g_object_unref(texture);
      g_object_unref(pixbuf);
    }
  }

  if (bytes) g_bytes_unref(bytes);
  if (ctx->picture) g_object_unref(ctx->picture);
  emoji_load_ctx_free(ctx);
}
#endif

/**
 * Clear and rebuild the flow box content.
 */
static void rebuild_content(GnostrEmojiContent *self, const char *content, GnostrEmojiList *emoji_list) {
  if (!self->flow_box) return;

  /* Clear existing children */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(self->flow_box)) != NULL) {
    gtk_flow_box_remove(GTK_FLOW_BOX(self->flow_box), child);
  }

  if (!content || !*content) return;

  /* Store plain text */
  g_clear_pointer(&self->plain_text, g_free);
  self->plain_text = g_strdup(content);

  /* If no emoji list, just add the whole content as text */
  if (!emoji_list || emoji_list->count == 0) {
    add_text_segment(self, content, strlen(content));
    return;
  }

  /* Parse content and replace :shortcode: patterns with emoji images */
  const char *p = content;
  const char *segment_start = p;

  while (*p) {
    if (*p == ':') {
      const char *end_pos = NULL;
      gchar *shortcode = find_shortcode(p, &end_pos);

      if (shortcode) {
        GnostrCustomEmoji *emoji = gnostr_emoji_find_by_shortcode(emoji_list, shortcode);
        if (emoji) {
          /* Add text segment before this emoji */
          if (p > segment_start) {
            add_text_segment(self, segment_start, p - segment_start);
          }

          /* Add the emoji image */
          add_emoji_image(self, emoji);

          /* Continue after the shortcode */
          p = end_pos;
          segment_start = p;
          g_free(shortcode);
          continue;
        }
        g_free(shortcode);
      }
    }
    p++;
  }

  /* Add any remaining text */
  if (p > segment_start) {
    add_text_segment(self, segment_start, p - segment_start);
  }
}

void gnostr_emoji_content_set_content(GnostrEmojiContent *self,
                                       const char *content,
                                       GnostrEmojiList *emoji_list) {
  g_return_if_fail(GNOSTR_IS_EMOJI_CONTENT(self));

  rebuild_content(self, content, emoji_list);
}

void gnostr_emoji_content_set_wrap(GnostrEmojiContent *self, gboolean wrap) {
  g_return_if_fail(GNOSTR_IS_EMOJI_CONTENT(self));
  self->wrap = wrap;
}

void gnostr_emoji_content_set_selectable(GnostrEmojiContent *self, gboolean selectable) {
  g_return_if_fail(GNOSTR_IS_EMOJI_CONTENT(self));
  self->selectable = selectable;
}

const char *gnostr_emoji_content_get_text(GnostrEmojiContent *self) {
  g_return_val_if_fail(GNOSTR_IS_EMOJI_CONTENT(self), NULL);
  return self->plain_text;
}
