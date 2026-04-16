/* gnostr-filter-set-manager.c — CRUD + persistence for GnostrFilterSet.
 *
 * SPDX-License-Identifier: MIT
 *
 * Storage format on disk:
 *
 *   {
 *     "version": 1,
 *     "filter_sets": [ <filter-set JSON>, ... ]
 *   }
 *
 * Only filter sets whose source is GNOSTR_FILTER_SET_SOURCE_CUSTOM are
 * written. Predefined sets are rebuilt on every load via
 * gnostr_filter_set_manager_install_defaults().
 *
 * nostrc-yg8j.2: FilterSet manager for CRUD operations.
 */

#define G_LOG_DOMAIN "gnostr-filter-set-manager"

#include "gnostr-filter-set-manager.h"

#include <json-glib/json-glib.h>
#include <errno.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * GObject boilerplate
 * ------------------------------------------------------------------------ */

struct _GnostrFilterSetManager {
  GObject parent_instance;

  gchar       *path;         /* owning: absolute path of storage file   */
  GListStore  *store;        /* owning: GListStore<GnostrFilterSet>     */
  GHashTable  *by_id;        /* owning: key=char* (shared w/ item->id)  */
};

G_DEFINE_TYPE(GnostrFilterSetManager, gnostr_filter_set_manager, G_TYPE_OBJECT)

/* ------------------------------------------------------------------------
 * Defaults
 * ------------------------------------------------------------------------ */

typedef struct {
  const gchar *id;
  const gchar *name;
  const gchar *description;
  const gchar *icon;
  const gchar *color;
  const gint   kinds[4];
  gsize        n_kinds;
} DefaultSpec;

static const DefaultSpec default_specs[] = {
  {
    .id          = "predefined-global",
    .name        = "Global",
    .description = "Public text notes from anyone the app has seen.",
    .icon        = "network-wireless-symbolic",
    .color       = "#3584e4",
    .kinds       = { 1 },
    .n_kinds     = 1,
  },
  {
    .id          = "predefined-follows",
    .name        = "Follows",
    .description = "Notes from accounts you follow.",
    .icon        = "people-symbolic",
    .color       = "#33d17a",
    .kinds       = { 1, 6 },
    .n_kinds     = 2,
  },
  {
    .id          = "predefined-mentions",
    .name        = "Mentions",
    .description = "Notes where you are mentioned.",
    .icon        = "mail-mark-important-symbolic",
    .color       = "#f6d32d",
    .kinds       = { 1 },
    .n_kinds     = 1,
  },
  {
    .id          = "predefined-media",
    .name        = "Media",
    .description = "Posts with pictures, video or audio.",
    .icon        = "image-x-generic-symbolic",
    .color       = "#c061cb",
    .kinds       = { 1, 20 },
    .n_kinds     = 2,
  },
};

/* ------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------ */

static gchar *
default_storage_path(void)
{
  const gchar *data_dir = g_get_user_data_dir();
  return g_build_filename(data_dir, "gnostr", "filter-sets.json", NULL);
}

/* Find the position of a set with @id inside the store. Returns -1 if
 * not found. O(n) but n is small (dozens at most). */
static gint
find_position_by_id(GnostrFilterSetManager *self, const gchar *id)
{
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->store));
  for (guint i = 0; i < n; i++) {
    GnostrFilterSet *fs = g_list_model_get_item(G_LIST_MODEL(self->store), i);
    gboolean match = g_strcmp0(gnostr_filter_set_get_id(fs), id) == 0;
    g_object_unref(fs);
    if (match) return (gint)i;
  }
  return -1;
}

static void
index_insert(GnostrFilterSetManager *self, GnostrFilterSet *fs)
{
  const gchar *id = gnostr_filter_set_get_id(fs);
  if (id && *id)
    g_hash_table_replace(self->by_id, g_strdup(id), fs); /* weak ref only */
}

static void
index_remove(GnostrFilterSetManager *self, const gchar *id)
{
  if (id && *id)
    g_hash_table_remove(self->by_id, id);
}

static void
rebuild_index(GnostrFilterSetManager *self)
{
  g_hash_table_remove_all(self->by_id);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->store));
  for (guint i = 0; i < n; i++) {
    GnostrFilterSet *fs = g_list_model_get_item(G_LIST_MODEL(self->store), i);
    index_insert(self, fs);
    g_object_unref(fs);
  }
}

/* ------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */

static void
gnostr_filter_set_manager_init(GnostrFilterSetManager *self)
{
  self->store = g_list_store_new(GNOSTR_TYPE_FILTER_SET);
  /* by_id stores a *weak* pointer to the item; the store owns the ref */
  self->by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

static void
gnostr_filter_set_manager_finalize(GObject *object)
{
  GnostrFilterSetManager *self = GNOSTR_FILTER_SET_MANAGER(object);
  g_clear_pointer(&self->path, g_free);
  g_clear_pointer(&self->by_id, g_hash_table_destroy);
  g_clear_object(&self->store);
  G_OBJECT_CLASS(gnostr_filter_set_manager_parent_class)->finalize(object);
}

static void
gnostr_filter_set_manager_class_init(GnostrFilterSetManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gnostr_filter_set_manager_finalize;
}

/* ------------------------------------------------------------------------
 * Construction
 * ------------------------------------------------------------------------ */

GnostrFilterSetManager *
gnostr_filter_set_manager_new(void)
{
  return gnostr_filter_set_manager_new_for_path(NULL);
}

GnostrFilterSetManager *
gnostr_filter_set_manager_new_for_path(const gchar *path)
{
  GnostrFilterSetManager *self = g_object_new(GNOSTR_TYPE_FILTER_SET_MANAGER, NULL);
  self->path = path ? g_strdup(path) : default_storage_path();
  return self;
}

GnostrFilterSetManager *
gnostr_filter_set_manager_get_default(void)
{
  static GnostrFilterSetManager *singleton = NULL;
  static gsize once = 0;
  if (g_once_init_enter(&once)) {
    GnostrFilterSetManager *inst = gnostr_filter_set_manager_new();
    singleton = inst;
    g_once_init_leave(&once, 1);
  }
  return singleton;
}

/* ------------------------------------------------------------------------
 * Defaults
 * ------------------------------------------------------------------------ */

void
gnostr_filter_set_manager_install_defaults(GnostrFilterSetManager *self)
{
  g_return_if_fail(GNOSTR_IS_FILTER_SET_MANAGER(self));

  for (gsize i = 0; i < G_N_ELEMENTS(default_specs); i++) {
    const DefaultSpec *spec = &default_specs[i];
    if (g_hash_table_contains(self->by_id, spec->id))
      continue;

    GnostrFilterSet *fs = gnostr_filter_set_new();
    gnostr_filter_set_set_id         (fs, spec->id);
    gnostr_filter_set_set_name       (fs, spec->name);
    gnostr_filter_set_set_description(fs, spec->description);
    gnostr_filter_set_set_icon       (fs, spec->icon);
    gnostr_filter_set_set_color      (fs, spec->color);
    gnostr_filter_set_set_source     (fs, GNOSTR_FILTER_SET_SOURCE_PREDEFINED);
    if (spec->n_kinds > 0)
      gnostr_filter_set_set_kinds(fs, spec->kinds, spec->n_kinds);

    g_list_store_append(self->store, fs);
    index_insert(self, fs);
    g_object_unref(fs);
  }
}

/* ------------------------------------------------------------------------
 * CRUD
 * ------------------------------------------------------------------------ */

gboolean
gnostr_filter_set_manager_add(GnostrFilterSetManager *self,
                              GnostrFilterSet *filter_set)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET_MANAGER(self), FALSE);
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(filter_set), FALSE);

  /* Ensure an id exists */
  const gchar *id = gnostr_filter_set_get_id(filter_set);
  if (!id || !*id) {
    gnostr_filter_set_set_id(filter_set, NULL); /* regenerates */
    id = gnostr_filter_set_get_id(filter_set);
  }
  if (g_hash_table_contains(self->by_id, id))
    return FALSE;

  g_list_store_append(self->store, filter_set);
  index_insert(self, filter_set);
  return TRUE;
}

gboolean
gnostr_filter_set_manager_update(GnostrFilterSetManager *self,
                                 GnostrFilterSet *filter_set)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET_MANAGER(self), FALSE);
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET(filter_set), FALSE);

  const gchar *id = gnostr_filter_set_get_id(filter_set);
  if (!id || !*id) return FALSE;

  gint pos = find_position_by_id(self, id);
  if (pos < 0) return FALSE;

  /* Replace in place with a clone so callers may keep their reference.
   * g_list_store_splice does an atomic remove+insert so the model emits
   * a single items-changed(pos, 1, 1) transition. */
  GnostrFilterSet *clone = gnostr_filter_set_clone(filter_set);
  gpointer additions[1] = { clone };
  g_list_store_splice(self->store, (guint)pos, 1, additions, 1);
  index_remove(self, id);
  index_insert(self, clone);
  g_object_unref(clone);
  return TRUE;
}

gboolean
gnostr_filter_set_manager_remove(GnostrFilterSetManager *self,
                                 const gchar *id)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET_MANAGER(self), FALSE);
  g_return_val_if_fail(id != NULL && *id, FALSE);

  gint pos = find_position_by_id(self, id);
  if (pos < 0) return FALSE;

  GnostrFilterSet *fs = g_list_model_get_item(G_LIST_MODEL(self->store), (guint)pos);
  gboolean predefined = (gnostr_filter_set_get_source(fs) ==
                         GNOSTR_FILTER_SET_SOURCE_PREDEFINED);
  g_object_unref(fs);
  if (predefined) return FALSE;

  g_list_store_remove(self->store, (guint)pos);
  index_remove(self, id);
  return TRUE;
}

/* ------------------------------------------------------------------------
 * Lookup
 * ------------------------------------------------------------------------ */

GnostrFilterSet *
gnostr_filter_set_manager_get(GnostrFilterSetManager *self,
                              const gchar *id)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET_MANAGER(self), NULL);
  if (!id || !*id) return NULL;
  return (GnostrFilterSet *)g_hash_table_lookup(self->by_id, id);
}

gboolean
gnostr_filter_set_manager_contains(GnostrFilterSetManager *self,
                                   const gchar *id)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET_MANAGER(self), FALSE);
  if (!id || !*id) return FALSE;
  return g_hash_table_contains(self->by_id, id);
}

guint
gnostr_filter_set_manager_count(GnostrFilterSetManager *self)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET_MANAGER(self), 0);
  return g_list_model_get_n_items(G_LIST_MODEL(self->store));
}

GListModel *
gnostr_filter_set_manager_get_model(GnostrFilterSetManager *self)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET_MANAGER(self), NULL);
  return G_LIST_MODEL(self->store);
}

/* ------------------------------------------------------------------------
 * Persistence
 * ------------------------------------------------------------------------ */

#define GNOSTR_FILTER_SET_MANAGER_FORMAT_VERSION 1

gboolean
gnostr_filter_set_manager_save(GnostrFilterSetManager *self, GError **error)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET_MANAGER(self), FALSE);

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "version");
  json_builder_add_int_value(b, GNOSTR_FILTER_SET_MANAGER_FORMAT_VERSION);
  json_builder_set_member_name(b, "filter_sets");
  json_builder_begin_array(b);

  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->store));
  for (guint i = 0; i < n; i++) {
    GnostrFilterSet *fs = g_list_model_get_item(G_LIST_MODEL(self->store), i);
    if (gnostr_filter_set_get_source(fs) != GNOSTR_FILTER_SET_SOURCE_CUSTOM) {
      g_object_unref(fs);
      continue;
    }
    g_autofree gchar *item_json = gnostr_filter_set_to_json(fs);
    g_object_unref(fs);
    if (!item_json) continue;

    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, item_json, -1, NULL)) {
      JsonNode *node = json_node_copy(json_parser_get_root(p));
      json_builder_add_value(b, node); /* takes ownership */
    }
    g_object_unref(p);
  }

  json_builder_end_array(b); /* filter_sets */
  json_builder_end_object(b); /* root */

  JsonGenerator *gen = json_generator_new();
  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, TRUE);
  gchar *out = json_generator_to_data(gen, NULL);
  json_node_free(root);
  g_object_unref(gen);
  g_object_unref(b);

  /* Ensure parent directory exists */
  g_autofree gchar *dir = g_path_get_dirname(self->path);
  if (g_mkdir_with_parents(dir, 0700) != 0 && errno != EEXIST) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "failed to create %s: %s", dir, g_strerror(errno));
    g_free(out);
    return FALSE;
  }

  gboolean ok = g_file_set_contents(self->path, out, -1, error);
  g_free(out);
  return ok;
}

gboolean
gnostr_filter_set_manager_load(GnostrFilterSetManager *self, GError **error)
{
  g_return_val_if_fail(GNOSTR_IS_FILTER_SET_MANAGER(self), FALSE);

  /* Reset in-memory state */
  g_list_store_remove_all(self->store);
  g_hash_table_remove_all(self->by_id);

  /* Always register predefined defaults first so they show up in order */
  gnostr_filter_set_manager_install_defaults(self);

  if (!g_file_test(self->path, G_FILE_TEST_EXISTS)) {
    /* Fresh install — defaults-only is a valid state */
    return TRUE;
  }

  g_autofree gchar *contents = NULL;
  gsize length = 0;
  if (!g_file_get_contents(self->path, &contents, &length, error))
    return FALSE;

  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, contents, (gssize)length, error)) {
    g_object_unref(parser);
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "filter set storage root must be an object");
    g_object_unref(parser);
    return FALSE;
  }

  JsonObject *obj = json_node_get_object(root);
  if (json_object_has_member(obj, "filter_sets")) {
    JsonNode *arr_node = json_object_get_member(obj, "filter_sets");
    if (JSON_NODE_HOLDS_ARRAY(arr_node)) {
      JsonArray *arr = json_node_get_array(arr_node);
      guint len = json_array_get_length(arr);
      for (guint i = 0; i < len; i++) {
        JsonNode *item = json_array_get_element(arr, i);
        if (!JSON_NODE_HOLDS_OBJECT(item)) continue;

        g_autoptr(JsonGenerator) gen = json_generator_new();
        json_generator_set_root(gen, item);
        g_autofree gchar *item_json = json_generator_to_data(gen, NULL);
        if (!item_json) continue;

        g_autoptr(GError) parse_err = NULL;
        GnostrFilterSet *fs = gnostr_filter_set_new_from_json(item_json, &parse_err);
        if (!fs) {
          g_warning("filter-set-manager: skipping invalid entry: %s",
                    parse_err ? parse_err->message : "unknown");
          continue;
        }

        /* Force loaded sets to CUSTOM — we never persist predefined */
        gnostr_filter_set_set_source(fs, GNOSTR_FILTER_SET_SOURCE_CUSTOM);

        const gchar *id = gnostr_filter_set_get_id(fs);
        if (!id || !*id || g_hash_table_contains(self->by_id, id)) {
          g_warning("filter-set-manager: skipping duplicate/empty id");
          g_object_unref(fs);
          continue;
        }
        g_list_store_append(self->store, fs);
        index_insert(self, fs);
        g_object_unref(fs);
      }
    }
  }

  g_object_unref(parser);

  /* Paranoia: ensure the hash index matches the store. */
  rebuild_index(self);
  return TRUE;
}
