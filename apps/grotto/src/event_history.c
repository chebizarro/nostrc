/* event_history.c - Transaction/event history storage implementation
 *
 * SPDX-License-Identifier: MIT
 */
#include "event_history.h"

#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>
#include <glib/gstdio.h>
#include "../../../libnostr/include/nostrc-profile-guard.h"

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
  GVariant *metadata;     /* Daemon activity: allowlisted fields only */
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
  g_clear_pointer(&self->metadata, g_variant_unref);

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
  gboolean activity_mode, activity_healthy, activity_writable;
  guint64 activity_sequence;
  guint activity_timer, activity_retention_days;
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
    gchar *dir = g_build_filename(conf, "grotto", NULL);
    path = g_build_filename(dir, "event_history.json", NULL);
    g_free(dir);
  }
  return path;
}

static void
gn_event_history_finalize(GObject *object)
{
  GnEventHistory *self = GN_EVENT_HISTORY(object);

  if (self->activity_timer) g_source_remove(self->activity_timer);

  /* Save if dirty before finalizing */
  if (self->dirty && !self->activity_mode)
    gn_event_history_save(self);

  g_clear_pointer(&self->entries, g_ptr_array_unref);
  g_free(self->path);

  G_OBJECT_CLASS(gn_event_history_parent_class)->finalize(object);
}

static void
gn_event_history_class_init(GnEventHistoryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gn_event_history_finalize;
  g_signal_new("activity-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT64);
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
  g_autofree gchar *dir = g_path_get_dirname(self->path);
  gboolean success = !nostrc_profile_guard_refuses_path(self->path) &&
      g_mkdir_with_parents(dir, 0700) == 0 && json_generator_to_file(gen, self->path, &error);

  if (!success) {
    g_warning("event_history: failed to save %s: %s", self->path, error ? error->message : "profile/directory unavailable");
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

/* G04: the same history manager, in daemon metadata mode. The old process-local
 * GUI/bunker entries are not evidence of daemon operations and are not migrated. */
#define ACTIVITY_MAX 10000u
#define ACTIVITY_AGE ((self->activity_retention_days ? self->activity_retention_days : 30) * 24 * 60 * 60)
static const gchar *activity_strings[] = {
  "request_id", "transport", "principal", "identity", "operation", "outcome", "error_code"
};
/* Error names only, never a backend message (which may contain secrets). */
static gboolean activity_error_code_ok(const gchar *s) {
  if (strlen(s) > 160) return FALSE;
  for (const gchar *p = s; *p; p++) if (!g_ascii_isalnum(*p) && *p != '.' && *p != '_') return FALSE;
  return TRUE;
}
static gboolean activity_outcome_ok(const gchar *s) {
  const gchar *names[] = {"success", "denied", "expired", "expired-caller", "cancelled", "failed"};
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) if (!g_strcmp0(s, names[i])) return TRUE;
  return FALSE;
}
static void activity_prune(GnEventHistory *self) {
  gint64 oldest = g_get_real_time() / G_USEC_PER_SEC - ACTIVITY_AGE;
  for (guint i = self->entries->len; i > 0; i--) {
    GnEventHistoryEntry *entry = self->entries->pdata[i - 1];
    if (entry->timestamp < oldest) g_ptr_array_remove_index(self->entries, i - 1);
  }
  if (self->entries->len > ACTIVITY_MAX) g_ptr_array_set_size(self->entries, ACTIVITY_MAX);
}
static gboolean activity_save(GnEventHistory *self, GError **error);
static gboolean activity_retention_due(gpointer data) {
  GnEventHistory *self = data; self->activity_timer = 0;
  activity_save(self, NULL); return G_SOURCE_REMOVE;
}
static void activity_schedule_retention(GnEventHistory *self) {
  if (self->activity_timer) { g_source_remove(self->activity_timer); self->activity_timer = 0; }
  if (!self->entries->len) return;
  gint64 oldest = G_MAXINT64, now = g_get_real_time() / G_USEC_PER_SEC;
  for (guint i = 0; i < self->entries->len; i++)
    oldest = MIN(oldest, ((GnEventHistoryEntry *)self->entries->pdata[i])->timestamp);
  /* A single expiry source, not a polling loop or a protocol timeout. */
  gint64 remaining = oldest >= now ? ACTIVITY_AGE : ACTIVITY_AGE - (now - oldest) + 1;
  guint seconds = (guint)CLAMP(remaining, 1, ACTIVITY_AGE);
  self->activity_timer = g_timeout_add_seconds(seconds, activity_retention_due, self);
}
static gboolean activity_save(GnEventHistory *self, GError **error) {
  /* The bounds hold in memory whether or not the store can persist them:
   * a read-only (unsupported/corrupt) store keeps recording while signing
   * continues, so it must not grow without limit. */
  activity_prune(self);
  if (!self->activity_writable || nostrc_profile_guard_refuses_path(self->path)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "activity store unavailable or unsupported schema");
    self->activity_healthy = FALSE; activity_schedule_retention(self);
    g_signal_emit_by_name(self, "activity-changed", self->activity_sequence); return FALSE;
  }
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "version"); json_builder_add_int_value(builder, 1);
  json_builder_set_member_name(builder, "sequence"); json_builder_add_int_value(builder, self->activity_sequence);
  json_builder_set_member_name(builder, "records"); json_builder_begin_array(builder);
  for (guint i = 0; i < self->entries->len; i++) {
    GnEventHistoryEntry *entry = self->entries->pdata[i];
    json_builder_add_value(builder, json_gvariant_serialize(entry->metadata));
  }
  json_builder_end_array(builder); json_builder_end_object(builder);
  g_autoptr(JsonNode) root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) gen = json_generator_new(); json_generator_set_root(gen, root);
  gsize size; g_autofree gchar *data = json_generator_to_data(gen, &size);
  g_autofree gchar *dir = g_path_get_dirname(self->path);
  gboolean ok = g_mkdir_with_parents(dir, 0700) == 0;
  if (!ok) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "cannot create activity directory");
  if (ok) ok = g_file_set_contents_full(self->path, data, size,
      G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
  self->activity_healthy = ok; activity_schedule_retention(self);
  g_signal_emit_by_name(self, "activity-changed", self->activity_sequence); return ok;
}
static gboolean json_integer(JsonObject *obj, const gchar *key, gint64 *out) {
  JsonNode *n = json_object_get_member(obj, key);
  if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64) return FALSE;
  *out = json_node_get_int(n); return TRUE;
}
static const gchar *json_text(JsonObject *obj, const gchar *key) {
  JsonNode *n = json_object_get_member(obj, key);
  return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : NULL;
}
GnEventHistory *gn_event_history_new_activity(const gchar *config_home) {
  GnEventHistory *self = gn_event_history_new(); self->activity_mode = TRUE; self->loaded = TRUE;
  g_free(self->path);
  self->path = g_build_filename(config_home ? config_home : g_get_user_config_dir(), "grotto", "activity.json", NULL);
  self->activity_healthy = self->activity_writable = !nostrc_profile_guard_refuses_path(self->path);
  if (!self->activity_writable) return self;
  g_autoptr(JsonParser) parser = json_parser_new(); g_autoptr(GError) error = NULL;
  if (!json_parser_load_from_file(parser, self->path, &error)) {
    if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) self->activity_healthy = self->activity_writable = FALSE;
    return self;
  }
  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) goto invalid;
  JsonObject *obj = json_node_get_object(root); gint64 version, sequence;
  if (!json_integer(obj, "version", &version) || version != 1 || !json_integer(obj, "sequence", &sequence) || sequence < 0) goto invalid;
  JsonNode *records = json_object_get_member(obj, "records");
  if (!records || !JSON_NODE_HOLDS_ARRAY(records)) goto invalid;
  self->activity_sequence = sequence;
  JsonArray *arr = json_node_get_array(records); guint64 previous = (guint64)sequence + 1;
  for (guint i = 0; i < json_array_get_length(arr); i++) {
    JsonNode *node = json_array_get_element(arr, i); gint64 seq, timestamp, kind;
    if (!JSON_NODE_HOLDS_OBJECT(node)) goto invalid;
    JsonObject *record = json_node_get_object(node);
    if (!json_integer(record, "sequence", &seq) || seq <= 0 || (guint64)seq >= previous ||
        !json_integer(record, "timestamp", &timestamp) || timestamp <= 0) goto invalid;
    previous = seq;
    GVariantBuilder row; g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&row, "{sv}", "sequence", g_variant_new_uint64(seq));
    g_variant_builder_add(&row, "{sv}", "timestamp", g_variant_new_uint64(timestamp));
    for (guint j = 0; j < G_N_ELEMENTS(activity_strings); j++) {
      const gchar *s = json_text(record, activity_strings[j]);
      if (!s || strlen(s) > 1024 || !g_utf8_validate(s, -1, NULL) ||
          (!strcmp(activity_strings[j], "outcome") && !activity_outcome_ok(s)) ||
          (!strcmp(activity_strings[j], "error_code") && !activity_error_code_ok(s))) {
        g_variant_builder_clear(&row); goto invalid;
      }
      g_variant_builder_add(&row, "{sv}", activity_strings[j], g_variant_new_string(s));
    }
    if (json_object_has_member(record, "event_kind")) {
      if (!json_integer(record, "event_kind", &kind) || kind < 0 || kind > G_MAXINT32) { g_variant_builder_clear(&row); goto invalid; }
      g_variant_builder_add(&row, "{sv}", "event_kind", g_variant_new_uint32(kind));
    }
    GnEventHistoryEntry *entry = g_object_new(GN_TYPE_EVENT_HISTORY_ENTRY, NULL);
    entry->timestamp = timestamp; entry->metadata = g_variant_ref_sink(g_variant_builder_end(&row));
    g_ptr_array_add(self->entries, entry);
  }
  guint before = self->entries->len; activity_prune(self);
  if (before != self->entries->len) activity_save(self, NULL);
  else activity_schedule_retention(self);
  return self;
invalid:
  g_ptr_array_set_size(self->entries, 0); self->activity_healthy = self->activity_writable = FALSE;
  return self;
}
static const gchar *activity_outcome(GVariant *snapshot) {
  const gchar *state = NULL; gboolean gone = FALSE;
  g_variant_lookup(snapshot, "state", "&s", &state);
  g_variant_lookup(snapshot, "caller_disconnected", "b", &gone);
  if (gone) return "expired-caller";
  if (!g_strcmp0(state, "SUCCEEDED")) return "success";
  if (!g_strcmp0(state, "DENIED")) return "denied";
  if (!g_strcmp0(state, "EXPIRED")) return "expired";
  if (!g_strcmp0(state, "CANCELLED")) return "cancelled";
  if (!g_strcmp0(state, "FAILED")) return "failed";
  return NULL;
}
guint64 gn_event_history_record_activity(GnEventHistory *self, GVariant *snapshot) {
  g_return_val_if_fail(self->activity_mode, 0);
  const gchar *outcome = activity_outcome(snapshot);
  if (!outcome) return 0;
  if (self->activity_sequence == G_MAXINT64) {
    self->activity_healthy = FALSE;
    g_signal_emit_by_name(self, "activity-changed", self->activity_sequence);
    return 0;
  }
  GnEventHistoryEntry *entry = g_object_new(GN_TYPE_EVENT_HISTORY_ENTRY, NULL);
  entry->timestamp = g_get_real_time() / G_USEC_PER_SEC;
  GVariantBuilder row; g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&row, "{sv}", "sequence", g_variant_new_uint64(++self->activity_sequence));
  g_variant_builder_add(&row, "{sv}", "timestamp", g_variant_new_uint64(entry->timestamp));
  for (guint i = 0; i < G_N_ELEMENTS(activity_strings); i++) {
    const gchar *key = activity_strings[i], *s = "";
    if (!strcmp(key, "outcome")) s = outcome;
    else g_variant_lookup(snapshot, key, "&s", &s);
    if (strlen(s) > 1024 || !g_utf8_validate(s, -1, NULL)) s = "";
    if (!strcmp(key, "error_code") && !activity_error_code_ok(s)) s = "";
    g_variant_builder_add(&row, "{sv}", key, g_variant_new_string(s));
  }
  guint32 kind;
  if (g_variant_lookup(snapshot, "event_kind", "u", &kind))
    g_variant_builder_add(&row, "{sv}", "event_kind", g_variant_new_uint32(kind));
  entry->metadata = g_variant_ref_sink(g_variant_builder_end(&row));
  g_ptr_array_insert(self->entries, 0, entry); activity_save(self, NULL);
  return self->activity_sequence;
}
GVariant *gn_event_history_list_activity(GnEventHistory *self, guint64 before, guint limit) {
  g_return_val_if_fail(self->activity_mode, NULL);
  guint before_count = self->entries->len; activity_prune(self);
  if (before_count != self->entries->len) activity_save(self, NULL);
  GVariantBuilder rows; g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
  guint added = 0; limit = MIN(limit, 100);
  for (guint i = 0; i < self->entries->len && added < limit; i++) {
    GnEventHistoryEntry *entry = self->entries->pdata[i]; guint64 seq = 0;
    g_variant_lookup(entry->metadata, "sequence", "t", &seq);
    if (before && seq >= before) continue;
    g_variant_builder_add_value(&rows, entry->metadata); added++;
  }
  return g_variant_ref_sink(g_variant_builder_end(&rows));
}
gboolean gn_event_history_clear_activity(GnEventHistory *self, GError **error) {
  g_return_val_if_fail(self->activity_mode, FALSE);
  GPtrArray *old = self->entries; self->entries = g_ptr_array_new_with_free_func(g_object_unref);
  if (!activity_save(self, error)) {
    g_ptr_array_unref(self->entries); self->entries = old; return FALSE;
  }
  g_ptr_array_unref(old); return TRUE;
}
gboolean gn_event_history_activity_healthy(GnEventHistory *self) { return self->activity_healthy; }
guint64 gn_event_history_activity_sequence(GnEventHistory *self) { return self->activity_sequence; }
void gn_event_history_set_retention_days(GnEventHistory *self, guint days) {
  g_return_if_fail(self->activity_mode);
  self->activity_retention_days = CLAMP(days, 1, 30);
  if (self->entries->len) activity_save(self, NULL);
  else activity_schedule_retention(self);
}
guint gn_event_history_signed_today(GnEventHistory *self) {
  g_return_val_if_fail(self->activity_mode, 0);
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autoptr(GDateTime) midnight = g_date_time_new_local(g_date_time_get_year(now), g_date_time_get_month(now), g_date_time_get_day_of_month(now), 0, 0, 0);
  gint64 start = g_date_time_to_unix(midnight); guint count = 0;
  for (guint i = 0; i < self->entries->len; i++) {
    GnEventHistoryEntry *entry = self->entries->pdata[i]; const gchar *op = NULL, *outcome = NULL;
    g_variant_lookup(entry->metadata, "operation", "&s", &op); g_variant_lookup(entry->metadata, "outcome", "&s", &outcome);
    if (entry->timestamp >= start && !g_strcmp0(op, "sign_event") && !g_strcmp0(outcome, "success")) count++;
  }
  return count;
}
