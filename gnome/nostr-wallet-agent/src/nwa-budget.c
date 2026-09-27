/* nwa-budget.c - see nwa-budget.h
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-budget.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <string.h>

typedef struct {
  guint64  limit;
  gboolean allow_read;
  gboolean allow_receive;
  gchar   *day;
  guint64  spent;
} AppRecord;

typedef struct {
  gchar   *app_id;
  gchar   *day;     /* day the amount was charged to */
  guint64  amount;
} Reservation;

struct _NwaBudgetStore {
  gchar        *path;
  NwaClockFunc  clock;
  gpointer      clock_data;
  GHashTable   *apps;          /* app_id -> AppRecord* */
  GHashTable   *reservations;  /* GUINT_TO_POINTER(id) -> Reservation* */
  guint         next_id;
};

static void
app_record_free(gpointer p)
{
  AppRecord *r = p;
  g_free(r->day);
  g_free(r);
}

static void
reservation_free(gpointer p)
{
  Reservation *r = p;
  g_free(r->app_id);
  g_free(r->day);
  g_free(r);
}

guint64
nwa_budget_fee_reserve(guint64 amount_msat)
{
  return MAX(amount_msat / 100, 1000);
}

gchar *
nwa_budget_store_default_path(void)
{
  return g_build_filename(g_get_user_state_dir(), "nostr-wallet", "budgets.json", NULL);
}

NwaBudgetStore *
nwa_budget_store_new(const gchar *path, NwaClockFunc clock, gpointer clock_data)
{
  NwaBudgetStore *self = g_new0(NwaBudgetStore, 1);
  self->path = g_strdup(path);
  self->clock = clock;
  self->clock_data = clock_data;
  self->apps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, app_record_free);
  self->reservations = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, reservation_free);
  self->next_id = 1;
  return self;
}

void
nwa_budget_store_free(NwaBudgetStore *self)
{
  if (!self) return;
  g_hash_table_unref(self->apps);
  g_hash_table_unref(self->reservations);
  g_free(self->path);
  g_free(self);
}

static gchar *
today(NwaBudgetStore *self)
{
  GDateTime *now = self->clock ? self->clock(self->clock_data) : g_date_time_new_now_local();
  gchar *s = g_date_time_format(now, "%Y-%m-%d");
  g_date_time_unref(now);
  return s;
}

/* Returns the record with its spend rolled over to today (NULL if unknown). */
static AppRecord *
lookup(NwaBudgetStore *self, const gchar *app_id, gboolean create)
{
  if (!app_id || !*app_id) return NULL;
  AppRecord *r = g_hash_table_lookup(self->apps, app_id);
  g_autofree gchar *d = today(self);
  if (!r) {
    if (!create) return NULL;
    r = g_new0(AppRecord, 1);
    r->day = g_strdup(d);
    g_hash_table_insert(self->apps, g_strdup(app_id), r);
  } else if (g_strcmp0(r->day, d) != 0) {
    g_free(r->day);
    r->day = g_strdup(d);
    r->spent = 0;
  }
  return r;
}

static void
autosave(NwaBudgetStore *self)
{
  GError *err = NULL;
  if (self->path && !nwa_budget_store_save(self, &err)) {
    g_warning("nostr-wallet-agent: could not save budgets to %s: %s",
              self->path, err ? err->message : "unknown");
    g_clear_error(&err);
  }
}

void
nwa_budget_store_get(NwaBudgetStore *self, const gchar *app_id, NwaBudgetInfo *out)
{
  memset(out, 0, sizeof *out);
  AppRecord *r = lookup(self, app_id, FALSE);
  if (!r) return;
  out->known = TRUE;
  out->limit_msat_per_day = r->limit;
  out->spent_today_msat = r->spent;
  out->remaining_msat = r->limit > r->spent ? r->limit - r->spent : 0;
  out->allow_read = r->allow_read;
  out->allow_receive = r->allow_receive;
}

void
nwa_budget_store_set_limit(NwaBudgetStore *self, const gchar *app_id, guint64 limit)
{
  AppRecord *r = lookup(self, app_id, TRUE);
  if (!r) return;
  r->limit = limit;
  autosave(self);
}

void
nwa_budget_store_set_allow_read(NwaBudgetStore *self, const gchar *app_id, gboolean allow)
{
  AppRecord *r = lookup(self, app_id, TRUE);
  if (!r) return;
  r->allow_read = allow;
  autosave(self);
}

void
nwa_budget_store_set_allow_receive(NwaBudgetStore *self, const gchar *app_id, gboolean allow)
{
  AppRecord *r = lookup(self, app_id, TRUE);
  if (!r) return;
  r->allow_receive = allow;
  autosave(self);
}

static gint
cmp_str(gconstpointer a, gconstpointer b)
{
  return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

GStrv
nwa_budget_store_list_apps(NwaBudgetStore *self)
{
  GPtrArray *ids = g_ptr_array_new();
  GHashTableIter it;
  gpointer k;
  g_hash_table_iter_init(&it, self->apps);
  while (g_hash_table_iter_next(&it, &k, NULL))
    g_ptr_array_add(ids, g_strdup(k));
  g_ptr_array_sort(ids, cmp_str);
  g_ptr_array_add(ids, NULL);
  return (GStrv)g_ptr_array_free(ids, FALSE);
}

guint
nwa_budget_store_reserve(NwaBudgetStore *self, const gchar *app_id,
                         guint64 amount, gboolean force)
{
  if (amount == 0) return 0;
  AppRecord *r = lookup(self, app_id, TRUE);
  if (!r) return 0;
  guint64 remaining = r->limit > r->spent ? r->limit - r->spent : 0;
  /* force (user-approved) never consults the limit; the record is then a
   * ledger only — e.g. the shared "(unidentified)" key. */
  if (!force && amount > remaining)
    return 0;
  r->spent = (G_MAXUINT64 - r->spent < amount) ? G_MAXUINT64 : r->spent + amount;

  Reservation *res = g_new0(Reservation, 1);
  res->app_id = g_strdup(app_id);
  res->day = g_strdup(r->day);
  res->amount = amount;
  guint id = self->next_id++;
  if (self->next_id == 0) self->next_id = 1;
  g_hash_table_insert(self->reservations, GUINT_TO_POINTER(id), res);
  autosave(self);
  return id;
}

void
nwa_budget_store_commit(NwaBudgetStore *self, guint reservation, guint64 final_msat)
{
  Reservation *res = g_hash_table_lookup(self->reservations, GUINT_TO_POINTER(reservation));
  if (!res) return;
  AppRecord *r = lookup(self, res->app_id, TRUE);
  if (r) {
    if (g_strcmp0(r->day, res->day) == 0) {
      /* same day: swap the reservation for the final amount */
      guint64 base = r->spent >= res->amount ? r->spent - res->amount : 0;
      r->spent = (G_MAXUINT64 - base < final_msat) ? G_MAXUINT64 : base + final_msat;
    } else {
      /* rolled over mid-flight: yesterday's reservation expired with
       * yesterday; charge the final amount to today */
      r->spent = (G_MAXUINT64 - r->spent < final_msat) ? G_MAXUINT64 : r->spent + final_msat;
    }
  }
  g_hash_table_remove(self->reservations, GUINT_TO_POINTER(reservation));
  autosave(self);
}

void
nwa_budget_store_release(NwaBudgetStore *self, guint reservation)
{
  Reservation *res = g_hash_table_lookup(self->reservations, GUINT_TO_POINTER(reservation));
  if (!res) return;
  AppRecord *r = lookup(self, res->app_id, FALSE);
  if (r && g_strcmp0(r->day, res->day) == 0)
    r->spent = r->spent >= res->amount ? r->spent - res->amount : 0;
  g_hash_table_remove(self->reservations, GUINT_TO_POINTER(reservation));
  autosave(self);
}

gboolean
nwa_budget_store_save(NwaBudgetStore *self, GError **error)
{
  if (!self->path) return TRUE;
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "version");
  json_builder_add_int_value(b, 2);
  json_builder_set_member_name(b, "apps");
  json_builder_begin_object(b);
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, self->apps);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    AppRecord *r = v;
    json_builder_set_member_name(b, k);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "limit_msat_per_day");
    json_builder_add_int_value(b, (gint64)MIN(r->limit, (guint64)G_MAXINT64));
    json_builder_set_member_name(b, "allow_read");
    json_builder_add_boolean_value(b, r->allow_read);
    json_builder_set_member_name(b, "allow_receive");
    json_builder_add_boolean_value(b, r->allow_receive);
    json_builder_set_member_name(b, "day");
    json_builder_add_string_value(b, r->day ? r->day : "");
    json_builder_set_member_name(b, "spent_msat");
    json_builder_add_int_value(b, (gint64)MIN(r->spent, (guint64)G_MAXINT64));
    json_builder_end_object(b);
  }
  json_builder_end_object(b);
  json_builder_end_object(b);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, TRUE);
  g_autofree gchar *data = json_generator_to_data(gen, NULL);

  g_autofree gchar *dir = g_path_get_dirname(self->path);
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    int e = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e),
                "cannot create %s: %s", dir, g_strerror(e));
    return FALSE;
  }
  return g_file_set_contents_full(self->path, data, -1,
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

static guint64
member_u64(JsonObject *o, const gchar *name)
{
  if (!json_object_has_member(o, name)) return 0;
  JsonNode *n = json_object_get_member(o, name);
  if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64) return 0;
  gint64 v = json_node_get_int(n);
  return v > 0 ? (guint64)v : 0;
}

gboolean
nwa_budget_store_load(NwaBudgetStore *self, GError **error)
{
  g_hash_table_remove_all(self->apps);
  if (!self->path) return TRUE;

  g_autofree gchar *data = NULL;
  gsize len = 0;
  GError *err = NULL;
  if (!g_file_get_contents(self->path, &data, &len, &err)) {
    if (g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_clear_error(&err);
      return TRUE;
    }
    g_propagate_error(error, err);
    return FALSE;
  }

  g_autoptr(JsonParser) p = json_parser_new();
  gboolean ok = json_parser_load_from_data(p, data, (gssize)len, NULL);
  JsonNode *root = ok ? json_parser_get_root(p) : NULL;
  JsonObject *apps = NULL;
  gint64 version = 1;
  if (root && JSON_NODE_HOLDS_OBJECT(root)) {
    JsonObject *ro = json_node_get_object(root);
    version = json_object_get_int_member_with_default(ro, "version", 1);
    if (json_object_has_member(ro, "apps") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(ro, "apps")))
      apps = json_object_get_object_member(ro, "apps");
  }
  if (!apps) {
    g_autofree gchar *bad = g_strconcat(self->path, ".corrupt", NULL);
    g_warning("nostr-wallet-agent: %s is not a valid budget file; moved to %s",
              self->path, bad);
    (void)g_rename(self->path, bad);
    return TRUE;
  }

  GList *members = json_object_get_members(apps);
  for (GList *l = members; l; l = l->next) {
    const gchar *id = l->data;
    JsonNode *n = json_object_get_member(apps, id);
    if (!id || !*id || !JSON_NODE_HOLDS_OBJECT(n)) continue;
    JsonObject *o = json_node_get_object(n);
    AppRecord *r = g_new0(AppRecord, 1);
    r->limit = member_u64(o, "limit_msat_per_day");
    r->spent = member_u64(o, "spent_msat");
    r->allow_read = json_object_has_member(o, "allow_read") &&
                    json_object_get_boolean_member_with_default(o, "allow_read", FALSE);
    /* Version 1: allow_read also meant "may create invoices" (nostrc-muhk). */
    r->allow_receive = version < 2 ? r->allow_read
                                   : json_object_get_boolean_member_with_default(o, "allow_receive", FALSE);
    r->day = g_strdup(json_object_get_string_member_with_default(o, "day", ""));
    g_hash_table_replace(self->apps, g_strdup(id), r);
  }
  g_list_free(members);
  return TRUE;
}
