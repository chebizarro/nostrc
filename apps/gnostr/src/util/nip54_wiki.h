/*
 * nip54_wiki.h - NIP-54 Wiki Utilities
 *
 * NIP-54 defines wiki article events (kind 30818) for collaborative,
 * addressable wiki-style content on Nostr.
 *
 * Wiki Event Structure:
 * - kind: 30818 (parameterized replaceable)
 * - content: Article content in Markdown format
 * - tags:
 *   - ["d", "<slug>"] - Article identifier/slug (required, lowercase)
 *   - ["title", "<title>"] - Display title
 *   - ["summary", "<description>"] - Short summary/description
 *   - ["a", "<kind>:<pubkey>:<d-tag>", "<relay-url>"] - Related articles
 *   - ["e", "<event-id>", "<relay-url>"] - Fork/merge source
 *   - ["published_at", "<unix-timestamp>"] - Original publication time
 *   - ["t", "<topic>"] - Topic/category tags
 *
 * Key characteristics:
 * - Parameterized replaceable events (NIP-33)
 * - Multiple authors can write articles with the same "d" tag
 * - Readers choose which version to display (reputation-based)
 * - Content should be in Markdown format
 */

#ifndef NIP54_WIKI_H
#define NIP54_WIKI_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Kind number for wiki article events */
#define NOSTR_KIND_WIKI 30818

/*
 * GnostrWikiArticle:
 * Structure containing parsed NIP-54 wiki article metadata.
 * All strings are owned by the structure and freed with gnostr_wiki_article_free().
 */
typedef struct {
  /* Event metadata */
  gchar *event_id;          /* Event ID (hex) */
  gchar *pubkey;            /* Author's pubkey (hex) */
  gint64 created_at;        /* Event timestamp */
  gint64 published_at;      /* Publication timestamp (0 if not specified) */

  /* Article content */
  gchar *d_tag;             /* Article identifier/slug (required) */
  gchar *title;             /* Display title */
  gchar *summary;           /* Short summary/description */
  gchar *content;           /* Full Markdown content */

  /* Related articles (array of "a" tag values) */
  gchar **related_articles; /* NULL-terminated array */
  gsize related_count;      /* Number of related articles */

  /* Topics/categories (from "t" tags) */
  gchar **topics;           /* NULL-terminated array */
  gsize topics_count;       /* Number of topics */

  /* Fork/merge references (from "e" tags) */
  gchar **fork_refs;        /* NULL-terminated array of event IDs */
  gsize fork_refs_count;    /* Number of fork references */
} GnostrWikiArticle;

/*
 * GnostrWikiRelatedArticle:
 * Parsed related article reference from "a" tag.
 */
typedef struct {
  gint kind;                /* Event kind (usually 30818) */
  gchar *pubkey;            /* Author pubkey (hex) */
  gchar *d_tag;             /* Article d-tag/slug */
  gchar *relay_hint;        /* Optional relay URL hint */
} GnostrWikiRelatedArticle;

/*
 * gnostr_wiki_article_new:
 *
 * Creates a new empty wiki article structure.
 * Use gnostr_wiki_article_free() to free.
 *
 * Returns: (transfer full): New wiki article structure.
 */
GnostrWikiArticle *gnostr_wiki_article_new(void);

/*
 * gnostr_wiki_article_free:
 * @article: The article to free, may be NULL.
 *
 * Frees a wiki article structure and all its contents.
 */
void gnostr_wiki_article_free(GnostrWikiArticle *article);

/*
 * gnostr_wiki_article_parse_json:
 * @event_json: JSON string of kind 30818 event.
 *
 * Parses a wiki article event from JSON.
 *
 * Returns: (transfer full) (nullable): Parsed article or NULL on error.
 */
GnostrWikiArticle *gnostr_wiki_article_parse_json(const char *event_json);

/*
 * gnostr_wiki_article_parse_tags:
 * @tags_json: JSON array string containing event tags.
 * @content: The event content (Markdown text).
 *
 * Parses NIP-54 specific tags from an event's tags array.
 *
 * Returns: (transfer full) (nullable): Parsed article or NULL on error.
 */
GnostrWikiArticle *gnostr_wiki_article_parse_tags(const char *tags_json,
                                                    const char *content);

/*
 * gnostr_wiki_is_wiki_article:
 * @kind: Event kind number.
 *
 * Returns: TRUE if kind is a wiki article event (30818).
 */
gboolean gnostr_wiki_is_wiki_article(int kind);

/*
 * gnostr_wiki_parse_a_tag:
 * @a_tag: The "a" tag value to parse.
 *
 * Parses a related article "a" tag into its components.
 *
 * Returns: (transfer full) (nullable): Parsed related article or NULL on error.
 */
GnostrWikiRelatedArticle *gnostr_wiki_parse_a_tag(const char *a_tag);

/*
 * gnostr_wiki_related_article_free:
 * @related: The related article to free, may be NULL.
 *
 * Frees a related article structure.
 */
void gnostr_wiki_related_article_free(GnostrWikiRelatedArticle *related);

/*
 * gnostr_wiki_build_a_tag:
 * @pubkey_hex: Author's public key.
 * @d_tag: Article d-tag/slug.
 *
 * Builds an "a" tag value for referencing this article.
 * Format: "30818:pubkey:d-tag"
 *
 * Returns: (transfer full): "a" tag value string.
 */
char *gnostr_wiki_build_a_tag(const char *pubkey_hex, const char *d_tag);

/*
 * gnostr_wiki_build_naddr:
 * @pubkey_hex: Author's public key in hex format.
 * @d_tag: The "d" tag value (article slug).
 * @relays: NULL-terminated array of relay URLs (may be NULL).
 *
 * Builds a NIP-19 naddr bech32 string for referencing this wiki article.
 * Useful for creating nostr: links.
 *
 * Returns: (transfer full) (nullable): Bech32 naddr string or NULL on error.
 */
char *gnostr_wiki_build_naddr(const char *pubkey_hex,
                               const char *d_tag,
                               const char **relays);

/*
 * gnostr_wiki_normalize_slug:
 * @title: Article title to normalize.
 *
 * Normalizes a title into a valid wiki slug.
 * Converts to lowercase, replaces spaces/special chars with hyphens.
 *
 * Returns: (transfer full): Normalized slug string.
 */
char *gnostr_wiki_normalize_slug(const char *title);

/*
 * gnostr_wiki_build_event_json:
 * @d_tag: Article identifier/slug.
 * @title: Display title.
 * @summary: (nullable): Short summary.
 * @content: Markdown content.
 * @related_articles: (nullable): NULL-terminated array of "a" tag values.
 * @topics: (nullable): NULL-terminated array of topics.
 *
 * Builds an unsigned wiki article event JSON for signing.
 * Caller must sign the event before publishing.
 *
 * Returns: (transfer full) (nullable): Unsigned event JSON or NULL on error.
 */
char *gnostr_wiki_build_event_json(const char *d_tag,
                                    const char *title,
                                    const char *summary,
                                    const char *content,
                                    const char **related_articles,
                                    const char **topics);

/*
 * gnostr_wiki_estimate_reading_time:
 * @content: Article content (markdown).
 * @words_per_minute: Reading speed (0 for default 200 WPM).
 *
 * Estimates reading time based on word count.
 *
 * Returns: Estimated reading time in minutes.
 */
int gnostr_wiki_estimate_reading_time(const char *content,
                                       int words_per_minute);

/*
 * gnostr_wiki_extract_table_of_contents:
 * @markdown: Markdown content.
 *
 * Extracts headings from markdown to build a table of contents.
 * Returns array of heading structs with level and text.
 *
 * Returns: (transfer full) (nullable): GPtrArray of GnostrWikiHeading or NULL.
 */
typedef struct {
  int level;        /* Heading level (1-6) */
  gchar *text;      /* Heading text */
  gchar *anchor;    /* URL-safe anchor ID */
} GnostrWikiHeading;

GPtrArray *gnostr_wiki_extract_table_of_contents(const char *markdown);

/*
 * gnostr_wiki_heading_free:
 * @heading: The heading to free, may be NULL.
 *
 * Frees a heading structure.
 */
void gnostr_wiki_heading_free(GnostrWikiHeading *heading);

G_END_DECLS

#endif /* NIP54_WIKI_H */
