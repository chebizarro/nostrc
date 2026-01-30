#ifndef NOSTR_JSON_H
#define NOSTR_JSON_H

#include "nostr-envelope.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

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

/* =========================================================================
 * In-memory JSON object manipulation helpers (backend-agnostic)
 * These provide abstraction over direct jansson calls for extra fields etc.
 * ========================================================================= */

/* Opaque handle representing an in-memory JSON value */
typedef void *NostrJsonValue;

/* Create a new empty JSON object. Returns NULL on failure.
 * Caller owns the returned handle and must free via nostr_json_value_free(). */
NostrJsonValue nostr_json_object_new(void);

/* Free a JSON value handle (decrements refcount / deallocates). */
void nostr_json_value_free(NostrJsonValue val);

/* Increment reference count on a JSON value. Returns the same handle. */
NostrJsonValue nostr_json_value_incref(NostrJsonValue val);

/* Set a key in an object to a given value. The object takes ownership of val.
 * Returns 0 on success, -1 on error. */
int nostr_json_object_set(NostrJsonValue obj, const char *key, NostrJsonValue val);

/* Get a value from an object by key. Returns NULL if not found.
 * The returned value is borrowed (not a new reference). */
NostrJsonValue nostr_json_object_get(NostrJsonValue obj, const char *key);

/* Delete a key from an object. Returns 0 on success, -1 on error. */
int nostr_json_object_del(NostrJsonValue obj, const char *key);

/* Type checking */
bool nostr_json_value_is_string(NostrJsonValue val);
bool nostr_json_value_is_number(NostrJsonValue val);
bool nostr_json_value_is_integer(NostrJsonValue val);
bool nostr_json_value_is_boolean(NostrJsonValue val);

/* Value extraction */
const char *nostr_json_value_string(NostrJsonValue val);
double nostr_json_value_number(NostrJsonValue val);
int64_t nostr_json_value_integer(NostrJsonValue val);
bool nostr_json_value_boolean(NostrJsonValue val);

/* ===========================================================================
 * Extended JSON Interface (nostrc-3nj)
 *
 * Additional helpers to eliminate direct backend (jansson/json-glib) usage.
 * These provide: iteration, building, type checking, 64-bit integers, doubles,
 * deep path access, and key enumeration.
 * ===========================================================================
 */

/* ---- 64-bit Integer and Double Getters ---- */

/* Get 64-bit integer at top-level key. Returns 0 on success, -1 on error. */
int nostr_json_get_int64(const char *json,
                         const char *entry_key,
                         int64_t *out_val);

/* Get 64-bit integer at nested path: json[object_key][entry_key]. */
int nostr_json_get_int64_at(const char *json,
                            const char *object_key,
                            const char *entry_key,
                            int64_t *out_val);

/* Get double-precision float at top-level key. Returns 0 on success, -1 on error. */
int nostr_json_get_double(const char *json,
                          const char *entry_key,
                          double *out_val);

/* Get double at nested path: json[object_key][entry_key]. */
int nostr_json_get_double_at(const char *json,
                             const char *object_key,
                             const char *entry_key,
                             double *out_val);

/* ---- Key Existence and Type Checking ---- */

/* JSON value type enumeration (matches common JSON types). */
typedef enum {
    NOSTR_JSON_NULL = 0,
    NOSTR_JSON_BOOL,
    NOSTR_JSON_INTEGER,
    NOSTR_JSON_REAL,      /* double/float */
    NOSTR_JSON_STRING,
    NOSTR_JSON_ARRAY,
    NOSTR_JSON_OBJECT,
    NOSTR_JSON_INVALID = -1  /* parse error or key not found */
} NostrJsonType;

/* Check if a key exists at top level. Returns true if key exists, false otherwise. */
bool nostr_json_has_key(const char *json, const char *key);

/* Check if a key exists within a nested object: json[object_key][key]. */
bool nostr_json_has_key_at(const char *json, const char *object_key, const char *key);

/* Get the type of a value at top-level key. Returns NOSTR_JSON_INVALID on error. */
NostrJsonType nostr_json_get_type(const char *json, const char *key);

/* Get the type of a value at nested path: json[object_key][key]. */
NostrJsonType nostr_json_get_type_at(const char *json, const char *object_key, const char *key);

/* ---- Deep Path Access ----
 * Access deeply nested values using dot-notation paths, e.g., "limitation.max_message_length".
 * Array indexing is also supported: "items.0.name" accesses items[0].name.
 */

/* Get string at deep path. Path uses dot-notation. */
int nostr_json_get_string_path(const char *json, const char *path, char **out_str);

/* Get int at deep path. */
int nostr_json_get_int_path(const char *json, const char *path, int *out_val);

/* Get int64 at deep path. */
int nostr_json_get_int64_path(const char *json, const char *path, int64_t *out_val);

/* Get double at deep path. */
int nostr_json_get_double_path(const char *json, const char *path, double *out_val);

/* Get bool at deep path. */
int nostr_json_get_bool_path(const char *json, const char *path, bool *out_val);

/* Get raw JSON (sub-document) at deep path. Caller must free result. */
int nostr_json_get_raw_path(const char *json, const char *path, char **out_raw);

/* ---- Array Helpers ---- */

/* Get length of array at top-level key. Returns 0 on success, -1 if not an array. */
int nostr_json_get_array_length(const char *json, const char *key, size_t *out_len);

/* Get string element from array at index. */
int nostr_json_get_array_string(const char *json, const char *key, size_t index, char **out_str);

/* Get int element from array at index. */
int nostr_json_get_array_int(const char *json, const char *key, size_t index, int *out_val);

/* Get int64 element from array at index. */
int nostr_json_get_array_int64(const char *json, const char *key, size_t index, int64_t *out_val);

/* ---- Object Key Enumeration ----
 * Enumerate all keys in a JSON object. Useful for dynamic schemas.
 */

/* Get all keys from top-level object.
 * Returns 0 on success, -1 on error. Caller must free each key and the array. */
int nostr_json_get_object_keys(const char *json,
                               char ***out_keys,
                               size_t *out_count);

/* Get all keys from nested object at object_key. */
int nostr_json_get_object_keys_at(const char *json,
                                  const char *object_key,
                                  char ***out_keys,
                                  size_t *out_count);

/* ---- Object Iteration (Callback-based) ----
 * Iterate over object key-value pairs without exposing backend types.
 * The callback receives key and the raw JSON value as a string.
 */

/* Callback type for object iteration.
 * Parameters: key, value_json (serialized value), user_data.
 * Return false to stop iteration early, true to continue. */
typedef bool (*NostrJsonObjectIterCb)(const char *key,
                                      const char *value_json,
                                      void *user_data);

/* Iterate over all key-value pairs in top-level object. */
int nostr_json_object_foreach(const char *json,
                              NostrJsonObjectIterCb callback,
                              void *user_data);

/* Iterate over all key-value pairs in nested object at object_key. */
int nostr_json_object_foreach_at(const char *json,
                                 const char *object_key,
                                 NostrJsonObjectIterCb callback,
                                 void *user_data);

/* ---- Array Iteration (Callback-based) ---- */

/* Callback type for array iteration.
 * Parameters: index, element_json (serialized element), user_data.
 * Return false to stop iteration early, true to continue. */
typedef bool (*NostrJsonArrayIterCb)(size_t index,
                                     const char *element_json,
                                     void *user_data);

/* Iterate over all elements in top-level array at key. */
int nostr_json_array_foreach(const char *json,
                             const char *key,
                             NostrJsonArrayIterCb callback,
                             void *user_data);

/* Iterate over top-level array (when JSON root is an array). */
int nostr_json_array_foreach_root(const char *json,
                                  NostrJsonArrayIterCb callback,
                                  void *user_data);

/* ---- JSON Building ----
 * Construct JSON documents programmatically without backend exposure.
 * Builder is opaque; use nostr_json_builder_* functions.
 */

typedef struct NostrJsonBuilder NostrJsonBuilder;

/* Create a new JSON builder. Caller must free with nostr_json_builder_free(). */
NostrJsonBuilder *nostr_json_builder_new(void);

/* Free the builder. */
void nostr_json_builder_free(NostrJsonBuilder *builder);

/* Begin an object. Must be matched with nostr_json_builder_end_object(). */
int nostr_json_builder_begin_object(NostrJsonBuilder *builder);

/* End the current object. */
int nostr_json_builder_end_object(NostrJsonBuilder *builder);

/* Begin an array. Must be matched with nostr_json_builder_end_array(). */
int nostr_json_builder_begin_array(NostrJsonBuilder *builder);

/* End the current array. */
int nostr_json_builder_end_array(NostrJsonBuilder *builder);

/* Set the key for the next value (when inside an object). */
int nostr_json_builder_set_key(NostrJsonBuilder *builder, const char *key);

/* Add a string value. */
int nostr_json_builder_add_string(NostrJsonBuilder *builder, const char *value);

/* Add an integer value. */
int nostr_json_builder_add_int(NostrJsonBuilder *builder, int value);

/* Add a 64-bit integer value. */
int nostr_json_builder_add_int64(NostrJsonBuilder *builder, int64_t value);

/* Add a double value. */
int nostr_json_builder_add_double(NostrJsonBuilder *builder, double value);

/* Add a boolean value. */
int nostr_json_builder_add_bool(NostrJsonBuilder *builder, bool value);

/* Add a null value. */
int nostr_json_builder_add_null(NostrJsonBuilder *builder);

/* Add raw JSON (must be valid JSON). */
int nostr_json_builder_add_raw(NostrJsonBuilder *builder, const char *raw_json);

/* Finalize and get the resulting JSON string. Caller must free the result.
 * This also resets the builder for reuse. */
char *nostr_json_builder_finish(NostrJsonBuilder *builder);

/* ---- Convenience Builders ----
 * One-shot helpers for common patterns.
 */

/* Build a simple object from key-value pairs (NULL-terminated).
 * Example: nostr_json_build_object("name", "Alice", "age", "30", NULL);
 * All values are treated as strings. Returns newly allocated JSON string. */
char *nostr_json_build_object(const char *key, ...);

/* Build a simple string array from values (NULL-terminated).
 * Example: nostr_json_build_string_array("a", "b", "c", NULL);
 * Returns newly allocated JSON string. */
char *nostr_json_build_string_array(const char *first, ...);

/* Build an int array from values.
 * Returns newly allocated JSON string. */
char *nostr_json_build_int_array(const int *values, size_t count);

/* ---- Validation ---- */

/* Check if a string is valid JSON. Returns true if valid, false otherwise. */
bool nostr_json_is_valid(const char *json);

/* Check if JSON represents an object. */
bool nostr_json_is_object_str(const char *json);

/* Check if JSON represents an array. */
bool nostr_json_is_array_str(const char *json);

/* ---- Transformation ---- */

/* Pretty-print JSON with indentation. Caller must free result. */
char *nostr_json_prettify(const char *json);

/* Compact JSON (remove whitespace). Caller must free result. */
char *nostr_json_compact(const char *json);

/* Merge two JSON objects. Keys in 'overlay' override keys in 'base'.
 * Both must be objects. Caller must free result. Returns NULL on error. */
char *nostr_json_merge_objects(const char *base, const char *overlay);

#endif // NOSTR_JSON_H
