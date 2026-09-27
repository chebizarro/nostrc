/* nd-nip89.c — see nd-nip89.h (nostrc-prqu.1). */
#include "nd-nip89.h"
#include "nd-event.h"
#include "nd-fetch.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>

#include "nostr/nip19/nip19.h"

#define DISCOVERY_BUDGET_MS 8000
#define NAME_MAX_CHARS 48

void nd_nip89_handler_free(gpointer p) {
  NdNip89Handler *h = p;
  if (!h) return;
  g_free(h->address);
  g_free(h->pubkey_hex);
  g_free(h->name);
  if (h->web) g_ptr_array_unref(h->web);
  g_free(h->app_id);
  g_free(h->event_json);
  g_free(h);
}

static void web_free(gpointer p) {
  NdNip89Web *w = p;
  g_free(w->template_url);
  g_free(w->entity);
  g_free(w);
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

static JsonNode *parse_json(const char *json) {
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json || !json_parser_load_from_data(p, json, -1, NULL)) return NULL;
  JsonNode *root = json_parser_get_root(p);
  return root ? json_node_copy(root) : NULL;
}

static gint64 event_created_at(JsonObject *o) {
  JsonNode *n = json_object_get_member(o, "created_at");
  if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64)
    return 0;
  return json_node_get_int(n);
}

static const char *tag_str(JsonArray *tag, guint i) {
  if (i >= json_array_get_length(tag)) return NULL;
  JsonNode *n = json_array_get_element(tag, i);
  if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING) return NULL;
  return json_node_get_string(n);
}

/* Printable, single-line, no bidi controls, at most NAME_MAX_CHARS. */
static char *sanitize_name(const char *in) {
  if (!in || !g_utf8_validate(in, -1, NULL)) return NULL;
  GString *s = g_string_new(NULL);
  guint n = 0;
  for (const char *p = in; *p && n < NAME_MAX_CHARS; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    GUnicodeType t = g_unichar_type(c);
    if (g_unichar_iscntrl(c) || t == G_UNICODE_FORMAT || t == G_UNICODE_LINE_SEPARATOR ||
        t == G_UNICODE_PARAGRAPH_SEPARATOR)
      continue; /* includes U+202A..202E / U+2066..2069 */
    g_string_append_unichar(s, c);
    n++;
  }
  g_strstrip(s->str);
  if (!*s->str) {
    g_string_free(s, TRUE);
    return NULL;
  }
  return g_string_free(s, FALSE);
}

static gboolean app_id_valid(const char *id) {
  return id && strlen(id) <= 255 && g_application_id_is_valid(id) && strchr(id, '.');
}

NdNip89Handler *nd_nip89_handler_from_json(const char *event_json, guint32 kind) {
  g_autoptr(NdEvent) ev = nd_event_parse(event_json, -1, NULL);
  if (!ev || !ev->validated || ev->kind != ND_NIP89_HANDLER_KIND || !ev->pubkey_hex ||
      !ev->d_tag)
    return NULL;
  g_autoptr(JsonNode) root = parse_json(ev->json);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return NULL;
  JsonObject *o = json_node_get_object(root);
  JsonArray *tags = json_object_has_member(o, "tags") ? json_object_get_array_member(o, "tags")
                                                      : NULL;
  if (!tags) return NULL;

  g_autofree char *kind_str = g_strdup_printf("%u", kind);
  gboolean has_k = FALSE;
  NdNip89Handler *h = g_new0(NdNip89Handler, 1);
  h->web = g_ptr_array_new_with_free_func(web_free);
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonNode *tn = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tn)) continue;
    JsonArray *t = json_node_get_array(tn);
    const char *name = tag_str(t, 0), *v = tag_str(t, 1);
    if (!name || !v) continue;
    if (strcmp(name, "k") == 0 && strcmp(v, kind_str) == 0) {
      has_k = TRUE;
    } else if (strcmp(name, "web") == 0 && strstr(v, "<bech32>") && strlen(v) <= 2048) {
      NdNip89Web *w = g_new0(NdNip89Web, 1);
      w->template_url = g_strdup(v);
      const char *ent = tag_str(t, 2);
      w->entity = ent && *ent ? g_ascii_strdown(ent, -1) : NULL;
      g_ptr_array_add(h->web, w);
    } else if ((strcmp(name, "flatpak") == 0 || strcmp(name, "linux") == 0) && !h->app_id) {
      g_autofree char *id = g_str_has_suffix(v, ".desktop")
                                ? g_strndup(v, strlen(v) - strlen(".desktop"))
                                : g_strdup(v);
      if (app_id_valid(id)) h->app_id = g_steal_pointer(&id);
    }
  }
  if (!has_k) {
    nd_nip89_handler_free(h);
    return NULL;
  }
  h->pubkey_hex = g_strdup(ev->pubkey_hex);
  h->address = g_strdup_printf("%d:%s:%s", ND_NIP89_HANDLER_KIND, ev->pubkey_hex, ev->d_tag);
  h->created_at = event_created_at(o);
  h->event_json = g_strdup(ev->json);

  /* content: optional kind-0-style metadata. */
  const char *content = json_object_has_member(o, "content")
                            ? json_object_get_string_member(o, "content")
                            : NULL;
  g_autoptr(JsonNode) meta = content && *content ? parse_json(content) : NULL;
  if (meta && JSON_NODE_HOLDS_OBJECT(meta)) {
    JsonObject *m = json_node_get_object(meta);
    const char *keys[] = {"display_name", "name"};
    for (guint i = 0; i < G_N_ELEMENTS(keys) && !h->name; i++) {
      JsonNode *n = json_object_get_member(m, keys[i]);
      if (n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING)
        h->name = sanitize_name(json_node_get_string(n));
    }
  }
  return h;
}

/* ------------------------------------------------------------------ */
/* Web URL                                                             */
/* ------------------------------------------------------------------ */

static gboolean https_url_ok(const char *url) {
  g_autoptr(GUri) u = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
  if (!u) return FALSE;
  const char *host = g_uri_get_host(u);
  return g_strcmp0(g_uri_get_scheme(u), "https") == 0 && host && *host &&
         !g_uri_get_userinfo(u);
}

char *nd_nip89_url_host(const char *url) {
  g_autoptr(GUri) u = url ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
  return u && g_uri_get_host(u) ? g_strdup(g_uri_get_host(u)) : NULL;
}

char *nd_nip89_web_url(const NdNip89Handler *h, const NdTarget *t) {
  if (!h || !t || !h->web) return NULL;
  g_autofree char *uri = nd_target_to_uri(t);
  if (!uri || !g_str_has_prefix(uri, "nostr:")) return NULL;
  const char *bech = uri + strlen("nostr:");
  const char *sep = strchr(bech, '1');
  if (!sep) return NULL;
  g_autofree char *entity = g_strndup(bech, (gsize)(sep - bech));

  const NdNip89Web *exact = NULL, *generic = NULL;
  for (guint i = 0; i < h->web->len; i++) {
    const NdNip89Web *w = g_ptr_array_index(h->web, i);
    if (w->entity && strcmp(w->entity, entity) == 0 && !exact) exact = w;
    else if (!w->entity && !generic) generic = w;
  }
  const NdNip89Web *pick = exact ? exact : generic;
  if (!pick) return NULL;
  g_auto(GStrv) parts = g_strsplit(pick->template_url, "<bech32>", -1);
  g_autofree char *url = g_strjoinv(bech, parts);
  return https_url_ok(url) ? g_steal_pointer(&url) : NULL;
}

/* ------------------------------------------------------------------ */
/* Ranking                                                             */
/* ------------------------------------------------------------------ */

void nd_nip89_apply_recommendations(GPtrArray *handlers, GPtrArray *recs, guint32 kind) {
  if (!handlers || !recs) return;
  g_autofree char *kind_str = g_strdup_printf("%u", kind);
  /* address -> set of recommender pubkeys */
  g_autoptr(GHashTable) by_addr =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_hash_table_unref);
  for (guint i = 0; i < recs->len; i++) {
    const NdEvent *ev = g_ptr_array_index(recs, i);
    if (!ev->validated || ev->kind != ND_NIP89_RECOMMEND_KIND || !ev->pubkey_hex ||
        g_strcmp0(ev->d_tag, kind_str) != 0)
      continue;
    g_autoptr(JsonNode) root = parse_json(ev->json);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) continue;
    JsonObject *o = json_node_get_object(root);
    JsonArray *tags = json_object_has_member(o, "tags") ? json_object_get_array_member(o, "tags")
                                                        : NULL;
    for (guint j = 0; tags && j < json_array_get_length(tags); j++) {
      JsonNode *tn = json_array_get_element(tags, j);
      if (!JSON_NODE_HOLDS_ARRAY(tn)) continue;
      JsonArray *t = json_node_get_array(tn);
      const char *name = tag_str(t, 0), *v = tag_str(t, 1);
      if (!name || !v || strcmp(name, "a") != 0 || !g_str_has_prefix(v, "31990:")) continue;
      GHashTable *set = g_hash_table_lookup(by_addr, v);
      if (!set) {
        set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        g_hash_table_insert(by_addr, g_strdup(v), set);
      }
      g_hash_table_add(set, g_strdup(ev->pubkey_hex));
    }
  }
  for (guint i = 0; i < handlers->len; i++) {
    NdNip89Handler *h = g_ptr_array_index(handlers, i);
    GHashTable *set = g_hash_table_lookup(by_addr, h->address);
    h->recommended_by = set ? g_hash_table_size(set) : 0;
  }
}

static gint rank_cmp(gconstpointer a, gconstpointer b) {
  const NdNip89Handler *x = *(NdNip89Handler *const *)a, *y = *(NdNip89Handler *const *)b;
  if (x->recommended_by != y->recommended_by) return x->recommended_by > y->recommended_by ? -1 : 1;
  gboolean xw = x->web && x->web->len, yw = y->web && y->web->len;
  if (xw != yw) return xw ? -1 : 1;
  if (x->created_at != y->created_at) return x->created_at > y->created_at ? -1 : 1;
  return g_strcmp0(x->address, y->address);
}

void nd_nip89_rank(GPtrArray *handlers) {
  if (!handlers) return;
  g_autoptr(GHashTable) newest = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < handlers->len; i++) {
    NdNip89Handler *h = g_ptr_array_index(handlers, i);
    NdNip89Handler *cur = g_hash_table_lookup(newest, h->address);
    if (!cur || h->created_at > cur->created_at) g_hash_table_insert(newest, h->address, h);
  }
  for (guint i = handlers->len; i-- > 0;) {
    NdNip89Handler *h = g_ptr_array_index(handlers, i);
    if (g_hash_table_lookup(newest, h->address) != h) g_ptr_array_remove_index(handlers, i);
  }
  g_ptr_array_sort(handlers, rank_cmp);
}

/* ------------------------------------------------------------------ */
/* Filters and list events                                             */
/* ------------------------------------------------------------------ */

static char *builder_finish(JsonBuilder *b) {
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  return json_to_string(root, FALSE);
}

char *nd_nip89_handlers_filter(guint32 kind) {
  g_autoptr(JsonBuilder) b = json_builder_new();
  g_autofree char *k = g_strdup_printf("%u", kind);
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "kinds");
  json_builder_begin_array(b);
  json_builder_add_int_value(b, ND_NIP89_HANDLER_KIND);
  json_builder_end_array(b);
  json_builder_set_member_name(b, "#k");
  json_builder_begin_array(b);
  json_builder_add_string_value(b, k);
  json_builder_end_array(b);
  json_builder_set_member_name(b, "limit");
  json_builder_add_int_value(b, 50);
  json_builder_end_object(b);
  return builder_finish(b);
}

char *nd_nip89_recommendations_filter(guint32 kind, const char *const *authors) {
  if (!authors || !authors[0]) return NULL;
  g_autoptr(JsonBuilder) b = json_builder_new();
  g_autofree char *k = g_strdup_printf("%u", kind);
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "kinds");
  json_builder_begin_array(b);
  json_builder_add_int_value(b, ND_NIP89_RECOMMEND_KIND);
  json_builder_end_array(b);
  json_builder_set_member_name(b, "#d");
  json_builder_begin_array(b);
  json_builder_add_string_value(b, k);
  json_builder_end_array(b);
  json_builder_set_member_name(b, "authors");
  json_builder_begin_array(b);
  for (guint i = 0; authors[i]; i++) json_builder_add_string_value(b, authors[i]);
  json_builder_end_array(b);
  json_builder_set_member_name(b, "limit");
  json_builder_add_int_value(b, 500);
  json_builder_end_object(b);
  return builder_finish(b);
}

char *nd_nip89_replaceable_filter(gint kind, const char *author_hex) {
  if (!author_hex) return NULL;
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "kinds");
  json_builder_begin_array(b);
  json_builder_add_int_value(b, kind);
  json_builder_end_array(b);
  json_builder_set_member_name(b, "authors");
  json_builder_begin_array(b);
  json_builder_add_string_value(b, author_hex);
  json_builder_end_array(b);
  json_builder_set_member_name(b, "limit");
  json_builder_add_int_value(b, 1);
  json_builder_end_object(b);
  return builder_finish(b);
}

static JsonArray *event_tags(JsonNode *root) {
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return NULL;
  JsonObject *o = json_node_get_object(root);
  return json_object_has_member(o, "tags") ? json_object_get_array_member(o, "tags") : NULL;
}

char **nd_nip89_read_relays(const char *relay_list_json) {
  g_autoptr(GStrvBuilder) out = g_strv_builder_new();
  g_autoptr(JsonNode) root = parse_json(relay_list_json);
  JsonArray *tags = event_tags(root);
  g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; tags && i < json_array_get_length(tags); i++) {
    JsonNode *tn = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tn)) continue;
    JsonArray *t = json_node_get_array(tn);
    const char *name = tag_str(t, 0), *url = tag_str(t, 1), *marker = tag_str(t, 2);
    if (g_strcmp0(name, "r") != 0 || !url) continue;
    if (marker && strcmp(marker, "read") != 0) continue;
    if (!nd_relay_url_acceptable(url) || g_hash_table_contains(seen, url)) continue;
    g_hash_table_add(seen, (gpointer)url);
    g_strv_builder_add(out, url);
  }
  return g_strv_builder_end(out);
}

static gboolean hex64(const char *s) {
  if (!s || strlen(s) != 64) return FALSE;
  for (const char *p = s; *p; p++)
    if (!g_ascii_isxdigit(*p) || g_ascii_isupper(*p)) return FALSE;
  return TRUE;
}

char **nd_nip89_follows(const char *contacts_json, guint max) {
  g_autoptr(GStrvBuilder) out = g_strv_builder_new();
  g_autoptr(JsonNode) root = parse_json(contacts_json);
  JsonArray *tags = event_tags(root);
  guint n = 0;
  for (guint i = 0; tags && i < json_array_get_length(tags) && n < max; i++) {
    JsonNode *tn = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tn)) continue;
    JsonArray *t = json_node_get_array(tn);
    if (g_strcmp0(tag_str(t, 0), "p") == 0 && hex64(tag_str(t, 1))) {
      g_strv_builder_add(out, tag_str(t, 1));
      n++;
    }
  }
  return g_strv_builder_end(out);
}

const char *nd_nip89_newest_json(GPtrArray *events) {
  const char *best = NULL;
  gint64 best_at = -1;
  for (guint i = 0; events && i < events->len; i++) {
    const NdEvent *ev = g_ptr_array_index(events, i);
    g_autoptr(JsonNode) root = parse_json(ev->json);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) continue;
    gint64 at = event_created_at(json_node_get_object(root));
    if (at > best_at) {
      best_at = at;
      best = ev->json;
    }
  }
  return best;
}

/* ------------------------------------------------------------------ */
/* Offer text                                                          */
/* ------------------------------------------------------------------ */

char *nd_nip89_offer_button(const NdNip89Handler *h, const char *url) {
  g_autofree char *host = nd_nip89_url_host(url);
  if (h->name && host) return g_strdup_printf("Open in %s (%s)", h->name, host);
  if (host) return g_strdup_printf("Open on %s", host);
  return NULL;
}

char *nd_nip89_offer_body(const NdNip89Handler *h, guint32 kind, const char *url) {
  GString *s = g_string_new(NULL);
  g_autofree char *host = nd_nip89_url_host(url);
  const char *name = h->name ? h->name : "An app";
  if (host)
    g_string_append_printf(s, "%s says it can show Nostr events of kind %u on the web at %s.",
                           name, kind, host);
  else
    g_string_append_printf(s, "%s says it can show Nostr events of kind %u.", name, kind);
  if (h->recommended_by == 1)
    g_string_append(s, " Recommended by 1 person you follow (or you).");
  else if (h->recommended_by > 1)
    g_string_append_printf(s, " Recommended by %u people you follow.", h->recommended_by);
  else
    g_string_append(s, " Nobody you follow recommends it: check the address before opening.");
  if (h->app_id)
    g_string_append_printf(s, " It is also listed as a desktop app (%s); look for it in "
                              "Software / on Flathub.", h->app_id);
  return g_string_free(s, FALSE);
}

/* ------------------------------------------------------------------ */
/* Cache                                                               */
/* ------------------------------------------------------------------ */

static char *cache_path(const char *dir, guint32 kind) {
  g_autofree char *d = dir ? g_strdup(dir)
                           : g_build_filename(g_get_user_cache_dir(), "nostr-dispatcher",
                                              "nip89", NULL);
  g_autofree char *name = g_strdup_printf("%u.json", kind);
  return g_build_filename(d, name, NULL);
}

GPtrArray *nd_nip89_cache_load(const char *dir, guint32 kind, gint64 now, gint64 max_age_s,
                               gboolean *out_fresh) {
  if (out_fresh) *out_fresh = FALSE;
  g_autofree char *path = cache_path(dir, kind);
  g_autofree char *data = NULL;
  gsize len = 0;
  if (!g_file_get_contents(path, &data, &len, NULL) || len > 4 * ND_EVENT_MAX_JSON) return NULL;
  g_autoptr(JsonNode) root = parse_json(data);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return NULL;
  JsonObject *o = json_node_get_object(root);
  if (!json_object_has_member(o, "fetched_at") || !json_object_has_member(o, "handlers"))
    return NULL;
  gint64 at = json_object_get_int_member(o, "fetched_at");
  JsonArray *arr = json_object_get_array_member(o, "handlers");
  if (!arr) return NULL;
  GPtrArray *out = g_ptr_array_new_with_free_func(nd_nip89_handler_free);
  for (guint i = 0; i < json_array_get_length(arr); i++) {
    JsonNode *en = json_array_get_element(arr, i);
    if (!JSON_NODE_HOLDS_OBJECT(en)) continue;
    JsonObject *e = json_node_get_object(en);
    const char *ev = json_object_has_member(e, "event")
                         ? json_object_get_string_member(e, "event")
                         : NULL;
    /* Re-validated on load: the cache is not a trust boundary. */
    NdNip89Handler *h = ev ? nd_nip89_handler_from_json(ev, kind) : NULL;
    if (!h) continue;
    h->recommended_by = json_object_has_member(e, "recommended_by")
                            ? (guint)MAX(0, json_object_get_int_member(e, "recommended_by"))
                            : 0;
    g_ptr_array_add(out, h);
  }
  gint64 ttl = max_age_s > 0 ? max_age_s
                             : (out->len ? ND_NIP89_TTL_FOUND_S : ND_NIP89_TTL_NONE_S);
  if (out_fresh) *out_fresh = at <= now && now - at < ttl;
  nd_nip89_rank(out);
  return out;
}

gboolean nd_nip89_cache_store(const char *dir, guint32 kind, GPtrArray *handlers, gint64 now,
                              GError **error) {
  g_autofree char *path = cache_path(dir, kind);
  g_autofree char *d = g_path_get_dirname(path);
  if (g_mkdir_with_parents(d, 0700) != 0) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "cannot create %s", d);
    return FALSE;
  }
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "fetched_at");
  json_builder_add_int_value(b, now);
  json_builder_set_member_name(b, "handlers");
  json_builder_begin_array(b);
  for (guint i = 0; handlers && i < handlers->len; i++) {
    const NdNip89Handler *h = g_ptr_array_index(handlers, i);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "event");
    json_builder_add_string_value(b, h->event_json);
    json_builder_set_member_name(b, "recommended_by");
    json_builder_add_int_value(b, h->recommended_by);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  json_builder_end_object(b);
  g_autofree char *json = builder_finish(b);
  return g_file_set_contents_full(path, json, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

/* ------------------------------------------------------------------ */
/* Presented offers (tokens)                                           */
/* ------------------------------------------------------------------ */

#define OFFER_GROUP "Offer"

static char *offers_dir(const char *dir) {
  return dir ? g_strdup(dir)
             : g_build_filename(g_get_user_runtime_dir(), "nostr-dispatcher", "nip89-offers",
                                NULL);
}

static gboolean token_valid(const char *t) {
  if (!t || strlen(t) != 32) return FALSE;
  for (const char *p = t; *p; p++)
    if (!g_ascii_isxdigit(*p) || g_ascii_isupper(*p)) return FALSE;
  return TRUE;
}

static char *random_token(void) {
  guint8 b[16];
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return NULL;
  ssize_t n = read(fd, b, sizeof(b));
  close(fd);
  if (n != (ssize_t)sizeof(b)) return NULL;
  char *hex = g_malloc(33);
  for (int i = 0; i < 16; i++) g_snprintf(hex + 2 * i, 3, "%02x", b[i]);
  return hex;
}

static void purge_expired(const char *d, gint64 now) {
  g_autoptr(GDir) dir = g_dir_open(d, 0, NULL);
  const char *name;
  while (dir && (name = g_dir_read_name(dir))) {
    if (!token_valid(name)) continue;
    g_autofree char *path = g_build_filename(d, name, NULL);
    g_autoptr(GKeyFile) kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL) ||
        g_key_file_get_int64(kf, OFFER_GROUP, "expires", NULL) < now)
      g_remove(path);
  }
}

char *nd_nip89_offer_store(const char *dir, guint32 kind, const char *address,
                           const char *uri, const char *app_id, gint64 now, GError **error) {
  g_autofree char *d = offers_dir(dir);
  if (g_mkdir_with_parents(d, 0700) != 0) {
    int e = errno;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(e), "cannot create %s", d);
    return NULL;
  }
  purge_expired(d, now);
  g_autofree char *token = random_token();
  if (!token) {
    g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "no randomness");
    return NULL;
  }
  g_autoptr(GKeyFile) kf = g_key_file_new();
  g_key_file_set_uint64(kf, OFFER_GROUP, "kind", kind);
  g_key_file_set_int64(kf, OFFER_GROUP, "expires", now + ND_NIP89_OFFER_TTL_S);
  if (address) g_key_file_set_string(kf, OFFER_GROUP, "address", address);
  if (uri) g_key_file_set_string(kf, OFFER_GROUP, "uri", uri);
  if (app_id) g_key_file_set_string(kf, OFFER_GROUP, "app_id", app_id);
  gsize len = 0;
  g_autofree char *data = g_key_file_to_data(kf, &len, NULL);
  g_autofree char *path = g_build_filename(d, token, NULL);
  if (!g_file_set_contents_full(path, data, (gssize)len, G_FILE_SET_CONTENTS_CONSISTENT, 0600,
                                error))
    return NULL;
  return g_steal_pointer(&token);
}

gboolean nd_nip89_offer_take(const char *dir, const char *token, gint64 now, guint32 *kind,
                             char **address, char **uri, char **app_id) {
  *address = *uri = *app_id = NULL;
  if (!token_valid(token)) return FALSE;
  g_autofree char *d = offers_dir(dir);
  g_autofree char *path = g_build_filename(d, token, NULL);
  g_autoptr(GKeyFile) kf = g_key_file_new();
  gboolean loaded = g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
  g_remove(path); /* single use, whatever happens next */
  if (!loaded || g_key_file_get_int64(kf, OFFER_GROUP, "expires", NULL) < now) return FALSE;
  guint64 k = g_key_file_get_uint64(kf, OFFER_GROUP, "kind", NULL);
  if (k > 65535) return FALSE;
  *kind = (guint32)k;
  *address = g_key_file_get_string(kf, OFFER_GROUP, "address", NULL);
  *uri = g_key_file_get_string(kf, OFFER_GROUP, "uri", NULL);
  *app_id = g_key_file_get_string(kf, OFFER_GROUP, "app_id", NULL);
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Discovery                                                           */
/* ------------------------------------------------------------------ */

static GDBusConnection *session_bus(void) {
  return g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
}

/* org.nostr.Signer.GetPublicKey without auto-starting a signer. */
static char *signer_pubkey_hex(GDBusConnection *bus) {
  if (!bus) return NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
      bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer", "GetPublicKey", NULL,
      G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL, NULL);
  const char *npub = NULL;
  if (r) g_variant_get(r, "(&s)", &npub);
  uint8_t pk[32];
  if (!npub || nostr_nip19_decode_npub(npub, pk) != 0) return NULL;
  char *hex = g_malloc(65);
  for (int i = 0; i < 32; i++) g_snprintf(hex + 2 * i, 3, "%02x", pk[i]);
  return hex;
}

/* org.nostr.Signer.GetRelays: relays the user configured (never fetched). */
static char **signer_relays(GDBusConnection *bus) {
  if (!bus) return NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
      bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer", "GetRelays", NULL,
      G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL, NULL);
  const char *json = NULL;
  if (r) g_variant_get(r, "(&s)", &json);
  g_autoptr(JsonNode) root = json ? parse_json(json) : NULL;
  if (!root || !JSON_NODE_HOLDS_ARRAY(root)) return NULL;
  JsonArray *a = json_node_get_array(root);
  g_autoptr(GStrvBuilder) out = g_strv_builder_new();
  for (guint i = 0; i < json_array_get_length(a); i++) {
    JsonNode *n = json_array_get_element(a, i);
    if (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING &&
        nd_relay_url_acceptable(json_node_get_string(n)))
      g_strv_builder_add(out, json_node_get_string(n));
  }
  return g_strv_builder_end(out);
}

static guint remaining_ms(gint64 deadline) {
  gint64 left = (deadline - g_get_monotonic_time()) / 1000;
  return left > 0 ? (guint)left : 0;
}

GPtrArray *nd_nip89_discover_sync(guint32 kind, const NdNip89Options *opts,
                                  GCancellable *cancellable) {
  NdNip89Options o = {0};
  if (opts) o = *opts;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  gboolean fresh = FALSE;
  GPtrArray *cached = nd_nip89_cache_load(o.cache_dir, kind, now, 0, &fresh);
  if (cached && fresh) {
    g_debug("nostr-dispatcher: NIP-89: kind %u from cache (%u handlers)", kind, cached->len);
    return cached;
  }
  g_clear_pointer(&cached, g_ptr_array_unref);

  g_autofree char *sock = o.socket_path ? g_strdup(o.socket_path) : nd_fetch_default_socket_path();
  gint64 deadline = g_get_monotonic_time() +
                    (gint64)(o.budget_ms ? o.budget_ms : DISCOVERY_BUDGET_MS) * 1000;
  g_autoptr(GDBusConnection) bus = o.no_signer ? NULL : session_bus();
  g_autofree char *pk = o.pubkey_hex ? g_strdup(o.pubkey_hex) : signer_pubkey_hex(bus);

  /* Network relays: explicit, else NIP-65 read relays (session relay copy
   * of the user's 10002), else what the user configured in the signer. */
  g_auto(GStrv) relays = NULL;
  if (o.use_network) {
    if (o.relays) {
      relays = g_strdupv((char **)o.relays);
    } else {
      if (pk) {
        g_autofree char *f = nd_nip89_replaceable_filter(10002, pk);
        g_autoptr(GPtrArray) rl = nd_fetch_collect_sync(f, 10002, sock, NULL,
                                                        remaining_ms(deadline), cancellable);
        const char *newest = nd_nip89_newest_json(rl);
        if (newest) relays = nd_nip89_read_relays(newest);
      }
      if (!relays || !relays[0]) {
        g_clear_pointer(&relays, g_strfreev);
        relays = signer_relays(bus);
      }
    }
    if (relays && g_strv_length(relays) > ND_NIP89_MAX_RELAYS) {
      g_free(relays[ND_NIP89_MAX_RELAYS]);
      for (guint i = ND_NIP89_MAX_RELAYS + 1; relays[i]; i++) g_free(relays[i]);
      relays[ND_NIP89_MAX_RELAYS] = NULL;
    }
  }
  const char *const *net = (const char *const *)relays;

  g_autofree char *hf = nd_nip89_handlers_filter(kind);
  g_autoptr(GPtrArray) evs = nd_fetch_collect_sync(hf, ND_NIP89_HANDLER_KIND, sock, net,
                                                   remaining_ms(deadline), cancellable);
  GPtrArray *handlers = g_ptr_array_new_with_free_func(nd_nip89_handler_free);
  for (guint i = 0; i < evs->len; i++) {
    NdNip89Handler *h = nd_nip89_handler_from_json(((NdEvent *)g_ptr_array_index(evs, i))->json,
                                                   kind);
    if (h) g_ptr_array_add(handlers, h);
  }

  /* Recommendations by the user and the user's follows. */
  if (handlers->len && pk) {
    g_autofree char *cf = nd_nip89_replaceable_filter(3, pk);
    g_autoptr(GPtrArray) k3 = nd_fetch_collect_sync(cf, 3, sock, net, remaining_ms(deadline),
                                                    cancellable);
    const char *contacts = nd_nip89_newest_json(k3);
    g_auto(GStrv) follows = contacts ? nd_nip89_follows(contacts, ND_NIP89_MAX_FOLLOWS)
                                     : g_new0(char *, 1);
    g_autoptr(GStrvBuilder) ab = g_strv_builder_new();
    g_strv_builder_add(ab, pk);
    g_strv_builder_addv(ab, (const char **)follows);
    g_auto(GStrv) authors = g_strv_builder_end(ab);
    g_autofree char *rf = nd_nip89_recommendations_filter(kind, (const char *const *)authors);
    g_autoptr(GPtrArray) recs = nd_fetch_collect_sync(rf, ND_NIP89_RECOMMEND_KIND, sock, net,
                                                      remaining_ms(deadline), cancellable);
    nd_nip89_apply_recommendations(handlers, recs, kind);
  }
  nd_nip89_rank(handlers);

  if (!g_cancellable_is_cancelled(cancellable)) {
    g_autoptr(GError) err = NULL;
    if (!nd_nip89_cache_store(o.cache_dir, kind, handlers, now, &err))
      g_debug("nostr-dispatcher: NIP-89 cache not written: %s", err->message);
  }
  g_message("nostr-dispatcher: NIP-89: kind %u: %u handler%s (session relay%s%s)", kind,
            handlers->len, handlers->len == 1 ? "" : "s",
            net && net[0] ? " + " : "", net && net[0] ? "read relays" : " only");
  return handlers;
}
