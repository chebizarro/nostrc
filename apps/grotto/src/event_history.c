/* event_history.c - Daemon activity metadata storage
 *
 * SPDX-License-Identifier: MIT
 */
#include "event_history.h"

#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>
#include <glib/gstdio.h>
#include "../../../libnostr/include/nostrc-profile-guard.h"

typedef struct {
  gint64 timestamp;
  GVariant *metadata;
} ActivityRecord;

static void activity_record_free(gpointer data) {
  ActivityRecord *record = data;
  g_variant_unref(record->metadata);
  g_free(record);
}

struct _GnEventHistory {
  GObject parent_instance;

  GPtrArray *entries;
  gchar *path;          /* File path for persistence */
  gboolean activity_healthy, activity_writable;
  guint64 activity_sequence;
  guint activity_timer, activity_retention_days;
};

G_DEFINE_FINAL_TYPE(GnEventHistory, gn_event_history, G_TYPE_OBJECT)

static void
gn_event_history_finalize(GObject *object)
{
  GnEventHistory *self = GN_EVENT_HISTORY(object);

  if (self->activity_timer) g_source_remove(self->activity_timer);

  g_ptr_array_unref(self->entries);
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
  self->entries = g_ptr_array_new_with_free_func(activity_record_free);
}

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
    ActivityRecord *entry = self->entries->pdata[i - 1];
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
    oldest = MIN(oldest, ((ActivityRecord *)self->entries->pdata[i])->timestamp);
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
    ActivityRecord *entry = self->entries->pdata[i];
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
  GnEventHistory *self = g_object_new(GN_TYPE_EVENT_HISTORY, NULL);
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
    ActivityRecord *entry = g_new0(ActivityRecord, 1);
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
  g_return_val_if_fail(GN_IS_EVENT_HISTORY(self), 0);
  const gchar *outcome = activity_outcome(snapshot);
  if (!outcome) return 0;
  if (self->activity_sequence == G_MAXINT64) {
    self->activity_healthy = FALSE;
    g_signal_emit_by_name(self, "activity-changed", self->activity_sequence);
    return 0;
  }
  ActivityRecord *entry = g_new0(ActivityRecord, 1);
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
  g_return_val_if_fail(GN_IS_EVENT_HISTORY(self), NULL);
  guint before_count = self->entries->len; activity_prune(self);
  if (before_count != self->entries->len) activity_save(self, NULL);
  GVariantBuilder rows; g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
  guint added = 0; limit = MIN(limit, 100);
  for (guint i = 0; i < self->entries->len && added < limit; i++) {
    ActivityRecord *entry = self->entries->pdata[i]; guint64 seq = 0;
    g_variant_lookup(entry->metadata, "sequence", "t", &seq);
    if (before && seq >= before) continue;
    g_variant_builder_add_value(&rows, entry->metadata); added++;
  }
  return g_variant_ref_sink(g_variant_builder_end(&rows));
}
gboolean gn_event_history_clear_activity(GnEventHistory *self, GError **error) {
  g_return_val_if_fail(GN_IS_EVENT_HISTORY(self), FALSE);
  GPtrArray *old = self->entries; self->entries = g_ptr_array_new_with_free_func(activity_record_free);
  if (!activity_save(self, error)) {
    g_ptr_array_unref(self->entries); self->entries = old; return FALSE;
  }
  g_ptr_array_unref(old); return TRUE;
}
gboolean gn_event_history_activity_healthy(GnEventHistory *self) { return self->activity_healthy; }
/* Does not prune; tests use this to inspect the bound after writes. */
guint gn_event_history_activity_count(GnEventHistory *self) { return self->entries->len; }
guint64 gn_event_history_activity_sequence(GnEventHistory *self) { return self->activity_sequence; }
void gn_event_history_set_retention_days(GnEventHistory *self, guint days) {
  g_return_if_fail(GN_IS_EVENT_HISTORY(self));
  self->activity_retention_days = CLAMP(days, 1, 30);
  if (self->entries->len) activity_save(self, NULL);
  else activity_schedule_retention(self);
}
guint gn_event_history_signed_today(GnEventHistory *self) {
  g_return_val_if_fail(GN_IS_EVENT_HISTORY(self), 0);
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autoptr(GDateTime) midnight = g_date_time_new_local(g_date_time_get_year(now), g_date_time_get_month(now), g_date_time_get_day_of_month(now), 0, 0, 0);
  gint64 start = g_date_time_to_unix(midnight); guint count = 0;
  for (guint i = 0; i < self->entries->len; i++) {
    ActivityRecord *entry = self->entries->pdata[i]; const gchar *op = NULL, *outcome = NULL;
    g_variant_lookup(entry->metadata, "operation", "&s", &op); g_variant_lookup(entry->metadata, "outcome", "&s", &outcome);
    if (entry->timestamp >= start && !g_strcmp0(op, "sign_event") && !g_strcmp0(outcome, "success")) count++;
  }
  return count;
}
