/*
 * nip_c0_snippets.h - NIP-C0 (0xC0/192) Code Snippets Utilities
 *
 * NIP-C0 defines kind 192 (0xC0) for code snippet events - sharing
 * code snippets on Nostr with programming language and metadata.
 *
 * Code Snippet Event Structure:
 * - kind: 192 (0xC0)
 * - content: The actual code
 * - tags:
 *   - ["title", "<title>"] - snippet title/name
 *   - ["lang", "<language>"] - programming language (rust, python, etc.)
 *   - ["description", "<desc>"] - what the code does
 *   - ["t", "<tag>"] - tags/categories (repeatable)
 *   - ["runtime", "<version>"] - runtime/compiler version
 *   - ["license", "<spdx-id>"] - license (MIT, Apache-2.0, etc.)
 */

#ifndef NIP_C0_SNIPPETS_H
#define NIP_C0_SNIPPETS_H

#include <glib.h>

G_BEGIN_DECLS

/* Kind number for code snippet events (0xC0 = 192 decimal) */
#define NIPC0_KIND_SNIPPET 192

/*
 * GnostrCodeSnippet:
 * Structure containing parsed NIP-C0 code snippet data.
 * All strings are owned by the structure and freed with gnostr_code_snippet_free().
 */
typedef struct {
  /* Event metadata */
  gchar *event_id;            /* Snippet event ID (hex) */
  gchar *pubkey;              /* Creator's pubkey (hex) */
  gint64 created_at;          /* Timestamp */

  /* Snippet content */
  gchar *code;                /* The actual code (content field) */
  gchar *title;               /* Snippet title/name from "title" tag */
  gchar *language;            /* Programming language from "lang" tag */
  gchar *description;         /* Description from "description" tag */

  /* Tags/categories - array of strings */
  gchar **tags;               /* Tag strings from "t" tags */
  gsize tag_count;            /* Number of tags */

  /* Optional metadata */
  gchar *runtime;             /* Runtime/compiler version from "runtime" tag */
  gchar *license;             /* SPDX license identifier from "license" tag */
} GnostrCodeSnippet;

/*
 * gnostr_code_snippet_new:
 *
 * Creates a new empty code snippet structure.
 * Use gnostr_code_snippet_free() to free.
 *
 * Returns: (transfer full): New code snippet structure.
 */
GnostrCodeSnippet *gnostr_code_snippet_new(void);

/*
 * gnostr_code_snippet_free:
 * @snippet: The code snippet to free, may be NULL.
 *
 * Frees a code snippet structure and all its contents.
 */
void gnostr_code_snippet_free(GnostrCodeSnippet *snippet);

/*
 * gnostr_code_snippet_parse:
 * @event_json: JSON string of kind 192 event.
 *
 * Parses a code snippet event from JSON.
 *
 * Returns: (transfer full) (nullable): Parsed snippet or NULL on error.
 */
GnostrCodeSnippet *gnostr_code_snippet_parse(const char *event_json);

/*
 * gnostr_code_snippet_build_tags:
 * @snippet: The code snippet to build tags for.
 *
 * Builds the tags array for a code snippet event.
 * The returned JSON array should be used in an event's "tags" field.
 *
 * Returns: (transfer full) (nullable): JSON array string of tags, or NULL on error.
 *          Caller must free with g_free().
 */
gchar *gnostr_code_snippet_build_tags(const GnostrCodeSnippet *snippet);

/*
 * gnostr_code_snippet_build_event_json:
 * @code: The actual code content.
 * @title: (nullable): Snippet title/name.
 * @language: (nullable): Programming language.
 * @description: (nullable): Description of what the code does.
 * @tags: (nullable): NULL-terminated array of tag strings.
 * @runtime: (nullable): Runtime/compiler version.
 * @license: (nullable): SPDX license identifier.
 *
 * Builds an unsigned code snippet event JSON for signing.
 * Caller must sign the event before publishing.
 *
 * Returns: (transfer full) (nullable): Unsigned event JSON or NULL on error.
 *          Caller must free with g_free().
 */
gchar *gnostr_code_snippet_build_event_json(const char *code,
                                             const char *title,
                                             const char *language,
                                             const char *description,
                                             const char **tags,
                                             const char *runtime,
                                             const char *license);

/*
 * gnostr_code_snippet_normalize_language:
 * @language: The language string to normalize.
 *
 * Normalizes a programming language name to a canonical form.
 * Handles common variations like "js" -> "javascript", "py" -> "python", etc.
 *
 * Returns: (transfer full): Normalized language string.
 *          Caller must free with g_free().
 */
gchar *gnostr_code_snippet_normalize_language(const char *language);

/*
 * gnostr_code_snippet_get_language_display_name:
 * @language: The normalized language identifier.
 *
 * Gets a human-readable display name for a programming language.
 *
 * Returns: (transfer full): Display name string.
 *          Caller must free with g_free().
 */
gchar *gnostr_code_snippet_get_language_display_name(const char *language);

/*
 * gnostr_code_snippet_dup:
 * @snippet: The code snippet to duplicate.
 *
 * Creates a deep copy of a code snippet structure.
 *
 * Returns: (transfer full) (nullable): Duplicated snippet or NULL if input is NULL.
 */
GnostrCodeSnippet *gnostr_code_snippet_dup(const GnostrCodeSnippet *snippet);

G_END_DECLS

#endif /* NIP_C0_SNIPPETS_H */
