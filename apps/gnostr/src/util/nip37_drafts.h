/*
 * nip37_drafts.h - NIP-37 Draft Events utility library
 *
 * NIP-37 defines kind 31234 for storing draft (unpublished) events.
 * The draft event content contains the full event JSON that would be
 * published once finalized.
 *
 * Tags:
 *   - ["d", "<unique-draft-id>"] - unique identifier for this draft
 *   - ["k", "<target-kind>"] - the kind of the draft event
 *   - ["e", "<event-id>"] - reference to event being edited (optional)
 *   - ["a", "<kind:pubkey:d-tag>"] - reference to addressable event being edited (optional)
 */

#ifndef NIP37_DRAFTS_H
#define NIP37_DRAFTS_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * NIP-37 Draft event kind (parameterized replaceable)
 */
#define NIP37_KIND_DRAFT 31234

/**
 * GnostrNip37Draft:
 *
 * Represents parsed metadata from a NIP-37 draft event.
 * The draft_json field contains the inner event that would be published.
 */
typedef struct _GnostrNip37Draft {
  gchar   *draft_id;        /* "d" tag value - unique draft identifier */
  gint     target_kind;     /* "k" tag value - kind of the draft event (0 if not set) */
  gchar   *draft_json;      /* Content field - the inner draft event JSON */
  gchar   *edit_event_id;   /* "e" tag value - event being edited (nullable) */
  gchar   *edit_addr;       /* "a" tag value - addressable event being edited (nullable) */
  gint64   created_at;      /* Event created_at timestamp */
} GnostrNip37Draft;

/**
 * gnostr_nip37_draft_new:
 *
 * Creates a new empty NIP-37 draft structure.
 *
 * Returns: (transfer full): A new #GnostrNip37Draft. Free with gnostr_nip37_draft_free().
 */
GnostrNip37Draft *gnostr_nip37_draft_new(void);

/**
 * gnostr_nip37_draft_free:
 * @draft: The draft to free.
 *
 * Frees a draft structure and all its contents.
 */
void gnostr_nip37_draft_free(GnostrNip37Draft *draft);

/**
 * gnostr_nip37_draft_parse:
 * @event_json: JSON string of a kind 31234 draft event.
 *
 * Parses a NIP-37 draft event and extracts its metadata.
 * The draft_json field will contain the content (the inner event JSON).
 *
 * Returns: (transfer full): Parsed draft or NULL on error.
 */
GnostrNip37Draft *gnostr_nip37_draft_parse(const gchar *event_json);

/**
 * gnostr_nip37_draft_build_tags:
 * @draft: The draft to build tags for.
 *
 * Builds the tags array JSON for a NIP-37 draft event.
 * Includes "d", "k", and optionally "e" or "a" tags based on draft fields.
 *
 * Returns: (transfer full): JSON string of the tags array (caller must free with g_free).
 */
gchar *gnostr_nip37_draft_build_tags(const GnostrNip37Draft *draft);

/**
 * gnostr_nip37_draft_get_content:
 * @draft: The draft.
 *
 * Gets the content for a NIP-37 draft event (the inner event JSON).
 * This is the draft_json field value.
 *
 * Returns: (transfer none): The draft content JSON or NULL if not set.
 */
const gchar *gnostr_nip37_draft_get_content(const GnostrNip37Draft *draft);

/**
 * gnostr_nip37_draft_set_content:
 * @draft: The draft.
 * @content_json: The inner event JSON to set as content.
 *
 * Sets the content (inner event JSON) for a draft.
 * Also attempts to extract the target_kind from the inner event.
 */
void gnostr_nip37_draft_set_content(GnostrNip37Draft *draft, const gchar *content_json);

/**
 * gnostr_nip37_draft_get_target_kind:
 * @draft: The draft.
 *
 * Gets the target kind of the draft event.
 * If the "k" tag was set, returns that value.
 * Otherwise, attempts to extract kind from the inner event JSON.
 *
 * Returns: The target event kind, or 0 if unknown.
 */
gint gnostr_nip37_draft_get_target_kind(const GnostrNip37Draft *draft);

/**
 * gnostr_nip37_draft_generate_id:
 *
 * Generates a unique draft identifier suitable for the "d" tag.
 * Format: "draft-<timestamp>-<random>"
 *
 * Returns: (transfer full): A new unique draft ID string (caller must free with g_free).
 */
gchar *gnostr_nip37_draft_generate_id(void);

/**
 * gnostr_nip37_is_draft_event:
 * @event_json: JSON string of an event.
 *
 * Checks if an event is a NIP-37 draft event (kind 31234).
 *
 * Returns: TRUE if the event is a draft event, FALSE otherwise.
 */
gboolean gnostr_nip37_is_draft_event(const gchar *event_json);

G_END_DECLS

#endif /* NIP37_DRAFTS_H */
