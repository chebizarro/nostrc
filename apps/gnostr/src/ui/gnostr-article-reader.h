/* gnostr-article-reader.h - NIP-23 Article Reader Side Panel (nostrc-zwn4) */

#ifndef GNOSTR_ARTICLE_READER_H
#define GNOSTR_ARTICLE_READER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_ARTICLE_READER (gnostr_article_reader_get_type())
G_DECLARE_FINAL_TYPE(GnostrArticleReader, gnostr_article_reader, GNOSTR, ARTICLE_READER, GtkWidget)

/**
 * GnostrArticleReader:
 *
 * A side-panel widget for reading NIP-23 long-form articles.
 * Fetches an article from NDB, parses NIP-23 metadata, and renders
 * the full markdown content using Pango markup.
 *
 * Signals:
 * - "close-requested" - user wants to close the reader
 * - "open-profile" (const char *pubkey_hex) - user clicked an author
 * - "open-url" (const char *url) - user clicked a link
 * - "zap-requested" (const char *event_id, const char *pubkey_hex, const char *lud16)
 * - "share-article" (const char *naddr) - user wants to share
 */

GtkWidget *gnostr_article_reader_new(void);

void gnostr_article_reader_load_event(GnostrArticleReader *self,
                                       const char *event_id_hex);

void gnostr_article_reader_clear(GnostrArticleReader *self);

G_END_DECLS

#endif /* GNOSTR_ARTICLE_READER_H */
