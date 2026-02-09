/*
 * gnostr-article-reader.c - NIP-23 Article Reader Side Panel (nostrc-zwn4)
 *
 * Fetches a kind 30023 event from NDB, parses NIP-23 metadata, and renders
 * the full markdown content using Pango markup in a side panel.
 */

#define G_LOG_DOMAIN "gnostr-article-reader"

#include "gnostr-article-reader.h"
#include "gnostr-avatar-cache.h"
#include "gnostr-profile-provider.h"
#include "../storage_ndb.h"
#include "../util/nip23.h"
#include "../util/markdown_pango.h"
#include "../util/utils.h"

#include <json-glib/json-glib.h>
#include <glib/gi18n.h>

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-article-reader.ui"

struct _GnostrArticleReader {
  GtkWidget parent_instance;

  /* Template children */
  GtkWidget *root_box;
  GtkWidget *btn_close;
  GtkWidget *header_title;

  GtkWidget *loading_box;
  GtkWidget *loading_spinner;
  GtkWidget *error_box;
  GtkWidget *error_label;
  GtkWidget *scroll_window;

  GtkWidget *content_box;
  GtkWidget *header_image;
  GtkWidget *lbl_title;
  GtkWidget *author_box;
  GtkWidget *avatar_overlay;
  GtkWidget *avatar_image;
  GtkWidget *avatar_initials;
  GtkWidget *btn_author;
  GtkWidget *lbl_author_name;
  GtkWidget *lbl_date;
  GtkWidget *lbl_reading_time;
  GtkWidget *hashtags_flow;
  GtkWidget *lbl_content;

  GtkWidget *btn_zap;
  GtkWidget *btn_share;
  GtkWidget *btn_open_external;

  /* State */
  char *event_id;
  char *pubkey_hex;
  char *d_tag;
  char *author_lud16;

#ifdef HAVE_SOUP3
  GCancellable *image_cancellable;
#endif
};

G_DEFINE_FINAL_TYPE(GnostrArticleReader, gnostr_article_reader, GTK_TYPE_WIDGET)

enum {
  SIGNAL_CLOSE_REQUESTED,
  SIGNAL_OPEN_PROFILE,
  SIGNAL_OPEN_URL,
  SIGNAL_ZAP_REQUESTED,
  SIGNAL_SHARE_ARTICLE,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* ---- State management ---- */

static void show_loading(GnostrArticleReader *self) {
  gtk_widget_set_visible(self->loading_box, TRUE);
  gtk_widget_set_visible(self->error_box, FALSE);
  gtk_widget_set_visible(self->scroll_window, FALSE);
  gtk_spinner_set_spinning(GTK_SPINNER(self->loading_spinner), TRUE);
}

static void show_error(GnostrArticleReader *self, const char *message) {
  gtk_spinner_set_spinning(GTK_SPINNER(self->loading_spinner), FALSE);
  gtk_widget_set_visible(self->loading_box, FALSE);
  gtk_widget_set_visible(self->scroll_window, FALSE);
  gtk_widget_set_visible(self->error_box, TRUE);
  if (message)
    gtk_label_set_text(GTK_LABEL(self->error_label), message);
}

static void show_content(GnostrArticleReader *self) {
  gtk_spinner_set_spinning(GTK_SPINNER(self->loading_spinner), FALSE);
  gtk_widget_set_visible(self->loading_box, FALSE);
  gtk_widget_set_visible(self->error_box, FALSE);
  gtk_widget_set_visible(self->scroll_window, TRUE);
}

/* ---- Helpers ---- */

static char *format_date(gint64 timestamp) {
  if (timestamp <= 0) return NULL;
  GDateTime *dt = g_date_time_new_from_unix_utc(timestamp);
  if (!dt) return NULL;
  char *result = g_date_time_format(dt, "%b %d, %Y");
  g_date_time_unref(dt);
  return result;
}

static void clear_hashtags(GnostrArticleReader *self) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->hashtags_flow))) != NULL) {
    gtk_flow_box_remove(GTK_FLOW_BOX(self->hashtags_flow), child);
  }
  gtk_widget_set_visible(self->hashtags_flow, FALSE);
}

static void add_hashtag(GnostrArticleReader *self, const char *tag) {
  GtkWidget *label = gtk_label_new(NULL);
  /* nostrc-rdam: Escape tag content to prevent Pango markup injection.
   * Hashtag strings come from untrusted NIP-23 event "t" tags. */
  char *escaped = g_markup_escape_text(tag, -1);
  char *markup = g_strdup_printf("<small>#%s</small>", escaped);
  g_free(escaped);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  g_free(markup);
  gtk_widget_add_css_class(label, "dim-label");
  gtk_flow_box_append(GTK_FLOW_BOX(self->hashtags_flow), label);
  gtk_widget_set_visible(self->hashtags_flow, TRUE);
}

/* ---- Signal handlers ---- */

static void on_close_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrArticleReader *self = GNOSTR_ARTICLE_READER(user_data);
  g_signal_emit(self, signals[SIGNAL_CLOSE_REQUESTED], 0);
}

static void on_author_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrArticleReader *self = GNOSTR_ARTICLE_READER(user_data);
  if (self->pubkey_hex)
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey_hex);
}

static void on_zap_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrArticleReader *self = GNOSTR_ARTICLE_READER(user_data);
  if (self->event_id && self->pubkey_hex)
    g_signal_emit(self, signals[SIGNAL_ZAP_REQUESTED], 0,
                  self->event_id, self->pubkey_hex,
                  self->author_lud16 ? self->author_lud16 : "");
}

static void on_share_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrArticleReader *self = GNOSTR_ARTICLE_READER(user_data);
  if (!self->pubkey_hex || !self->d_tag) return;

  char *naddr = gnostr_article_build_naddr(NOSTR_KIND_LONG_FORM,
                                            self->pubkey_hex,
                                            self->d_tag, NULL);
  if (naddr) {
    char *uri = g_strdup_printf("nostr:%s", naddr);
    g_signal_emit(self, signals[SIGNAL_SHARE_ARTICLE], 0, uri);
    g_free(uri);
    g_free(naddr);
  }
}

static void on_open_external_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrArticleReader *self = GNOSTR_ARTICLE_READER(user_data);
  if (!self->pubkey_hex || !self->d_tag) return;

  char *naddr = gnostr_article_build_naddr(NOSTR_KIND_LONG_FORM,
                                            self->pubkey_hex,
                                            self->d_tag, NULL);
  if (naddr) {
    char *url = g_strdup_printf("https://habla.news/a/%s", naddr);
    g_signal_emit(self, signals[SIGNAL_OPEN_URL], 0, url);
    g_free(url);
    g_free(naddr);
  }
}

/* ---- Async header image loading ---- */

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>

static void on_header_image_ready(GObject *source, GAsyncResult *result, gpointer user_data) {
  GnostrArticleReader *self = GNOSTR_ARTICLE_READER(user_data);
  GError *error = NULL;
  GInputStream *stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);
  if (error || !stream) {
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning("article_reader: header image fetch failed: %s", error->message);
    g_clear_error(&error);
    if (stream) g_object_unref(stream);
    g_object_unref(self);
    return;
  }

  /* nostrc-8bxd: Store GBytes in a variable to avoid leak and NULL crash.
   * g_input_stream_read_bytes() can return NULL on error, and the returned
   * GBytes must be unreffed after use. */
  GBytes *bytes = g_input_stream_read_bytes(stream, 10 * 1024 * 1024, NULL, &error);
  g_object_unref(stream);

  if (!bytes || g_bytes_get_size(bytes) == 0) {
    g_warning("article_reader: empty or failed header image read");
    if (bytes) g_bytes_unref(bytes);
    g_clear_error(&error);
    g_object_unref(self);
    return;
  }

  GdkTexture *texture = gdk_texture_new_from_bytes(bytes, &error);
  g_bytes_unref(bytes);

  if (texture) {
    gtk_picture_set_paintable(GTK_PICTURE(self->header_image), GDK_PAINTABLE(texture));
    gtk_widget_set_visible(self->header_image, TRUE);
    g_object_unref(texture);
  } else {
    if (error)
      g_warning("article_reader: failed to create header texture: %s", error->message);
    g_clear_error(&error);
  }
  g_object_unref(self);
}

static void load_header_image(GnostrArticleReader *self, const char *url) {
  if (!url || !*url) return;

  SoupSession *session = gnostr_get_shared_soup_session();
  if (!session) return;

  if (self->image_cancellable) {
    g_cancellable_cancel(self->image_cancellable);
    g_clear_object(&self->image_cancellable);
  }
  self->image_cancellable = g_cancellable_new();

  SoupMessage *msg = soup_message_new("GET", url);
  if (!msg) return;

  soup_session_send_async(session, msg, G_PRIORITY_DEFAULT,
                          self->image_cancellable,
                          on_header_image_ready,
                          g_object_ref(self));
  g_object_unref(msg);
}
#else
static void load_header_image(GnostrArticleReader *self, const char *url) {
  (void)self;
  (void)url;
}
#endif

/* ---- GObject boilerplate ---- */

static void gnostr_article_reader_dispose(GObject *object) {
  GnostrArticleReader *self = GNOSTR_ARTICLE_READER(object);

#ifdef HAVE_SOUP3
  if (self->image_cancellable) {
    g_cancellable_cancel(self->image_cancellable);
    g_clear_object(&self->image_cancellable);
  }
#endif

  gtk_widget_set_layout_manager(GTK_WIDGET(self), NULL);
  g_clear_pointer(&self->root_box, gtk_widget_unparent);

  G_OBJECT_CLASS(gnostr_article_reader_parent_class)->dispose(object);
}

static void gnostr_article_reader_finalize(GObject *object) {
  GnostrArticleReader *self = GNOSTR_ARTICLE_READER(object);

  g_free(self->event_id);
  g_free(self->pubkey_hex);
  g_free(self->d_tag);
  g_free(self->author_lud16);

  G_OBJECT_CLASS(gnostr_article_reader_parent_class)->finalize(object);
}

static void gnostr_article_reader_class_init(GnostrArticleReaderClass *klass) {
  GObjectClass *obj_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  obj_class->dispose = gnostr_article_reader_dispose;
  obj_class->finalize = gnostr_article_reader_finalize;

  signals[SIGNAL_CLOSE_REQUESTED] = g_signal_new(
      "close-requested", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
      0, NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  signals[SIGNAL_OPEN_PROFILE] = g_signal_new(
      "open-profile", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
      0, NULL, NULL, g_cclosure_marshal_VOID__STRING, G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_URL] = g_signal_new(
      "open-url", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
      0, NULL, NULL, g_cclosure_marshal_VOID__STRING, G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_ZAP_REQUESTED] = g_signal_new(
      "zap-requested", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL, G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIGNAL_SHARE_ARTICLE] = g_signal_new(
      "share-article", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
      0, NULL, NULL, g_cclosure_marshal_VOID__STRING, G_TYPE_NONE, 1, G_TYPE_STRING);

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "article-reader");
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);

  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, root_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, btn_close);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, header_title);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, loading_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, loading_spinner);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, error_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, error_label);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, scroll_window);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, content_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, header_image);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, lbl_title);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, author_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, avatar_overlay);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, avatar_image);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, avatar_initials);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, btn_author);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, lbl_author_name);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, lbl_date);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, lbl_reading_time);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, hashtags_flow);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, lbl_content);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, btn_zap);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, btn_share);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleReader, btn_open_external);
}

static void gnostr_article_reader_init(GnostrArticleReader *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  g_signal_connect(self->btn_close, "clicked", G_CALLBACK(on_close_clicked), self);
  g_signal_connect(self->btn_author, "clicked", G_CALLBACK(on_author_clicked), self);
  g_signal_connect(self->btn_zap, "clicked", G_CALLBACK(on_zap_clicked), self);
  g_signal_connect(self->btn_share, "clicked", G_CALLBACK(on_share_clicked), self);
  g_signal_connect(self->btn_open_external, "clicked", G_CALLBACK(on_open_external_clicked), self);
}

/* ---- Public API ---- */

GtkWidget *gnostr_article_reader_new(void) {
  return g_object_new(GNOSTR_TYPE_ARTICLE_READER, NULL);
}

void gnostr_article_reader_load_event(GnostrArticleReader *self,
                                       const char *event_id_hex) {
  g_return_if_fail(GNOSTR_IS_ARTICLE_READER(self));
  g_return_if_fail(event_id_hex != NULL);

  show_loading(self);

  /* Clear previous state */
  g_clear_pointer(&self->event_id, g_free);
  g_clear_pointer(&self->pubkey_hex, g_free);
  g_clear_pointer(&self->d_tag, g_free);
  g_clear_pointer(&self->author_lud16, g_free);
  self->event_id = g_strdup(event_id_hex);

  /* Fetch event JSON from NDB */
  char *json = NULL;
  int json_len = 0;
  if (storage_ndb_get_note_by_id_nontxn(event_id_hex, &json, &json_len) != 0 || !json) {
    show_error(self, "Article not found in local database");
    return;
  }

  /* Parse event JSON */
  JsonParser *parser = json_parser_new();
  GError *error = NULL;
  if (!json_parser_load_from_data(parser, json, json_len, &error)) {
    g_warning("Failed to parse article JSON: %s", error->message);
    g_error_free(error);
    g_object_unref(parser);
    show_error(self, "Failed to parse article data");
    return;
  }

  JsonNode *root = json_parser_get_root(parser);
  JsonObject *obj = json_node_get_object(root);
  if (!obj) {
    g_object_unref(parser);
    show_error(self, "Invalid article data");
    return;
  }

  /* Extract content and pubkey */
  const char *content = json_object_has_member(obj, "content")
      ? json_object_get_string_member(obj, "content") : NULL;
  const char *pubkey = json_object_has_member(obj, "pubkey")
      ? json_object_get_string_member(obj, "pubkey") : NULL;
  gint64 created_at = json_object_has_member(obj, "created_at")
      ? json_object_get_int_member(obj, "created_at") : 0;

  self->pubkey_hex = g_strdup(pubkey);

  /* Extract and serialize tags for NIP-23 parser */
  GnostrArticleMeta *meta = NULL;
  if (json_object_has_member(obj, "tags")) {
    JsonArray *tags = json_object_get_array_member(obj, "tags");
    JsonGenerator *gen = json_generator_new();
    JsonNode *tags_node = json_node_new(JSON_NODE_ARRAY);
    json_node_set_array(tags_node, tags);
    json_generator_set_root(gen, tags_node);
    char *tags_json = json_generator_to_data(gen, NULL);
    json_node_unref(tags_node);
    g_object_unref(gen);

    meta = gnostr_article_parse_tags(tags_json);
    g_free(tags_json);
  }

  /* Populate title */
  const char *title = (meta && meta->title) ? meta->title : "Untitled";
  gtk_label_set_text(GTK_LABEL(self->lbl_title), title);

  /* Store d_tag for share/naddr */
  if (meta && meta->d_tag) {
    self->d_tag = g_strdup(meta->d_tag);
  }

  /* Header image */
  gtk_widget_set_visible(self->header_image, FALSE);
  if (meta && meta->image && *meta->image) {
    load_header_image(self, meta->image);
  }

  /* Date */
  gint64 ts = (meta && meta->published_at > 0) ? meta->published_at : created_at;
  g_autofree char *date_str = format_date(ts);
  gtk_label_set_text(GTK_LABEL(self->lbl_date), date_str ? date_str : "");

  /* Reading time */
  if (content && *content) {
    int minutes = gnostr_article_estimate_reading_time(content, 0);
    g_autofree char *rt = g_strdup_printf("%d min read", minutes > 0 ? minutes : 1);
    gtk_label_set_text(GTK_LABEL(self->lbl_reading_time), rt);
  } else {
    gtk_label_set_text(GTK_LABEL(self->lbl_reading_time), "");
  }

  /* Hashtags */
  clear_hashtags(self);
  if (meta && meta->hashtags) {
    for (gsize i = 0; i < meta->hashtags_count; i++) {
      add_hashtag(self, meta->hashtags[i]);
    }
  }

  /* Author profile */
  gtk_label_set_text(GTK_LABEL(self->lbl_author_name), "Unknown");
  gtk_widget_set_visible(self->avatar_image, FALSE);
  gtk_widget_set_visible(self->avatar_initials, TRUE);

  if (pubkey) {
    GnostrProfileMeta *profile = gnostr_profile_provider_get(pubkey);
    if (profile) {
      const char *name = profile->display_name ? profile->display_name : profile->name;
      if (name && *name)
        gtk_label_set_text(GTK_LABEL(self->lbl_author_name), name);

      if (profile->picture && *profile->picture) {
        GdkTexture *cached = gnostr_avatar_try_load_cached(profile->picture);
        if (cached) {
          gtk_picture_set_paintable(GTK_PICTURE(self->avatar_image), GDK_PAINTABLE(cached));
          gtk_widget_set_visible(self->avatar_image, TRUE);
          gtk_widget_set_visible(self->avatar_initials, FALSE);
          g_object_unref(cached);
        } else {
          gnostr_avatar_download_async(profile->picture, self->avatar_image, self->avatar_initials);
        }
      }

      self->author_lud16 = g_strdup(profile->lud16);
      gnostr_profile_meta_free(profile);
    }
  }

  /* Markdown content */
  if (content && *content) {
    char *pango = markdown_to_pango(content, 0);
    if (pango) {
      gtk_label_set_markup(GTK_LABEL(self->lbl_content), pango);
      g_free(pango);
    } else {
      gtk_label_set_text(GTK_LABEL(self->lbl_content), content);
    }
  } else {
    gtk_label_set_text(GTK_LABEL(self->lbl_content), "(No content)");
  }

  if (meta)
    gnostr_article_meta_free(meta);
  g_object_unref(parser);

  show_content(self);
  g_debug("[ARTICLE-READER] Loaded article: %s", event_id_hex);
}

void gnostr_article_reader_clear(GnostrArticleReader *self) {
  g_return_if_fail(GNOSTR_IS_ARTICLE_READER(self));

  g_clear_pointer(&self->event_id, g_free);
  g_clear_pointer(&self->pubkey_hex, g_free);
  g_clear_pointer(&self->d_tag, g_free);
  g_clear_pointer(&self->author_lud16, g_free);

  gtk_label_set_text(GTK_LABEL(self->lbl_title), "");
  gtk_label_set_text(GTK_LABEL(self->lbl_author_name), "Unknown");
  gtk_label_set_text(GTK_LABEL(self->lbl_date), "");
  gtk_label_set_text(GTK_LABEL(self->lbl_reading_time), "");
  gtk_label_set_text(GTK_LABEL(self->lbl_content), "");
  gtk_widget_set_visible(self->header_image, FALSE);
  clear_hashtags(self);

  gtk_widget_set_visible(self->loading_box, FALSE);
  gtk_widget_set_visible(self->error_box, FALSE);
  gtk_widget_set_visible(self->scroll_window, FALSE);
}
