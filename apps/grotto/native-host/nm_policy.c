/* nm_policy.c - input policy for the NIP-07 bridge (nostrc-jjyp) */
#include "nm_policy.h"

#include <string.h>

/* ------------------------------------------------------------------------
 * Origin -> app_id
 * ---------------------------------------------------------------------- */

static gboolean host_label_char(gchar c) {
  return g_ascii_isalnum(c) || c == '-' || c == '_';
}

/* DNS-ish hostname: dot-separated non-empty labels of [A-Za-z0-9_-],
 * each <= 63 chars, no trailing dot. */
static gboolean valid_hostname(const gchar *h, gsize n) {
  if (n == 0 || n > 253) return FALSE;
  gsize label = 0;
  for (gsize i = 0; i < n; i++) {
    if (h[i] == '.') {
      if (label == 0) return FALSE;
      label = 0;
      continue;
    }
    if (!host_label_char(h[i])) return FALSE;
    if (++label > 63) return FALSE;
  }
  return label > 0;
}

static gboolean valid_ipv6_literal(const gchar *h, gsize n) {
  if (n < 2 || n > 45) return FALSE;
  gboolean colon = FALSE;
  for (gsize i = 0; i < n; i++) {
    if (h[i] == ':') colon = TRUE;
    else if (!g_ascii_isxdigit(h[i]) && h[i] != '.') return FALSE;
  }
  return colon;
}

static gboolean is_loopback_host(const gchar *host /* lowercase, IPv6 bracketed */) {
  return g_strcmp0(host, "localhost") == 0 ||
         g_str_has_suffix(host, ".localhost") ||
         g_strcmp0(host, "127.0.0.1") == 0 ||
         g_strcmp0(host, "[::1]") == 0;
}

gchar *nm_origin_to_app_id(const gchar *origin, const gchar **why) {
  const gchar *dummy;
  if (!why) why = &dummy;
  *why = NULL;

  if (!origin || !*origin) { *why = "missing origin"; return NULL; }
  gsize total = strlen(origin);
  if (total > NM_MAX_ORIGIN_LEN) { *why = "origin too long"; return NULL; }
  for (gsize i = 0; i < total; i++) {
    guchar c = (guchar)origin[i];
    if (c <= 0x20 || c >= 0x7f) { *why = "origin contains non-ASCII or whitespace"; return NULL; }
  }

  const gchar *sep = strstr(origin, "://");
  if (!sep) { *why = "not a tuple origin"; return NULL; }
  g_autofree gchar *scheme = g_ascii_strdown(origin, sep - origin);
  gboolean https = g_strcmp0(scheme, "https") == 0;
  gboolean http = g_strcmp0(scheme, "http") == 0;
  if (!https && !http) { *why = "scheme must be https (or http for localhost)"; return NULL; }

  const gchar *p = sep + 3;
  g_autofree gchar *host = NULL;
  if (*p == '[') {
    const gchar *close = strchr(p, ']');
    if (!close) { *why = "unterminated IPv6 literal"; return NULL; }
    if (!valid_ipv6_literal(p + 1, (gsize)(close - p - 1))) { *why = "invalid IPv6 literal"; return NULL; }
    g_autofree gchar *lit = g_ascii_strdown(p + 1, close - p - 1);
    host = g_strdup_printf("[%s]", lit);
    p = close + 1;
  } else {
    const gchar *end = p;
    while (*end && *end != ':' && *end != '/') {
      if (*end == '@' || *end == '?' || *end == '#' || *end == '\\') {
        *why = "origin must not carry userinfo, query or fragment";
        return NULL;
      }
      end++;
    }
    if (!valid_hostname(p, (gsize)(end - p))) { *why = "invalid host"; return NULL; }
    host = g_ascii_strdown(p, end - p);
    p = end;
  }

  gint port = -1;
  if (*p == ':') {
    p++;
    const gchar *digits = p;
    while (g_ascii_isdigit(*p)) p++;
    gsize nd = (gsize)(p - digits);
    if (nd == 0 || nd > 5) { *why = "invalid port"; return NULL; }
    port = (gint)g_ascii_strtoll(digits, NULL, 10);
    if (port < 1 || port > 65535) { *why = "invalid port"; return NULL; }
  }
  if (*p == '/') p++;
  if (*p != '\0') { *why = "origin must not carry a path, query or fragment"; return NULL; }

  if (http && !is_loopback_host(host)) {
    *why = "insecure origin (http is only allowed for localhost)";
    return NULL;
  }

  if ((https && port == 443) || (http && port == 80)) port = -1;
  gchar *canon = port > 0 ? g_strdup_printf("%s://%s:%d", scheme, host, port)
                          : g_strdup_printf("%s://%s", scheme, host);
  if (strcmp(canon, origin) != 0) {
    g_free(canon);
    *why = "origin is not in canonical (browser-serialized) form";
    return NULL;
  }
  return canon;
}

/* ------------------------------------------------------------------------
 * Unsigned event validation
 * ---------------------------------------------------------------------- */

static gboolean node_is_int(JsonNode *n, gint64 *out) {
  if (!n || !JSON_NODE_HOLDS_VALUE(n)) return FALSE;
  if (json_node_get_value_type(n) != G_TYPE_INT64) return FALSE;
  if (out) *out = json_node_get_int(n);
  return TRUE;
}

static gboolean node_is_string(JsonNode *n) {
  return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING;
}

gchar *nm_event_canonicalize(JsonNode *event, gint64 now, const gchar **why) {
  const gchar *dummy;
  if (!why) why = &dummy;
  *why = NULL;

  if (!event || !JSON_NODE_HOLDS_OBJECT(event)) { *why = "event must be an object"; return NULL; }
  JsonObject *obj = json_node_get_object(event);

  gint64 kind = 0;
  if (!node_is_int(json_object_get_member(obj, "kind"), &kind)) {
    *why = "kind must be an integer";
    return NULL;
  }
  if (kind < 0 || kind > 65535) { *why = "kind out of range"; return NULL; }

  gint64 created_at = 0;
  JsonNode *ca = json_object_get_member(obj, "created_at");
  if (ca && !JSON_NODE_HOLDS_NULL(ca)) {
    if (!node_is_int(ca, &created_at)) { *why = "created_at must be an integer"; return NULL; }
    if (created_at < 0) { *why = "created_at must be non-negative"; return NULL; }
  }
  if (created_at == 0) created_at = now;

  JsonNode *content = json_object_get_member(obj, "content");
  if (!node_is_string(content)) { *why = "content must be a string"; return NULL; }

  JsonNode *tags = json_object_get_member(obj, "tags");
  if (!tags || !JSON_NODE_HOLDS_ARRAY(tags)) { *why = "tags must be an array"; return NULL; }
  JsonArray *tarr = json_node_get_array(tags);
  guint ntags = json_array_get_length(tarr);
  for (guint i = 0; i < ntags; i++) {
    JsonNode *tag = json_array_get_element(tarr, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag)) { *why = "each tag must be an array"; return NULL; }
    JsonArray *items = json_node_get_array(tag);
    guint nitems = json_array_get_length(items);
    for (guint j = 0; j < nitems; j++) {
      if (!node_is_string(json_array_get_element(items, j))) {
        *why = "tag items must be strings";
        return NULL;
      }
    }
  }

  JsonNode *pk = json_object_get_member(obj, "pubkey");
  if (pk && !JSON_NODE_HOLDS_NULL(pk)) {
    if (!node_is_string(pk) || !nm_is_hex64(json_node_get_string(pk))) {
      *why = "pubkey must be 64 hex characters";
      return NULL;
    }
  }

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "kind");
  json_builder_add_int_value(b, kind);
  json_builder_set_member_name(b, "created_at");
  json_builder_add_int_value(b, created_at);
  json_builder_set_member_name(b, "tags");
  json_builder_add_value(b, json_node_copy(tags));
  json_builder_set_member_name(b, "content");
  json_builder_add_value(b, json_node_copy(content));
  json_builder_end_object(b);

  g_autoptr(JsonNode) root = json_builder_get_root(b);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  return json_generator_to_data(gen, NULL);
}

/* ------------------------------------------------------------------------
 * Signed-event checks
 * ---------------------------------------------------------------------- */

static void nip01_string(GString *out, const gchar *s, gboolean escape_del) {
  g_string_append_c(out, '"');
  for (const guchar *p = (const guchar *)s; *p; p++) {
    switch (*p) {
      case '"':  g_string_append(out, "\\\""); break;
      case '\\': g_string_append(out, "\\\\"); break;
      case '\b': g_string_append(out, "\\b"); break;
      case '\t': g_string_append(out, "\\t"); break;
      case '\n': g_string_append(out, "\\n"); break;
      case '\f': g_string_append(out, "\\f"); break;
      case '\r': g_string_append(out, "\\r"); break;
      default:
        if (*p < 0x20 || (escape_del && *p == 0x7f))
          g_string_append_printf(out, "\\u%04x", (guint)*p);
        else
          g_string_append_c(out, (gchar)*p);
    }
  }
  g_string_append_c(out, '"');
}

gchar *nm_event_id(const gchar *pubkey_hex, gint64 created_at, gint64 kind,
                   JsonNode *tags, const gchar *content, gboolean escape_del) {
  if (!pubkey_hex || !tags || !JSON_NODE_HOLDS_ARRAY(tags) || !content) return NULL;
  g_autoptr(GString) s = g_string_new("[0,");
  nip01_string(s, pubkey_hex, escape_del);
  g_string_append_printf(s, ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",[", created_at, kind);
  JsonArray *tarr = json_node_get_array(tags);
  for (guint i = 0; i < json_array_get_length(tarr); i++) {
    JsonNode *tag = json_array_get_element(tarr, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag)) return NULL;
    if (i) g_string_append_c(s, ',');
    g_string_append_c(s, '[');
    JsonArray *items = json_node_get_array(tag);
    for (guint j = 0; j < json_array_get_length(items); j++) {
      JsonNode *it = json_array_get_element(items, j);
      if (!node_is_string(it)) return NULL;
      if (j) g_string_append_c(s, ',');
      nip01_string(s, json_node_get_string(it), escape_del);
    }
    g_string_append_c(s, ']');
  }
  g_string_append(s, "],");
  nip01_string(s, content, escape_del);
  g_string_append_c(s, ']');
  return g_compute_checksum_for_string(G_CHECKSUM_SHA256, s->str, (gssize)s->len);
}

static gchar *node_to_json(JsonNode *n) {
  g_autoptr(JsonGenerator) g = json_generator_new();
  json_generator_set_root(g, n);
  return json_generator_to_data(g, NULL);
}

static const gchar *obj_string(JsonObject *o, const gchar *name) {
  JsonNode *n = json_object_get_member(o, name);
  return node_is_string(n) ? json_node_get_string(n) : NULL;
}

static gboolean is_hex_len(const gchar *s, gsize want) {
  if (!s || strlen(s) != want) return FALSE;
  for (gsize i = 0; i < want; i++) if (!g_ascii_isxdigit(s[i])) return FALSE;
  return TRUE;
}

gboolean nm_signed_event_matches(const gchar *canonical_unsigned, JsonNode *signed_event,
                                 const gchar **why) {
  const gchar *dummy;
  if (!why) why = &dummy;
  *why = NULL;

  g_autoptr(JsonParser) p = json_parser_new();
  if (!canonical_unsigned || !json_parser_load_from_data(p, canonical_unsigned, -1, NULL)) {
    *why = "no request to compare against";
    return FALSE;
  }
  JsonObject *want = json_node_get_object(json_parser_get_root(p));
  if (!signed_event || !JSON_NODE_HOLDS_OBJECT(signed_event)) { *why = "reply is not an object"; return FALSE; }
  JsonObject *got = json_node_get_object(signed_event);

  gint64 kind = -1, created_at = -1;
  if (!node_is_int(json_object_get_member(got, "kind"), &kind) ||
      kind != json_object_get_int_member(want, "kind")) { *why = "kind changed"; return FALSE; }
  if (!node_is_int(json_object_get_member(got, "created_at"), &created_at) ||
      created_at != json_object_get_int_member(want, "created_at")) { *why = "created_at changed"; return FALSE; }
  const gchar *content = obj_string(got, "content");
  if (g_strcmp0(content, json_object_get_string_member(want, "content")) != 0) { *why = "content changed"; return FALSE; }
  JsonNode *tags = json_object_get_member(got, "tags");
  if (!tags || !JSON_NODE_HOLDS_ARRAY(tags)) { *why = "tags missing"; return FALSE; }
  g_autofree gchar *tags_got = node_to_json(tags);
  g_autofree gchar *tags_want = node_to_json(json_object_get_member(want, "tags"));
  if (g_strcmp0(tags_got, tags_want) != 0) { *why = "tags changed"; return FALSE; }

  const gchar *pk = obj_string(got, "pubkey");
  const gchar *id = obj_string(got, "id");
  if (!nm_is_hex64(pk)) { *why = "pubkey malformed"; return FALSE; }
  if (!nm_is_hex64(id)) { *why = "id malformed"; return FALSE; }
  if (!is_hex_len(obj_string(got, "sig"), 128)) { *why = "sig malformed"; return FALSE; }

  for (int esc = 1; esc >= 0; esc--) {
    g_autofree gchar *calc = nm_event_id(pk, created_at, kind, tags, content, esc);
    if (calc && g_ascii_strcasecmp(calc, id) == 0) return TRUE;
  }
  *why = "id does not match the event";
  return FALSE;
}

/* ------------------------------------------------------------------------
 * Hex / npub
 * ---------------------------------------------------------------------- */

gboolean nm_is_hex64(const gchar *s) {
  if (!s) return FALSE;
  gsize i = 0;
  for (; s[i]; i++) {
    if (i >= 64 || !g_ascii_isxdigit(s[i])) return FALSE;
  }
  return i == 64;
}

gchar *nm_pubkey_normalize(const gchar *s) {
  return nm_is_hex64(s) ? g_ascii_strdown(s, 64) : NULL;
}

static const gchar BECH32_CHARSET[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

static guint32 bech32_polymod_step(guint32 pre) {
  guint8 b = (guint8)(pre >> 25);
  return ((pre & 0x1FFFFFFu) << 5) ^
         (-((b >> 0) & 1) & 0x3b6a57b2u) ^ (-((b >> 1) & 1) & 0x26508e6du) ^
         (-((b >> 2) & 1) & 0x1ea119fau) ^ (-((b >> 3) & 1) & 0x3d4233ddu) ^
         (-((b >> 4) & 1) & 0x2a1462b3u);
}

gchar *nm_pubkey_to_hex(const gchar *in) {
  if (!in) return NULL;
  if (nm_is_hex64(in)) return g_ascii_strdown(in, 64);

  /* npub1 + 52 data chars + 6 checksum chars = 63 */
  gsize n = strlen(in);
  if (n != 63) return NULL;
  g_autofree gchar *s = g_ascii_strdown(in, n);
  g_autofree gchar *upper = g_ascii_strup(in, n);
  if (strcmp(in, s) != 0 && strcmp(in, upper) != 0) return NULL; /* mixed case */
  if (!g_str_has_prefix(s, "npub1")) return NULL;

  const gchar *hrp = "npub";
  guint32 chk = 1;
  for (const gchar *h = hrp; *h; h++) chk = bech32_polymod_step(chk) ^ ((guchar)*h >> 5);
  chk = bech32_polymod_step(chk);
  for (const gchar *h = hrp; *h; h++) chk = bech32_polymod_step(chk) ^ ((guchar)*h & 0x1f);

  guint8 data[58];
  const gchar *d = s + 5;
  for (gsize i = 0; i < 58; i++) {
    const gchar *pos = strchr(BECH32_CHARSET, d[i]);
    if (!pos || !d[i]) return NULL;
    data[i] = (guint8)(pos - BECH32_CHARSET);
    chk = bech32_polymod_step(chk) ^ data[i];
  }
  if (chk != 1) return NULL;

  /* 52 five-bit groups -> 32 bytes (260 bits, 4 zero padding bits). */
  guint8 out[32];
  guint32 acc = 0;
  gint bits = 0;
  gsize o = 0;
  for (gsize i = 0; i < 52; i++) {
    acc = (acc << 5) | data[i];
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      if (o >= sizeof out) return NULL;
      out[o++] = (guint8)((acc >> bits) & 0xFF);
    }
  }
  if (o != 32 || bits >= 5 || (acc & ((1u << bits) - 1)) != 0) return NULL;

  gchar *hex = g_malloc(65);
  for (gsize i = 0; i < 32; i++) g_snprintf(hex + 2 * i, 3, "%02x", out[i]);
  hex[64] = '\0';
  return hex;
}

/* ------------------------------------------------------------------------
 * getRelays shape
 * ---------------------------------------------------------------------- */

JsonNode *nm_relays_to_nip07(const gchar *relays_json) {
  if (!relays_json) return NULL;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, relays_json, -1, NULL)) return NULL;
  JsonNode *root = json_parser_get_root(parser);
  if (!root) return NULL;
  if (JSON_NODE_HOLDS_OBJECT(root)) return json_node_copy(root);
  if (!JSON_NODE_HOLDS_ARRAY(root)) return NULL;

  JsonObject *out = json_object_new();
  JsonArray *arr = json_node_get_array(root);
  guint n = json_array_get_length(arr);
  for (guint i = 0; i < n; i++) {
    JsonNode *el = json_array_get_element(arr, i);
    if (!node_is_string(el)) continue;
    const gchar *url = json_node_get_string(el);
    if (!g_str_has_prefix(url, "wss://") && !g_str_has_prefix(url, "ws://")) continue;
    JsonObject *rw = json_object_new();
    json_object_set_boolean_member(rw, "read", TRUE);
    json_object_set_boolean_member(rw, "write", TRUE);
    json_object_set_object_member(out, url, rw);
  }
  JsonNode *node = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(node, out);
  return node;
}

gboolean nm_json_has_nul_escape(const gchar *json, gsize len) {
  if (!json) return FALSE;
  gsize i = 0;
  while (i < len) {
    if (json[i] != '\\') { i++; continue; }
    gsize j = i;
    while (j < len && json[j] == '\\') j++;
    gsize run = j - i;
    if ((run & 1) && j + 5 <= len && (json[j] == 'u' || json[j] == 'U') &&
        json[j + 1] == '0' && json[j + 2] == '0' && json[j + 3] == '0' && json[j + 4] == '0')
      return TRUE;
    i = j;
  }
  return FALSE;
}
