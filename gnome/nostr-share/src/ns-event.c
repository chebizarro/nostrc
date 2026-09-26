/* ns-event.c - Tag builders and unsigned event JSON
 *
 * SPDX-License-Identifier: MIT
 */
#include "ns-event.h"
#include "ns-kind.h"

#include <nostr/nip19/nip19.h>

#include <stdarg.h>
#include <string.h>

void
ns_blob_meta_clear(NsBlobMeta *m)
{
  if (m == NULL)
    return;
  g_clear_pointer(&m->url, g_free);
  g_clear_pointer(&m->mime, g_free);
  g_clear_pointer(&m->alt, g_free);
  memset(m, 0, sizeof(*m));
}

void
ns_tags_add(JsonArray *tags, const gchar *first, ...)
{
  JsonArray *tag = json_array_new();
  va_list ap;
  va_start(ap, first);
  for (const gchar *v = first; v != NULL; v = va_arg(ap, const gchar *))
    json_array_add_string_element(tag, v);
  va_end(ap);
  json_array_add_array_element(tags, tag);
}

gboolean
ns_tags_has(JsonArray *tags, const gchar *name, const gchar *value)
{
  guint n = json_array_get_length(tags);
  for (guint i = 0; i < n; i++) {
    JsonNode *node = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(node))
      continue;
    JsonArray *tag = json_node_get_array(node);
    if (json_array_get_length(tag) < 2)
      continue;
    if (g_strcmp0(json_array_get_string_element(tag, 0), name) == 0 &&
        g_strcmp0(json_array_get_string_element(tag, 1), value) == 0)
      return TRUE;
  }
  return FALSE;
}

/* ---- URLs ---- */

static gboolean
url_char(gunichar c)
{
  if (g_unichar_isspace(c))
    return FALSE;
  return c != '<' && c != '>' && c != '"' && c != '`';
}

static gchar *
trim_url(const gchar *start, const gchar *end)
{
  /* Trim trailing punctuation that is almost never part of a URL when a
   * URL ends a sentence, and closing brackets without an opener. */
  while (end > start) {
    gchar last = end[-1];
    if (strchr(".,;:!?'", last) != NULL) {
      end--;
      continue;
    }
    if (last == ')' || last == ']' || last == '}') {
      gchar open = last == ')' ? '(' : last == ']' ? '[' : '{';
      gint balance = 0;
      for (const gchar *p = start; p < end; p++) {
        if (*p == open) balance++;
        else if (*p == last) balance--;
      }
      if (balance < 0) {
        end--;
        continue;
      }
    }
    break;
  }
  return g_strndup(start, (gsize)(end - start));
}

GPtrArray *
ns_extract_urls(const gchar *text)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  if (text == NULL)
    return out;

  const gchar *p = text;
  while (*p != '\0') {
    const gchar *hit = NULL;
    const gchar *h1 = strstr(p, "http://");
    const gchar *h2 = strstr(p, "https://");
    if (h1 != NULL && (h2 == NULL || h1 < h2)) hit = h1;
    else hit = h2;
    if (hit == NULL)
      break;

    /* Require a boundary before the scheme so "xhttps://" or the tail of
     * a nostr: URI never matches. */
    if (hit > text) {
      gchar prev = hit[-1];
      if (g_ascii_isalnum(prev) || prev == ':' || prev == '/' || prev == '+') {
        p = hit + 4;
        continue;
      }
    }

    const gchar *end = hit;
    while (*end != '\0') {
      gunichar c = g_utf8_get_char_validated(end, -1);
      if (c == (gunichar)-1 || c == (gunichar)-2 || !url_char(c))
        break;
      end = g_utf8_next_char(end);
    }
    gchar *url = trim_url(hit, end);
    const gchar *after_scheme = strstr(url, "://") + 3;
    gboolean dup = FALSE;
    for (guint i = 0; i < out->len; i++)
      if (g_str_equal(g_ptr_array_index(out, i), url))
        dup = TRUE;
    if (*after_scheme != '\0' && !dup)
      g_ptr_array_add(out, url);
    else
      g_free(url);
    p = end;
  }
  return out;
}

void
ns_tags_add_urls(JsonArray *tags, const gchar *text)
{
  g_autoptr(GPtrArray) urls = ns_extract_urls(text);
  for (guint i = 0; i < urls->len; i++) {
    const gchar *u = g_ptr_array_index(urls, i);
    if (!ns_tags_has(tags, "r", u))
      ns_tags_add(tags, "r", u, NULL);
  }
}

/* ---- Media ---- */

JsonArray *
ns_imeta_tag_new(const NsBlobMeta *m)
{
  JsonArray *tag = json_array_new();
  json_array_add_string_element(tag, "imeta");
  g_autofree gchar *url  = g_strdup_printf("url %s", m->url);
  g_autofree gchar *mime = g_strdup_printf("m %s", m->mime);
  g_autofree gchar *x    = g_strdup_printf("x %s", m->sha256);
  g_autofree gchar *size = g_strdup_printf("size %" G_GUINT64_FORMAT, m->size);
  json_array_add_string_element(tag, url);
  json_array_add_string_element(tag, mime);
  json_array_add_string_element(tag, x);
  json_array_add_string_element(tag, size);
  if (m->width > 0 && m->height > 0) {
    g_autofree gchar *dim = g_strdup_printf("dim %ux%u", m->width, m->height);
    json_array_add_string_element(tag, dim);
  }
  if (m->alt != NULL && *m->alt != '\0') {
    g_autofree gchar *alt = g_strdup_printf("alt %s", m->alt);
    json_array_add_string_element(tag, alt);
  }
  return tag;
}

void
ns_tags_add_file_metadata(JsonArray *tags, const NsBlobMeta *m)
{
  g_autofree gchar *size = g_strdup_printf("%" G_GUINT64_FORMAT, m->size);
  ns_tags_add(tags, "url", m->url, NULL);
  ns_tags_add(tags, "m", m->mime, NULL);
  ns_tags_add(tags, "x", m->sha256, NULL);
  ns_tags_add(tags, "ox", m->sha256, NULL);
  ns_tags_add(tags, "size", size, NULL);
  if (m->width > 0 && m->height > 0) {
    g_autofree gchar *dim = g_strdup_printf("%ux%u", m->width, m->height);
    ns_tags_add(tags, "dim", dim, NULL);
  }
  if (m->alt != NULL && *m->alt != '\0')
    ns_tags_add(tags, "alt", m->alt, NULL);
}

static const gchar *
ext_for_mime(const gchar *mime)
{
  static const struct { const gchar *mime, *ext; } map[] = {
    { "image/jpeg", "jpg" },  { "image/png", "png" },   { "image/gif", "gif" },
    { "image/webp", "webp" }, { "image/avif", "avif" }, { "image/heic", "heic" },
    { "image/svg+xml", "svg" },
    { "video/mp4", "mp4" },   { "video/webm", "webm" }, { "video/quicktime", "mov" },
    { "video/x-matroska", "mkv" },
    { "audio/mpeg", "mp3" },  { "audio/ogg", "ogg" },   { "audio/flac", "flac" },
    { "audio/x-flac", "flac" }, { "audio/wav", "wav" }, { "audio/x-wav", "wav" },
    { "audio/mp4", "m4a" },   { "audio/opus", "opus" },
    { "application/pdf", "pdf" }, { "text/plain", "txt" },
  };
  if (mime == NULL)
    return NULL;
  for (gsize i = 0; i < G_N_ELEMENTS(map); i++)
    if (g_str_equal(map[i].mime, mime))
      return map[i].ext;
  return NULL;
}

gchar *
ns_blossom_blob_url(const gchar *server, const gchar *sha256, const gchar *mime)
{
  g_autofree gchar *base = g_strdup(server);
  gsize n = strlen(base);
  while (n > 0 && base[n - 1] == '/')
    base[--n] = '\0';
  const gchar *ext = ext_for_mime(mime);
  return ext ? g_strdup_printf("%s/%s.%s", base, sha256, ext)
             : g_strdup_printf("%s/%s", base, sha256);
}

GStrv
ns_blossom_servers_from_event(const gchar *event_json)
{
  g_autoptr(GStrvBuilder) b = g_strv_builder_new();
  g_autoptr(JsonParser) parser = json_parser_new();
  if (event_json != NULL && json_parser_load_from_data(parser, event_json, -1, NULL)) {
    JsonNode *root = json_parser_get_root(parser);
    if (JSON_NODE_HOLDS_OBJECT(root)) {
      JsonObject *obj = json_node_get_object(root);
      JsonNode *tn = json_object_get_member(obj, "tags");
      if (json_object_get_int_member_with_default(obj, "kind", 0) == 10063 &&
          tn != NULL && JSON_NODE_HOLDS_ARRAY(tn)) {
        JsonArray *tags = json_node_get_array(tn);
        g_autoptr(GPtrArray) seen = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < json_array_get_length(tags); i++) {
          JsonNode *en = json_array_get_element(tags, i);
          if (!JSON_NODE_HOLDS_ARRAY(en))
            continue;
          JsonArray *tag = json_node_get_array(en);
          if (json_array_get_length(tag) < 2)
            continue;
          JsonNode *k = json_array_get_element(tag, 0);
          JsonNode *v = json_array_get_element(tag, 1);
          if (!JSON_NODE_HOLDS_VALUE(k) || !JSON_NODE_HOLDS_VALUE(v) ||
              json_node_get_value_type(k) != G_TYPE_STRING ||
              json_node_get_value_type(v) != G_TYPE_STRING)
            continue;
          if (!g_str_equal(json_node_get_string(k), "server"))
            continue;
          const gchar *url = json_node_get_string(v);
          if (!g_str_has_prefix(url, "https://") || strlen(url) <= 8)
            continue;
          gchar *norm = g_strdup(url);
          gsize n = strlen(norm);
          while (n > 8 && norm[n - 1] == '/')
            norm[--n] = '\0';
          gboolean dup = FALSE;
          for (guint j = 0; j < seen->len; j++)
            if (g_str_equal(g_ptr_array_index(seen, j), norm))
              dup = TRUE;
          if (dup) {
            g_free(norm);
            continue;
          }
          g_ptr_array_add(seen, norm);
          g_strv_builder_add(b, norm);
        }
      }
    }
  }
  return g_strv_builder_end(b);
}

/* ---- NIP-23 ---- */

gchar *
ns_slugify(const gchar *title)
{
  GString *s = g_string_new(NULL);
  if (title != NULL) {
    g_autofree gchar *ascii = g_str_to_ascii(title, "C");
    gboolean dash = FALSE;
    for (const gchar *p = ascii; *p != '\0' && s->len < 80; p++) {
      if (g_ascii_isalnum(*p)) {
        if (dash && s->len > 0)
          g_string_append_c(s, '-');
        g_string_append_c(s, g_ascii_tolower(*p));
        dash = FALSE;
      } else {
        dash = TRUE;
      }
    }
  }
  if (s->len == 0) {
    g_string_free(s, TRUE);
    g_autoptr(GDateTime) now = g_date_time_new_now_utc();
    return g_date_time_format(now, "note-%Y%m%d-%H%M%S");
  }
  return g_string_free(s, FALSE);
}

gchar *
ns_markdown_title(const gchar *markdown)
{
  if (markdown == NULL)
    return NULL;
  g_auto(GStrv) lines = g_strsplit(markdown, "\n", -1);
  gboolean in_front_matter = FALSE;
  gboolean in_fence = FALSE;
  for (guint i = 0; lines[i] != NULL; i++) {
    gchar *line = g_strchomp(lines[i]);
    if (i == 0 && g_str_equal(line, "---")) {
      in_front_matter = TRUE;
      continue;
    }
    if (in_front_matter) {
      if (g_str_equal(line, "---") || g_str_equal(line, "..."))
        in_front_matter = FALSE;
      else if (g_str_has_prefix(line, "title:")) {
        gchar *v = g_strstrip(line + 6);
        gsize n = strlen(v);
        if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0]) {
          v[n - 1] = '\0';
          v++;
        }
        if (*v != '\0')
          return g_strdup(v);
      }
      continue;
    }
    if (g_str_has_prefix(line, "```") || g_str_has_prefix(line, "~~~")) {
      in_fence = !in_fence;
      continue;
    }
    if (in_fence)
      continue;
    const gchar *l = line;
    guint indent = 0;
    while (*l == ' ' && indent < 4) { l++; indent++; }
    if (indent < 4 && l[0] == '#' && (l[1] == ' ' || l[1] == '\t')) {
      gchar *t = g_strstrip(g_strdup(l + 2));
      /* Drop an optional closing sequence of #s. */
      gsize n = strlen(t);
      while (n > 0 && t[n - 1] == '#') t[--n] = '\0';
      g_strchomp(t);
      if (*t != '\0')
        return t;
      g_free(t);
    }
  }
  return NULL;
}

void
ns_tags_add_article(JsonArray *tags, const gchar *slug, const gchar *title,
                    gint64 published_at)
{
  g_autofree gchar *ts = g_strdup_printf("%" G_GINT64_FORMAT, published_at);
  ns_tags_add(tags, "d", slug, NULL);
  if (title != NULL && *title != '\0')
    ns_tags_add(tags, "title", title, NULL);
  ns_tags_add(tags, "published_at", ts, NULL);
}

/* ---- --to ---- */

void
ns_recipient_clear(NsRecipient *r)
{
  if (r == NULL)
    return;
  g_clear_pointer(&r->pubkey_hex, g_free);
  g_clear_pointer(&r->npub, g_free);
  g_clear_pointer(&r->group_id, g_free);
  g_clear_pointer(&r->relay_url, g_free);
  r->type = NS_RECIPIENT_NONE;
}

static gboolean
is_hex64(const gchar *s)
{
  if (s == NULL || strlen(s) != 64)
    return FALSE;
  for (const gchar *p = s; *p; p++)
    if (!g_ascii_isxdigit(*p))
      return FALSE;
  return TRUE;
}

static gboolean
valid_group_id(const gchar *id)
{
  if (id == NULL || *id == '\0' || strlen(id) > 128)
    return FALSE;
  for (const gchar *p = id; *p; p++)
    if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
      return FALSE;
  return TRUE;
}

gboolean
ns_recipient_parse(const gchar *to, NsRecipient *out, GError **error)
{
  g_return_val_if_fail(out != NULL, FALSE);
  memset(out, 0, sizeof(*out));
  if (to == NULL || *to == '\0')
    return TRUE;

  g_autofree gchar *s = g_strstrip(g_strdup(to));
  if (g_str_has_prefix(s, "nostr:"))
    memmove(s, s + 6, strlen(s + 6) + 1);

  if (g_str_has_prefix(s, "npub1")) {
    uint8_t pk[32];
    if (nostr_nip19_decode_npub(s, pk) != 0) {
      g_set_error(error, NS_ERROR, NS_ERROR_BAD_INPUT, "invalid npub: %s", s);
      return FALSE;
    }
    GString *hex = g_string_sized_new(64);
    for (int i = 0; i < 32; i++)
      g_string_append_printf(hex, "%02x", pk[i]);
    out->type = NS_RECIPIENT_MENTION;
    out->pubkey_hex = g_string_free(hex, FALSE);
    out->npub = g_strdup(s);
    return TRUE;
  }

  if (is_hex64(s)) {
    uint8_t pk[32];
    for (int i = 0; i < 32; i++)
      pk[i] = (uint8_t)((g_ascii_xdigit_value(s[2 * i]) << 4) |
                        g_ascii_xdigit_value(s[2 * i + 1]));
    char *npub = NULL;
    if (nostr_nip19_encode_npub(pk, &npub) != 0 || npub == NULL) {
      g_set_error(error, NS_ERROR, NS_ERROR_BAD_INPUT, "invalid pubkey: %s", s);
      return FALSE;
    }
    out->type = NS_RECIPIENT_MENTION;
    out->pubkey_hex = g_ascii_strdown(s, -1);
    out->npub = g_strdup(npub);
    free(npub);
    return TRUE;
  }

  /* NIP-29 group identifier: <host>'<group-id> (optionally wss://host). */
  const gchar *apos = strchr(s, '\'');
  if (apos != NULL) {
    g_autofree gchar *host = g_strndup(s, (gsize)(apos - s));
    const gchar *gid = apos + 1;
    const gchar *h = host;
    gboolean insecure = FALSE;
    if (g_str_has_prefix(h, "wss://")) h += 6;
    else if (g_str_has_prefix(h, "ws://")) { h += 5; insecure = TRUE; }
    g_autofree gchar *hostonly = g_strdup(h);
    gsize n = strlen(hostonly);
    while (n > 0 && hostonly[n - 1] == '/') hostonly[--n] = '\0';
    if (*hostonly == '\0' || strchr(hostonly, '/') != NULL ||
        strchr(hostonly, '@') != NULL || !valid_group_id(gid)) {
      g_set_error(error, NS_ERROR, NS_ERROR_BAD_INPUT,
                  "invalid NIP-29 group '%s' (expected host'group-id)", s);
      return FALSE;
    }
    out->type = NS_RECIPIENT_GROUP;
    out->group_id = g_strdup(gid);
    out->relay_url = g_strdup_printf("%s%s", insecure ? "ws://" : "wss://",
                                     hostonly);
    return TRUE;
  }

  g_set_error(error, NS_ERROR, NS_ERROR_BAD_INPUT,
              "--to expects an npub, a hex pubkey or a NIP-29 group "
              "(host'group-id); got '%s'", s);
  return FALSE;
}

void
ns_tags_add_recipient(JsonArray *tags, const NsRecipient *r)
{
  if (r == NULL)
    return;
  if (r->type == NS_RECIPIENT_MENTION && !ns_tags_has(tags, "p", r->pubkey_hex))
    ns_tags_add(tags, "p", r->pubkey_hex, NULL);
  else if (r->type == NS_RECIPIENT_GROUP && !ns_tags_has(tags, "h", r->group_id))
    ns_tags_add(tags, "h", r->group_id, NULL);
}

/* ---- Event JSON ---- */

gchar *
ns_event_unsigned_json(gint kind, gint64 created_at, const gchar *pubkey_hex,
                       JsonArray *tags, const gchar *content, gboolean pretty)
{
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  if (pubkey_hex != NULL && *pubkey_hex != '\0') {
    json_builder_set_member_name(b, "pubkey");
    json_builder_add_string_value(b, pubkey_hex);
  }
  json_builder_set_member_name(b, "created_at");
  json_builder_add_int_value(b, created_at);
  json_builder_set_member_name(b, "kind");
  json_builder_add_int_value(b, kind);
  json_builder_set_member_name(b, "tags");
  JsonNode *tn = json_node_new(JSON_NODE_ARRAY);
  json_node_set_array(tn, tags);
  json_builder_add_value(b, tn);
  json_builder_set_member_name(b, "content");
  json_builder_add_string_value(b, content ? content : "");
  json_builder_end_object(b);

  g_autoptr(JsonNode) root = json_builder_get_root(b);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_pretty(gen, pretty);
  json_generator_set_root(gen, root);
  return json_generator_to_data(gen, NULL);
}

gchar *
ns_json_pretty(const gchar *json)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  if (json == NULL || !json_parser_load_from_data(parser, json, -1, NULL))
    return g_strdup(json);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_pretty(gen, TRUE);
  json_generator_set_root(gen, json_parser_get_root(parser));
  return json_generator_to_data(gen, NULL);
}

gchar *
ns_sha256_hex(const guint8 *data, gsize len)
{
  return g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, len);
}
