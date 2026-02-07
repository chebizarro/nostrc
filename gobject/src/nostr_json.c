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
