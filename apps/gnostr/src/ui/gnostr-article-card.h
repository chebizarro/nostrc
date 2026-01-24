/*
 * gnostr-article-card.h - NIP-23 Long-form Content Card Widget
 *
 * Displays kind 30023 long-form article events with:
 * - Title from "title" tag
 * - Summary from "summary" tag
 * - Author avatar/name from profile lookup
 * - Publication date from "published_at" tag
 * - Header image from "image" tag
 * - Markdown content rendered to Pango markup
 * - Support for "a" tag references to articles
 */

#ifndef GNOSTR_ARTICLE_CARD_H
#define GNOSTR_ARTICLE_CARD_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_ARTICLE_CARD (gnostr_article_card_get_type())

G_DECLARE_FINAL_TYPE(GnostrArticleCard, gnostr_article_card, GNOSTR, ARTICLE_CARD, GtkWidget)

/* Signals:
 * "open-profile" (gchar* pubkey_hex, gpointer user_data) - open author profile
 * "open-article" (gchar* event_id_hex, gpointer user_data) - open full article view
 * "open-url" (gchar* url, gpointer user_data) - open external URL
 * "zap-requested" (gchar* event_id, gchar* pubkey_hex, gchar* lud16, gpointer user_data)
 * "bookmark-toggled" (gchar* event_id, gboolean is_bookmarked, gpointer user_data)
 * "share-article" (gchar* nostr_uri, gpointer user_data)
 */

typedef struct _GnostrArticleCard GnostrArticleCard;

GnostrArticleCard *gnostr_article_card_new(void);

/* Set the article metadata (from event tags) */
void gnostr_article_card_set_article(GnostrArticleCard *self,
                                      const char *event_id,
                                      const char *d_tag,
                                      const char *title,
                                      const char *summary,
                                      const char *image_url,
                                      gint64 published_at);

/* Set the author information */
void gnostr_article_card_set_author(GnostrArticleCard *self,
                                     const char *display_name,
                                     const char *handle,
                                     const char *avatar_url,
                                     const char *pubkey_hex);

/* Set the full markdown content */
void gnostr_article_card_set_content(GnostrArticleCard *self,
                                      const char *markdown_content);

/* Set NIP-05 verification status */
void gnostr_article_card_set_nip05(GnostrArticleCard *self,
                                    const char *nip05,
                                    const char *pubkey_hex);

/* Set author's lightning address for zapping */
void gnostr_article_card_set_author_lud16(GnostrArticleCard *self,
                                           const char *lud16);

/* Set bookmark state */
void gnostr_article_card_set_bookmarked(GnostrArticleCard *self,
                                         gboolean is_bookmarked);

/* Set login state (affects button sensitivity) */
void gnostr_article_card_set_logged_in(GnostrArticleCard *self,
                                        gboolean logged_in);

/* Get the d-tag identifier for this article */
const char *gnostr_article_card_get_d_tag(GnostrArticleCard *self);

/* Get the article's NIP-33 "a" tag reference (kind:pubkey:d-tag) */
char *gnostr_article_card_get_a_tag(GnostrArticleCard *self);

G_END_DECLS

#endif /* GNOSTR_ARTICLE_CARD_H */
