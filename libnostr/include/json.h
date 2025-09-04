#ifndef NOSTR_JSON_H
#define NOSTR_JSON_H

#include "nostr-envelope.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include <stdbool.h>

// Structure to hold JSON function pointers
typedef struct _NostrJsonInterface {
    void (*init)(void);
    void (*cleanup)(void);
    char *(*serialize_event)(const NostrEvent *event);
    int (*deserialize_event)(NostrEvent *event, const char *json_str);
    char *(*serialize_envelope)(const NostrEnvelope *envelope);
    int (*deserialize_envelope)(NostrEnvelope *envelope, const char *json_str);
    char *(*serialize_filter)(const NostrFilter *filter);
    int (*deserialize_filter)(NostrFilter *filter, const char *json_str);

} NostrJsonInterface;

extern NostrJsonInterface *json_interface;
void nostr_set_json_interface(NostrJsonInterface *interface);
void nostr_json_init(void);
void nostr_json_cleanup(void);
/* When enabled, compact inline parsers are bypassed and all (de)serialization
 * goes through the configured json_interface backend instead. Useful to avoid
 * buggy fast paths or to debug inconsistencies. */
void nostr_json_force_fallback(bool enable);
char *nostr_event_serialize(const NostrEvent *event);
int nostr_event_deserialize(NostrEvent *event, const char *json);
char *nostr_envelope_serialize(const NostrEnvelope *envelope);
int nostr_envelope_deserialize(NostrEnvelope *envelope, const char *json);
char *nostr_filter_serialize(const NostrFilter *filter);
int nostr_filter_deserialize(NostrFilter *filter, const char *json);

/* Generic helpers (backend-agnostic) for simple nested lookups.
 * These parse the input JSON per call and return newly allocated results. */
/* Get string at top_object[entry_key] where top_object is at object_key. */
int nostr_json_get_string_at(const char *json,
                             const char *object_key,
                             const char *entry_key,
                             char **out_str);

/* Get array of strings at top_object[entry_key] where top_object is at object_key. */
int nostr_json_get_string_array_at(const char *json,
                                   const char *object_key,
                                   const char *entry_key,
                                   char ***out_array,
                                   size_t *out_count);

/* Convenience top-level getters (no object_key) */
int nostr_json_get_string(const char *json,
                          const char *entry_key,
                          char **out_str);
int nostr_json_get_string_array(const char *json,
                                const char *entry_key,
                                char ***out_array,
                                size_t *out_count);

/* Integer and boolean getters */
int nostr_json_get_int(const char *json,
                       const char *entry_key,
                       int *out_val);
int nostr_json_get_bool(const char *json,
                        const char *entry_key,
                        bool *out_val);

/* Get raw JSON (compact string) at top-level entry_key.
 * Returns 0 on success and sets *out_raw to a newly-allocated string representing
 * the JSON value at entry_key (object, array, string with quotes, number, etc.).
 * Caller must free *out_raw. Returns -1 if key is missing or parse fails. */
int nostr_json_get_raw(const char *json,
                       const char *entry_key,
                       char **out_raw);
/* Parse a top-level JSON array of numbers into an owned int buffer.
 * Semantics:
 *  - Every element must be numeric (integer or real); reals are truncated to int.
 *  - Returns 0 on success, -1 on error (e.g., non-numeric element).
 *  - On success, '*out_items' is non-NULL and must be freed by the caller, even when
 *    '*out_count == 0' (empty array yields a non-NULL buffer for convenience).
 */
int nostr_json_get_int_array(const char *json,
                             const char *entry_key,
                             int **out_items,
                             size_t *out_count);

/* Nested variants under object_key */
int nostr_json_get_int_at(const char *json,
                          const char *object_key,
                          const char *entry_key,
                          int *out_val);
int nostr_json_get_bool_at(const char *json,
                           const char *object_key,
                           const char *entry_key,
                           bool *out_val);
/* Nested variant under 'object_key'. Same semantics as nostr_json_get_int_array(). */
int nostr_json_get_int_array_at(const char *json,
                                const char *object_key,
                                const char *entry_key,
                                int **out_items,
                                size_t *out_count);

/* Array-of-objects helpers (for structures like fees.* arrays) */
int nostr_json_get_array_length_at(const char *json,
                                   const char *object_key,
                                   const char *entry_key,
                                   size_t *out_len);
int nostr_json_get_int_in_object_array_at(const char *json,
                                          const char *object_key,
                                          const char *entry_key,
                                          size_t index,
                                          const char *field_key,
                                          int *out_val);
int nostr_json_get_string_in_object_array_at(const char *json,
                                             const char *object_key,
                                             const char *entry_key,
                                             size_t index,
                                             const char *field_key,
                                             char **out_str);
int nostr_json_get_int_array_in_object_array_at(const char *json,
                                                const char *object_key,
                                                const char *entry_key,
                                                size_t index,
                                                const char *field_key,
                                                int **out_items,
                                                size_t *out_count);

#endif // NOSTR_JSON_H
