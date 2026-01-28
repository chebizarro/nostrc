/*
 * gnostr-article-card.c - NIP-23 Long-form Content Card Widget
 *
 * Displays kind 30023 long-form article events.
 */

#include "gnostr-article-card.h"
#include "gnostr-avatar-cache.h"
#include "../util/markdown_pango.h"
#include "../util/nip05.h"
#include "../util/nip84_highlights.h"
#include "../util/utils.h"
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <nostr/nip19/nip19.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-article-card.ui"

/* Average reading speed in words per minute */
#define READING_WPM 200

struct _GnostrArticleCard {
  GtkWidget parent_instance;

  /* Template widgets */
  GtkWidget *root;
  GtkWidget *header_image_overlay;
  GtkWidget *header_image;
  GtkWidget *header_gradient;
  GtkWidget *content_box;
  GtkWidget *btn_avatar;
  GtkWidget *avatar_overlay;
  GtkWidget *avatar_image;
  GtkWidget *avatar_initials;
  GtkWidget *btn_author_name;
  GtkWidget *lbl_author_name;
  GtkWidget *lbl_author_handle;
  GtkWidget *lbl_publish_date;
  GtkWidget *nip05_badge;
  GtkWidget *btn_title;
  GtkWidget *lbl_title;
  GtkWidget *lbl_summary;
  GtkWidget *btn_read_more;
  GtkWidget *hashtags_box;
  GtkWidget *btn_menu;
  GtkWidget *btn_zap;
  GtkWidget *lbl_zap_count;
  GtkWidget *btn_bookmark;
  GtkWidget *btn_share;
  GtkWidget *lbl_reading_time;
  GtkWidget *menu_popover;

  /* State */
  gchar *event_id;
  gchar *d_tag;
  gchar *pubkey_hex;
  gchar *author_lud16;
  gchar *nip05;
  gint64 published_at;
  gboolean is_bookmarked;
  gboolean is_logged_in;
  gchar *content_markdown;

#ifdef HAVE_SOUP3
  GCancellable *avatar_cancellable;
  GCancellable *header_cancellable;
  /* Uses gnostr_get_shared_soup_session() instead of per-widget session */
#endif

  GCancellable *nip05_cancellable;
};

G_DEFINE_TYPE(GnostrArticleCard, gnostr_article_card, GTK_TYPE_WIDGET)

enum {
  SIGNAL_OPEN_PROFILE,
  SIGNAL_OPEN_ARTICLE,
  SIGNAL_OPEN_URL,
  SIGNAL_ZAP_REQUESTED,
  SIGNAL_BOOKMARK_TOGGLED,
  SIGNAL_SHARE_ARTICLE,
  SIGNAL_HIGHLIGHT_REQUESTED,  /* NIP-84 highlight request */
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static void gnostr_article_card_dispose(GObject *object) {
  GnostrArticleCard *self = GNOSTR_ARTICLE_CARD(object);

  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
  }

#ifdef HAVE_SOUP3
  if (self->avatar_cancellable) {
    g_cancellable_cancel(self->avatar_cancellable);
    g_clear_object(&self->avatar_cancellable);
  }
  if (self->header_cancellable) {
    g_cancellable_cancel(self->header_cancellable);
    g_clear_object(&self->header_cancellable);
  }
  /* Shared session is managed globally - do not clear here */
#endif

  if (self->menu_popover) {
    if (GTK_IS_POPOVER(self->menu_popover)) {
      gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
    }
    gtk_widget_unparent(self->menu_popover);
    self->menu_popover = NULL;
  }

  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_ARTICLE_CARD);
  G_OBJECT_CLASS(gnostr_article_card_parent_class)->dispose(object);
}

static void gnostr_article_card_finalize(GObject *obj) {
  GnostrArticleCard *self = GNOSTR_ARTICLE_CARD(obj);

  g_clear_pointer(&self->event_id, g_free);
  g_clear_pointer(&self->d_tag, g_free);
  g_clear_pointer(&self->pubkey_hex, g_free);
  g_clear_pointer(&self->author_lud16, g_free);
  g_clear_pointer(&self->nip05, g_free);
  g_clear_pointer(&self->content_markdown, g_free);

  G_OBJECT_CLASS(gnostr_article_card_parent_class)->finalize(obj);
}

/* Helper: compute reading time from word count */
static gchar *compute_reading_time(const char *content) {
  if (!content || !*content) return NULL;

  /* Count words by spaces */
  gint word_count = 0;
  gboolean in_word = FALSE;

  for (const char *p = content; *p; p++) {
    if (g_ascii_isspace(*p)) {
      in_word = FALSE;
    } else if (!in_word) {
      in_word = TRUE;
      word_count++;
    }
  }

  gint minutes = (word_count + READING_WPM - 1) / READING_WPM;
  if (minutes < 1) minutes = 1;

  return g_strdup_printf("%d min read", minutes);
}

/* Helper: format publication date */
static gchar *format_publish_date(gint64 published_at) {
  if (published_at <= 0) return g_strdup(_("Unknown date"));

  GDateTime *dt = g_date_time_new_from_unix_local(published_at);
  if (!dt) return g_strdup(_("Unknown date"));

  GDateTime *now = g_date_time_new_now_local();
  GTimeSpan diff = g_date_time_difference(now, dt);
  g_date_time_unref(now);

  gchar *result;
  gint64 seconds = diff / G_TIME_SPAN_SECOND;

  if (seconds < 60) {
    result = g_strdup(_("Just now"));
  } else if (seconds < 3600) {
    gint minutes = (gint)(seconds / 60);
    result = g_strdup_printf(g_dngettext(NULL, "%d minute ago", "%d minutes ago", minutes), minutes);
  } else if (seconds < 86400) {
    gint hours = (gint)(seconds / 3600);
    result = g_strdup_printf(g_dngettext(NULL, "%d hour ago", "%d hours ago", hours), hours);
  } else if (seconds < 604800) {
    gint days = (gint)(seconds / 86400);
    result = g_strdup_printf(g_dngettext(NULL, "%d day ago", "%d days ago", days), days);
  } else {
    /* Show full date for older articles */
    result = g_date_time_format(dt, "%B %d, %Y");
  }

  g_date_time_unref(dt);
  return result;
}

/* Set avatar initials fallback */
static void set_avatar_initials(GnostrArticleCard *self, const char *display, const char *handle) {
  if (!GTK_IS_LABEL(self->avatar_initials)) return;

  const char *src = (display && *display) ? display : (handle && *handle ? handle : "AN");
  char initials[3] = {0};
  int i = 0;

  for (const char *p = src; *p && i < 2; p++) {
    if (g_ascii_isalnum(*p)) {
      initials[i++] = g_ascii_toupper(*p);
    }
  }
  if (i == 0) {
    initials[0] = 'A';
    initials[1] = 'N';
  }

  gtk_label_set_text(GTK_LABEL(self->avatar_initials), initials);
  if (self->avatar_image) gtk_widget_set_visible(self->avatar_image, FALSE);
  gtk_widget_set_visible(self->avatar_initials, TRUE);
}

/* Click handlers */
static void on_avatar_clicked(GtkButton *btn, gpointer user_data) {
  GnostrArticleCard *self = GNOSTR_ARTICLE_CARD(user_data);
  (void)btn;
  if (self->pubkey_hex && *self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey_hex);
  }
}

static void on_author_name_clicked(GtkButton *btn, gpointer user_data) {
  GnostrArticleCard *self = GNOSTR_ARTICLE_CARD(user_data);
  (void)btn;
  if (self->pubkey_hex && *self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey_hex);
  }
}

static void on_title_clicked(GtkButton *btn, gpointer user_data) {
  GnostrArticleCard *self = GNOSTR_ARTICLE_CARD(user_data);
  (void)btn;
  if (self->event_id && *self->event_id) {
    g_signal_emit(self, signals[SIGNAL_OPEN_ARTICLE], 0, self->event_id);
  }
}

static void on_read_more_clicked(GtkButton *btn, gpointer user_data) {
  GnostrArticleCard *self = GNOSTR_ARTICLE_CARD(user_data);
  (void)btn;
  if (self->event_id && *self->event_id) {
    g_signal_emit(self, signals[SIGNAL_OPEN_ARTICLE], 0, self->event_id);
  }
}

static void on_zap_clicked(GtkButton *btn, gpointer user_data) {
  GnostrArticleCard *self = GNOSTR_ARTICLE_CARD(user_data);
  (void)btn;
  if (self->event_id && self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_ZAP_REQUESTED], 0,
                  self->event_id, self->pubkey_hex, self->author_lud16);
  }
}

static void on_bookmark_clicked(GtkButton *btn, gpointer user_data) {
  GnostrArticleCard *self = GNOSTR_ARTICLE_CARD(user_data);
  (void)btn;

  if (!self->event_id) return;

  self->is_bookmarked = !self->is_bookmarked;

  /* Update button icon */
  if (GTK_IS_BUTTON(self->btn_bookmark)) {
    gtk_button_set_icon_name(GTK_BUTTON(self->btn_bookmark),
      self->is_bookmarked ? "user-bookmarks-symbolic" : "bookmark-new-symbolic");
  }

  g_signal_emit(self, signals[SIGNAL_BOOKMARK_TOGGLED], 0,
                self->event_id, self->is_bookmarked);
}

/* Helper: convert hex to bytes */
static gboolean hex_to_bytes_32(const char *hex, unsigned char out[32]) {
  if (!hex || strlen(hex) != 64) return FALSE;
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(hex + i * 2, "%2x", &byte) != 1) return FALSE;
    out[i] = (unsigned char)byte;
  }
  return TRUE;
}

static void on_share_clicked(GtkButton *btn, gpointer user_data) {
  GnostrArticleCard *self = GNOSTR_ARTICLE_CARD(user_data);
  (void)btn;

  if (!self->event_id || !self->pubkey_hex || !self->d_tag) return;

  /* Build naddr for NIP-33 addressable event */
  NostrNAddrConfig naddr_cfg = {
    .identifier = self->d_tag,
    .public_key = self->pubkey_hex,
    .kind = 30023,
    .relays = NULL,
    .relays_count = 0
  };

  char *encoded = NULL;
  NostrPointer *ptr = NULL;

  if (nostr_pointer_from_naddr_config(&naddr_cfg, &ptr) == 0 && ptr) {
    nostr_pointer_to_bech32(ptr, &encoded);
    nostr_pointer_free(ptr);
  }

  if (encoded) {
    char *uri = g_strdup_printf("nostr:%s", encoded);
    g_signal_emit(self, signals[SIGNAL_SHARE_ARTICLE], 0, uri);
    g_free(uri);
    free(encoded);
  }
}

static void on_menu_clicked(GtkButton *btn, gpointer user_data) {
  GnostrArticleCard *self = GNOSTR_ARTICLE_CARD(user_data);
  (void)btn;

  if (!self->menu_popover) {
    self->menu_popover = gtk_popover_new();

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    /* Copy Article Link */
    GtkWidget *copy_btn = gtk_button_new();
    GtkWidget *copy_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *copy_icon = gtk_image_new_from_icon_name("edit-copy-symbolic");
    GtkWidget *copy_label = gtk_label_new(_("Copy Article Link"));
    gtk_box_append(GTK_BOX(copy_box), copy_icon);
    gtk_box_append(GTK_BOX(copy_box), copy_label);
    gtk_button_set_child(GTK_BUTTON(copy_btn), copy_box);
    gtk_button_set_has_frame(GTK_BUTTON(copy_btn), FALSE);
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_share_clicked), self);
    gtk_box_append(GTK_BOX(box), copy_btn);

    /* View Author Profile */
    GtkWidget *profile_btn = gtk_button_new();
    GtkWidget *profile_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *profile_icon = gtk_image_new_from_icon_name("avatar-default-symbolic");
    GtkWidget *profile_label = gtk_label_new(_("View Author Profile"));
    gtk_box_append(GTK_BOX(profile_box), profile_icon);
    gtk_box_append(GTK_BOX(profile_box), profile_label);
    gtk_button_set_child(GTK_BUTTON(profile_btn), profile_box);
    gtk_button_set_has_frame(GTK_BUTTON(profile_btn), FALSE);
    g_signal_connect(profile_btn, "clicked", G_CALLBACK(on_avatar_clicked), self);
    gtk_box_append(GTK_BOX(box), profile_btn);

    gtk_popover_set_child(GTK_POPOVER(self->menu_popover), box);
    gtk_widget_set_parent(self->menu_popover, GTK_WIDGET(self->btn_menu));
  }

  gtk_popover_popup(GTK_POPOVER(self->menu_popover));
}

static void gnostr_article_card_class_init(GnostrArticleCardClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);

  gclass->dispose = gnostr_article_card_dispose;
  gclass->finalize = gnostr_article_card_finalize;

  gtk_widget_class_set_layout_manager_type(wclass, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_template_from_resource(wclass, UI_RESOURCE);

  /* Bind template children */
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, root);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, header_image_overlay);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, header_image);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, header_gradient);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, content_box);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, btn_avatar);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, avatar_overlay);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, avatar_image);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, avatar_initials);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, btn_author_name);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, lbl_author_name);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, lbl_author_handle);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, lbl_publish_date);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, nip05_badge);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, btn_title);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, lbl_title);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, lbl_summary);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, btn_read_more);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, hashtags_box);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, btn_menu);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, btn_zap);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, lbl_zap_count);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, btn_bookmark);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, btn_share);
  gtk_widget_class_bind_template_child(wclass, GnostrArticleCard, lbl_reading_time);

  /* Signals */
  signals[SIGNAL_OPEN_PROFILE] = g_signal_new("open-profile",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_ARTICLE] = g_signal_new("open-article",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_URL] = g_signal_new("open-url",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_ZAP_REQUESTED] = g_signal_new("zap-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIGNAL_BOOKMARK_TOGGLED] = g_signal_new("bookmark-toggled",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);

  signals[SIGNAL_SHARE_ARTICLE] = g_signal_new("share-article",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  /* NIP-84 highlight request: highlighted_text, context, a_tag, pubkey_hex */
  signals[SIGNAL_HIGHLIGHT_REQUESTED] = g_signal_new("highlight-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
}

static void gnostr_article_card_init(GnostrArticleCard *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  gtk_widget_add_css_class(GTK_WIDGET(self), "article-card");

  /* Connect click handlers */
  if (GTK_IS_BUTTON(self->btn_avatar)) {
    g_signal_connect(self->btn_avatar, "clicked", G_CALLBACK(on_avatar_clicked), self);
  }
  if (GTK_IS_BUTTON(self->btn_author_name)) {
    g_signal_connect(self->btn_author_name, "clicked", G_CALLBACK(on_author_name_clicked), self);
  }
  if (GTK_IS_BUTTON(self->btn_title)) {
    g_signal_connect(self->btn_title, "clicked", G_CALLBACK(on_title_clicked), self);
  }
  if (GTK_IS_BUTTON(self->btn_read_more)) {
    g_signal_connect(self->btn_read_more, "clicked", G_CALLBACK(on_read_more_clicked), self);
  }
  if (GTK_IS_BUTTON(self->btn_zap)) {
    g_signal_connect(self->btn_zap, "clicked", G_CALLBACK(on_zap_clicked), self);
  }
  if (GTK_IS_BUTTON(self->btn_bookmark)) {
    g_signal_connect(self->btn_bookmark, "clicked", G_CALLBACK(on_bookmark_clicked), self);
  }
  if (GTK_IS_BUTTON(self->btn_share)) {
    g_signal_connect(self->btn_share, "clicked", G_CALLBACK(on_share_clicked), self);
  }
  if (GTK_IS_BUTTON(self->btn_menu)) {
    g_signal_connect(self->btn_menu, "clicked", G_CALLBACK(on_menu_clicked), self);
  }

#ifdef HAVE_SOUP3
  self->avatar_cancellable = g_cancellable_new();
  self->header_cancellable = g_cancellable_new();
  /* Uses shared session from gnostr_get_shared_soup_session() */
#endif
}

GnostrArticleCard *gnostr_article_card_new(void) {
  return g_object_new(GNOSTR_TYPE_ARTICLE_CARD, NULL);
}

#ifdef HAVE_SOUP3
static void on_header_image_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrArticleCard *self = GNOSTR_ARTICLE_CARD(user_data);

  if (!GNOSTR_IS_ARTICLE_CARD(self)) return;

  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  if (!bytes || error) {
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("Article: Failed to load header image: %s", error->message);
    }
    if (error) g_error_free(error);
    return;
  }

  GdkTexture *texture = gdk_texture_new_from_bytes(bytes, &error);
  g_bytes_unref(bytes);

  if (!texture || error) {
    if (error) {
      g_debug("Article: Failed to create texture: %s", error->message);
      g_error_free(error);
    }
    return;
  }

  if (GTK_IS_PICTURE(self->header_image)) {
    gtk_picture_set_paintable(GTK_PICTURE(self->header_image), GDK_PAINTABLE(texture));
    gtk_widget_set_visible(self->header_image_overlay, TRUE);
  }

  g_object_unref(texture);
}

static void load_header_image(GnostrArticleCard *self, const char *url) {
  if (!url || !*url) return;

  /* Create HTTP request */
  SoupMessage *msg = soup_message_new("GET", url);
  if (!msg) return;

  soup_session_send_and_read_async(
    gnostr_get_shared_soup_session(),
    msg,
    G_PRIORITY_LOW,
    self->header_cancellable,
    on_header_image_loaded,
    self
  );

  g_object_unref(msg);
}
#endif

void gnostr_article_card_set_article(GnostrArticleCard *self,
                                      const char *event_id,
                                      const char *d_tag,
                                      const char *title,
                                      const char *summary,
                                      const char *image_url,
                                      gint64 published_at) {
  if (!GNOSTR_IS_ARTICLE_CARD(self)) return;

  /* Store IDs */
  g_clear_pointer(&self->event_id, g_free);
  g_clear_pointer(&self->d_tag, g_free);
  self->event_id = g_strdup(event_id);
  self->d_tag = g_strdup(d_tag);
  self->published_at = published_at;

  /* Set title */
  if (GTK_IS_LABEL(self->lbl_title)) {
    gtk_label_set_text(GTK_LABEL(self->lbl_title),
      (title && *title) ? title : _("Untitled Article"));
  }

  /* Set summary with markdown conversion */
  if (GTK_IS_LABEL(self->lbl_summary)) {
    if (summary && *summary) {
      gchar *pango_summary = markdown_to_pango_summary(summary, 300);
      gtk_label_set_markup(GTK_LABEL(self->lbl_summary), pango_summary);
      gtk_widget_set_visible(self->lbl_summary, TRUE);
      g_free(pango_summary);
    } else {
      gtk_widget_set_visible(self->lbl_summary, FALSE);
    }
  }

  /* Set publication date */
  if (GTK_IS_LABEL(self->lbl_publish_date)) {
    gchar *date_str = format_publish_date(published_at);
    gtk_label_set_text(GTK_LABEL(self->lbl_publish_date), date_str);

    /* Set tooltip with full date */
    if (published_at > 0) {
      GDateTime *dt = g_date_time_new_from_unix_local(published_at);
      if (dt) {
        gchar *full_date = g_date_time_format(dt, "%B %d, %Y at %l:%M %p");
        gtk_widget_set_tooltip_text(GTK_WIDGET(self->lbl_publish_date), full_date);
        g_free(full_date);
        g_date_time_unref(dt);
      }
    }
    g_free(date_str);
  }

  /* Load header image */
#ifdef HAVE_SOUP3
  if (image_url && *image_url) {
    load_header_image(self, image_url);
  }
#else
  (void)image_url;
#endif
}

void gnostr_article_card_set_author(GnostrArticleCard *self,
                                     const char *display_name,
                                     const char *handle,
                                     const char *avatar_url,
                                     const char *pubkey_hex) {
  if (!GNOSTR_IS_ARTICLE_CARD(self)) return;

  g_clear_pointer(&self->pubkey_hex, g_free);
  self->pubkey_hex = g_strdup(pubkey_hex);

  /* Set author name */
  if (GTK_IS_LABEL(self->lbl_author_name)) {
    gtk_label_set_text(GTK_LABEL(self->lbl_author_name),
      (display_name && *display_name) ? display_name : (handle ? handle : _("Anonymous")));
  }

  /* Set handle */
  if (GTK_IS_LABEL(self->lbl_author_handle)) {
    gchar *handle_str = g_strdup_printf("@%s", (handle && *handle) ? handle : "anon");
    gtk_label_set_text(GTK_LABEL(self->lbl_author_handle), handle_str);
    g_free(handle_str);
  }

  /* Set avatar */
  set_avatar_initials(self, display_name, handle);

#ifdef HAVE_SOUP3
  if (avatar_url && *avatar_url && GTK_IS_PICTURE(self->avatar_image)) {
    GdkTexture *cached = gnostr_avatar_try_load_cached(avatar_url);
    if (cached) {
      gtk_picture_set_paintable(GTK_PICTURE(self->avatar_image), GDK_PAINTABLE(cached));
      gtk_widget_set_visible(self->avatar_image, TRUE);
      gtk_widget_set_visible(self->avatar_initials, FALSE);
      g_object_unref(cached);
    } else {
      gnostr_avatar_download_async(avatar_url, self->avatar_image, self->avatar_initials);
    }
  }
#endif
}

void gnostr_article_card_set_content(GnostrArticleCard *self,
                                      const char *markdown_content) {
  if (!GNOSTR_IS_ARTICLE_CARD(self)) return;

  g_clear_pointer(&self->content_markdown, g_free);
  self->content_markdown = g_strdup(markdown_content);

  /* Compute and display reading time */
  if (GTK_IS_LABEL(self->lbl_reading_time) && markdown_content && *markdown_content) {
    gchar *reading_time = compute_reading_time(markdown_content);
    if (reading_time) {
      gtk_label_set_text(GTK_LABEL(self->lbl_reading_time), reading_time);
      gtk_widget_set_visible(self->lbl_reading_time, TRUE);
      g_free(reading_time);
    }
  }
}

/* NIP-05 verification callback */
static void on_nip05_verified(GnostrNip05Result *result, gpointer user_data) {
  GnostrArticleCard *self = GNOSTR_ARTICLE_CARD(user_data);

  if (!GNOSTR_IS_ARTICLE_CARD(self) || !GTK_IS_IMAGE(self->nip05_badge)) {
    gnostr_nip05_result_free(result);
    return;
  }

  gboolean verified = (result && result->status == GNOSTR_NIP05_STATUS_VERIFIED);
  gtk_widget_set_visible(self->nip05_badge, verified);

  if (verified && result->identifier) {
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->nip05_badge), result->identifier);
  }

  gnostr_nip05_result_free(result);
}

void gnostr_article_card_set_nip05(GnostrArticleCard *self,
                                    const char *nip05,
                                    const char *pubkey_hex) {
  if (!GNOSTR_IS_ARTICLE_CARD(self)) return;

  g_clear_pointer(&self->nip05, g_free);
  self->nip05 = g_strdup(nip05);

  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
  }

  if (!nip05 || !*nip05 || !pubkey_hex) {
    gtk_widget_set_visible(self->nip05_badge, FALSE);
    return;
  }

  /* Start async verification */
  self->nip05_cancellable = g_cancellable_new();

  gnostr_nip05_verify_async(nip05, pubkey_hex, on_nip05_verified, self, self->nip05_cancellable);
}

void gnostr_article_card_set_author_lud16(GnostrArticleCard *self,
                                           const char *lud16) {
  if (!GNOSTR_IS_ARTICLE_CARD(self)) return;

  g_clear_pointer(&self->author_lud16, g_free);
  self->author_lud16 = g_strdup(lud16);

  /* Enable/disable zap button based on lightning address availability */
  if (GTK_IS_WIDGET(self->btn_zap)) {
    gtk_widget_set_sensitive(self->btn_zap, lud16 && *lud16 && self->is_logged_in);
  }
}

void gnostr_article_card_set_bookmarked(GnostrArticleCard *self,
                                         gboolean is_bookmarked) {
  if (!GNOSTR_IS_ARTICLE_CARD(self)) return;

  self->is_bookmarked = is_bookmarked;

  if (GTK_IS_BUTTON(self->btn_bookmark)) {
    gtk_button_set_icon_name(GTK_BUTTON(self->btn_bookmark),
      is_bookmarked ? "user-bookmarks-symbolic" : "bookmark-new-symbolic");
  }
}

void gnostr_article_card_set_logged_in(GnostrArticleCard *self,
                                        gboolean logged_in) {
  if (!GNOSTR_IS_ARTICLE_CARD(self)) return;

  self->is_logged_in = logged_in;

  /* Update button sensitivity */
  if (GTK_IS_WIDGET(self->btn_zap)) {
    gtk_widget_set_sensitive(self->btn_zap,
      logged_in && self->author_lud16 && *self->author_lud16);
  }
  if (GTK_IS_WIDGET(self->btn_bookmark)) {
    gtk_widget_set_sensitive(self->btn_bookmark, logged_in);
  }
}

const char *gnostr_article_card_get_d_tag(GnostrArticleCard *self) {
  g_return_val_if_fail(GNOSTR_IS_ARTICLE_CARD(self), NULL);
  return self->d_tag;
}

char *gnostr_article_card_get_a_tag(GnostrArticleCard *self) {
  g_return_val_if_fail(GNOSTR_IS_ARTICLE_CARD(self), NULL);

  if (!self->pubkey_hex || !self->d_tag) return NULL;

  /* NIP-33 "a" tag format: kind:pubkey:d-tag */
  return g_strdup_printf("30023:%s:%s", self->pubkey_hex, self->d_tag);
}
