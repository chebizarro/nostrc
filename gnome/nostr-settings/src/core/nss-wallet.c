/* nss-wallet.c — see nss-wallet.h.
 * SPDX-License-Identifier: MIT
 */
#include "nss-wallet.h"

#include <json-glib/json-glib.h>
#include <string.h>

void
nss_budget_free(NssBudget *b)
{
  if (b == NULL)
    return;
  g_free(b->app_id);
  g_free(b);
}

gchar *
nss_wallet_budgets_path(void)
{
  return g_build_filename(g_get_user_state_dir(), "nostr-wallet", "budgets.json", NULL);
}

static guint64
member_u64(JsonObject *o, const gchar *name)
{
  if (!json_object_has_member(o, name))
    return 0;
  JsonNode *n = json_object_get_member(o, name);
  if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64)
    return 0;
  gint64 v = json_node_get_int(n);
  return v > 0 ? (guint64)v : 0;
}

static gint
by_app(gconstpointer a, gconstpointer b)
{
  const NssBudget *x = *(NssBudget *const *)a, *y = *(NssBudget *const *)b;
  return g_strcmp0(x->app_id, y->app_id);
}

GPtrArray *
nss_wallet_budgets_load(const gchar *path, const gchar *today, GError **error)
{
  GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)nss_budget_free);
  g_autofree gchar *data = NULL;
  gsize len = 0;
  GError *local = NULL;
  if (!g_file_get_contents(path, &data, &len, &local)) {
    if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_clear_error(&local);
      return out;
    }
    g_propagate_error(error, local);
    g_ptr_array_unref(out);
    return NULL;
  }
  g_autofree gchar *day = NULL;
  if (today == NULL) {
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    day = g_date_time_format(now, "%Y-%m-%d");
    today = day;
  }
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, data, (gssize)len, NULL) ||
      json_parser_get_root(p) == NULL ||
      !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "%s is not a budgets document", path);
    g_ptr_array_unref(out);
    return NULL;
  }
  JsonObject *root = json_node_get_object(json_parser_get_root(p));
  /* Version 1 had one flag, allow_read, meaning read AND receive. */
  gint64 version = json_object_get_int_member_with_default(root, "version", 1);
  if (!json_object_has_member(root, "apps") ||
      !JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "apps")))
    return out;
  JsonObject *apps = json_object_get_object_member(root, "apps");
  g_autoptr(GList) ids = json_object_get_members(apps);
  for (GList *l = ids; l; l = l->next) {
    JsonNode *n = json_object_get_member(apps, l->data);
    if (!JSON_NODE_HOLDS_OBJECT(n))
      continue;
    JsonObject *o = json_node_get_object(n);
    NssBudget *b = g_new0(NssBudget, 1);
    b->app_id = g_strdup(l->data);
    b->limit_msat_per_day = member_u64(o, "limit_msat_per_day");
    const gchar *d = json_object_has_member(o, "day")
      ? json_object_get_string_member_with_default(o, "day", "") : "";
    b->spent_today_msat = g_strcmp0(d, today) == 0 ? member_u64(o, "spent_msat") : 0;
    b->allow_read = json_object_get_boolean_member_with_default(o, "allow_read", FALSE);
    b->allow_receive = version < 2
      ? b->allow_read
      : json_object_get_boolean_member_with_default(o, "allow_receive", FALSE);
    g_ptr_array_add(out, b);
  }
  g_ptr_array_sort(out, by_app);
  return out;
}

GPtrArray *
nss_wallet_apps_from_variant(GVariant *apps)
{
  GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)nss_budget_free);
  if (apps == NULL || !g_variant_is_of_type(apps, G_VARIANT_TYPE("a{sa{sv}}")))
    return out;
  GVariantIter it;
  const gchar *id;
  GVariant *rec;
  g_variant_iter_init(&it, apps);
  while (g_variant_iter_next(&it, "{&s@a{sv}}", &id, &rec)) {
    NssBudget *b = g_new0(NssBudget, 1);
    b->app_id = g_strdup(id);
    (void)g_variant_lookup(rec, "limit_msat_per_day", "t", &b->limit_msat_per_day);
    (void)g_variant_lookup(rec, "spent_today_msat", "t", &b->spent_today_msat);
    (void)g_variant_lookup(rec, "allow_read", "b", &b->allow_read);
    /* An agent without "allow_receive" still lets a read grant create
     * invoices. */
    if (!g_variant_lookup(rec, "allow_receive", "b", &b->allow_receive))
      b->allow_receive = b->allow_read;
    g_ptr_array_add(out, b);
    g_variant_unref(rec);
  }
  g_ptr_array_sort(out, by_app);
  return out;
}

gchar *
nss_format_sats(guint64 msat)
{
  guint64 sats = msat / 1000;
  g_autofree gchar *raw = g_strdup_printf("%" G_GUINT64_FORMAT, sats);
  GString *s = g_string_new(NULL);
  gsize n = strlen(raw);
  for (gsize i = 0; i < n; i++) {
    if (i > 0 && (n - i) % 3 == 0)
      g_string_append_c(s, ',');
    g_string_append_c(s, raw[i]);
  }
  g_string_append(s, sats == 1 ? " sat" : " sats");
  return g_string_free(s, FALSE);
}

gchar *
nss_wallet_app_label(const gchar *app_id)
{
  if (app_id == NULL || *app_id == '\0')
    return g_strdup("Unknown app");
  if (g_strcmp0(app_id, NSS_WALLET_SHELL_APP_ID) == 0)
    return g_strdup("GNOME Shell");
  if (strstr(app_id, "://") != NULL)
    return g_strdup_printf("Website %s", app_id);
  if (g_str_has_prefix(app_id, "exe:")) {
    g_autofree gchar *base = g_path_get_basename(app_id + 4);
    return g_strdup_printf("%s (unverified)", base);
  }
  return g_strdup(app_id);
}
