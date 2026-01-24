/*
 * nip84_highlights.h - NIP-84 Highlights Utilities
 *
 * NIP-84 defines kind 9802 for highlight events - saving text selections
 * from notes, articles, or external URLs with contextual information.
 *
 * Highlight Event Structure:
 * - kind: 9802
 * - content: The highlighted text
 * - tags:
 *   - ["context", "..."] - surrounding text for context
 *   - ["e", "<event-id>", "<relay-url>", "mention"] - source note (kind 1)
 *   - ["a", "<kind>:<pubkey>:<d-tag>", "<relay-url>", "mention"] - addressable event (articles)
 *   - ["r", "<url>"] - external URL source
 *   - ["p", "<pubkey>", "<relay-url>"] - original author
 *   - ["comment", "..."] - optional user annotation
 */

#ifndef NIP84_HIGHLIGHTS_H
#define NIP84_HIGHLIGHTS_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Kind number for highlight events */
#define NOSTR_KIND_HIGHLIGHT 9802

/*
 * GnostrHighlightSource:
 * Enumeration of possible highlight source types.
 */
typedef enum {
  GNOSTR_HIGHLIGHT_SOURCE_NONE,     /* Unknown/no source */
  GNOSTR_HIGHLIGHT_SOURCE_NOTE,     /* kind 1 text note (via "e" tag) */
  GNOSTR_HIGHLIGHT_SOURCE_ARTICLE,  /* kind 30023 article (via "a" tag) */
  GNOSTR_HIGHLIGHT_SOURCE_URL,      /* External URL (via "r" tag) */
} GnostrHighlightSource;

/*
 * GnostrHighlight:
 * Structure containing parsed NIP-84 highlight data.
 * All strings are owned by the structure and freed with gnostr_highlight_free().
 */
typedef struct {
  /* Event metadata */
  gchar *event_id;            /* Highlight event ID (hex) */
  gchar *pubkey;              /* Creator's pubkey (hex) */
  gint64 created_at;          /* Timestamp */

  /* Highlight content */
  gchar *highlighted_text;    /* The actual highlighted text (content field) */
  gchar *context;             /* Surrounding context from "context" tag */
  gchar *comment;             /* User's annotation/comment */

  /* Source reference */
  GnostrHighlightSource source_type;
  gchar *source_event_id;     /* For NOTE: event ID */
  gchar *source_a_tag;        /* For ARTICLE: full a-tag value */
  gchar *source_url;          /* For URL: external URL */
  gchar *source_relay_hint;   /* Relay hint for source */

  /* Author reference */
  gchar *author_pubkey;       /* Original content author's pubkey */
  gchar *author_relay_hint;   /* Relay hint for author */
} GnostrHighlight;

/*
 * gnostr_highlight_new:
 *
 * Creates a new empty highlight structure.
 * Use gnostr_highlight_free() to free.
 *
 * Returns: (transfer full): New highlight structure.
 */
GnostrHighlight *gnostr_highlight_new(void);

/*
 * gnostr_highlight_free:
 * @highlight: The highlight to free, may be NULL.
 *
 * Frees a highlight structure and all its contents.
 */
void gnostr_highlight_free(GnostrHighlight *highlight);

/*
 * gnostr_highlight_parse_json:
 * @event_json: JSON string of kind 9802 event.
 *
 * Parses a highlight event from JSON.
 *
 * Returns: (transfer full) (nullable): Parsed highlight or NULL on error.
 */
GnostrHighlight *gnostr_highlight_parse_json(const char *event_json);

/*
 * gnostr_highlight_parse_tags:
 * @tags_json: JSON array string containing event tags.
 * @content: The event content (highlighted text).
 *
 * Parses NIP-84 specific tags from an event's tags array.
 *
 * Returns: (transfer full) (nullable): Parsed highlight or NULL on error.
 */
GnostrHighlight *gnostr_highlight_parse_tags(const char *tags_json,
                                               const char *content);

/*
 * gnostr_highlight_build_event_json:
 * @highlighted_text: The text that was highlighted.
 * @context: (nullable): Surrounding context text.
 * @comment: (nullable): User's annotation.
 * @source_event_id: (nullable): Source event ID (for notes).
 * @source_a_tag: (nullable): Source "a" tag (for articles).
 * @source_url: (nullable): Source URL (for external content).
 * @author_pubkey: (nullable): Original author's pubkey.
 * @relay_hint: (nullable): Relay URL hint.
 *
 * Builds an unsigned highlight event JSON for signing.
 * Caller must sign the event before publishing.
 *
 * Returns: (transfer full) (nullable): Unsigned event JSON or NULL on error.
 */
char *gnostr_highlight_build_event_json(const char *highlighted_text,
                                         const char *context,
                                         const char *comment,
                                         const char *source_event_id,
                                         const char *source_a_tag,
                                         const char *source_url,
                                         const char *author_pubkey,
                                         const char *relay_hint);

/*
 * gnostr_highlight_build_from_note:
 * @highlighted_text: The text that was highlighted.
 * @context: (nullable): Surrounding context text.
 * @comment: (nullable): User's annotation.
 * @note_event_id: Source note's event ID (hex).
 * @note_author_pubkey: Source note's author pubkey (hex).
 * @relay_hint: (nullable): Relay URL hint.
 *
 * Convenience function to build a highlight from a kind 1 note.
 *
 * Returns: (transfer full) (nullable): Unsigned event JSON or NULL on error.
 */
char *gnostr_highlight_build_from_note(const char *highlighted_text,
                                        const char *context,
                                        const char *comment,
                                        const char *note_event_id,
                                        const char *note_author_pubkey,
                                        const char *relay_hint);

/*
 * gnostr_highlight_build_from_article:
 * @highlighted_text: The text that was highlighted.
 * @context: (nullable): Surrounding context text.
 * @comment: (nullable): User's annotation.
 * @article_kind: Article kind (30023 or 30024).
 * @article_pubkey: Article author's pubkey (hex).
 * @article_d_tag: Article's "d" tag value.
 * @relay_hint: (nullable): Relay URL hint.
 *
 * Convenience function to build a highlight from a NIP-23 article.
 *
 * Returns: (transfer full) (nullable): Unsigned event JSON or NULL on error.
 */
char *gnostr_highlight_build_from_article(const char *highlighted_text,
                                           const char *context,
                                           const char *comment,
                                           int article_kind,
                                           const char *article_pubkey,
                                           const char *article_d_tag,
                                           const char *relay_hint);

/*
 * gnostr_highlight_build_from_url:
 * @highlighted_text: The text that was highlighted.
 * @context: (nullable): Surrounding context text.
 * @comment: (nullable): User's annotation.
 * @url: The external URL.
 *
 * Convenience function to build a highlight from an external URL.
 *
 * Returns: (transfer full) (nullable): Unsigned event JSON or NULL on error.
 */
char *gnostr_highlight_build_from_url(const char *highlighted_text,
                                       const char *context,
                                       const char *comment,
                                       const char *url);

/*
 * gnostr_highlight_get_source_description:
 * @highlight: The highlight to describe.
 *
 * Gets a human-readable description of the highlight source.
 *
 * Returns: (transfer full): Description string (e.g., "From note by npub1...")
 */
char *gnostr_highlight_get_source_description(const GnostrHighlight *highlight);

/*
 * gnostr_highlight_extract_context:
 * @full_text: The full text content.
 * @selection_start: Start index of selection.
 * @selection_end: End index of selection.
 * @context_chars: Number of context characters to include (before/after).
 *
 * Extracts highlighted text with surrounding context from full text.
 * Attempts to find natural break points (sentences/paragraphs) for context.
 *
 * Returns: (transfer full) (nullable): Context string or NULL if invalid indices.
 */
char *gnostr_highlight_extract_context(const char *full_text,
                                        gsize selection_start,
                                        gsize selection_end,
                                        gsize context_chars);

G_END_DECLS

#endif /* NIP84_HIGHLIGHTS_H */
