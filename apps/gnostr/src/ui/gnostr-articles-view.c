/*
 * gnostr-articles-view.c - Long-form Content Browse View
 *
 * Displays browsable lists of NIP-54 Wiki and NIP-23 Long-form articles.
 */

#define G_LOG_DOMAIN "gnostr-articles-view"

#include "gnostr-articles-view.h"
#include "gnostr-wiki-card.h"
#include "gnostr-article-card.h"
#include <glib/gi18n.h>

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-articles-view.ui"

/* NIP-23 Long-form content */
#define KIND_LONG_FORM 30023

/* NIP-54 Wiki article */
#define KIND_WIKI 30818

struct _GnostrArticlesView {
  GtkWidget parent_instance;

  /* Template widgets */
  GtkWidget *root;
  GtkWidget *header_box;
  GtkWidget *title_label;
  GtkWidget *search_entry;
  GtkWidget *btn_all;
  GtkWidget *btn_wiki;
  GtkWidget *btn_blog;
  GtkWidget *lbl_count;
  GtkWidget *content_stack;
  GtkWidget *articles_scroll;
  GtkWidget *articles_list;
  GtkWidget *empty_state;
  GtkWidget *loading_spinner;
  GtkWidget *topic_filter_box;
  GtkWidget *topic_filter_label;
  GtkWidget *btn_clear_topic;

  /* Model */
  GListStore *articles_model;
  GtkSingleSelection *selection;
  GtkListItemFactory *factory;

  /* State */
  GnostrArticlesType type_filter;
  gchar *topic_filter;
  gchar *search_text;
  gboolean articles_loaded;
  gboolean is_logged_in;
  guint search_debounce_id;
};

G_DEFINE_TYPE(GnostrArticlesView, gnostr_articles_view, GTK_TYPE_WIDGET)

enum {
  SIGNAL_OPEN_ARTICLE,
  SIGNAL_OPEN_PROFILE,
  SIGNAL_TOPIC_CLICKED,
  SIGNAL_ZAP_REQUESTED,
  SIGNAL_BOOKMARK_TOGGLED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* --- Article List Item (GObject wrapper) --- */

#define GNOSTR_TYPE_ARTICLE_ITEM (gnostr_article_item_get_type())
G_DECLARE_FINAL_TYPE(GnostrArticleItem, gnostr_article_item, GNOSTR, ARTICLE_ITEM, GObject)

struct _GnostrArticleItem {
  GObject parent_instance;
  gint kind;                /* 30023 or 30818 */
  gchar *event_id;
  gchar *d_tag;
  gchar *pubkey_hex;
  gchar *title;
  gchar *summary;
  gchar *image_url;
  gchar *content;
  gint64 published_at;
  gint64 created_at;
  gchar **topics;
  gsize topics_count;
  /* Author info (cached from profile) */
  gchar *author_name;
  gchar *author_handle;
  gchar *author_avatar;
  gchar *author_nip05;
  gchar *author_lud16;
};

G_DEFINE_TYPE(GnostrArticleItem, gnostr_article_item, G_TYPE_OBJECT)

static void gnostr_article_item_finalize(GObject *object) {
  GnostrArticleItem *self = GNOSTR_ARTICLE_ITEM(object);
  g_free(self->event_id);
  g_free(self->d_tag);
  g_free(self->pubkey_hex);
  g_free(self->title);
  g_free(self->summary);
  g_free(self->image_url);
  g_free(self->content);
  g_free(self->author_name);
  g_free(self->author_handle);
  g_free(self->author_avatar);
  g_free(self->author_nip05);
  g_free(self->author_lud16);
  if (self->topics) {
    for (gsize i = 0; i < self->topics_count; i++) {
      g_free(self->topics[i]);
    }
    g_free(self->topics);
  }
  G_OBJECT_CLASS(gnostr_article_item_parent_class)->finalize(object);
}

static void gnostr_article_item_class_init(GnostrArticleItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = gnostr_article_item_finalize;
}

static void gnostr_article_item_init(GnostrArticleItem *self) {
  (void)self;
}

static GnostrArticleItem *gnostr_article_item_new(void) {
  return g_object_new(GNOSTR_TYPE_ARTICLE_ITEM, NULL);
}

/* --- Forward declarations --- */
static void update_content_state(GnostrArticlesView *self);
static void update_article_count(GnostrArticlesView *self);
static void apply_filters(GnostrArticlesView *self);

/* --- Row signal handlers --- */

static void on_card_open_article(GtkWidget *card, const char *event_id, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);
  gint kind = KIND_LONG_FORM;

  /* Determine kind from card type */
  if (GNOSTR_IS_WIKI_CARD(card)) {
    kind = KIND_WIKI;
  }

  g_signal_emit(self, signals[SIGNAL_OPEN_ARTICLE], 0, event_id, kind);
}

static void on_card_open_profile(GtkWidget *card, const char *pubkey_hex, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);
  (void)card;
  g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey_hex);
}

static void on_card_topic_clicked(GtkWidget *card, const char *topic, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);
  (void)card;

  /* Set topic filter */
  gnostr_articles_view_set_topic_filter(self, topic);

  g_signal_emit(self, signals[SIGNAL_TOPIC_CLICKED], 0, topic);
}

static void on_card_zap_requested(GtkWidget *card, const char *event_id,
                                   const char *pubkey_hex, const char *lud16,
                                   gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);
  (void)card;
  g_signal_emit(self, signals[SIGNAL_ZAP_REQUESTED], 0, event_id, pubkey_hex, lud16);
}

static void on_card_bookmark_toggled(GtkWidget *card, const char *event_id,
                                      gboolean is_bookmarked, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);
  (void)card;
  g_signal_emit(self, signals[SIGNAL_BOOKMARK_TOGGLED], 0, event_id, is_bookmarked);
}

/* --- List item factory --- */

static void setup_article_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  (void)user_data;

  /* Create a placeholder box - actual card type determined in bind */
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  gtk_list_item_set_child(list_item, box);
}

static void bind_article_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);

  GtkWidget *box = gtk_list_item_get_child(list_item);
  GnostrArticleItem *item = gtk_list_item_get_item(list_item);

  if (!item) return;

  /* Clear existing children */
  GtkWidget *child = gtk_widget_get_first_child(box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_box_remove(GTK_BOX(box), child);
    child = next;
  }

  /* Create appropriate card based on kind */
  GtkWidget *card = NULL;

  if (item->kind == KIND_WIKI) {
    GnostrWikiCard *wiki_card = gnostr_wiki_card_new();
    card = GTK_WIDGET(wiki_card);

    gnostr_wiki_card_set_article(wiki_card,
                                  item->event_id,
                                  item->d_tag,
                                  item->title,
                                  item->summary,
                                  item->published_at,
                                  item->created_at);

    gnostr_wiki_card_set_author(wiki_card,
                                 item->author_name,
                                 item->author_handle,
                                 item->author_avatar,
                                 item->pubkey_hex);

    if (item->content) {
      gnostr_wiki_card_set_content(wiki_card, item->content);
    }

    if (item->topics && item->topics_count > 0) {
      gnostr_wiki_card_set_topics(wiki_card,
                                   (const char **)item->topics,
                                   item->topics_count);
    }

    if (item->author_nip05) {
      gnostr_wiki_card_set_nip05(wiki_card, item->author_nip05, item->pubkey_hex);
    }

    if (item->author_lud16) {
      gnostr_wiki_card_set_author_lud16(wiki_card, item->author_lud16);
    }

    gnostr_wiki_card_set_logged_in(wiki_card, self->is_logged_in);

    /* Connect signals */
    g_signal_connect(wiki_card, "open-article", G_CALLBACK(on_card_open_article), self);
    g_signal_connect(wiki_card, "open-profile", G_CALLBACK(on_card_open_profile), self);
    g_signal_connect(wiki_card, "topic-clicked", G_CALLBACK(on_card_topic_clicked), self);
    g_signal_connect(wiki_card, "zap-requested", G_CALLBACK(on_card_zap_requested), self);
    g_signal_connect(wiki_card, "bookmark-toggled", G_CALLBACK(on_card_bookmark_toggled), self);

  } else {
    /* KIND_LONG_FORM (30023) */
    GnostrArticleCard *article_card = gnostr_article_card_new();
    card = GTK_WIDGET(article_card);

    gnostr_article_card_set_article(article_card,
                                     item->event_id,
                                     item->d_tag,
                                     item->title,
                                     item->summary,
                                     item->image_url,
                                     item->published_at);

    gnostr_article_card_set_author(article_card,
                                    item->author_name,
                                    item->author_handle,
                                    item->author_avatar,
                                    item->pubkey_hex);

    if (item->content) {
      gnostr_article_card_set_content(article_card, item->content);
    }

    if (item->author_nip05) {
      gnostr_article_card_set_nip05(article_card, item->author_nip05, item->pubkey_hex);
    }

    if (item->author_lud16) {
      gnostr_article_card_set_author_lud16(article_card, item->author_lud16);
    }

    gnostr_article_card_set_logged_in(article_card, self->is_logged_in);

    /* Connect signals */
    g_signal_connect(article_card, "open-article", G_CALLBACK(on_card_open_article), self);
    g_signal_connect(article_card, "open-profile", G_CALLBACK(on_card_open_profile), self);
    g_signal_connect(article_card, "zap-requested", G_CALLBACK(on_card_zap_requested), self);
    g_signal_connect(article_card, "bookmark-toggled", G_CALLBACK(on_card_bookmark_toggled), self);
  }

  if (card) {
    gtk_box_append(GTK_BOX(box), card);
  }
}

static void unbind_article_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  (void)user_data;

  GtkWidget *box = gtk_list_item_get_child(list_item);
  if (!box) return;

  /* Remove and destroy children */
  GtkWidget *child = gtk_widget_get_first_child(box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);

    /* Disconnect all signals */
    g_signal_handlers_disconnect_matched(child, G_SIGNAL_MATCH_DATA,
                                          0, 0, NULL, NULL, user_data);

    gtk_box_remove(GTK_BOX(box), child);
    child = next;
  }
}

/* --- Filter button handlers --- */

static void on_filter_all_toggled(GtkToggleButton *button, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);

  if (gtk_toggle_button_get_active(button)) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_wiki), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_blog), FALSE);
    self->type_filter = GNOSTR_ARTICLES_TYPE_ALL;
    apply_filters(self);
  } else {
    /* Don't allow all to be inactive */
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_wiki)) &&
        !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_blog))) {
      gtk_toggle_button_set_active(button, TRUE);
    }
  }
}

static void on_filter_wiki_toggled(GtkToggleButton *button, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);

  if (gtk_toggle_button_get_active(button)) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_all), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_blog), FALSE);
    self->type_filter = GNOSTR_ARTICLES_TYPE_WIKI;
    apply_filters(self);
  } else {
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_all)) &&
        !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_blog))) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_all), TRUE);
    }
  }
}

static void on_filter_blog_toggled(GtkToggleButton *button, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);

  if (gtk_toggle_button_get_active(button)) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_all), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_wiki), FALSE);
    self->type_filter = GNOSTR_ARTICLES_TYPE_BLOG;
    apply_filters(self);
  } else {
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_all)) &&
        !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_wiki))) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_all), TRUE);
    }
  }
}

static void on_clear_topic_clicked(GtkButton *button, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);
  (void)button;
  gnostr_articles_view_set_topic_filter(self, NULL);
}

/* --- Search handling --- */

static gboolean search_debounce_cb(gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);
  self->search_debounce_id = 0;

  const char *text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
  g_free(self->search_text);
  self->search_text = g_strdup(text && *text ? text : NULL);

  apply_filters(self);

  return G_SOURCE_REMOVE;
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);
  (void)entry;

  if (self->search_debounce_id) {
    g_source_remove(self->search_debounce_id);
  }
  self->search_debounce_id = g_timeout_add(300, search_debounce_cb, self);
}

/* --- Filter application --- */

static gboolean item_matches_filter(GnostrArticleItem *item, GnostrArticlesView *self) {
  /* Type filter */
  if (self->type_filter == GNOSTR_ARTICLES_TYPE_WIKI && item->kind != KIND_WIKI) {
    return FALSE;
  }
  if (self->type_filter == GNOSTR_ARTICLES_TYPE_BLOG && item->kind != KIND_LONG_FORM) {
    return FALSE;
  }

  /* Topic filter */
  if (self->topic_filter && *self->topic_filter) {
    gboolean topic_match = FALSE;
    if (item->topics) {
      for (gsize i = 0; i < item->topics_count; i++) {
        if (g_ascii_strcasecmp(item->topics[i], self->topic_filter) == 0) {
          topic_match = TRUE;
          break;
        }
      }
    }
    if (!topic_match) return FALSE;
  }

  /* Search text filter */
  if (self->search_text && *self->search_text) {
    gchar *search_lower = g_utf8_strdown(self->search_text, -1);
    gboolean match = FALSE;

    if (item->title) {
      gchar *title_lower = g_utf8_strdown(item->title, -1);
      if (strstr(title_lower, search_lower)) match = TRUE;
      g_free(title_lower);
    }

    if (!match && item->summary) {
      gchar *summary_lower = g_utf8_strdown(item->summary, -1);
      if (strstr(summary_lower, search_lower)) match = TRUE;
      g_free(summary_lower);
    }

    if (!match && item->author_name) {
      gchar *name_lower = g_utf8_strdown(item->author_name, -1);
      if (strstr(name_lower, search_lower)) match = TRUE;
      g_free(name_lower);
    }

    g_free(search_lower);
    if (!match) return FALSE;
  }

  return TRUE;
}

static void apply_filters(GnostrArticlesView *self) {
  /* For now, just update the content state.
   * Full filtering requires a filtered list model which would be added
   * when integrating with the actual data source. */
  update_content_state(self);
}

/* --- State updates --- */

static void update_article_count(GnostrArticlesView *self) {
  guint count = g_list_model_get_n_items(G_LIST_MODEL(self->articles_model));
  gchar *text = g_strdup_printf("%u articles", count);
  gtk_label_set_text(GTK_LABEL(self->lbl_count), text);
  g_free(text);
}

static void update_content_state(GnostrArticlesView *self) {
  guint count = g_list_model_get_n_items(G_LIST_MODEL(self->articles_model));

  if (count == 0) {
    gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "empty");
  } else {
    gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "results");
  }

  /* Update topic filter visibility */
  if (self->topic_filter && *self->topic_filter) {
    gtk_label_set_text(GTK_LABEL(self->topic_filter_label), self->topic_filter);
    gtk_widget_set_visible(self->topic_filter_box, TRUE);
  } else {
    gtk_widget_set_visible(self->topic_filter_box, FALSE);
  }

  update_article_count(self);
}

/* --- GObject implementation --- */

static void gnostr_articles_view_dispose(GObject *object) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(object);

  if (self->search_debounce_id) {
    g_source_remove(self->search_debounce_id);
    self->search_debounce_id = 0;
  }

  g_clear_object(&self->articles_model);
  g_clear_object(&self->selection);
  g_clear_object(&self->factory);

  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_ARTICLES_VIEW);

  G_OBJECT_CLASS(gnostr_articles_view_parent_class)->dispose(object);
}

static void gnostr_articles_view_finalize(GObject *object) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(object);

  g_free(self->topic_filter);
  g_free(self->search_text);

  G_OBJECT_CLASS(gnostr_articles_view_parent_class)->finalize(object);
}

static void gnostr_articles_view_class_init(GnostrArticlesViewClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_articles_view_dispose;
  object_class->finalize = gnostr_articles_view_finalize;

  /* Load template */
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);

  /* Bind template children */
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, root);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, header_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, title_label);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, search_entry);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, btn_all);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, btn_wiki);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, btn_blog);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, lbl_count);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, content_stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, articles_scroll);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, articles_list);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, empty_state);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, loading_spinner);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, topic_filter_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, topic_filter_label);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, btn_clear_topic);

  /* Signals */
  signals[SIGNAL_OPEN_ARTICLE] = g_signal_new(
    "open-article",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);

  signals[SIGNAL_OPEN_PROFILE] = g_signal_new(
    "open-profile",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_TOPIC_CLICKED] = g_signal_new(
    "topic-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_ZAP_REQUESTED] = g_signal_new(
    "zap-requested",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIGNAL_BOOKMARK_TOGGLED] = g_signal_new(
    "bookmark-toggled",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);

  /* CSS */
  gtk_widget_class_set_css_name(widget_class, "articles-view");

  /* Layout */
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void gnostr_articles_view_init(GnostrArticlesView *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->type_filter = GNOSTR_ARTICLES_TYPE_ALL;
  self->topic_filter = NULL;
  self->search_text = NULL;
  self->articles_loaded = FALSE;
  self->is_logged_in = FALSE;
  self->search_debounce_id = 0;

  /* Create model */
  self->articles_model = g_list_store_new(GNOSTR_TYPE_ARTICLE_ITEM);
  self->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(self->articles_model)));
  gtk_single_selection_set_autoselect(self->selection, FALSE);
  gtk_single_selection_set_can_unselect(self->selection, TRUE);

  /* Create factory */
  self->factory = gtk_signal_list_item_factory_new();
  g_signal_connect(self->factory, "setup", G_CALLBACK(setup_article_row), self);
  g_signal_connect(self->factory, "bind", G_CALLBACK(bind_article_row), self);
  g_signal_connect(self->factory, "unbind", G_CALLBACK(unbind_article_row), self);

  /* Set up list view */
  gtk_list_view_set_model(GTK_LIST_VIEW(self->articles_list),
                          GTK_SELECTION_MODEL(self->selection));
  gtk_list_view_set_factory(GTK_LIST_VIEW(self->articles_list), self->factory);

  /* Connect filter button signals */
  g_signal_connect(self->btn_all, "toggled", G_CALLBACK(on_filter_all_toggled), self);
  g_signal_connect(self->btn_wiki, "toggled", G_CALLBACK(on_filter_wiki_toggled), self);
  g_signal_connect(self->btn_blog, "toggled", G_CALLBACK(on_filter_blog_toggled), self);
  g_signal_connect(self->btn_clear_topic, "clicked", G_CALLBACK(on_clear_topic_clicked), self);

  /* Connect search signal */
  g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_search_changed), self);

  /* Default to All filter active */
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_all), TRUE);

  /* Start with empty state */
  gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "empty");
}

GnostrArticlesView *gnostr_articles_view_new(void) {
  return g_object_new(GNOSTR_TYPE_ARTICLES_VIEW, NULL);
}

void gnostr_articles_view_set_type_filter(GnostrArticlesView *self,
                                           GnostrArticlesType type) {
  g_return_if_fail(GNOSTR_IS_ARTICLES_VIEW(self));

  if (self->type_filter == type) return;

  self->type_filter = type;

  /* Update toggle buttons */
  switch (type) {
    case GNOSTR_ARTICLES_TYPE_ALL:
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_all), TRUE);
      break;
    case GNOSTR_ARTICLES_TYPE_WIKI:
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_wiki), TRUE);
      break;
    case GNOSTR_ARTICLES_TYPE_BLOG:
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_blog), TRUE);
      break;
  }
}

GnostrArticlesType gnostr_articles_view_get_type_filter(GnostrArticlesView *self) {
  g_return_val_if_fail(GNOSTR_IS_ARTICLES_VIEW(self), GNOSTR_ARTICLES_TYPE_ALL);
  return self->type_filter;
}

void gnostr_articles_view_set_topic_filter(GnostrArticlesView *self,
                                            const char *topic) {
  g_return_if_fail(GNOSTR_IS_ARTICLES_VIEW(self));

  g_free(self->topic_filter);
  self->topic_filter = g_strdup(topic);

  apply_filters(self);
}

const char *gnostr_articles_view_get_topic_filter(GnostrArticlesView *self) {
  g_return_val_if_fail(GNOSTR_IS_ARTICLES_VIEW(self), NULL);
  return self->topic_filter;
}

void gnostr_articles_view_set_search_text(GnostrArticlesView *self,
                                           const char *text) {
  g_return_if_fail(GNOSTR_IS_ARTICLES_VIEW(self));

  gtk_editable_set_text(GTK_EDITABLE(self->search_entry), text ? text : "");
}

void gnostr_articles_view_load_articles(GnostrArticlesView *self) {
  g_return_if_fail(GNOSTR_IS_ARTICLES_VIEW(self));

  if (self->articles_loaded) return;

  self->articles_loaded = TRUE;

  /* TODO: Load articles from nostrdb
   * For now, just update the content state to show empty or loaded articles */
  update_content_state(self);
}

void gnostr_articles_view_refresh(GnostrArticlesView *self) {
  g_return_if_fail(GNOSTR_IS_ARTICLES_VIEW(self));

  self->articles_loaded = FALSE;
  g_list_store_remove_all(self->articles_model);
  gnostr_articles_view_load_articles(self);
}

void gnostr_articles_view_set_loading(GnostrArticlesView *self,
                                       gboolean is_loading) {
  g_return_if_fail(GNOSTR_IS_ARTICLES_VIEW(self));

  if (is_loading) {
    gtk_spinner_start(GTK_SPINNER(self->loading_spinner));
    gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "loading");
  } else {
    gtk_spinner_stop(GTK_SPINNER(self->loading_spinner));
    update_content_state(self);
  }
}

guint gnostr_articles_view_get_article_count(GnostrArticlesView *self) {
  g_return_val_if_fail(GNOSTR_IS_ARTICLES_VIEW(self), 0);
  return g_list_model_get_n_items(G_LIST_MODEL(self->articles_model));
}

void gnostr_articles_view_set_logged_in(GnostrArticlesView *self,
                                         gboolean logged_in) {
  g_return_if_fail(GNOSTR_IS_ARTICLES_VIEW(self));
  self->is_logged_in = logged_in;
}
