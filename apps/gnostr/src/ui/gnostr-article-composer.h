/* gnostr-article-composer.h - NIP-23 Article Composer (nostrc-zwn4) */

#ifndef GNOSTR_ARTICLE_COMPOSER_H
#define GNOSTR_ARTICLE_COMPOSER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_ARTICLE_COMPOSER (gnostr_article_composer_get_type())
G_DECLARE_FINAL_TYPE(GnostrArticleComposer, gnostr_article_composer, GNOSTR, ARTICLE_COMPOSER, GtkWidget)

/**
 * GnostrArticleComposer:
 *
 * A widget for creating and editing NIP-23 long-form articles.
 * Contains fields for title, summary, image URL, hashtags, d-tag,
 * and a markdown editor with preview toggle.
 *
 * Signals:
 * - "publish-requested" (gboolean is_draft) - user wants to publish or save draft
 */

GtkWidget *gnostr_article_composer_new(void);

const char *gnostr_article_composer_get_title(GnostrArticleComposer *self);
const char *gnostr_article_composer_get_summary(GnostrArticleComposer *self);
const char *gnostr_article_composer_get_image_url(GnostrArticleComposer *self);
const char *gnostr_article_composer_get_content(GnostrArticleComposer *self);
const char *gnostr_article_composer_get_d_tag(GnostrArticleComposer *self);
char **gnostr_article_composer_get_hashtags(GnostrArticleComposer *self);

G_END_DECLS

#endif /* GNOSTR_ARTICLE_COMPOSER_H */
