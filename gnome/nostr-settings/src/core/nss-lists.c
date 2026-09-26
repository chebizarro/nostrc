/* nss-lists.c — see nss-lists.h.
 * SPDX-License-Identifier: MIT
 */
#include "nss-lists.h"

#include <json-glib/json-glib.h>
#include <string.h>

G_DEFINE_QUARK(nss-lists-error-quark, nss_lists_error)

NssRelayEntry *
nss_relay_entry_new(const gchar *url, gboolean read, gboolean write)
{
  NssRelayEntry *e = g_new0(NssRelayEntry, 1);
  e->url = g_strdup(url);
  e->read = read;
  e->write = write;
  return e;
}

void
nss_relay_entry_free(NssRelayEntry *e)
{
  if (e == NULL)
    return;
  g_free(e->url);
  g_free(e);
}

static gchar *
normalize(const gchar *in, const gchar *const *schemes, gboolean strip_slash,
          GError **error)
{
  g_autofree gchar *t = g_strstrip(g_strdup(in ? in : ""));
  GError *local = NULL;
  g_autoptr(GUri) u = *t ? g_uri_parse(t, G_URI_FLAGS_NONE, &local) : NULL;
  const gchar *scheme = u ? g_uri_get_scheme(u) : NULL;
  gboolean scheme_ok = FALSE;
  for (guint i = 0; scheme && schemes[i]; i++)
    if (g_ascii_strcasecmp(scheme, schemes[i]) == 0)
      scheme_ok = TRUE;
  const gchar *host = u ? g_uri_get_host(u) : NULL;
  if (u == NULL || !scheme_ok || host == NULL || *host == '\0' ||
      g_uri_get_userinfo(u) != NULL || g_uri_get_query(u) != NULL ||
      g_uri_get_fragment(u) != NULL || strpbrk(t, " \t\r\n") != NULL) {
    g_clear_error(&local);
    g_autofree gchar *want = g_strjoinv(":// or ", (gchar **)schemes);
    g_set_error(error, NSS_LISTS_ERROR, NSS_LISTS_ERROR_BAD_URL,
                "“%s” is not a valid %s:// URL", t, want);
    return NULL;
  }
  g_autofree gchar *ls = g_ascii_strdown(scheme, -1);
  g_autofree gchar *lh = g_ascii_strdown(host, -1);
  const gchar *path = g_uri_get_path(u);
  g_autofree gchar *p = g_strdup(path ? path : "");
  if (strip_slash) {
    gsize n = strlen(p);
    while (n > 0 && p[n - 1] == '/')
      p[--n] = '\0';
  }
  gint port = g_uri_get_port(u);
  gboolean v6 = strchr(lh, ':') != NULL;
  return port > 0
    ? g_strdup_printf("%s://%s%s%s:%d%s", ls, v6 ? "[" : "", lh, v6 ? "]" : "", port, p)
    : g_strdup_printf("%s://%s%s%s%s", ls, v6 ? "[" : "", lh, v6 ? "]" : "", p);
}

gchar *
nss_relay_url_normalize(const gchar *in, GError **error)
{
  static const gchar *const schemes[] = { "wss", "ws", NULL };
  return normalize(in, schemes, FALSE, error);
}

gchar *
nss_blossom_url_normalize(const gchar *in, GError **error)
{
  static const gchar *const schemes[] = { "https", NULL };
  return normalize(in, schemes, TRUE, error);
}

/* Parse @json as an event object of @kind; returns its tags array. */
static JsonArray *
event_tags(JsonParser *p, const gchar *json, gint kind, GError **error)
{
  if (json == NULL || !json_parser_load_from_data(p, json, -1, NULL) ||
      json_parser_get_root(p) == NULL ||
      !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p)))
    goto bad;
  JsonObject *o = json_node_get_object(json_parser_get_root(p));
  if (!json_object_has_member(o, "kind") ||
      json_object_get_int_member(o, "kind") != kind ||
      !json_object_has_member(o, "tags") ||
      !JSON_NODE_HOLDS_ARRAY(json_object_get_member(o, "tags")))
    goto bad;
  return json_object_get_array_member(o, "tags");
bad:
  g_set_error(error, NSS_LISTS_ERROR, NSS_LISTS_ERROR_BAD_EVENT,
              "not a kind-%d event", kind);
  return NULL;
}

static const gchar *
tag_str(JsonArray *t, guint i)
{
  if (i >= json_array_get_length(t))
    return NULL;
  JsonNode *n = json_array_get_element(t, i);
  return JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING
    ? json_node_get_string(n) : NULL;
}

GPtrArray *
nss_relay_list_parse(const gchar *event_json, GError **error)
{
  g_autoptr(JsonParser) p = json_parser_new();
  JsonArray *tags = event_tags(p, event_json, NSS_KIND_RELAY_LIST, error);
  if (tags == NULL)
    return NULL;
  GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)nss_relay_entry_free);
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonNode *tn = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tn))
      continue;
    JsonArray *t = json_node_get_array(tn);
    if (g_strcmp0(tag_str(t, 0), "r") != 0)
      continue;
    g_autofree gchar *url = nss_relay_url_normalize(tag_str(t, 1), NULL);
    if (url == NULL)
      continue;
    const gchar *marker = tag_str(t, 2);
    gboolean r = marker == NULL || g_str_equal(marker, "read");
    gboolean w = marker == NULL || g_str_equal(marker, "write");
    if (!r && !w)
      continue; /* unknown marker */
    NssRelayEntry *existing = NULL;
    for (guint j = 0; j < out->len; j++)
      if (g_str_equal(((NssRelayEntry *)g_ptr_array_index(out, j))->url, url))
        existing = g_ptr_array_index(out, j);
    if (existing != NULL) {
      existing->read |= r;
      existing->write |= w;
    } else {
      g_ptr_array_add(out, nss_relay_entry_new(url, r, w));
    }
  }
  return out;
}

static gchar *
build_event(gint kind, const gchar *pubkey_hex, gint64 created_at, JsonBuilder *tags_b)
{
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  if (pubkey_hex != NULL) {
    json_builder_set_member_name(b, "pubkey");
    json_builder_add_string_value(b, pubkey_hex);
  }
  json_builder_set_member_name(b, "created_at");
  json_builder_add_int_value(b, created_at);
  json_builder_set_member_name(b, "kind");
  json_builder_add_int_value(b, kind);
  json_builder_set_member_name(b, "tags");
  g_autoptr(JsonNode) tags = json_builder_get_root(tags_b);
  json_builder_add_value(b, json_node_copy(tags));
  json_builder_set_member_name(b, "content");
  json_builder_add_string_value(b, "");
  json_builder_end_object(b);
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  return json_to_string(root, FALSE);
}

gchar *
nss_relay_list_build(GPtrArray *entries, const gchar *pubkey_hex, gint64 created_at,
                     GError **error)
{
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(JsonBuilder) tb = json_builder_new();
  json_builder_begin_array(tb);
  for (guint i = 0; entries && i < entries->len; i++) {
    NssRelayEntry *e = g_ptr_array_index(entries, i);
    gchar *url = nss_relay_url_normalize(e->url, error);
    if (url == NULL)
      return NULL;
    if (!g_hash_table_add(seen, url)) {
      g_set_error(error, NSS_LISTS_ERROR, NSS_LISTS_ERROR_DUPLICATE,
                  "%s is listed twice", url);
      return NULL;
    }
    if (!e->read && !e->write) {
      g_set_error(error, NSS_LISTS_ERROR, NSS_LISTS_ERROR_NO_MARKER,
                  "%s is neither a read nor a write relay; remove it instead", url);
      return NULL;
    }
    json_builder_begin_array(tb);
    json_builder_add_string_value(tb, "r");
    json_builder_add_string_value(tb, url);
    if (!(e->read && e->write))
      json_builder_add_string_value(tb, e->read ? "read" : "write");
    json_builder_end_array(tb);
  }
  json_builder_end_array(tb);
  return build_event(NSS_KIND_RELAY_LIST, pubkey_hex, created_at, tb);
}

gchar **
nss_relay_list_urls(GPtrArray *entries, gboolean want_write)
{
  g_autoptr(GStrvBuilder) b = g_strv_builder_new();
  for (guint i = 0; entries && i < entries->len; i++) {
    NssRelayEntry *e = g_ptr_array_index(entries, i);
    if (want_write ? e->write : e->read)
      g_strv_builder_add(b, e->url);
  }
  return g_strv_builder_end(b);
}

gchar **
nss_blossom_list_parse(const gchar *event_json, GError **error)
{
  g_autoptr(JsonParser) p = json_parser_new();
  JsonArray *tags = event_tags(p, event_json, NSS_KIND_BLOSSOM_LIST, error);
  if (tags == NULL)
    return NULL;
  g_autoptr(GStrvBuilder) b = g_strv_builder_new();
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonNode *tn = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tn))
      continue;
    JsonArray *t = json_node_get_array(tn);
    if (g_strcmp0(tag_str(t, 0), "server") != 0)
      continue;
    gchar *url = nss_blossom_url_normalize(tag_str(t, 1), NULL);
    if (url == NULL)
      continue;
    if (g_hash_table_add(seen, url))
      g_strv_builder_add(b, url);
  }
  return g_strv_builder_end(b);
}

gchar *
nss_blossom_list_build(const gchar *const *servers, const gchar *pubkey_hex,
                       gint64 created_at, GError **error)
{
  if (servers == NULL || servers[0] == NULL) {
    g_set_error(error, NSS_LISTS_ERROR, NSS_LISTS_ERROR_EMPTY,
                "add at least one media server");
    return NULL;
  }
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(JsonBuilder) tb = json_builder_new();
  json_builder_begin_array(tb);
  for (guint i = 0; servers[i]; i++) {
    gchar *url = nss_blossom_url_normalize(servers[i], error);
    if (url == NULL)
      return NULL;
    if (!g_hash_table_add(seen, url)) {
      g_set_error(error, NSS_LISTS_ERROR, NSS_LISTS_ERROR_DUPLICATE,
                  "%s is listed twice", url);
      return NULL;
    }
    json_builder_begin_array(tb);
    json_builder_add_string_value(tb, "server");
    json_builder_add_string_value(tb, url);
    json_builder_end_array(tb);
  }
  json_builder_end_array(tb);
  return build_event(NSS_KIND_BLOSSOM_LIST, pubkey_hex, created_at, tb);
}

gchar **
nss_strv_union(const gchar *const *a, const gchar *const *b, const gchar *const *c)
{
  const gchar *const *lists[] = { a, b, c };
  g_autoptr(GStrvBuilder) sb = g_strv_builder_new();
  g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint l = 0; l < G_N_ELEMENTS(lists); l++)
    for (guint i = 0; lists[l] && lists[l][i]; i++)
      if (g_hash_table_add(seen, (gpointer)lists[l][i]))
        g_strv_builder_add(sb, lists[l][i]);
  return g_strv_builder_end(sb);
}

static gchar **
all_urls(GPtrArray *entries)
{
  g_autoptr(GStrvBuilder) b = g_strv_builder_new();
  for (guint i = 0; entries && i < entries->len; i++)
    g_strv_builder_add(b, ((NssRelayEntry *)g_ptr_array_index(entries, i))->url);
  return g_strv_builder_end(b);
}

gchar **
nss_relay_list_publish_targets(GPtrArray *new_list, GPtrArray *old_list,
                               const gchar *const *signer_relays, gchar ***required)
{
  g_auto(GStrv) n = all_urls(new_list);
  g_auto(GStrv) o = all_urls(old_list);
  if (required)
    *required = nss_relay_list_urls(new_list, TRUE);
  return nss_strv_union((const gchar *const *)n, (const gchar *const *)o, signer_relays);
}

gchar **
nss_blossom_publish_targets(GPtrArray *relay_list, const gchar *const *signer_relays,
                            gchar ***required)
{
  g_auto(GStrv) w = nss_relay_list_urls(relay_list, TRUE);
  const gchar *const *base = (w && w[0]) ? (const gchar *const *)w : signer_relays;
  if (required)
    *required = nss_strv_union(base, NULL, NULL);
  return nss_strv_union(base, NULL, NULL);
}
