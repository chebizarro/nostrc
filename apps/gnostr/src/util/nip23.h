/*
 * nip23.h - NIP-23 Long-form Content Utilities
 *
 * NIP-23 defines kind 30023 for long-form content (articles/blog posts).
 * This module provides utilities for parsing and extracting article metadata
 * from event tags.
 *
 * Required tags for kind 30023:
 * - "d" - unique identifier for the article (required for addressable events)
 *
 * Optional tags:
 * - "title" - article title
 * - "summary" - short description/excerpt
 * - "image" - header/cover image URL
 * - "published_at" - original publication timestamp (unix seconds)
 * - "t" - hashtags/topics (multiple allowed)
 * - "a" - references to other articles
 * - "client" - client application that created the article
 */

#ifndef NIP23_H
#define NIP23_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Kind number for long-form content */
#define NOSTR_KIND_LONG_FORM 30023

/* Kind number for draft articles */
#define NOSTR_KIND_LONG_FORM_DRAFT 30024

/*
 * GnostrArticleMeta:
 * Structure containing parsed NIP-23 article metadata.
 * All strings are owned by the structure and freed with gnostr_article_meta_free().
 */
typedef struct {
  gchar *d_tag;          /* Unique identifier (required) */
  gchar *title;          /* Article title */
  gchar *summary;        /* Short summary/description */
  gchar *image;          /* Header image URL */
  gint64 published_at;   /* Publication timestamp (0 if not specified) */
  gchar **hashtags;      /* NULL-terminated array of hashtags (without #) */
  gsize hashtags_count;  /* Number of hashtags */
  gchar *client;         /* Client application name */
} GnostrArticleMeta;

/*
 * gnostr_article_meta_new:
 *
 * Creates a new empty article metadata structure.
 * Use gnostr_article_meta_free() to free.
 *
 * Returns: (transfer full): New article metadata.
 */
GnostrArticleMeta *gnostr_article_meta_new(void);

/*
 * gnostr_article_meta_free:
 * @meta: The metadata to free, may be NULL.
 *
 * Frees an article metadata structure and all its contents.
 */
void gnostr_article_meta_free(GnostrArticleMeta *meta);

/*
 * gnostr_article_parse_tags:
 * @tags_json: JSON array string containing event tags.
 *
 * Parses NIP-23 specific tags from an event's tags array.
 * The tags_json should be the JSON representation of the tags array
 * (e.g., from nostr_event_get_tags()).
 *
 * Returns: (transfer full) (nullable): Parsed metadata or NULL on error.
 */
GnostrArticleMeta *gnostr_article_parse_tags(const char *tags_json);

/*
 * gnostr_article_parse_tags_array:
 * @tags: Array of tag arrays from nostrdb.
 * @tags_count: Number of tags.
 *
 * Parses NIP-23 specific tags from a nostrdb tag iterator.
 * Alternative to gnostr_article_parse_tags for nostrdb integration.
 *
 * Returns: (transfer full) (nullable): Parsed metadata or NULL on error.
 */
GnostrArticleMeta *gnostr_article_parse_tags_iter(void *txn, void *ndb_note);

/*
 * gnostr_article_is_article:
 * @kind: Event kind number.
 *
 * Returns: TRUE if kind is a long-form content event (30023 or 30024).
 */
gboolean gnostr_article_is_article(int kind);

/*
 * gnostr_article_build_naddr:
 * @kind: Event kind (30023 or 30024).
 * @pubkey_hex: Author's public key in hex format.
 * @d_tag: The "d" tag value.
 * @relays: NULL-terminated array of relay URLs (may be NULL).
 *
 * Builds a NIP-19 naddr bech32 string for referencing this article.
 * Useful for creating nostr: links and "a" tag values.
 *
 * Returns: (transfer full) (nullable): Bech32 naddr string or NULL on error.
 */
char *gnostr_article_build_naddr(int kind, const char *pubkey_hex,
                                  const char *d_tag, const char **relays);

/*
 * gnostr_article_build_a_tag:
 * @kind: Event kind.
 * @pubkey_hex: Author's public key.
 * @d_tag: The "d" tag value.
 *
 * Builds an "a" tag value for referencing this article.
 * Format: "kind:pubkey:d-tag"
 *
 * Returns: (transfer full): "a" tag value string.
 */
char *gnostr_article_build_a_tag(int kind, const char *pubkey_hex,
                                  const char *d_tag);

/*
 * gnostr_article_parse_a_tag:
 * @a_tag: The "a" tag value to parse.
 * @out_kind: (out) (nullable): Parsed kind number.
 * @out_pubkey: (out) (transfer full) (nullable): Parsed pubkey hex.
 * @out_d_tag: (out) (transfer full) (nullable): Parsed d-tag.
 *
 * Parses an "a" tag value into its components.
 *
 * Returns: TRUE if parsing succeeded.
 */
gboolean gnostr_article_parse_a_tag(const char *a_tag,
                                     int *out_kind,
                                     char **out_pubkey,
                                     char **out_d_tag);

/*
 * gnostr_article_estimate_reading_time:
 * @content: Article content (markdown or plain text).
 * @words_per_minute: Reading speed (0 for default 200 WPM).
 *
 * Estimates reading time based on word count.
 *
 * Returns: Estimated reading time in minutes.
 */
int gnostr_article_estimate_reading_time(const char *content,
                                          int words_per_minute);

G_END_DECLS

#endif /* NIP23_H */
