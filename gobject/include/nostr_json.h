#ifndef GNOSTR_JSON_H
#define GNOSTR_JSON_H

#include <glib-object.h>
#include "nostr-error.h"

G_BEGIN_DECLS

/* =========================================================================
 * GNostrJsonBuilder
 *
 * GObject wrapper around NostrJsonBuilder for constructing JSON documents
 * programmatically. Provides a stack-based builder pattern with GError
 * support and GIR annotations for language bindings.
 *
 * ## Example
 *
 * |[<!-- language="C" -->
 * g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new ();
 * gnostr_json_builder_begin_object (builder);
 * gnostr_json_builder_set_key (builder, "name");
 * gnostr_json_builder_add_string (builder, "Alice");
 * gnostr_json_builder_set_key (builder, "kind");
 * gnostr_json_builder_add_int (builder, 1);
 * gnostr_json_builder_end_object (builder);
 * g_autofree gchar *json = gnostr_json_builder_finish (builder);
 * ]|
 *
 * Since: 0.1
 * ========================================================================= */

#define GNOSTR_TYPE_JSON_BUILDER (gnostr_json_builder_get_type())
G_DECLARE_FINAL_TYPE(GNostrJsonBuilder, gnostr_json_builder, GNOSTR, JSON_BUILDER, GObject)

/**
 * gnostr_json_builder_new:
 *
 * Creates a new JSON builder.
 *
 * Returns: (transfer full): a new #GNostrJsonBuilder
 */
GNostrJsonBuilder *gnostr_json_builder_new(void);

/**
 * gnostr_json_builder_begin_object:
 * @self: a #GNostrJsonBuilder
 *
 * Begins a JSON object. Must be matched with gnostr_json_builder_end_object().
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_json_builder_begin_object(GNostrJsonBuilder *self);

/**
 * gnostr_json_builder_end_object:
 * @self: a #GNostrJsonBuilder
 *
 * Ends the current JSON object.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_json_builder_end_object(GNostrJsonBuilder *self);

/**
 * gnostr_json_builder_begin_array:
 * @self: a #GNostrJsonBuilder
 *
 * Begins a JSON array. Must be matched with gnostr_json_builder_end_array().
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_json_builder_begin_array(GNostrJsonBuilder *self);

/**
 * gnostr_json_builder_end_array:
 * @self: a #GNostrJsonBuilder
 *
 * Ends the current JSON array.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_json_builder_end_array(GNostrJsonBuilder *self);

/**
 * gnostr_json_builder_set_key:
 * @self: a #GNostrJsonBuilder
 * @key: the key name
 *
 * Sets the key for the next value when inside an object.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_json_builder_set_key(GNostrJsonBuilder *self, const gchar *key);

/**
 * gnostr_json_builder_add_string:
 * @self: a #GNostrJsonBuilder
 * @value: (nullable): a string value
 *
 * Adds a string value.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_json_builder_add_string(GNostrJsonBuilder *self, const gchar *value);

/**
 * gnostr_json_builder_add_int:
 * @self: a #GNostrJsonBuilder
 * @value: an integer value
 *
 * Adds an integer value.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_json_builder_add_int(GNostrJsonBuilder *self, gint value);

/**
 * gnostr_json_builder_add_int64:
 * @self: a #GNostrJsonBuilder
 * @value: a 64-bit integer value
 *
 * Adds a 64-bit integer value.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_json_builder_add_int64(GNostrJsonBuilder *self, gint64 value);

/**
 * gnostr_json_builder_add_double:
 * @self: a #GNostrJsonBuilder
 * @value: a double value
 *
 * Adds a floating-point value.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_json_builder_add_double(GNostrJsonBuilder *self, gdouble value);

/**
 * gnostr_json_builder_add_boolean:
 * @self: a #GNostrJsonBuilder
 * @value: a boolean value
 *
 * Adds a boolean value.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_json_builder_add_boolean(GNostrJsonBuilder *self, gboolean value);

/**
 * gnostr_json_builder_add_null:
 * @self: a #GNostrJsonBuilder
 *
 * Adds a JSON null value.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_json_builder_add_null(GNostrJsonBuilder *self);

/**
 * gnostr_json_builder_add_raw:
 * @self: a #GNostrJsonBuilder
 * @raw_json: a valid JSON fragment
 *
 * Adds a raw JSON fragment. The string must be valid JSON.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_json_builder_add_raw(GNostrJsonBuilder *self, const gchar *raw_json);

/**
 * gnostr_json_builder_finish:
 * @self: a #GNostrJsonBuilder
 *
 * Finalizes the builder and returns the resulting JSON string.
 * The builder is reset and can be reused afterwards.
 *
 * Returns: (transfer full) (nullable): a newly allocated JSON string, or %NULL on error
 */
gchar *gnostr_json_builder_finish(GNostrJsonBuilder *self);

/* =========================================================================
 * JSON Parsing Utilities
 *
 * GError-based wrappers around the libnostr JSON parsing functions.
 * These provide GObject-idiomatic error handling for extracting values
 * from JSON strings.
 * ========================================================================= */

/**
 * gnostr_json_get_string:
 * @json: a JSON string
 * @key: the top-level key to extract
 * @error: (nullable): return location for a #GError
 *
 * Extracts a string value from a top-level JSON key.
 *
 * Returns: (transfer full) (nullable): the string value, or %NULL on error
 */
gchar *gnostr_json_get_string(const gchar *json, const gchar *key, GError **error);

/**
 * gnostr_json_get_int:
 * @json: a JSON string
 * @key: the top-level key to extract
 * @error: (nullable): return location for a #GError
 *
 * Extracts an integer value from a top-level JSON key.
 * Check @error to distinguish a real 0 from an error return.
 *
 * Returns: the integer value, or 0 on error
 */
gint gnostr_json_get_int(const gchar *json, const gchar *key, GError **error);

/**
 * gnostr_json_get_int64:
 * @json: a JSON string
 * @key: the top-level key to extract
 * @error: (nullable): return location for a #GError
 *
 * Extracts a 64-bit integer value from a top-level JSON key.
 *
 * Returns: the int64 value, or 0 on error
 */
gint64 gnostr_json_get_int64(const gchar *json, const gchar *key, GError **error);

/**
 * gnostr_json_get_double:
 * @json: a JSON string
 * @key: the top-level key to extract
 * @error: (nullable): return location for a #GError
 *
 * Extracts a double value from a top-level JSON key.
 *
 * Returns: the double value, or 0.0 on error
 */
gdouble gnostr_json_get_double(const gchar *json, const gchar *key, GError **error);

/**
 * gnostr_json_get_boolean:
 * @json: a JSON string
 * @key: the top-level key to extract
 * @error: (nullable): return location for a #GError
 *
 * Extracts a boolean value from a top-level JSON key.
 * Check @error to distinguish a real %FALSE from an error return.
 *
 * Returns: the boolean value, or %FALSE on error
 */
gboolean gnostr_json_get_boolean(const gchar *json, const gchar *key, GError **error);

/**
 * gnostr_json_get_string_array:
 * @json: a JSON string
 * @key: the top-level key to extract
 * @error: (nullable): return location for a #GError
 *
 * Extracts an array of strings from a top-level JSON key.
 *
 * Returns: (transfer full) (nullable): a newly allocated %NULL-terminated
 *   string array, or %NULL on error. Free with g_strfreev().
 */
GStrv gnostr_json_get_string_array(const gchar *json, const gchar *key, GError **error);

/**
 * gnostr_json_get_raw:
 * @json: a JSON string
 * @key: the top-level key to extract
 * @error: (nullable): return location for a #GError
 *
 * Extracts a raw JSON fragment from a top-level key. The returned
 * string is a compact JSON representation of the value (quoted for
 * strings, etc.).
 *
 * Returns: (transfer full) (nullable): the raw JSON fragment, or %NULL on error
 */
gchar *gnostr_json_get_raw(const gchar *json, const gchar *key, GError **error);

/* ---- Deep Path Access ---- */

/**
 * gnostr_json_get_string_path:
 * @json: a JSON string
 * @path: dot-notation path (e.g., "limitation.max_message_length")
 * @error: (nullable): return location for a #GError
 *
 * Extracts a string at a deep dot-notation path. Array indexing is
 * supported: "items.0.name" accesses items[0].name.
 *
 * Returns: (transfer full) (nullable): the string value, or %NULL on error
 */
gchar *gnostr_json_get_string_path(const gchar *json, const gchar *path, GError **error);

/**
 * gnostr_json_get_int_path:
 * @json: a JSON string
 * @path: dot-notation path
 * @error: (nullable): return location for a #GError
 *
 * Returns: the integer value, or 0 on error
 */
gint gnostr_json_get_int_path(const gchar *json, const gchar *path, GError **error);

/**
 * gnostr_json_get_int64_path:
 * @json: a JSON string
 * @path: dot-notation path
 * @error: (nullable): return location for a #GError
 *
 * Returns: the int64 value, or 0 on error
 */
gint64 gnostr_json_get_int64_path(const gchar *json, const gchar *path, GError **error);

/**
 * gnostr_json_get_double_path:
 * @json: a JSON string
 * @path: dot-notation path
 * @error: (nullable): return location for a #GError
 *
 * Returns: the double value, or 0.0 on error
 */
gdouble gnostr_json_get_double_path(const gchar *json, const gchar *path, GError **error);

/**
 * gnostr_json_get_boolean_path:
 * @json: a JSON string
 * @path: dot-notation path
 * @error: (nullable): return location for a #GError
 *
 * Returns: the boolean value, or %FALSE on error
 */
gboolean gnostr_json_get_boolean_path(const gchar *json, const gchar *path, GError **error);

/**
 * gnostr_json_get_raw_path:
 * @json: a JSON string
 * @path: dot-notation path
 * @error: (nullable): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the raw JSON fragment, or %NULL on error
 */
gchar *gnostr_json_get_raw_path(const gchar *json, const gchar *path, GError **error);

/* ---- Validation & Transformation ---- */

/**
 * gnostr_json_is_valid:
 * @json: a string to validate
 *
 * Checks whether a string is valid JSON.
 *
 * Returns: %TRUE if the string is valid JSON
 */
gboolean gnostr_json_is_valid(const gchar *json);

/**
 * gnostr_json_prettify:
 * @json: a JSON string
 * @error: (nullable): return location for a #GError
 *
 * Pretty-prints JSON with indentation.
 *
 * Returns: (transfer full) (nullable): a newly allocated pretty-printed string,
 *   or %NULL on error
 */
gchar *gnostr_json_prettify(const gchar *json, GError **error);

/**
 * gnostr_json_compact_string:
 * @json: a JSON string
 * @error: (nullable): return location for a #GError
 *
 * Compacts JSON by removing whitespace.
 *
 * Returns: (transfer full) (nullable): a newly allocated compact string,
 *   or %NULL on error
 */
gchar *gnostr_json_compact_string(const gchar *json, GError **error);

/**
 * gnostr_json_merge:
 * @base: a JSON object string
 * @overlay: a JSON object string whose keys override @base
 * @error: (nullable): return location for a #GError
 *
 * Merges two JSON objects. Keys in @overlay override those in @base.
 *
 * Returns: (transfer full) (nullable): a newly allocated merged JSON string,
 *   or %NULL on error
 */
gchar *gnostr_json_merge(const gchar *base, const gchar *overlay, GError **error);

G_END_DECLS

#endif /* GNOSTR_JSON_H */
