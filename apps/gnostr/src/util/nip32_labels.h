/**
 * NIP-32 Labeling Support for gnostr
 *
 * Implements kind 1985 label events for categorizing/tagging content.
 * - "L" tag = label namespace (e.g., "ugc", "social.coracle.ontology")
 * - "l" tag = label value within namespace
 * - "e" or "p" tags reference the labeled event/pubkey
 */

#ifndef GNOSTR_NIP32_LABELS_H
#define GNOSTR_NIP32_LABELS_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Label event kind per NIP-32 */
#define NOSTR_KIND_LABEL 1985

/* Common label namespaces */
#define NIP32_NS_UGC "ugc"                          /* User-generated content */
#define NIP32_NS_REVIEW "review"                    /* Content review/moderation */
#define NIP32_NS_ISO639 "ISO-639-1"                 /* Language codes */
#define NIP32_NS_QUALITY "quality"                  /* Content quality */

/* Structure representing a single label */
typedef struct {
  char *namespace;    /* L tag value - the namespace */
  char *label;        /* l tag value - the actual label */
  char *event_id_hex; /* The event this label references (if any) */
  char *pubkey_hex;   /* The pubkey this label references (if any) */
  char *label_author; /* Pubkey of who created the label */
  gint64 created_at;  /* When the label was created */
} GnostrLabel;

/* Structure representing labels for a specific event */
typedef struct {
  char *event_id_hex;  /* The event being labeled */
  GPtrArray *labels;   /* Array of GnostrLabel* */
} GnostrEventLabels;

/* Free a GnostrLabel structure */
void gnostr_label_free(gpointer label);

/* Free a GnostrEventLabels structure */
void gnostr_event_labels_free(gpointer event_labels);

/* Parse a kind 1985 event JSON into labels.
 * Returns a newly allocated GPtrArray of GnostrLabel*, or NULL on error.
 * The caller must free the result with g_ptr_array_unref(). */
GPtrArray *gnostr_nip32_parse_label_event(const char *event_json);

/* Query local NostrDB for labels on a specific event.
 * Returns a GnostrEventLabels* or NULL if no labels found.
 * The caller must free the result with gnostr_event_labels_free(). */
GnostrEventLabels *gnostr_nip32_get_labels_for_event(const char *event_id_hex);

/* Query local NostrDB for all label events by a specific user.
 * Returns a GPtrArray of GnostrLabel*, or NULL if none found.
 * The caller must free the result with g_ptr_array_unref(). */
GPtrArray *gnostr_nip32_get_labels_by_user(const char *pubkey_hex);

/* Build unsigned kind 1985 event JSON for labeling an event.
 * namespace: the L tag value (e.g., "ugc")
 * label: the l tag value (e.g., "topic:bitcoin")
 * event_id_hex: the event to label
 * event_pubkey_hex: pubkey of the event author
 * Returns newly allocated JSON string or NULL on error. Caller must g_free(). */
char *gnostr_nip32_build_label_event_json(const char *namespace,
                                           const char *label,
                                           const char *event_id_hex,
                                           const char *event_pubkey_hex);

/* Build unsigned kind 1985 event JSON for labeling a pubkey (profile).
 * namespace: the L tag value
 * label: the l tag value
 * pubkey_hex: the pubkey to label
 * Returns newly allocated JSON string or NULL on error. Caller must g_free(). */
char *gnostr_nip32_build_profile_label_event_json(const char *namespace,
                                                   const char *label,
                                                   const char *pubkey_hex);

/* Subscribe to label events for a set of event IDs.
 * Returns subscription ID or 0 on failure. */
uint64_t gnostr_nip32_subscribe_labels(const char **event_ids, size_t count);

/* Format a label for display (e.g., "bitcoin" or "ugc:good").
 * Returns newly allocated string. Caller must g_free(). */
char *gnostr_nip32_format_label(const GnostrLabel *label);

/* Common labels for quick access in UI */
typedef struct {
  const char *namespace;
  const char *label;
  const char *display_name;
} GnostrPredefinedLabel;

/* Get array of predefined labels for the "Add Label" dialog.
 * Returns a NULL-terminated array of GnostrPredefinedLabel. */
const GnostrPredefinedLabel *gnostr_nip32_get_predefined_labels(void);

G_END_DECLS

#endif /* GNOSTR_NIP32_LABELS_H */
