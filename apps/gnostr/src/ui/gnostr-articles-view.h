/*
 * gnostr-articles-view.h - Long-form Content Browse View
 *
 * Displays browsable lists of:
 * - NIP-54 Wiki articles (kind 30818)
 * - NIP-23 Long-form articles (kind 30023)
 *
 * Features:
 * - Toggle between Wiki and Blog/Articles
 * - Search/filter by topic
 * - Virtualized list for performance
 */

#ifndef GNOSTR_ARTICLES_VIEW_H
#define GNOSTR_ARTICLES_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_ARTICLES_VIEW (gnostr_articles_view_get_type())

G_DECLARE_FINAL_TYPE(GnostrArticlesView, gnostr_articles_view, GNOSTR, ARTICLES_VIEW, GtkWidget)

/**
 * Content type enumeration for filtering
 */
typedef enum {
  GNOSTR_ARTICLES_TYPE_ALL,
  GNOSTR_ARTICLES_TYPE_WIKI,    /* NIP-54 kind 30818 */
  GNOSTR_ARTICLES_TYPE_BLOG     /* NIP-23 kind 30023 */
} GnostrArticlesType;

/**
 * Signals:
 * "open-article" (gchar* event_id_hex, gint kind, gpointer user_data)
 *   - Emitted when user clicks to view an article
 *   - kind is 30818 for wiki, 30023 for long-form
 *
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user clicks an author's profile
 *
 * "topic-clicked" (gchar* topic, gpointer user_data)
 *   - Emitted when user clicks a topic tag to filter
 *
 * "zap-requested" (gchar* event_id, gchar* pubkey_hex, gchar* lud16, gpointer user_data)
 *   - Emitted when user wants to zap an article author
 *
 * "bookmark-toggled" (gchar* event_id, gboolean is_bookmarked, gpointer user_data)
 *   - Emitted when bookmark state changes
 */

/**
 * gnostr_articles_view_new:
 *
 * Creates a new articles browse view widget.
 *
 * Returns: (transfer full): A new #GnostrArticlesView
 */
GnostrArticlesView *gnostr_articles_view_new(void);

/**
 * gnostr_articles_view_set_type_filter:
 * @self: the articles view
 * @type: the content type to show
 *
 * Filter articles by type (all, wiki only, blog only).
 */
void gnostr_articles_view_set_type_filter(GnostrArticlesView *self,
                                           GnostrArticlesType type);

/**
 * gnostr_articles_view_get_type_filter:
 * @self: the articles view
 *
 * Returns: The current type filter.
 */
GnostrArticlesType gnostr_articles_view_get_type_filter(GnostrArticlesView *self);

/**
 * gnostr_articles_view_set_topic_filter:
 * @self: the articles view
 * @topic: (nullable): topic to filter by, or NULL to show all
 *
 * Filter articles by topic/tag.
 */
void gnostr_articles_view_set_topic_filter(GnostrArticlesView *self,
                                            const char *topic);

/**
 * gnostr_articles_view_get_topic_filter:
 * @self: the articles view
 *
 * Returns: (transfer none) (nullable): The current topic filter or NULL.
 */
const char *gnostr_articles_view_get_topic_filter(GnostrArticlesView *self);

/**
 * gnostr_articles_view_set_search_text:
 * @self: the articles view
 * @text: (nullable): search text, or NULL to clear
 *
 * Filter articles by search text (matches title, summary, author).
 */
void gnostr_articles_view_set_search_text(GnostrArticlesView *self,
                                           const char *text);

/**
 * gnostr_articles_view_load_articles:
 * @self: the articles view
 *
 * Load articles from the local cache. Call this when the view becomes visible.
 */
void gnostr_articles_view_load_articles(GnostrArticlesView *self);

/**
 * gnostr_articles_view_refresh:
 * @self: the articles view
 *
 * Force reload articles from the database.
 */
void gnostr_articles_view_refresh(GnostrArticlesView *self);

/**
 * gnostr_articles_view_set_loading:
 * @self: the articles view
 * @is_loading: whether to show loading state
 *
 * Show/hide loading spinner.
 */
void gnostr_articles_view_set_loading(GnostrArticlesView *self,
                                       gboolean is_loading);

/**
 * gnostr_articles_view_get_article_count:
 * @self: the articles view
 *
 * Returns: Number of articles currently displayed.
 */
guint gnostr_articles_view_get_article_count(GnostrArticlesView *self);

/**
 * gnostr_articles_view_set_logged_in:
 * @self: the articles view
 * @logged_in: whether user is logged in
 *
 * Set login state (affects zap/bookmark button sensitivity).
 */
void gnostr_articles_view_set_logged_in(GnostrArticlesView *self,
                                         gboolean logged_in);

G_END_DECLS

#endif /* GNOSTR_ARTICLES_VIEW_H */
