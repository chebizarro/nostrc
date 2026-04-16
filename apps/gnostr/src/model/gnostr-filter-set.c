/* gnostr-filter-set.c — Implementation of GnostrFilterSet.
 *
 * SPDX-License-Identifier: MIT
 *
 * nostrc-yg8j.1: FilterSet data model and serialization.
 */

#define G_LOG_DOMAIN "gnostr-filter-set"

#include "gnostr-filter-set.h"

#include <json-glib/json-glib.h>
#include <string.h>

struct _GnostrFilterSet {
  GObject parent_instance;

  /* Identity / metadata */
  gchar *id;
  gchar *name;
  gchar *description;
  gchar *icon;
  gchar *color;
  GnostrFilterSetSource source;

  /* Filter criteria */
  gchar **authors;    /* NULL-terminated, may be NULL */
  GArray *kinds;      /* element-type=gint, may be NULL */
  gchar **hashtags;   /* NULL-terminated, may be NULL */
  gint64 since;
  gint64 until;
  gint   limit;
};

G_DEFINE_TYPE(GnostrFilterSet, gnostr_filter_set, G_TYPE_OBJECT)

/* ------------------------------------------------------------------------
 * Internals
 * ------------------------------------------------------------------------ */

static gchar *
generate_filter_set_id(void)
{
  /* Not a cryptographic UUID — just a reasonably unique identifier suitable
   * for dedup and storage keys. Format: "fs-<hex>". */
  guint32 r1 = g_random_int();
  guint32 r2 = g_random_int();
  return g_strdup_printf("fs-%08x%08x", r1, r2);
}

static gchar **
strv_dup_or_null(const gchar * const *src)
{
  if (!src || !src[0])
    return NULL;
  return g_strdupv((gchar **)src);
}

static gboolean
strv_equal(const gchar * const *a, const gchar * const *b)
{
  gsize na = a ? g_strv_length((gchar **)a) : 0;
  gsize nb = b ? g_strv_length((gchar **)b) : 0;
  if (na != nb)
    return FALSE;
  for (gsize i = 0; i < na; i++) {
    if (g_strcmp0(a[i], b[i]) != 0)
      return FALSE;
  }
  return TRUE;
}

static GArray *
int_array_dup_or_null(const gint *src, gsize n)
{
  if (!src || n == 0)
    return NULL;
  GArray *out = g_array_sized_new(FALSE, FALSE, sizeof(gint), n);
  g_array_append_vals(out, src, n);
  return out;
}

static gboolean
int_array_equal(GArray *a, GArray *b)
{
  gsize na = a ? a->len : 0;
  gsize nb = b ? b->len : 0;
  if (na != nb)
    return FALSE;
  for (gsize i = 0; i < na; i++) {
    if (g_array_index(a, gint, i) != g_array_index(b, gint, i))
      return FALSE;
  }
  return TRUE;
}

static const gchar *
source_to_string(GnostrFilterSetSource src)
{
  switch (src) {
    case GNOSTR_FILTER_SET_SOURCE_PREDEFINED: return "predefined";
    case GNOSTR_FILTER_SET_SOURCE_CUSTOM:
    default:                                  return "custom";
  }
}

static GnostrFilterSetSource
source_from_string(const gchar *s)
{
  if (g_strcmp0(s, "predefined") == 0)
    return GNOSTR_FILTER_SET_SOURCE_PREDEFINED;
  return GNOSTR_FILTER_SET_SOURCE_CUSTOM;
}

/* ------------------------------------------------------------------------
 * GObject lifecycle
 * ------------------------------------------------------------------------ */

static void
gnostr_filter_set_init(GnostrFilterSet *self)
{
  self->id          = generate_filter_set_id();
  self->name        = NULL;
  self->description = NULL;
  self->icon        = NULL;
  self->color       = NULL;
  self->source      = GNOSTR_FILTER_SET_SOURCE_CUSTOM;
  self->authors     = NULL;
  self->kinds       = NULL;
  self->hashtags    = NULL;
  self->since       = 0;
  self->until       = 0;
  self->limit       = 0;
}

static void
gnostr_filter_set_finalize(GObject *obj)
{
  GnostrFilterSet *self = GNOSTR_FILTER_SET(obj);

  g_clear_pointer(&self->id,          g_free);
  g_clear_pointer(&self->name,        g_free);
  g_clear_pointer(&self->description, g_free);
  g_clear_pointer(&self->icon,        g_free);
  g_clear_pointer(&self->color,       g_free);
  g_clear_pointer(&self->authors,     g_strfreev);
  g_clear_pointer(&self->hashtags,    g_strfreev);
  if (self->kinds) {
    g_array_free(self->kinds, TRUE);
    self->kinds = NULL;
  }

  G_OBJECT_CLASS(gnostr_filter_set_parent_class)->finalize(obj);
}

static void
gnostr_filter_set_class_init(GnostrFilterSetClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gnostr_filter_set_finalize;
}

/* ------------------------------------------------------------------------
 * Construction
 * ------------------------------------------------------------------------ */

GnostrFilterSet *
gnostr_filter_set_new(void)
{
  return g_object_new(GNOSTR_TYPE_FILTER_SET, NULL);
}

GnostrFilterSet *
gnostr_filter_set_new_with_name(const gchar *name)
{
  GnostrFilterSet *self = gnostr_filter_set_new();
  gnostr_filter_set_set_name(self, name);
  return self;
}

GnostrFilterSet *
gnostr_filter_set_clone(GnostrFilterSet *self)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(self), NULL);

  GnostrFilterSet *copy = gnostr_filter_set_new();

  /* Overwrite the auto-generated id with the source's id. */
  g_free(copy->id);
  copy->id = g_strdup(self->id);

  copy->name        = g_strdup(self->name);
  copy->description = g_strdup(self->description);
  copy->icon        = g_strdup(self->icon);
  copy->color       = g_strdup(self->color);
  copy->source      = self->source;
  copy->authors     = strv_dup_or_null((const gchar * const *)self->authors);
  copy->hashtags    = strv_dup_or_null((const gchar * const *)self->hashtags);
  if (self->kinds && self->kinds->len > 0) {
    copy->kinds = int_array_dup_or_null(&g_array_index(self->kinds, gint, 0),
                                         self->kinds->len);
  }
  copy->since = self->since;
  copy->until = self->until;
  copy->limit = self->limit;
  return copy;
}

/* ------------------------------------------------------------------------
 * Identity / metadata accessors
 * ------------------------------------------------------------------------ */

#define GETSET_STRING(field)                                                \
  const gchar *                                                             \
  gnostr_filter_set_get_##field(GnostrFilterSet *self)                      \
  {                                                                         \
    g_return_val_if_fail(GNOSTR_IS_FILTER_SET(self), NULL);                 \
    return self->field;                                                     \
  }                                                                         \
  void                                                                      \
  gnostr_filter_set_set_##field(GnostrFilterSet *self, const gchar *value)  \
  {                                                                         \
    g_return_if_fail(GNOSTR_IS_FILTER_SET(self));                           \
    if (g_strcmp0(self->field, value) == 0)                                 \
      return;                                                               \
    g_free(self->field);                                                    \
    self->field = (value && *value) ? g_strdup(value) : NULL;               \
  }

const gchar *
gnostr_filter_set_get_id(GnostrFilterSet *self)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(self), NULL);
  return self->id;
}

/* id setter needs special handling: NULL should regenerate an id, not clear it */
void
gnostr_filter_set_set_id(GnostrFilterSet *self, const gchar *id)
{
  g_return_if_fail(GNOSTR_IS_FILTER_SET(self));
  g_free(self->id);
  self->id = (id && *id) ? g_strdup(id) : generate_filter_set_id();
}

GETSET_STRING(name)
GETSET_STRING(description)
GETSET_STRING(icon)
GETSET_STRING(color)

#undef GETSET_STRING

GnostrFilterSetSource
gnostr_filter_set_get_source(GnostrFilterSet *self)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(self), GNOSTR_FILTER_SET_SOURCE_CUSTOM);
  return self->source;
}

void
gnostr_filter_set_set_source(GnostrFilterSet *self, GnostrFilterSetSource source)
{
  g_return_if_fail(GNOSTR_IS_FILTER_SET(self));
  self->source = source;
}

/* ------------------------------------------------------------------------
 * Filter criteria accessors
 * ------------------------------------------------------------------------ */

const gchar * const *
gnostr_filter_set_get_authors(GnostrFilterSet *self)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(self), NULL);
  return (const gchar * const *)self->authors;
}

void
gnostr_filter_set_set_authors(GnostrFilterSet *self, const gchar * const *authors)
{
  g_return_if_fail(GNOSTR_IS_FILTER_SET(self));
  gchar **old = self->authors;
  self->authors = strv_dup_or_null(authors);
  g_strfreev(old);
}

const gint *
gnostr_filter_set_get_kinds(GnostrFilterSet *self, gsize *n_kinds)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(self), NULL);
  if (!self->kinds || self->kinds->len == 0) {
    if (n_kinds) *n_kinds = 0;
    return NULL;
  }
  if (n_kinds) *n_kinds = self->kinds->len;
  return (const gint *)self->kinds->data;
}

void
gnostr_filter_set_set_kinds(GnostrFilterSet *self, const gint *kinds, gsize n_kinds)
{
  g_return_if_fail(GNOSTR_IS_FILTER_SET(self));
  if (self->kinds) {
    g_array_free(self->kinds, TRUE);
    self->kinds = NULL;
  }
  self->kinds = int_array_dup_or_null(kinds, n_kinds);
}

const gchar * const *
gnostr_filter_set_get_hashtags(GnostrFilterSet *self)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(self), NULL);
  return (const gchar * const *)self->hashtags;
}

void
gnostr_filter_set_set_hashtags(GnostrFilterSet *self, const gchar * const *hashtags)
{
  g_return_if_fail(GNOSTR_IS_FILTER_SET(self));
  gchar **old = self->hashtags;
  self->hashtags = strv_dup_or_null(hashtags);
  g_strfreev(old);
}

gint64 gnostr_filter_set_get_since(GnostrFilterSet *self)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(self), 0);
  return self->since;
}
void gnostr_filter_set_set_since(GnostrFilterSet *self, gint64 since)
{
  g_return_if_fail(GNOSTR_IS_FILTER_SET(self));
  self->since = since;
}
gint64 gnostr_filter_set_get_until(GnostrFilterSet *self)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(self), 0);
  return self->until;
}
void gnostr_filter_set_set_until(GnostrFilterSet *self, gint64 until)
{
  g_return_if_fail(GNOSTR_IS_FILTER_SET(self));
  self->until = until;
}
gint gnostr_filter_set_get_limit(GnostrFilterSet *self)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(self), 0);
  return self->limit;
}
void gnostr_filter_set_set_limit(GnostrFilterSet *self, gint limit)
{
  g_return_if_fail(GNOSTR_IS_FILTER_SET(self));
  self->limit = limit > 0 ? limit : 0;
}

/* ------------------------------------------------------------------------
 * Inspection
 * ------------------------------------------------------------------------ */

gboolean
gnostr_filter_set_is_empty(GnostrFilterSet *self)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(self), TRUE);
  if (self->authors  && self->authors[0])            return FALSE;
  if (self->hashtags && self->hashtags[0])           return FALSE;
  if (self->kinds    && self->kinds->len > 0)        return FALSE;
  if (self->since != 0 || self->until != 0)          return FALSE;
  if (self->limit != 0)                              return FALSE;
  return TRUE;
}

gboolean
gnostr_filter_set_equal(GnostrFilterSet *a, GnostrFilterSet *b)
{
  if (a == b) return TRUE;
  if (!a || !b) return FALSE;
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(a), FALSE);
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(b), FALSE);

  if (g_strcmp0(a->id,          b->id)          != 0) return FALSE;
  if (g_strcmp0(a->name,        b->name)        != 0) return FALSE;
  if (g_strcmp0(a->description, b->description) != 0) return FALSE;
  if (g_strcmp0(a->icon,        b->icon)        != 0) return FALSE;
  if (g_strcmp0(a->color,       b->color)       != 0) return FALSE;
  if (a->source != b->source)                         return FALSE;
  if (a->since  != b->since)                          return FALSE;
  if (a->until  != b->until)                          return FALSE;
  if (a->limit  != b->limit)                          return FALSE;
  if (!strv_equal((const gchar * const *)a->authors,
                   (const gchar * const *)b->authors))    return FALSE;
  if (!strv_equal((const gchar * const *)a->hashtags,
                   (const gchar * const *)b->hashtags))   return FALSE;
  if (!int_array_equal(a->kinds, b->kinds))               return FALSE;
  return TRUE;
}

/* ------------------------------------------------------------------------
 * JSON serialization (via json-glib)
 * ------------------------------------------------------------------------ */

/* Helpers to add optional members — they skip NULL/empty values so the output
 * stays compact and round-trips through gnostr_filter_set_new_from_json(). */

static void
add_string_member(JsonBuilder *b, const char *key, const char *value)
{
  if (!value || !*value) return;
  json_builder_set_member_name(b, key);
  json_builder_add_string_value(b, value);
}

static void
add_int_member(JsonBuilder *b, const char *key, gint64 value)
{
  if (value == 0) return;
  json_builder_set_member_name(b, key);
  json_builder_add_int_value(b, value);
}

static void
add_strv_member(JsonBuilder *b, const char *key, gchar **strv)
{
  if (!strv || !strv[0]) return;
  json_builder_set_member_name(b, key);
  json_builder_begin_array(b);
  for (gchar **p = strv; *p; p++)
    json_builder_add_string_value(b, *p);
  json_builder_end_array(b);
}

static void
add_kinds_member(JsonBuilder *b, const char *key, GArray *kinds)
{
  if (!kinds || kinds->len == 0) return;
  json_builder_set_member_name(b, key);
  json_builder_begin_array(b);
  for (guint i = 0; i < kinds->len; i++)
    json_builder_add_int_value(b, g_array_index(kinds, gint, i));
  json_builder_end_array(b);
}

gchar *
gnostr_filter_set_to_json(GnostrFilterSet *self)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(self), NULL);

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  add_string_member(b, "id",          self->id);
  add_string_member(b, "name",        self->name);
  add_string_member(b, "description", self->description);
  add_string_member(b, "icon",        self->icon);
  add_string_member(b, "color",       self->color);

  json_builder_set_member_name(b, "source");
  json_builder_add_string_value(b, source_to_string(self->source));

  json_builder_set_member_name(b, "filter");
  json_builder_begin_object(b);
  add_strv_member (b, "authors",  self->authors);
  add_kinds_member(b, "kinds",    self->kinds);
  add_strv_member (b, "hashtags", self->hashtags);
  add_int_member  (b, "since",    self->since);
  add_int_member  (b, "until",    self->until);
  add_int_member  (b, "limit",    self->limit);
  json_builder_end_object(b); /* filter */

  json_builder_end_object(b); /* root */

  JsonGenerator *gen = json_generator_new();
  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(gen, root);
  gchar *out = json_generator_to_data(gen, NULL);

  json_node_free(root);
  g_object_unref(gen);
  g_object_unref(b);
  return out;
}

/* ---- JSON → object --------------------------------------------------- */

static const gchar *
read_string_member(JsonObject *obj, const char *key)
{
  if (!json_object_has_member(obj, key)) return NULL;
  JsonNode *n = json_object_get_member(obj, key);
  if (!JSON_NODE_HOLDS_VALUE(n)) return NULL;
  return json_node_get_string(n);
}

static gint64
read_int_member(JsonObject *obj, const char *key)
{
  if (!json_object_has_member(obj, key)) return 0;
  JsonNode *n = json_object_get_member(obj, key);
  if (!JSON_NODE_HOLDS_VALUE(n)) return 0;
  return json_node_get_int(n);
}

static gchar **
read_strv_member(JsonObject *obj, const char *key)
{
  if (!json_object_has_member(obj, key)) return NULL;
  JsonNode *n = json_object_get_member(obj, key);
  if (!JSON_NODE_HOLDS_ARRAY(n)) return NULL;
  JsonArray *arr = json_node_get_array(n);
  guint len = json_array_get_length(arr);
  if (len == 0) return NULL;

  gchar **out = g_new0(gchar *, len + 1);
  guint out_i = 0;
  for (guint i = 0; i < len; i++) {
    JsonNode *item = json_array_get_element(arr, i);
    if (JSON_NODE_HOLDS_VALUE(item)) {
      const gchar *s = json_node_get_string(item);
      if (s && *s) out[out_i++] = g_strdup(s);
    }
  }
  out[out_i] = NULL;
  if (out_i == 0) {
    g_strfreev(out);
    return NULL;
  }
  return out;
}

static GArray *
read_kinds_member(JsonObject *obj, const char *key)
{
  if (!json_object_has_member(obj, key)) return NULL;
  JsonNode *n = json_object_get_member(obj, key);
  if (!JSON_NODE_HOLDS_ARRAY(n)) return NULL;
  JsonArray *arr = json_node_get_array(n);
  guint len = json_array_get_length(arr);
  if (len == 0) return NULL;
  GArray *out = g_array_sized_new(FALSE, FALSE, sizeof(gint), len);
  for (guint i = 0; i < len; i++) {
    JsonNode *item = json_array_get_element(arr, i);
    if (JSON_NODE_HOLDS_VALUE(item)) {
      gint v = (gint)json_node_get_int(item);
      g_array_append_val(out, v);
    }
  }
  if (out->len == 0) {
    g_array_free(out, TRUE);
    return NULL;
  }
  return out;
}

GnostrFilterSet *
gnostr_filter_set_new_from_json(const gchar *json, GError **error)
{
  g_return_val_if_fail(json != NULL, NULL);

  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json, -1, error)) {
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "filter set JSON must be an object");
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);
  GnostrFilterSet *self = gnostr_filter_set_new();

  const gchar *id = read_string_member(obj, "id");
  if (id && *id) {
    g_free(self->id);
    self->id = g_strdup(id);
  }
  gnostr_filter_set_set_name        (self, read_string_member(obj, "name"));
  gnostr_filter_set_set_description (self, read_string_member(obj, "description"));
  gnostr_filter_set_set_icon        (self, read_string_member(obj, "icon"));
  gnostr_filter_set_set_color       (self, read_string_member(obj, "color"));
  self->source = source_from_string (read_string_member(obj, "source"));

  if (json_object_has_member(obj, "filter")) {
    JsonNode *fnode = json_object_get_member(obj, "filter");
    if (JSON_NODE_HOLDS_OBJECT(fnode)) {
      JsonObject *filt = json_node_get_object(fnode);
      self->authors  = read_strv_member (filt, "authors");
      self->kinds    = read_kinds_member(filt, "kinds");
      self->hashtags = read_strv_member (filt, "hashtags");
      self->since    = read_int_member  (filt, "since");
      self->until    = read_int_member  (filt, "until");
      self->limit    = (gint)read_int_member(filt, "limit");
    }
  }

  g_object_unref(parser);
  return self;
}

/* ------------------------------------------------------------------------
 * GVariant serialization (a{sv} dict for GSettings)
 * ------------------------------------------------------------------------ */

GVariant *
gnostr_filter_set_to_variant(GnostrFilterSet *self)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(self), NULL);

  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));

  if (self->id)
    g_variant_builder_add(&b, "{sv}", "id", g_variant_new_string(self->id));
  if (self->name)
    g_variant_builder_add(&b, "{sv}", "name", g_variant_new_string(self->name));
  if (self->description)
    g_variant_builder_add(&b, "{sv}", "description", g_variant_new_string(self->description));
  if (self->icon)
    g_variant_builder_add(&b, "{sv}", "icon", g_variant_new_string(self->icon));
  if (self->color)
    g_variant_builder_add(&b, "{sv}", "color", g_variant_new_string(self->color));

  g_variant_builder_add(&b, "{sv}", "source",
                        g_variant_new_string(source_to_string(self->source)));

  if (self->authors && self->authors[0]) {
    g_variant_builder_add(&b, "{sv}", "authors",
                          g_variant_new_strv((const gchar * const *)self->authors, -1));
  }
  if (self->hashtags && self->hashtags[0]) {
    g_variant_builder_add(&b, "{sv}", "hashtags",
                          g_variant_new_strv((const gchar * const *)self->hashtags, -1));
  }
  if (self->kinds && self->kinds->len > 0) {
    GVariantBuilder kb;
    g_variant_builder_init(&kb, G_VARIANT_TYPE("ai"));
    for (guint i = 0; i < self->kinds->len; i++)
      g_variant_builder_add(&kb, "i", g_array_index(self->kinds, gint, i));
    g_variant_builder_add(&b, "{sv}", "kinds", g_variant_builder_end(&kb));
  }
  if (self->since != 0)
    g_variant_builder_add(&b, "{sv}", "since", g_variant_new_int64(self->since));
  if (self->until != 0)
    g_variant_builder_add(&b, "{sv}", "until", g_variant_new_int64(self->until));
  if (self->limit != 0)
    g_variant_builder_add(&b, "{sv}", "limit", g_variant_new_int32(self->limit));

  return g_variant_builder_end(&b);
}

static const gchar *
variant_get_string_or_null(GVariant *v)
{
  if (!v) return NULL;
  if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
    return g_variant_get_string(v, NULL);
  return NULL;
}

GnostrFilterSet *
gnostr_filter_set_new_from_variant(GVariant *variant)
{
  g_return_val_if_fail(variant != NULL, NULL);
  if (!g_variant_is_of_type(variant, G_VARIANT_TYPE("a{sv}")))
    return NULL;

  GnostrFilterSet *self = gnostr_filter_set_new();
  GVariantIter iter;
  const gchar *key;
  GVariant *val;

  g_variant_iter_init(&iter, variant);
  while (g_variant_iter_loop(&iter, "{&sv}", &key, &val)) {
    if (g_strcmp0(key, "id") == 0) {
      const gchar *s = variant_get_string_or_null(val);
      if (s && *s) {
        g_free(self->id);
        self->id = g_strdup(s);
      }
    } else if (g_strcmp0(key, "name") == 0) {
      gnostr_filter_set_set_name(self, variant_get_string_or_null(val));
    } else if (g_strcmp0(key, "description") == 0) {
      gnostr_filter_set_set_description(self, variant_get_string_or_null(val));
    } else if (g_strcmp0(key, "icon") == 0) {
      gnostr_filter_set_set_icon(self, variant_get_string_or_null(val));
    } else if (g_strcmp0(key, "color") == 0) {
      gnostr_filter_set_set_color(self, variant_get_string_or_null(val));
    } else if (g_strcmp0(key, "source") == 0) {
      self->source = source_from_string(variant_get_string_or_null(val));
    } else if (g_strcmp0(key, "authors") == 0 &&
               g_variant_is_of_type(val, G_VARIANT_TYPE_STRING_ARRAY)) {
      g_strfreev(self->authors);
      self->authors = g_variant_dup_strv(val, NULL);
    } else if (g_strcmp0(key, "hashtags") == 0 &&
               g_variant_is_of_type(val, G_VARIANT_TYPE_STRING_ARRAY)) {
      g_strfreev(self->hashtags);
      self->hashtags = g_variant_dup_strv(val, NULL);
    } else if (g_strcmp0(key, "kinds") == 0 &&
               g_variant_is_of_type(val, G_VARIANT_TYPE("ai"))) {
      if (self->kinds) {
        g_array_free(self->kinds, TRUE);
        self->kinds = NULL;
      }
      gsize n;
      gconstpointer data = g_variant_get_fixed_array(val, &n, sizeof(gint32));
      if (n > 0) {
        self->kinds = g_array_sized_new(FALSE, FALSE, sizeof(gint), n);
        const gint32 *src = (const gint32 *)data;
        for (gsize i = 0; i < n; i++) {
          gint v = (gint)src[i];
          g_array_append_val(self->kinds, v);
        }
      }
    } else if (g_strcmp0(key, "since") == 0 &&
               g_variant_is_of_type(val, G_VARIANT_TYPE_INT64)) {
      self->since = g_variant_get_int64(val);
    } else if (g_strcmp0(key, "until") == 0 &&
               g_variant_is_of_type(val, G_VARIANT_TYPE_INT64)) {
      self->until = g_variant_get_int64(val);
    } else if (g_strcmp0(key, "limit") == 0 &&
               g_variant_is_of_type(val, G_VARIANT_TYPE_INT32)) {
      self->limit = g_variant_get_int32(val);
    }
  }
  return self;
}
