/*
 * gnostr-wiki-card.c - NIP-54 Wiki Article Card Widget Implementation
 *
 * Displays kind 30818 wiki article events.
 */

#include "gnostr-wiki-card.h"
#include "gnostr-avatar-cache.h"
#include "../util/markdown_pango.h"
#include "../util/nip54_wiki.h"
#include "../util/nip05.h"
#include "../util/utils.h"
#include <glib/gi18n.h>
#include <nostr-gobject-1.0/nostr_nip19.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

/* Average reading speed in words per minute */
#define READING_WPM 200

/* Maximum summary length in card view */
#define MAX_SUMMARY_LENGTH 300

struct _GnostrWikiCard {
  GtkWidget parent_instance;

  /* Main layout */
  GtkWidget *root;
  GtkWidget *content_box;

  /* Header section */
  GtkWidget *header_box;
  GtkWidget *btn_avatar;
  GtkWidget *avatar_overlay;
  GtkWidget *avatar_image;
  GtkWidget *avatar_initials;
  GtkWidget *author_info_box;
  GtkWidget *btn_author_name;
  GtkWidget *lbl_author_name;
  GtkWidget *lbl_author_handle;
  GtkWidget *nip05_badge;
  GtkWidget *lbl_updated_date;

  /* Article header */
  GtkWidget *btn_title;
  GtkWidget *lbl_title;
  GtkWidget *lbl_summary;
  GtkWidget *lbl_reading_time;

  /* Topics flow box */
  GtkWidget *topics_box;

  /* Related articles section */
  GtkWidget *related_section;
  GtkWidget *related_box;

  /* Content area (for expanded view) */
  GtkWidget *content_expander;
  GtkWidget *full_content_label;
  GtkWidget *toc_box;  /* Table of contents */

  /* Action buttons */
  GtkWidget *actions_box;
  GtkWidget *btn_expand;
  GtkWidget *btn_zap;
  GtkWidget *lbl_zap_count;
  GtkWidget *btn_bookmark;
  GtkWidget *btn_share;
  GtkWidget *btn_menu;
  GtkWidget *menu_popover;

  /* State */
  gchar *event_id;
  gchar *d_tag;
  gchar *pubkey_hex;
  gchar *author_lud16;
  gchar *nip05;
  gint64 published_at;
  gint64 created_at;
  gboolean is_bookmarked;
  gboolean is_logged_in;
  gboolean is_expanded;
  gchar *content_markdown;

  /* Related articles */
  gchar **related_a_tags;
  gsize related_count;

  /* Topics */
  gchar **topics;
  gsize topics_count;

#ifdef HAVE_SOUP3
  GCancellable *avatar_cancellable;
  /* Uses gnostr_get_shared_soup_session() instead of per-widget session */
#endif

  GCancellable *nip05_cancellable;
};

G_DEFINE_TYPE(GnostrWikiCard, gnostr_wiki_card, GTK_TYPE_WIDGET)

enum {
  SIGNAL_OPEN_PROFILE,
  SIGNAL_OPEN_ARTICLE,
  SIGNAL_OPEN_RELATED,
  SIGNAL_OPEN_URL,
  SIGNAL_ZAP_REQUESTED,
  SIGNAL_BOOKMARK_TOGGLED,
  SIGNAL_SHARE_ARTICLE,
  SIGNAL_TOPIC_CLICKED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void rebuild_topics(GnostrWikiCard *self);
static void rebuild_related_articles(GnostrWikiCard *self);
static void update_content_view(GnostrWikiCard *self);

static void gnostr_wiki_card_dispose(GObject *object) {
  GnostrWikiCard *self = GNOSTR_WIKI_CARD(object);

  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
  }

#ifdef HAVE_SOUP3
  if (self->avatar_cancellable) {
    g_cancellable_cancel(self->avatar_cancellable);
    g_clear_object(&self->avatar_cancellable);
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

  /* Unparent all children */
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_widget_unparent(child);
    child = next;
  }

  G_OBJECT_CLASS(gnostr_wiki_card_parent_class)->dispose(object);
}

static void gnostr_wiki_card_finalize(GObject *obj) {
  GnostrWikiCard *self = GNOSTR_WIKI_CARD(obj);

  g_clear_pointer(&self->event_id, g_free);
  g_clear_pointer(&self->d_tag, g_free);
  g_clear_pointer(&self->pubkey_hex, g_free);
  g_clear_pointer(&self->author_lud16, g_free);
  g_clear_pointer(&self->nip05, g_free);
  g_clear_pointer(&self->content_markdown, g_free);

  if (self->related_a_tags) {
    for (gsize i = 0; i < self->related_count; i++) {
      g_free(self->related_a_tags[i]);
    }
    g_free(self->related_a_tags);
  }

  if (self->topics) {
    for (gsize i = 0; i < self->topics_count; i++) {
      g_free(self->topics[i]);
    }
    g_free(self->topics);
  }

  G_OBJECT_CLASS(gnostr_wiki_card_parent_class)->finalize(obj);
}

/* Helper: compute reading time from word count */
static gchar *compute_reading_time(const char *content) {
  if (!content || !*content) return NULL;

  int minutes = gnostr_wiki_estimate_reading_time(content, READING_WPM);
  return g_strdup_printf("%d min read", minutes);
}

/* Helper: format date for display */
static gchar *format_date(gint64 timestamp) {
  if (timestamp <= 0) return g_strdup(_("Unknown date"));

  GDateTime *dt = g_date_time_new_from_unix_local(timestamp);
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
    result = g_date_time_format(dt, "%B %d, %Y");
  }

  g_date_time_unref(dt);
  return result;
}

/* Set avatar initials fallback */
static void set_avatar_initials(GnostrWikiCard *self, const char *display, const char *handle) {
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
  GnostrWikiCard *self = GNOSTR_WIKI_CARD(user_data);
  (void)btn;
  if (self->pubkey_hex && *self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey_hex);
  }
}

static void on_author_name_clicked(GtkButton *btn, gpointer user_data) {
  GnostrWikiCard *self = GNOSTR_WIKI_CARD(user_data);
  (void)btn;
  if (self->pubkey_hex && *self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey_hex);
  }
}

static void on_title_clicked(GtkButton *btn, gpointer user_data) {
  GnostrWikiCard *self = GNOSTR_WIKI_CARD(user_data);
  (void)btn;
  if (self->event_id && *self->event_id) {
    g_signal_emit(self, signals[SIGNAL_OPEN_ARTICLE], 0, self->event_id);
  }
}

static void on_expand_clicked(GtkButton *btn, gpointer user_data) {
  GnostrWikiCard *self = GNOSTR_WIKI_CARD(user_data);
  (void)btn;

  self->is_expanded = !self->is_expanded;
  update_content_view(self);

  /* Update expand button icon/label */
  if (GTK_IS_BUTTON(self->btn_expand)) {
    gtk_button_set_icon_name(GTK_BUTTON(self->btn_expand),
      self->is_expanded ? "go-up-symbolic" : "go-down-symbolic");
    gtk_widget_set_tooltip_text(self->btn_expand,
      self->is_expanded ? _("Collapse") : _("Expand"));
  }
}

static void on_zap_clicked(GtkButton *btn, gpointer user_data) {
  GnostrWikiCard *self = GNOSTR_WIKI_CARD(user_data);
  (void)btn;
  if (self->event_id && self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_ZAP_REQUESTED], 0,
                  self->event_id, self->pubkey_hex, self->author_lud16);
  }
}

static void on_bookmark_clicked(GtkButton *btn, gpointer user_data) {
  GnostrWikiCard *self = GNOSTR_WIKI_CARD(user_data);
  (void)btn;

  if (!self->event_id) return;

  self->is_bookmarked = !self->is_bookmarked;

  if (GTK_IS_BUTTON(self->btn_bookmark)) {
    gtk_button_set_icon_name(GTK_BUTTON(self->btn_bookmark),
      self->is_bookmarked ? "user-bookmarks-symbolic" : "bookmark-new-symbolic");
  }

  g_signal_emit(self, signals[SIGNAL_BOOKMARK_TOGGLED], 0,
                self->event_id, self->is_bookmarked);
}

static void on_share_clicked(GtkButton *btn, gpointer user_data) {
  GnostrWikiCard *self = GNOSTR_WIKI_CARD(user_data);
  (void)btn;

  if (!self->pubkey_hex || !self->d_tag) return;

  /* Build naddr for NIP-33 addressable event */
  char *naddr = gnostr_wiki_build_naddr(self->pubkey_hex, self->d_tag, NULL);
  if (naddr) {
    g_autofree char *uri = g_strdup_printf("nostr:%s", naddr);
    g_signal_emit(self, signals[SIGNAL_SHARE_ARTICLE], 0, uri);
    free(naddr);
  }
}

static void on_topic_clicked(GtkButton *btn, gpointer user_data) {
  GnostrWikiCard *self = GNOSTR_WIKI_CARD(user_data);
  const char *topic = g_object_get_data(G_OBJECT(btn), "topic");

  if (topic && *topic) {
    g_signal_emit(self, signals[SIGNAL_TOPIC_CLICKED], 0, topic);
  }
}

static void on_related_clicked(GtkButton *btn, gpointer user_data) {
  GnostrWikiCard *self = GNOSTR_WIKI_CARD(user_data);
  const char *a_tag = g_object_get_data(G_OBJECT(btn), "a-tag");

  if (a_tag && *a_tag) {
    g_signal_emit(self, signals[SIGNAL_OPEN_RELATED], 0, a_tag);
  }
}

static void on_menu_clicked(GtkButton *btn, gpointer user_data) {
  GnostrWikiCard *self = GNOSTR_WIKI_CARD(user_data);
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

/* Update content view based on expanded state */
static void update_content_view(GnostrWikiCard *self) {
  if (self->is_expanded && self->content_markdown && *self->content_markdown) {
    /* Show full rendered content */
    if (GTK_IS_WIDGET(self->content_expander)) {
      gchar *pango_content = markdown_to_pango(self->content_markdown, 0);
      if (GTK_IS_LABEL(self->full_content_label)) {
        gtk_label_set_markup(GTK_LABEL(self->full_content_label), pango_content);
      }
      gtk_widget_set_visible(self->content_expander, TRUE);
      g_free(pango_content);
    }

    /* Build table of contents */
    if (GTK_IS_BOX(self->toc_box)) {
      /* Clear existing TOC */
      GtkWidget *child = gtk_widget_get_first_child(self->toc_box);
      while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(self->toc_box), child);
        child = next;
      }

      GPtrArray *toc = gnostr_wiki_extract_table_of_contents(self->content_markdown);
      if (toc && toc->len > 0) {
        GtkWidget *toc_title = gtk_label_new(_("Contents"));
        gtk_widget_add_css_class(toc_title, "heading");
        gtk_label_set_xalign(GTK_LABEL(toc_title), 0.0);
        gtk_box_append(GTK_BOX(self->toc_box), toc_title);

        for (guint i = 0; i < toc->len; i++) {
          GnostrWikiHeading *heading = g_ptr_array_index(toc, i);
          GtkWidget *item = gtk_label_new(heading->text);
          gtk_label_set_xalign(GTK_LABEL(item), 0.0);

          /* Indent based on heading level */
          int indent = (heading->level - 1) * 12;
          gtk_widget_set_margin_start(item, indent);

          gtk_widget_add_css_class(item, "toc-item");
          gtk_box_append(GTK_BOX(self->toc_box), item);
        }

        gtk_widget_set_visible(self->toc_box, TRUE);
        g_ptr_array_unref(toc);
      } else {
        gtk_widget_set_visible(self->toc_box, FALSE);
      }
    }
  } else {
    /* Hide full content when collapsed */
    if (GTK_IS_WIDGET(self->content_expander)) {
      gtk_widget_set_visible(self->content_expander, FALSE);
    }
    if (GTK_IS_WIDGET(self->toc_box)) {
      gtk_widget_set_visible(self->toc_box, FALSE);
    }
  }
}

/* Rebuild topics flow box */
static void rebuild_topics(GnostrWikiCard *self) {
  if (!GTK_IS_FLOW_BOX(self->topics_box)) return;

  /* Clear existing */
  GtkWidget *child = gtk_widget_get_first_child(self->topics_box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_flow_box_remove(GTK_FLOW_BOX(self->topics_box), child);
    child = next;
  }

  if (!self->topics || self->topics_count == 0) {
    gtk_widget_set_visible(self->topics_box, FALSE);
    return;
  }

  for (gsize i = 0; i < self->topics_count; i++) {
    GtkWidget *btn = gtk_button_new_with_label(self->topics[i]);
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    gtk_widget_add_css_class(btn, "topic-tag");
    gtk_widget_add_css_class(btn, "pill");

    g_object_set_data_full(G_OBJECT(btn), "topic",
      g_strdup(self->topics[i]), g_free);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_topic_clicked), self);

    gtk_flow_box_append(GTK_FLOW_BOX(self->topics_box), btn);
  }

  gtk_widget_set_visible(self->topics_box, TRUE);
}

/* Rebuild related articles section */
static void rebuild_related_articles(GnostrWikiCard *self) {
  if (!GTK_IS_BOX(self->related_box)) return;

  /* Clear existing */
  GtkWidget *child = gtk_widget_get_first_child(self->related_box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_box_remove(GTK_BOX(self->related_box), child);
    child = next;
  }

  if (!self->related_a_tags || self->related_count == 0) {
    gtk_widget_set_visible(self->related_section, FALSE);
    return;
  }

  for (gsize i = 0; i < self->related_count; i++) {
    GnostrWikiRelatedArticle *related = gnostr_wiki_parse_a_tag(self->related_a_tags[i]);
    if (!related) continue;

    GtkWidget *btn = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    gtk_widget_add_css_class(btn, "related-article-link");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon = gtk_image_new_from_icon_name("document-open-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 14);

    /* Use d-tag as display name, or truncated pubkey */
    g_autofree gchar *label_text = related->d_tag && *related->d_tag
      ? g_strdup(related->d_tag)
      : g_strdup_printf("%.8s...", related->pubkey);

    GtkWidget *label = gtk_label_new(label_text);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_button_set_child(GTK_BUTTON(btn), box);

    g_object_set_data_full(G_OBJECT(btn), "a-tag",
      g_strdup(self->related_a_tags[i]), g_free);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_related_clicked), self);

    gtk_box_append(GTK_BOX(self->related_box), btn);
    gnostr_wiki_related_article_free(related);
  }

  gtk_widget_set_visible(self->related_section, TRUE);
}

static void gnostr_wiki_card_class_init(GnostrWikiCardClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);

  gclass->dispose = gnostr_wiki_card_dispose;
  gclass->finalize = gnostr_wiki_card_finalize;

  gtk_widget_class_set_layout_manager_type(wclass, GTK_TYPE_BOX_LAYOUT);

  /* Signals */
  signals[SIGNAL_OPEN_PROFILE] = g_signal_new("open-profile",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_ARTICLE] = g_signal_new("open-article",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_RELATED] = g_signal_new("open-related",
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

  signals[SIGNAL_TOPIC_CLICKED] = g_signal_new("topic-clicked",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gnostr_wiki_card_init(GnostrWikiCard *self) {
  GtkLayoutManager *layout = gtk_widget_get_layout_manager(GTK_WIDGET(self));
  gtk_orientable_set_orientation(GTK_ORIENTABLE(layout), GTK_ORIENTATION_VERTICAL);

  gtk_widget_add_css_class(GTK_WIDGET(self), "wiki-card");
  gtk_widget_add_css_class(GTK_WIDGET(self), "card");

  /* Root container */
  self->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(self->root, 12);
  gtk_widget_set_margin_end(self->root, 12);
  gtk_widget_set_margin_top(self->root, 12);
  gtk_widget_set_margin_bottom(self->root, 12);
  gtk_widget_set_parent(self->root, GTK_WIDGET(self));

  /* Header with author info */
  self->header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  /* Avatar button */
  self->btn_avatar = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_avatar), FALSE);
  gtk_widget_add_css_class(self->btn_avatar, "avatar-button");

  self->avatar_overlay = gtk_overlay_new();
  gtk_widget_set_size_request(self->avatar_overlay, 40, 40);

  self->avatar_image = gtk_picture_new();
  gtk_widget_add_css_class(self->avatar_image, "avatar");
  gtk_widget_set_visible(self->avatar_image, FALSE);
  gtk_overlay_set_child(GTK_OVERLAY(self->avatar_overlay), self->avatar_image);

  self->avatar_initials = gtk_label_new("AN");
  gtk_widget_add_css_class(self->avatar_initials, "avatar-initials");
  gtk_widget_set_halign(self->avatar_initials, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->avatar_initials, GTK_ALIGN_CENTER);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->avatar_overlay), self->avatar_initials);

  gtk_button_set_child(GTK_BUTTON(self->btn_avatar), self->avatar_overlay);
  g_signal_connect(self->btn_avatar, "clicked", G_CALLBACK(on_avatar_clicked), self);
  gtk_box_append(GTK_BOX(self->header_box), self->btn_avatar);

  /* Author info */
  self->author_info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(self->author_info_box, TRUE);

  /* Author name button */
  self->btn_author_name = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_author_name), FALSE);
  gtk_widget_set_halign(self->btn_author_name, GTK_ALIGN_START);

  GtkWidget *name_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  self->lbl_author_name = gtk_label_new(_("Anonymous"));
  gtk_label_set_xalign(GTK_LABEL(self->lbl_author_name), 0.0);
  gtk_widget_add_css_class(self->lbl_author_name, "author-name");
  gtk_box_append(GTK_BOX(name_row), self->lbl_author_name);

  self->nip05_badge = gtk_image_new_from_icon_name("emblem-ok-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(self->nip05_badge), 14);
  gtk_widget_add_css_class(self->nip05_badge, "nip05-badge");
  gtk_widget_set_visible(self->nip05_badge, FALSE);
  gtk_box_append(GTK_BOX(name_row), self->nip05_badge);

  gtk_button_set_child(GTK_BUTTON(self->btn_author_name), name_row);
  g_signal_connect(self->btn_author_name, "clicked", G_CALLBACK(on_author_name_clicked), self);
  gtk_box_append(GTK_BOX(self->author_info_box), self->btn_author_name);

  /* Handle and updated date row */
  GtkWidget *meta_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  self->lbl_author_handle = gtk_label_new("@anon");
  gtk_widget_add_css_class(self->lbl_author_handle, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(self->lbl_author_handle), 0.0);
  gtk_box_append(GTK_BOX(meta_row), self->lbl_author_handle);

  GtkWidget *separator = gtk_label_new("\342\200\242");  /* bullet */
  gtk_widget_add_css_class(separator, "dim-label");
  gtk_box_append(GTK_BOX(meta_row), separator);

  self->lbl_updated_date = gtk_label_new("");
  gtk_widget_add_css_class(self->lbl_updated_date, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(self->lbl_updated_date), 0.0);
  gtk_box_append(GTK_BOX(meta_row), self->lbl_updated_date);

  gtk_box_append(GTK_BOX(self->author_info_box), meta_row);
  gtk_box_append(GTK_BOX(self->header_box), self->author_info_box);
  gtk_box_append(GTK_BOX(self->root), self->header_box);

  /* Title button */
  self->btn_title = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_title), FALSE);
  gtk_widget_set_halign(self->btn_title, GTK_ALIGN_START);

  self->lbl_title = gtk_label_new(_("Untitled Article"));
  gtk_label_set_wrap(GTK_LABEL(self->lbl_title), TRUE);
  gtk_label_set_xalign(GTK_LABEL(self->lbl_title), 0.0);
  gtk_widget_add_css_class(self->lbl_title, "wiki-title");
  gtk_widget_add_css_class(self->lbl_title, "title-2");
  gtk_button_set_child(GTK_BUTTON(self->btn_title), self->lbl_title);
  g_signal_connect(self->btn_title, "clicked", G_CALLBACK(on_title_clicked), self);
  gtk_box_append(GTK_BOX(self->root), self->btn_title);

  /* Summary */
  self->lbl_summary = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(self->lbl_summary), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(self->lbl_summary), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign(GTK_LABEL(self->lbl_summary), 0.0);
  gtk_widget_add_css_class(self->lbl_summary, "wiki-summary");
  gtk_widget_set_visible(self->lbl_summary, FALSE);
  gtk_box_append(GTK_BOX(self->root), self->lbl_summary);

  /* Reading time */
  self->lbl_reading_time = gtk_label_new(NULL);
  gtk_widget_add_css_class(self->lbl_reading_time, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(self->lbl_reading_time), 0.0);
  gtk_widget_set_visible(self->lbl_reading_time, FALSE);
  gtk_box_append(GTK_BOX(self->root), self->lbl_reading_time);

  /* Topics flow box */
  self->topics_box = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->topics_box), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->topics_box), 10);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->topics_box), 6);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->topics_box), 4);
  gtk_widget_set_visible(self->topics_box, FALSE);
  gtk_box_append(GTK_BOX(self->root), self->topics_box);

  /* Related articles section */
  self->related_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_top(self->related_section, 8);

  GtkWidget *related_header = gtk_label_new(_("Related Articles"));
  gtk_widget_add_css_class(related_header, "heading");
  gtk_label_set_xalign(GTK_LABEL(related_header), 0.0);
  gtk_box_append(GTK_BOX(self->related_section), related_header);

  self->related_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_append(GTK_BOX(self->related_section), self->related_box);

  gtk_widget_set_visible(self->related_section, FALSE);
  gtk_box_append(GTK_BOX(self->root), self->related_section);

  /* Table of contents (shown when expanded) */
  self->toc_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(self->toc_box, "wiki-toc");
  gtk_widget_set_margin_top(self->toc_box, 8);
  gtk_widget_set_visible(self->toc_box, FALSE);
  gtk_box_append(GTK_BOX(self->root), self->toc_box);

  /* Full content (shown when expanded) */
  self->content_expander = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_top(self->content_expander, 8);

  self->full_content_label = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(self->full_content_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(self->full_content_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign(GTK_LABEL(self->full_content_label), 0.0);
  gtk_label_set_selectable(GTK_LABEL(self->full_content_label), TRUE);
  gtk_widget_add_css_class(self->full_content_label, "wiki-content");
  gtk_box_append(GTK_BOX(self->content_expander), self->full_content_label);

  gtk_widget_set_visible(self->content_expander, FALSE);
  gtk_box_append(GTK_BOX(self->root), self->content_expander);

  /* Action buttons row */
  self->actions_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(self->actions_box, 8);
  gtk_widget_set_halign(self->actions_box, GTK_ALIGN_START);

  /* Expand button */
  self->btn_expand = gtk_button_new_from_icon_name("go-down-symbolic");
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_expand), FALSE);
  gtk_widget_set_tooltip_text(self->btn_expand, _("Expand"));
  g_signal_connect(self->btn_expand, "clicked", G_CALLBACK(on_expand_clicked), self);
  gtk_box_append(GTK_BOX(self->actions_box), self->btn_expand);

  /* Zap button */
  self->btn_zap = gtk_button_new_from_icon_name("emblem-favorite-symbolic");
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_zap), FALSE);
  gtk_widget_set_tooltip_text(self->btn_zap, _("Zap"));
  gtk_widget_set_sensitive(self->btn_zap, FALSE);
  g_signal_connect(self->btn_zap, "clicked", G_CALLBACK(on_zap_clicked), self);
  gtk_box_append(GTK_BOX(self->actions_box), self->btn_zap);

  /* Bookmark button */
  self->btn_bookmark = gtk_button_new_from_icon_name("bookmark-new-symbolic");
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_bookmark), FALSE);
  gtk_widget_set_tooltip_text(self->btn_bookmark, _("Bookmark"));
  gtk_widget_set_sensitive(self->btn_bookmark, FALSE);
  g_signal_connect(self->btn_bookmark, "clicked", G_CALLBACK(on_bookmark_clicked), self);
  gtk_box_append(GTK_BOX(self->actions_box), self->btn_bookmark);

  /* Share button */
  self->btn_share = gtk_button_new_from_icon_name("emblem-shared-symbolic");
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_share), FALSE);
  gtk_widget_set_tooltip_text(self->btn_share, _("Share"));
  g_signal_connect(self->btn_share, "clicked", G_CALLBACK(on_share_clicked), self);
  gtk_box_append(GTK_BOX(self->actions_box), self->btn_share);

  /* Menu button */
  self->btn_menu = gtk_button_new_from_icon_name("view-more-symbolic");
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_menu), FALSE);
  gtk_widget_set_tooltip_text(self->btn_menu, _("More options"));
  g_signal_connect(self->btn_menu, "clicked", G_CALLBACK(on_menu_clicked), self);
  gtk_box_append(GTK_BOX(self->actions_box), self->btn_menu);

  gtk_box_append(GTK_BOX(self->root), self->actions_box);

#ifdef HAVE_SOUP3
  self->avatar_cancellable = g_cancellable_new();
  /* Uses shared session from gnostr_get_shared_soup_session() */
#endif

  self->is_expanded = FALSE;
  self->is_bookmarked = FALSE;
  self->is_logged_in = FALSE;
}

GnostrWikiCard *gnostr_wiki_card_new(void) {
  return g_object_new(GNOSTR_TYPE_WIKI_CARD, NULL);
}

void gnostr_wiki_card_set_article(GnostrWikiCard *self,
                                   const char *event_id,
                                   const char *d_tag,
                                   const char *title,
                                   const char *summary,
                                   gint64 published_at,
                                   gint64 created_at) {
  g_return_if_fail(GNOSTR_IS_WIKI_CARD(self));

  /* Store IDs */
  g_clear_pointer(&self->event_id, g_free);
  g_clear_pointer(&self->d_tag, g_free);
  self->event_id = g_strdup(event_id);
  self->d_tag = g_strdup(d_tag);
  self->published_at = published_at;
  self->created_at = created_at;

  /* Set title */
  if (GTK_IS_LABEL(self->lbl_title)) {
    gtk_label_set_text(GTK_LABEL(self->lbl_title),
      (title && *title) ? title : _("Untitled Article"));
  }

  /* Set summary with markdown conversion */
  if (GTK_IS_LABEL(self->lbl_summary)) {
    if (summary && *summary) {
      gchar *pango_summary = markdown_to_pango_summary(summary, MAX_SUMMARY_LENGTH);
      gtk_label_set_markup(GTK_LABEL(self->lbl_summary), pango_summary);
      gtk_widget_set_visible(self->lbl_summary, TRUE);
      g_free(pango_summary);
    } else {
      gtk_widget_set_visible(self->lbl_summary, FALSE);
    }
  }

  /* Set last updated date (prefer created_at as it shows when this version was made) */
  if (GTK_IS_LABEL(self->lbl_updated_date)) {
    gint64 display_time = created_at > 0 ? created_at : published_at;
    gchar *date_str = format_date(display_time);
    g_autofree gchar *updated_text = g_strdup_printf(_("Updated %s"), date_str);
    gtk_label_set_text(GTK_LABEL(self->lbl_updated_date), updated_text);

    /* Set tooltip with full date */
    if (display_time > 0) {
      GDateTime *dt = g_date_time_new_from_unix_local(display_time);
      if (dt) {
        gchar *full_date = g_date_time_format(dt, "%B %d, %Y at %l:%M %p");
        gtk_widget_set_tooltip_text(self->lbl_updated_date, full_date);
        g_free(full_date);
        g_date_time_unref(dt);
      }
    }
    g_free(date_str);
  }
}

void gnostr_wiki_card_set_author(GnostrWikiCard *self,
                                  const char *display_name,
                                  const char *handle,
                                  const char *avatar_url,
                                  const char *pubkey_hex) {
  g_return_if_fail(GNOSTR_IS_WIKI_CARD(self));

  g_clear_pointer(&self->pubkey_hex, g_free);
  self->pubkey_hex = g_strdup(pubkey_hex);

  /* Set author name */
  if (GTK_IS_LABEL(self->lbl_author_name)) {
    gtk_label_set_text(GTK_LABEL(self->lbl_author_name),
      (display_name && *display_name) ? display_name : (handle ? handle : _("Anonymous")));
  }

  /* Set handle */
  if (GTK_IS_LABEL(self->lbl_author_handle)) {
    g_autofree gchar *handle_str = g_strdup_printf("@%s", (handle && *handle) ? handle : "anon");
    gtk_label_set_text(GTK_LABEL(self->lbl_author_handle), handle_str);
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
#else
  (void)avatar_url;
#endif
}

void gnostr_wiki_card_set_content(GnostrWikiCard *self,
                                   const char *markdown_content) {
  g_return_if_fail(GNOSTR_IS_WIKI_CARD(self));

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

  /* Update content view if expanded */
  if (self->is_expanded) {
    update_content_view(self);
  }
}

void gnostr_wiki_card_set_related_articles(GnostrWikiCard *self,
                                            const char **a_tags,
                                            gsize count) {
  g_return_if_fail(GNOSTR_IS_WIKI_CARD(self));

  /* Free existing */
  if (self->related_a_tags) {
    for (gsize i = 0; i < self->related_count; i++) {
      g_free(self->related_a_tags[i]);
    }
    g_free(self->related_a_tags);
    self->related_a_tags = NULL;
    self->related_count = 0;
  }

  /* Copy new */
  if (a_tags && count > 0) {
    self->related_a_tags = g_new0(gchar*, count + 1);
    for (gsize i = 0; i < count; i++) {
      self->related_a_tags[i] = g_strdup(a_tags[i]);
    }
    self->related_count = count;
  }

  rebuild_related_articles(self);
}

void gnostr_wiki_card_set_topics(GnostrWikiCard *self,
                                  const char **topics,
                                  gsize count) {
  g_return_if_fail(GNOSTR_IS_WIKI_CARD(self));

  /* Free existing */
  if (self->topics) {
    for (gsize i = 0; i < self->topics_count; i++) {
      g_free(self->topics[i]);
    }
    g_free(self->topics);
    self->topics = NULL;
    self->topics_count = 0;
  }

  /* Copy new */
  if (topics && count > 0) {
    self->topics = g_new0(gchar*, count + 1);
    for (gsize i = 0; i < count; i++) {
      self->topics[i] = g_strdup(topics[i]);
    }
    self->topics_count = count;
  }

  rebuild_topics(self);
}

/* NIP-05 verification callback */
static void on_nip05_verified(GnostrNip05Result *result, gpointer user_data) {
  GnostrWikiCard *self = GNOSTR_WIKI_CARD(user_data);

  if (!GNOSTR_IS_WIKI_CARD(self) || !GTK_IS_IMAGE(self->nip05_badge)) {
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

void gnostr_wiki_card_set_nip05(GnostrWikiCard *self,
                                 const char *nip05,
                                 const char *pubkey_hex) {
  g_return_if_fail(GNOSTR_IS_WIKI_CARD(self));

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

void gnostr_wiki_card_set_author_lud16(GnostrWikiCard *self,
                                        const char *lud16) {
  g_return_if_fail(GNOSTR_IS_WIKI_CARD(self));

  g_clear_pointer(&self->author_lud16, g_free);
  self->author_lud16 = g_strdup(lud16);

  /* Enable/disable zap button */
  if (GTK_IS_WIDGET(self->btn_zap)) {
    gtk_widget_set_sensitive(self->btn_zap, lud16 && *lud16 && self->is_logged_in);
  }
}

void gnostr_wiki_card_set_bookmarked(GnostrWikiCard *self,
                                      gboolean is_bookmarked) {
  g_return_if_fail(GNOSTR_IS_WIKI_CARD(self));

  self->is_bookmarked = is_bookmarked;

  if (GTK_IS_BUTTON(self->btn_bookmark)) {
    gtk_button_set_icon_name(GTK_BUTTON(self->btn_bookmark),
      is_bookmarked ? "user-bookmarks-symbolic" : "bookmark-new-symbolic");
  }
}

void gnostr_wiki_card_set_logged_in(GnostrWikiCard *self,
                                     gboolean logged_in) {
  g_return_if_fail(GNOSTR_IS_WIKI_CARD(self));

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

void gnostr_wiki_card_set_expanded(GnostrWikiCard *self,
                                    gboolean expanded) {
  g_return_if_fail(GNOSTR_IS_WIKI_CARD(self));

  if (self->is_expanded == expanded) return;

  self->is_expanded = expanded;
  update_content_view(self);

  if (GTK_IS_BUTTON(self->btn_expand)) {
    gtk_button_set_icon_name(GTK_BUTTON(self->btn_expand),
      expanded ? "go-up-symbolic" : "go-down-symbolic");
    gtk_widget_set_tooltip_text(self->btn_expand,
      expanded ? _("Collapse") : _("Expand"));
  }
}

gboolean gnostr_wiki_card_get_expanded(GnostrWikiCard *self) {
  g_return_val_if_fail(GNOSTR_IS_WIKI_CARD(self), FALSE);
  return self->is_expanded;
}

const char *gnostr_wiki_card_get_d_tag(GnostrWikiCard *self) {
  g_return_val_if_fail(GNOSTR_IS_WIKI_CARD(self), NULL);
  return self->d_tag;
}

char *gnostr_wiki_card_get_a_tag(GnostrWikiCard *self) {
  g_return_val_if_fail(GNOSTR_IS_WIKI_CARD(self), NULL);

  if (!self->pubkey_hex || !self->d_tag) return NULL;

  return gnostr_wiki_build_a_tag(self->pubkey_hex, self->d_tag);
}

const char *gnostr_wiki_card_get_event_id(GnostrWikiCard *self) {
  g_return_val_if_fail(GNOSTR_IS_WIKI_CARD(self), NULL);
  return self->event_id;
}

const char *gnostr_wiki_card_get_pubkey(GnostrWikiCard *self) {
  g_return_val_if_fail(GNOSTR_IS_WIKI_CARD(self), NULL);
  return self->pubkey_hex;
}
