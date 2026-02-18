/* event_history.c - Transaction/event history storage implementation
 *
 * SPDX-License-Identifier: MIT
 */
#include "event_history.h"

#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

/* ============================================================================
 * GnEventHistoryEntry Implementation
 * ============================================================================ */

struct _GnEventHistoryEntry {
  GObject parent_instance;

  gchar *id;              /* Unique entry ID */
  gint64 timestamp;       /* Unix timestamp */
  gchar *event_id;        /* Nostr event ID (hex) */
  gint event_kind;        /* Event kind number */
  gchar *client_pubkey;   /* Client public key (hex) */
  gchar *client_app;      /* Client application name */
  gchar *identity;        /* Identity npub that signed */
  gchar *method;          /* NIP-46 method */
  GnEventHistoryResult result;  /* Operation result */
  gchar *content_preview; /* Truncated content preview */
};

G_DEFINE_FINAL_TYPE(GnEventHistoryEntry, gn_event_history_entry, G_TYPE_OBJECT)

static void
gn_event_history_entry_finalize(GObject *object)
{
  GnEventHistoryEntry *self = GN_EVENT_HISTORY_ENTRY(object);

  g_free(self->id);
  g_free(self->event_id);
  g_free(self->client_pubkey);
  g_free(self->client_app);
  g_free(self->identity);
  g_free(self->method);
  g_free(self->content_preview);

  G_OBJECT_CLASS(gn_event_history_entry_parent_class)->finalize(object);
}

static void
gn_event_history_entry_class_init(GnEventHistoryEntryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gn_event_history_entry_finalize;
}

static void
gn_event_history_entry_init(GnEventHistoryEntry *self)
{
  self->event_kind = -1;
  self->result = GN_EVENT_HISTORY_SUCCESS;
}

static GnEventHistoryEntry *
gn_event_history_entry_new(const gchar *event_id,
                            gint event_kind,
                            const gchar *client_pubkey,
                            const gchar *client_app,
                            const gchar *identity,
                            const gchar *method,
                            GnEventHistoryResult result,
                            const gchar *content_preview)
{
  GnEventHistoryEntry *self = g_object_new(GN_TYPE_EVENT_HISTORY_ENTRY, NULL);

  /* Generate unique ID */
  self->id = g_strdup_printf("%ld_%08x", (long)time(NULL), g_random_int());
  self->timestamp = (gint64)time(NULL);
  self->event_id = g_strdup(event_id);
  self->event_kind = event_kind;
  self->client_pubkey = g_strdup(client_pubkey);
  self->client_app = g_strdup(client_app);
  self->identity = g_strdup(identity);
  self->method = g_strdup(method ? method : "sign_event");
  self->result = result;
  self->content_preview = g_strdup(content_preview);

  return self;
}

const gchar *
gn_event_history_entry_get_id(GnEventHistoryEntry *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY_ENTRY(self), NULL);
  return self->id;
}

gint64
gn_event_history_entry_get_timestamp(GnEventHistoryEntry *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY_ENTRY(self), 0);
  return self->timestamp;
}

const gchar *
gn_event_history_entry_get_event_id(GnEventHistoryEntry *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY_ENTRY(self), NULL);
  return self->event_id;
}

gint
gn_event_history_entry_get_event_kind(GnEventHistoryEntry *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY_ENTRY(self), -1);
  return self->event_kind;
}

const gchar *
gn_event_history_entry_get_client_pubkey(GnEventHistoryEntry *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY_ENTRY(self), NULL);
  return self->client_pubkey;
}

const gchar *
gn_event_history_entry_get_client_app(GnEventHistoryEntry *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY_ENTRY(self), NULL);
  return self->client_app;
}

const gchar *
gn_event_history_entry_get_identity(GnEventHistoryEntry *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY_ENTRY(self), NULL);
  return self->identity;
}

const gchar *
gn_event_history_entry_get_method(GnEventHistoryEntry *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY_ENTRY(self), "sign_event");
  return self->method;
}

GnEventHistoryResult
gn_event_history_entry_get_result(GnEventHistoryEntry *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY_ENTRY(self), GN_EVENT_HISTORY_ERROR);
  return self->result;
}

const gchar *
gn_event_history_entry_get_content_preview(GnEventHistoryEntry *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY_ENTRY(self), NULL);
  return self->content_preview;
}

gchar *
gn_event_history_entry_get_truncated_event_id(GnEventHistoryEntry *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY_ENTRY(self), NULL);

  if (!self->event_id || strlen(self->event_id) < 12)
    return g_strdup(self->event_id);

  /* Format: first8...last4 */
  gsize len = strlen(self->event_id);
  return g_strdup_printf("%.8s...%.4s", self->event_id, self->event_id + len - 4);
}

gchar *
gn_event_history_entry_format_timestamp(GnEventHistoryEntry *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY_ENTRY(self), NULL);

  GDateTime *dt = g_date_time_new_from_unix_local(self->timestamp);
  if (!dt)
    return g_strdup("Unknown");

  gchar *formatted = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S");
  g_date_time_unref(dt);

  return formatted;
}

/* ============================================================================
 * GnEventHistory Implementation
 * ============================================================================ */

struct _GnEventHistory {
  GObject parent_instance;

  GPtrArray *entries;   /* Array of GnEventHistoryEntry */
  gchar *path;          /* File path for persistence */
  gboolean loaded;      /* Whether data has been loaded */
  gboolean dirty;       /* Whether data needs saving */
};

G_DEFINE_FINAL_TYPE(GnEventHistory, gn_event_history, G_TYPE_OBJECT)

/* Singleton instance */
static GnEventHistory *default_history = NULL;

static const gchar *
get_history_path(void)
{
  static gchar *path = NULL;
  if (!path) {
    const gchar *conf = g_get_user_config_dir();
    gchar *dir = g_build_filename(conf, "gnostr-signer", NULL);
    g_mkdir_with_parents(dir, 0700);
    path = g_build_filename(dir, "event_history.json", NULL);
    g_free(dir);
  }
  return path;
}

static void
gn_event_history_finalize(GObject *object)
{
  GnEventHistory *self = GN_EVENT_HISTORY(object);

  /* Save if dirty before finalizing */
  if (self->dirty)
    gn_event_history_save(self);

  if (self->entries)
    g_ptr_array_unref(self->entries);
  g_free(self->path);

  G_OBJECT_CLASS(gn_event_history_parent_class)->finalize(object);
}

static void
gn_event_history_class_init(GnEventHistoryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gn_event_history_finalize;
}

static void
gn_event_history_init(GnEventHistory *self)
{
  self->entries = g_ptr_array_new_with_free_func(g_object_unref);
  self->path = g_strdup(get_history_path());
  self->loaded = FALSE;
  self->dirty = FALSE;
}

GnEventHistory *
gn_event_history_new(void)
{
  return g_object_new(GN_TYPE_EVENT_HISTORY, NULL);
}

GnEventHistory *
gn_event_history_get_default(void)
{
  if (!default_history) {
    default_history = gn_event_history_new();
    gn_event_history_load(default_history);
  }
  return default_history;
}

static const gchar *
result_to_string(GnEventHistoryResult result)
{
  switch (result) {
    case GN_EVENT_HISTORY_SUCCESS: return "success";
    case GN_EVENT_HISTORY_DENIED: return "denied";
    case GN_EVENT_HISTORY_ERROR: return "error";
    case GN_EVENT_HISTORY_TIMEOUT: return "timeout";
    default: return "unknown";
  }
}

static GnEventHistoryResult
result_from_string(const gchar *str)
{
  if (!str) return GN_EVENT_HISTORY_ERROR;
  if (g_strcmp0(str, "success") == 0) return GN_EVENT_HISTORY_SUCCESS;
  if (g_strcmp0(str, "denied") == 0) return GN_EVENT_HISTORY_DENIED;
  if (g_strcmp0(str, "timeout") == 0) return GN_EVENT_HISTORY_TIMEOUT;
  return GN_EVENT_HISTORY_ERROR;
}

gboolean
gn_event_history_load(GnEventHistory *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY(self), FALSE);

  if (self->loaded)
    return TRUE;

  /* Clear existing entries */
  g_ptr_array_set_size(self->entries, 0);

  /* Check if file exists */
  if (!g_file_test(self->path, G_FILE_TEST_EXISTS)) {
    self->loaded = TRUE;
    return TRUE;
  }

  /* Load JSON file */
  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_file(parser, self->path, &error)) {
    g_warning("event_history: failed to load %s: %s", self->path, error->message);
    g_clear_error(&error);
    self->loaded = TRUE;
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
    g_warning("event_history: invalid format in %s", self->path);
    self->loaded = TRUE;
    return FALSE;
  }

  JsonArray *arr = json_node_get_array(root);
  guint len = json_array_get_length(arr);

  for (guint i = 0; i < len; i++) {
    JsonObject *obj = json_array_get_object_element(arr, i);
    if (!obj) continue;

    GnEventHistoryEntry *entry = g_object_new(GN_TYPE_EVENT_HISTORY_ENTRY, NULL);

    entry->id = g_strdup(json_object_get_string_member_with_default(obj, "id", ""));
    entry->timestamp = json_object_get_int_member_with_default(obj, "timestamp", 0);
    entry->event_id = g_strdup(json_object_get_string_member_with_default(obj, "event_id", NULL));
    entry->event_kind = (gint)json_object_get_int_member_with_default(obj, "event_kind", -1);
    entry->client_pubkey = g_strdup(json_object_get_string_member_with_default(obj, "client_pubkey", NULL));
    entry->client_app = g_strdup(json_object_get_string_member_with_default(obj, "client_app", NULL));
    entry->identity = g_strdup(json_object_get_string_member_with_default(obj, "identity", NULL));
    entry->method = g_strdup(json_object_get_string_member_with_default(obj, "method", "sign_event"));
    entry->result = result_from_string(json_object_get_string_member_with_default(obj, "result", "error"));
    entry->content_preview = g_strdup(json_object_get_string_member_with_default(obj, "content_preview", NULL));

    g_ptr_array_add(self->entries, entry);
  }

  self->loaded = TRUE;
  self->dirty = FALSE;

  g_debug("event_history: loaded %u entries from %s", len, self->path);
  return TRUE;
}

gboolean
gn_event_history_save(GnEventHistory *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY(self), FALSE);

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);

  for (guint i = 0; i < self->entries->len; i++) {
    GnEventHistoryEntry *entry = g_ptr_array_index(self->entries, i);

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, entry->id ? entry->id : "");

    json_builder_set_member_name(builder, "timestamp");
    json_builder_add_int_value(builder, entry->timestamp);

    if (entry->event_id) {
      json_builder_set_member_name(builder, "event_id");
      json_builder_add_string_value(builder, entry->event_id);
    }

    json_builder_set_member_name(builder, "event_kind");
    json_builder_add_int_value(builder, entry->event_kind);

    if (entry->client_pubkey) {
      json_builder_set_member_name(builder, "client_pubkey");
      json_builder_add_string_value(builder, entry->client_pubkey);
    }

    if (entry->client_app) {
      json_builder_set_member_name(builder, "client_app");
      json_builder_add_string_value(builder, entry->client_app);
    }

    if (entry->identity) {
      json_builder_set_member_name(builder, "identity");
      json_builder_add_string_value(builder, entry->identity);
    }

    json_builder_set_member_name(builder, "method");
    json_builder_add_string_value(builder, entry->method ? entry->method : "sign_event");

    json_builder_set_member_name(builder, "result");
    json_builder_add_string_value(builder, result_to_string(entry->result));

    if (entry->content_preview) {
      json_builder_set_member_name(builder, "content_preview");
      json_builder_add_string_value(builder, entry->content_preview);
    }

    json_builder_end_object(builder);
  }

  json_builder_end_array(builder);

  JsonNode *root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, TRUE);

  GError *error = NULL;
  gboolean success = json_generator_to_file(gen, self->path, &error);

  if (!success) {
    g_warning("event_history: failed to save %s: %s", self->path, error->message);
    g_clear_error(&error);
  } else {
    self->dirty = FALSE;
    g_debug("event_history: saved %u entries to %s", self->entries->len, self->path);
  }


  return success;
}

GnEventHistoryEntry *
gn_event_history_add_entry(GnEventHistory *self,
                            const gchar *event_id,
                            gint event_kind,
                            const gchar *client_pubkey,
                            const gchar *client_app,
                            const gchar *identity,
                            const gchar *method,
                            GnEventHistoryResult result,
                            const gchar *content_preview)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY(self), NULL);

  /* Ensure loaded */
  if (!self->loaded)
    gn_event_history_load(self);

  GnEventHistoryEntry *entry = gn_event_history_entry_new(
    event_id, event_kind, client_pubkey, client_app,
    identity, method, result, content_preview);

  /* Insert at beginning (newest first) */
  g_ptr_array_insert(self->entries, 0, entry);
  self->dirty = TRUE;

  /* Auto-save after adding */
  gn_event_history_save(self);

  g_debug("event_history: added entry kind=%d method=%s result=%s",
          event_kind, method ? method : "sign_event", result_to_string(result));

  return entry;
}

GPtrArray *
gn_event_history_list_entries(GnEventHistory *self,
                               guint offset,
                               guint limit)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY(self), NULL);

  if (!self->loaded)
    gn_event_history_load(self);

  GPtrArray *result = g_ptr_array_new_with_free_func(g_object_unref);

  guint count = 0;
  for (guint i = offset; i < self->entries->len; i++) {
    if (limit > 0 && count >= limit)
      break;

    GnEventHistoryEntry *entry = g_ptr_array_index(self->entries, i);
    g_ptr_array_add(result, g_object_ref(entry));
    count++;
  }

  return result;
}

GPtrArray *
gn_event_history_filter_by_kind(GnEventHistory *self,
                                 gint kind,
                                 guint offset,
                                 guint limit)
{
  return gn_event_history_filter(self, kind, NULL, 0, 0, offset, limit);
}

GPtrArray *
gn_event_history_filter_by_client(GnEventHistory *self,
                                   const gchar *client_pubkey,
                                   guint offset,
                                   guint limit)
{
  return gn_event_history_filter(self, -1, client_pubkey, 0, 0, offset, limit);
}

GPtrArray *
gn_event_history_filter_by_date_range(GnEventHistory *self,
                                        gint64 start_time,
                                        gint64 end_time,
                                        guint offset,
                                        guint limit)
{
  return gn_event_history_filter(self, -1, NULL, start_time, end_time, offset, limit);
}

GPtrArray *
gn_event_history_filter(GnEventHistory *self,
                         gint kind,
                         const gchar *client_pubkey,
                         gint64 start_time,
                         gint64 end_time,
                         guint offset,
                         guint limit)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY(self), NULL);

  if (!self->loaded)
    gn_event_history_load(self);

  GPtrArray *result = g_ptr_array_new_with_free_func(g_object_unref);

  guint skipped = 0;
  guint count = 0;

  for (guint i = 0; i < self->entries->len; i++) {
    GnEventHistoryEntry *entry = g_ptr_array_index(self->entries, i);

    /* Apply kind filter */
    if (kind >= 0 && entry->event_kind != kind)
      continue;

    /* Apply client filter */
    if (client_pubkey && g_strcmp0(entry->client_pubkey, client_pubkey) != 0)
      continue;

    /* Apply date range filter */
    if (start_time > 0 && entry->timestamp < start_time)
      continue;
    if (end_time > 0 && entry->timestamp > end_time)
      continue;

    /* Apply offset */
    if (skipped < offset) {
      skipped++;
      continue;
    }

    /* Apply limit */
    if (limit > 0 && count >= limit)
      break;

    g_ptr_array_add(result, g_object_ref(entry));
    count++;
  }

  return result;
}

guint
gn_event_history_get_entry_count(GnEventHistory *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY(self), 0);

  if (!self->loaded)
    gn_event_history_load(self);

  return self->entries->len;
}

gint *
gn_event_history_get_unique_kinds(GnEventHistory *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY(self), NULL);

  if (!self->loaded)
    gn_event_history_load(self);

  GHashTable *kinds = g_hash_table_new(g_direct_hash, g_direct_equal);

  for (guint i = 0; i < self->entries->len; i++) {
    GnEventHistoryEntry *entry = g_ptr_array_index(self->entries, i);
    g_hash_table_add(kinds, GINT_TO_POINTER(entry->event_kind));
  }

  guint size = g_hash_table_size(kinds);
  gint *result = g_new0(gint, size + 1);
  result[size] = -1;  /* Terminator */

  GHashTableIter iter;
  gpointer key;
  guint idx = 0;
  g_hash_table_iter_init(&iter, kinds);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    result[idx++] = GPOINTER_TO_INT(key);
  }

  g_hash_table_unref(kinds);
  return result;
}

gchar **
gn_event_history_get_unique_clients(GnEventHistory *self)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY(self), NULL);

  if (!self->loaded)
    gn_event_history_load(self);

  GHashTable *clients = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < self->entries->len; i++) {
    GnEventHistoryEntry *entry = g_ptr_array_index(self->entries, i);
    if (entry->client_pubkey && entry->client_pubkey[0])
      g_hash_table_add(clients, g_strdup(entry->client_pubkey));
  }

  guint size = g_hash_table_size(clients);
  gchar **result = g_new0(gchar*, size + 1);

  GHashTableIter iter;
  gpointer key;
  guint idx = 0;
  g_hash_table_iter_init(&iter, clients);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    result[idx++] = g_strdup(key);
  }

  g_hash_table_unref(clients);
  return result;
}

void
gn_event_history_clear(GnEventHistory *self)
{
  g_return_if_fail(GN_IS_EVENT_HISTORY(self));

  g_ptr_array_set_size(self->entries, 0);
  self->dirty = TRUE;
  gn_event_history_save(self);

  g_debug("event_history: cleared all entries");
}

gchar *
gn_event_history_export_json(GnEventHistory *self,
                              GPtrArray *entries,
                              gboolean pretty)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY(self), NULL);

  if (!self->loaded)
    gn_event_history_load(self);

  GPtrArray *to_export = entries ? entries : self->entries;

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);

  for (guint i = 0; i < to_export->len; i++) {
    GnEventHistoryEntry *entry = g_ptr_array_index(to_export, i);

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, entry->id ? entry->id : "");

    json_builder_set_member_name(builder, "timestamp");
    json_builder_add_int_value(builder, entry->timestamp);

    gchar *formatted_time = gn_event_history_entry_format_timestamp(entry);
    json_builder_set_member_name(builder, "timestamp_formatted");
    json_builder_add_string_value(builder, formatted_time);
    g_free(formatted_time);

    json_builder_set_member_name(builder, "event_id");
    json_builder_add_string_value(builder, entry->event_id ? entry->event_id : "");

    json_builder_set_member_name(builder, "event_kind");
    json_builder_add_int_value(builder, entry->event_kind);

    json_builder_set_member_name(builder, "client_pubkey");
    json_builder_add_string_value(builder, entry->client_pubkey ? entry->client_pubkey : "");

    json_builder_set_member_name(builder, "client_app");
    json_builder_add_string_value(builder, entry->client_app ? entry->client_app : "");

    json_builder_set_member_name(builder, "identity");
    json_builder_add_string_value(builder, entry->identity ? entry->identity : "");

    json_builder_set_member_name(builder, "method");
    json_builder_add_string_value(builder, entry->method ? entry->method : "");

    json_builder_set_member_name(builder, "result");
    json_builder_add_string_value(builder, result_to_string(entry->result));

    json_builder_set_member_name(builder, "content_preview");
    json_builder_add_string_value(builder, entry->content_preview ? entry->content_preview : "");

    json_builder_end_object(builder);
  }

  json_builder_end_array(builder);

  JsonNode *root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, pretty);

  gchar *json_str = json_generator_to_data(gen, NULL);


  return json_str;
}

gchar *
gn_event_history_export_csv(GnEventHistory *self,
                             GPtrArray *entries)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY(self), NULL);

  if (!self->loaded)
    gn_event_history_load(self);

  GPtrArray *to_export = entries ? entries : self->entries;

  GString *csv = g_string_new("");

  /* CSV header */
  g_string_append(csv, "id,timestamp,timestamp_formatted,event_id,event_kind,"
                       "client_pubkey,client_app,identity,method,result,content_preview\n");

  for (guint i = 0; i < to_export->len; i++) {
    GnEventHistoryEntry *entry = g_ptr_array_index(to_export, i);

    gchar *formatted_time = gn_event_history_entry_format_timestamp(entry);

    /* Escape fields that might contain commas or quotes */
    gchar *escaped_preview = NULL;
    if (entry->content_preview) {
      GString *esc = g_string_new("");
      g_string_append_c(esc, '"');
      for (const gchar *p = entry->content_preview; *p; p++) {
        if (*p == '"') g_string_append(esc, "\"\"");
        else g_string_append_c(esc, *p);
      }
      g_string_append_c(esc, '"');
      escaped_preview = g_string_free(esc, FALSE);
    }

    gchar *escaped_app = NULL;
    if (entry->client_app) {
      GString *esc = g_string_new("");
      g_string_append_c(esc, '"');
      for (const gchar *p = entry->client_app; *p; p++) {
        if (*p == '"') g_string_append(esc, "\"\"");
        else g_string_append_c(esc, *p);
      }
      g_string_append_c(esc, '"');
      escaped_app = g_string_free(esc, FALSE);
    }

    g_string_append_printf(csv, "%s,%ld,%s,%s,%d,%s,%s,%s,%s,%s,%s\n",
      entry->id ? entry->id : "",
      (long)entry->timestamp,
      formatted_time,
      entry->event_id ? entry->event_id : "",
      entry->event_kind,
      entry->client_pubkey ? entry->client_pubkey : "",
      escaped_app ? escaped_app : (entry->client_app ? entry->client_app : ""),
      entry->identity ? entry->identity : "",
      entry->method ? entry->method : "",
      result_to_string(entry->result),
      escaped_preview ? escaped_preview : "");

    g_free(formatted_time);
    g_free(escaped_preview);
    g_free(escaped_app);
  }

  return g_string_free(csv, FALSE);
}

gboolean
gn_event_history_export_to_file(GnEventHistory *self,
                                 const gchar *path,
                                 const gchar *format,
                                 GPtrArray *entries,
                                 GError **error)
{
  g_return_val_if_fail(GN_IS_EVENT_HISTORY(self), FALSE);
  g_return_val_if_fail(path != NULL, FALSE);
  g_return_val_if_fail(format != NULL, FALSE);

  gchar *content = NULL;

  if (g_strcmp0(format, "json") == 0) {
    content = gn_event_history_export_json(self, entries, TRUE);
  } else if (g_strcmp0(format, "csv") == 0) {
    content = gn_event_history_export_csv(self, entries);
  } else {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Unknown export format: %s (expected 'json' or 'csv')", format);
    return FALSE;
  }

  if (!content) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to generate export content");
    return FALSE;
  }

  gboolean success = g_file_set_contents(path, content, -1, error);
  g_free(content);

  if (success) {
    g_debug("event_history: exported to %s (%s format)", path, format);
  }

  return success;
}
