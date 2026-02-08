/**
 * SPDX-License-Identifier: MIT
 *
 * GNostrJsonBuilder and JSON utility functions.
 *
 * Provides a GObject wrapper around NostrJsonBuilder and GError-based
 * wrappers around the libnostr JSON parsing/utility functions.
 */

#include "nostr_json.h"
#include <glib.h>
#include <stdarg.h>

/* Core libnostr JSON API */
#include "json.h"

/* =========================================================================
 * GNostrJsonBuilder
 * ========================================================================= */

struct _GNostrJsonBuilder {
    GObject parent_instance;
    NostrJsonBuilder *builder;
};

G_DEFINE_TYPE(GNostrJsonBuilder, gnostr_json_builder, G_TYPE_OBJECT)

static void
gnostr_json_builder_finalize(GObject *object)
{
    GNostrJsonBuilder *self = GNOSTR_JSON_BUILDER(object);

    if (self->builder) {
        nostr_json_builder_free(self->builder);
        self->builder = NULL;
    }

    G_OBJECT_CLASS(gnostr_json_builder_parent_class)->finalize(object);
}

static void
gnostr_json_builder_class_init(GNostrJsonBuilderClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = gnostr_json_builder_finalize;
}

static void
gnostr_json_builder_init(GNostrJsonBuilder *self)
{
    self->builder = nostr_json_builder_new();
}

GNostrJsonBuilder *
gnostr_json_builder_new(void)
{
    return g_object_new(GNOSTR_TYPE_JSON_BUILDER, NULL);
}

gboolean
gnostr_json_builder_begin_object(GNostrJsonBuilder *self)
{
    g_return_val_if_fail(GNOSTR_IS_JSON_BUILDER(self), FALSE);
    g_return_val_if_fail(self->builder != NULL, FALSE);
    return nostr_json_builder_begin_object(self->builder) == 0;
}

gboolean
gnostr_json_builder_end_object(GNostrJsonBuilder *self)
{
    g_return_val_if_fail(GNOSTR_IS_JSON_BUILDER(self), FALSE);
    g_return_val_if_fail(self->builder != NULL, FALSE);
    return nostr_json_builder_end_object(self->builder) == 0;
}

gboolean
gnostr_json_builder_begin_array(GNostrJsonBuilder *self)
{
    g_return_val_if_fail(GNOSTR_IS_JSON_BUILDER(self), FALSE);
    g_return_val_if_fail(self->builder != NULL, FALSE);
    return nostr_json_builder_begin_array(self->builder) == 0;
}

gboolean
gnostr_json_builder_end_array(GNostrJsonBuilder *self)
{
    g_return_val_if_fail(GNOSTR_IS_JSON_BUILDER(self), FALSE);
    g_return_val_if_fail(self->builder != NULL, FALSE);
    return nostr_json_builder_end_array(self->builder) == 0;
}

gboolean
gnostr_json_builder_set_key(GNostrJsonBuilder *self, const gchar *key)
{
    g_return_val_if_fail(GNOSTR_IS_JSON_BUILDER(self), FALSE);
    g_return_val_if_fail(self->builder != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);
    return nostr_json_builder_set_key(self->builder, key) == 0;
}

gboolean
gnostr_json_builder_add_string(GNostrJsonBuilder *self, const gchar *value)
{
    g_return_val_if_fail(GNOSTR_IS_JSON_BUILDER(self), FALSE);
    g_return_val_if_fail(self->builder != NULL, FALSE);
    return nostr_json_builder_add_string(self->builder, value) == 0;
}

gboolean
gnostr_json_builder_add_int(GNostrJsonBuilder *self, gint value)
{
    g_return_val_if_fail(GNOSTR_IS_JSON_BUILDER(self), FALSE);
    g_return_val_if_fail(self->builder != NULL, FALSE);
    return nostr_json_builder_add_int(self->builder, value) == 0;
}

gboolean
gnostr_json_builder_add_int64(GNostrJsonBuilder *self, gint64 value)
{
    g_return_val_if_fail(GNOSTR_IS_JSON_BUILDER(self), FALSE);
    g_return_val_if_fail(self->builder != NULL, FALSE);
    return nostr_json_builder_add_int64(self->builder, value) == 0;
}

gboolean
gnostr_json_builder_add_double(GNostrJsonBuilder *self, gdouble value)
{
    g_return_val_if_fail(GNOSTR_IS_JSON_BUILDER(self), FALSE);
    g_return_val_if_fail(self->builder != NULL, FALSE);
    return nostr_json_builder_add_double(self->builder, value) == 0;
}

gboolean
gnostr_json_builder_add_boolean(GNostrJsonBuilder *self, gboolean value)
{
    g_return_val_if_fail(GNOSTR_IS_JSON_BUILDER(self), FALSE);
    g_return_val_if_fail(self->builder != NULL, FALSE);
    return nostr_json_builder_add_bool(self->builder, value) == 0;
}

gboolean
gnostr_json_builder_add_null(GNostrJsonBuilder *self)
{
    g_return_val_if_fail(GNOSTR_IS_JSON_BUILDER(self), FALSE);
    g_return_val_if_fail(self->builder != NULL, FALSE);
    return nostr_json_builder_add_null(self->builder) == 0;
}

gboolean
gnostr_json_builder_add_raw(GNostrJsonBuilder *self, const gchar *raw_json)
{
    g_return_val_if_fail(GNOSTR_IS_JSON_BUILDER(self), FALSE);
    g_return_val_if_fail(self->builder != NULL, FALSE);
    g_return_val_if_fail(raw_json != NULL, FALSE);
    return nostr_json_builder_add_raw(self->builder, raw_json) == 0;
}

gchar *
gnostr_json_builder_finish(GNostrJsonBuilder *self)
{
    g_return_val_if_fail(GNOSTR_IS_JSON_BUILDER(self), NULL);
    g_return_val_if_fail(self->builder != NULL, NULL);
    return nostr_json_builder_finish(self->builder);
}

/* =========================================================================
 * JSON Parsing Utilities
 * ========================================================================= */

gchar *
gnostr_json_get_string(const gchar *json, const gchar *key, GError **error)
{
    g_return_val_if_fail(json != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    char *out = NULL;
    int rc = nostr_json_get_string(json, key, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get string for key '%s'", key);
        return NULL;
    }
    return out;
}

gint
gnostr_json_get_int(const gchar *json, const gchar *key, GError **error)
{
    g_return_val_if_fail(json != NULL, 0);
    g_return_val_if_fail(key != NULL, 0);

    int out = 0;
    int rc = nostr_json_get_int(json, key, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get int for key '%s'", key);
        return 0;
    }
    return out;
}

gint64
gnostr_json_get_int64(const gchar *json, const gchar *key, GError **error)
{
    g_return_val_if_fail(json != NULL, 0);
    g_return_val_if_fail(key != NULL, 0);

    int64_t out = 0;
    int rc = nostr_json_get_int64(json, key, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get int64 for key '%s'", key);
        return 0;
    }
    return (gint64)out;
}

gdouble
gnostr_json_get_double(const gchar *json, const gchar *key, GError **error)
{
    g_return_val_if_fail(json != NULL, 0.0);
    g_return_val_if_fail(key != NULL, 0.0);

    double out = 0.0;
    int rc = nostr_json_get_double(json, key, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get double for key '%s'", key);
        return 0.0;
    }
    return out;
}

gboolean
gnostr_json_get_boolean(const gchar *json, const gchar *key, GError **error)
{
    g_return_val_if_fail(json != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    bool out = false;
    int rc = nostr_json_get_bool(json, key, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get boolean for key '%s'", key);
        return FALSE;
    }
    return out ? TRUE : FALSE;
}

GStrv
gnostr_json_get_string_array(const gchar *json, const gchar *key, GError **error)
{
    g_return_val_if_fail(json != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    char **arr = NULL;
    size_t count = 0;
    int rc = nostr_json_get_string_array(json, key, &arr, &count);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get string array for key '%s'", key);
        return NULL;
    }

    /* Convert to NULL-terminated GStrv */
    gchar **result = g_new0(gchar *, count + 1);
    for (size_t i = 0; i < count; i++) {
        result[i] = g_strdup(arr[i]);
        free(arr[i]);
    }
    free(arr);
    return result;
}

gchar *
gnostr_json_get_raw(const gchar *json, const gchar *key, GError **error)
{
    g_return_val_if_fail(json != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    char *out = NULL;
    int rc = nostr_json_get_raw(json, key, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get raw JSON for key '%s'", key);
        return NULL;
    }
    return out;
}

/* ---- Deep Path Access ---- */

gchar *
gnostr_json_get_string_path(const gchar *json, const gchar *path, GError **error)
{
    g_return_val_if_fail(json != NULL, NULL);
    g_return_val_if_fail(path != NULL, NULL);

    char *out = NULL;
    int rc = nostr_json_get_string_path(json, path, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get string at path '%s'", path);
        return NULL;
    }
    return out;
}

gint
gnostr_json_get_int_path(const gchar *json, const gchar *path, GError **error)
{
    g_return_val_if_fail(json != NULL, 0);
    g_return_val_if_fail(path != NULL, 0);

    int out = 0;
    int rc = nostr_json_get_int_path(json, path, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get int at path '%s'", path);
        return 0;
    }
    return out;
}

gint64
gnostr_json_get_int64_path(const gchar *json, const gchar *path, GError **error)
{
    g_return_val_if_fail(json != NULL, 0);
    g_return_val_if_fail(path != NULL, 0);

    int64_t out = 0;
    int rc = nostr_json_get_int64_path(json, path, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get int64 at path '%s'", path);
        return 0;
    }
    return (gint64)out;
}

gdouble
gnostr_json_get_double_path(const gchar *json, const gchar *path, GError **error)
{
    g_return_val_if_fail(json != NULL, 0.0);
    g_return_val_if_fail(path != NULL, 0.0);

    double out = 0.0;
    int rc = nostr_json_get_double_path(json, path, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get double at path '%s'", path);
        return 0.0;
    }
    return out;
}

gboolean
gnostr_json_get_boolean_path(const gchar *json, const gchar *path, GError **error)
{
    g_return_val_if_fail(json != NULL, FALSE);
    g_return_val_if_fail(path != NULL, FALSE);

    bool out = false;
    int rc = nostr_json_get_bool_path(json, path, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get boolean at path '%s'", path);
        return FALSE;
    }
    return out ? TRUE : FALSE;
}

gchar *
gnostr_json_get_raw_path(const gchar *json, const gchar *path, GError **error)
{
    g_return_val_if_fail(json != NULL, NULL);
    g_return_val_if_fail(path != NULL, NULL);

    char *out = NULL;
    int rc = nostr_json_get_raw_path(json, path, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get raw JSON at path '%s'", path);
        return NULL;
    }
    return out;
}

/* ---- Array Validation ---- */

gboolean
gnostr_json_is_array_str(const gchar *json)
{
    if (json == NULL)
        return FALSE;
    return nostr_json_is_array_str(json) ? TRUE : FALSE;
}

gboolean
gnostr_json_is_object_str(const gchar *json)
{
    if (json == NULL)
        return FALSE;
    return nostr_json_is_object_str(json) ? TRUE : FALSE;
}

gboolean
gnostr_json_has_key(const gchar *json, const gchar *key)
{
    if (json == NULL || key == NULL)
        return FALSE;
    return nostr_json_has_key(json, key) ? TRUE : FALSE;
}

/* ---- Array Access ---- */

gint
gnostr_json_get_array_length(const gchar *json, const gchar *key, GError **error)
{
    g_return_val_if_fail(json != NULL, -1);

    size_t len = 0;
    int rc = nostr_json_get_array_length(json, key, &len);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get array length for key '%s'", key ? key : "(root)");
        return -1;
    }
    return (gint)len;
}

gchar *
gnostr_json_get_array_string(const gchar *json, const gchar *key, gint index, GError **error)
{
    g_return_val_if_fail(json != NULL, NULL);
    g_return_val_if_fail(index >= 0, NULL);

    char *out = NULL;
    int rc = nostr_json_get_array_string(json, key, (size_t)index, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get array string at index %d for key '%s'",
                    index, key ? key : "(root)");
        return NULL;
    }
    return out;
}

/* ---- Nested Object Access ---- */

gchar *
gnostr_json_get_string_at(const gchar *json, const gchar *object_key, const gchar *entry_key, GError **error)
{
    g_return_val_if_fail(json != NULL, NULL);
    g_return_val_if_fail(object_key != NULL, NULL);
    g_return_val_if_fail(entry_key != NULL, NULL);

    char *out = NULL;
    int rc = nostr_json_get_string_at(json, object_key, entry_key, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get string at '%s.%s'", object_key, entry_key);
        return NULL;
    }
    return out;
}

gint
gnostr_json_get_int_at(const gchar *json, const gchar *object_key, const gchar *entry_key, GError **error)
{
    g_return_val_if_fail(json != NULL, 0);
    g_return_val_if_fail(object_key != NULL, 0);
    g_return_val_if_fail(entry_key != NULL, 0);

    int out = 0;
    int rc = nostr_json_get_int_at(json, object_key, entry_key, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get int at '%s.%s'", object_key, entry_key);
        return 0;
    }
    return out;
}

gint64
gnostr_json_get_int64_at(const gchar *json, const gchar *object_key, const gchar *entry_key, GError **error)
{
    g_return_val_if_fail(json != NULL, 0);
    g_return_val_if_fail(object_key != NULL, 0);
    g_return_val_if_fail(entry_key != NULL, 0);

    int64_t out = 0;
    int rc = nostr_json_get_int64_at(json, object_key, entry_key, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get int64 at '%s.%s'", object_key, entry_key);
        return 0;
    }
    return (gint64)out;
}

gboolean
gnostr_json_get_bool_at(const gchar *json, const gchar *object_key, const gchar *entry_key, GError **error)
{
    g_return_val_if_fail(json != NULL, FALSE);
    g_return_val_if_fail(object_key != NULL, FALSE);
    g_return_val_if_fail(entry_key != NULL, FALSE);

    bool out = false;
    int rc = nostr_json_get_bool_at(json, object_key, entry_key, &out);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get bool at '%s.%s'", object_key, entry_key);
        return FALSE;
    }
    return out ? TRUE : FALSE;
}

GStrv
gnostr_json_get_string_array_at(const gchar *json, const gchar *object_key, const gchar *entry_key, GError **error)
{
    g_return_val_if_fail(json != NULL, NULL);
    g_return_val_if_fail(object_key != NULL, NULL);
    g_return_val_if_fail(entry_key != NULL, NULL);

    char **arr = NULL;
    size_t count = 0;
    int rc = nostr_json_get_string_array_at(json, object_key, entry_key, &arr, &count);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                    "Failed to get string array at '%s.%s'", object_key, entry_key);
        return NULL;
    }

    gchar **result = g_new0(gchar *, count + 1);
    for (size_t i = 0; i < count; i++) {
        result[i] = g_strdup(arr[i]);
        free(arr[i]);
    }
    free(arr);
    return result;
}

/* ---- Array Iteration ---- */

/* Adapter to bridge GObject callback to core callback */
typedef struct {
    GNostrJsonArrayIterCb callback;
    gpointer user_data;
} _ArrayIterAdapterData;

static bool
_array_iter_adapter(size_t index, const char *element_json, void *user_data)
{
    _ArrayIterAdapterData *data = user_data;
    return data->callback((gsize)index, element_json, data->user_data) ? true : false;
}

void
gnostr_json_array_foreach(const gchar *json, const gchar *key, GNostrJsonArrayIterCb callback, gpointer user_data)
{
    g_return_if_fail(json != NULL);
    g_return_if_fail(key != NULL);
    g_return_if_fail(callback != NULL);

    _ArrayIterAdapterData adapter = { callback, user_data };
    nostr_json_array_foreach(json, key, _array_iter_adapter, &adapter);
}

void
gnostr_json_array_foreach_root(const gchar *json, GNostrJsonArrayIterCb callback, gpointer user_data)
{
    g_return_if_fail(json != NULL);
    g_return_if_fail(callback != NULL);

    _ArrayIterAdapterData adapter = { callback, user_data };
    nostr_json_array_foreach_root(json, _array_iter_adapter, &adapter);
}

/* ---- Type Introspection ---- */

GNostrJsonType
gnostr_json_get_value_type(const gchar *json, const gchar *key)
{
    if (json == NULL || key == NULL)
        return GNOSTR_JSON_TYPE_INVALID;

    NostrJsonType t = nostr_json_get_type(json, key);
    /* Enum values match by design */
    return (GNostrJsonType)t;
}

/* ---- Convenience Builders ---- */

gchar *
gnostr_json_build_string_array(const gchar *first, ...)
{
    /* Build using the builder API to avoid va_list forwarding issues */
    GNostrJsonBuilder *b = gnostr_json_builder_new();
    gnostr_json_builder_begin_array(b);

    if (first != NULL) {
        gnostr_json_builder_add_string(b, first);

        va_list args;
        va_start(args, first);
        const gchar *s;
        while ((s = va_arg(args, const gchar *)) != NULL) {
            gnostr_json_builder_add_string(b, s);
        }
        va_end(args);
    }

    gnostr_json_builder_end_array(b);
    gchar *result = gnostr_json_builder_finish(b);
    g_object_unref(b);
    return result;
}

/* ---- Validation & Transformation ---- */

gboolean
gnostr_json_is_valid(const gchar *json)
{
    if (json == NULL)
        return FALSE;
    return nostr_json_is_valid(json) ? TRUE : FALSE;
}

gchar *
gnostr_json_prettify(const gchar *json, GError **error)
{
    g_return_val_if_fail(json != NULL, NULL);

    char *out = nostr_json_prettify(json);
    if (out == NULL) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                            "Failed to prettify JSON");
        return NULL;
    }
    return out;
}

gchar *
gnostr_json_compact_string(const gchar *json, GError **error)
{
    g_return_val_if_fail(json != NULL, NULL);

    char *out = nostr_json_compact(json);
    if (out == NULL) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                            "Failed to compact JSON");
        return NULL;
    }
    return out;
}

gchar *
gnostr_json_merge(const gchar *base, const gchar *overlay, GError **error)
{
    g_return_val_if_fail(base != NULL, NULL);
    g_return_val_if_fail(overlay != NULL, NULL);

    char *out = nostr_json_merge_objects(base, overlay);
    if (out == NULL) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                            "Failed to merge JSON objects");
        return NULL;
    }
    return out;
}
