/* nsp-query.c — see nsp-query.h. */
#include "nsp-query.h"

#include <string.h>

#include "nostr_nip05.h"
#include "nsp-text.h"

void nsp_query_free(NspQuery *q) {
  if (!q) return;
  nd_target_free(q->target);
  g_free(q->hex);
  g_free(q->nip05);
  g_free(q->nip05_local);
  g_free(q->nip05_domain);
  g_free(q->text);
  g_strfreev(q->words);
  g_free(q);
}

const char *nsp_query_type_name(NspQueryType t) {
  switch (t) {
  case NSP_QUERY_PROFILE: return "profile";
  case NSP_QUERY_EVENT: return "event";
  case NSP_QUERY_ADDRESS: return "address";
  case NSP_QUERY_HEX: return "hex";
  case NSP_QUERY_NIP05: return "nip05";
  case NSP_QUERY_TEXT: return "text";
  default: return "none";
  }
}

gboolean nsp_query_is_identifier(const NspQuery *q) {
  return q && q->type != NSP_QUERY_NONE && q->type != NSP_QUERY_TEXT;
}

static gboolean is_hex64(const char *s) {
  if (!s || strlen(s) != 64) return FALSE;
  for (const char *p = s; *p; p++)
    if (!g_ascii_isxdigit(*p)) return FALSE;
  return TRUE;
}

/* bech32 data charset (lower case). */
static gboolean is_bech32_data(const char *s) {
  for (const char *p = s; *p; p++)
    if (!strchr("qpzry9x8gf2tvdw0s3jn54khce6mua7l", *p)) return FALSE;
  return TRUE;
}

typedef enum { B_NOT, B_ID, B_SECRET } BechKind;

/* Classify a lower-cased single token as a NIP-19 identifier attempt. */
static BechKind bech_kind(const char *lower) {
  const char *s = lower;
  if (g_str_has_prefix(s, "web+nostr:")) s += 10;
  else if (g_str_has_prefix(s, "nostr:")) s += 6;
  while (*s == '/') s++;
  static const struct { const char *hrp; BechKind k; } hrps[] = {
      {"npub1", B_ID},    {"nprofile1", B_ID}, {"note1", B_ID},
      {"nevent1", B_ID},  {"naddr1", B_ID},    {"nrelay1", B_ID},
      {"nsec1", B_SECRET}, {"ncryptsec1", B_SECRET},
  };
  for (guint i = 0; i < G_N_ELEMENTS(hrps); i++) {
    gsize n = strlen(hrps[i].hrp);
    if (g_str_has_prefix(s, hrps[i].hrp) && is_bech32_data(s + n)) return hrps[i].k;
  }
  /* A scheme prefix alone ("nostr:") is still an identifier attempt. */
  return s != lower ? B_ID : B_NOT;
}

gboolean nsp_nip05_domain_plausible(const char *domain) {
  if (!domain || !*domain) return FALSE;
  const char *dot = strrchr(domain, '.');
  if (!dot || dot == domain || !dot[1]) return FALSE;
  const char *tld = dot + 1;
  if (strlen(tld) < 2) return FALSE;
  for (const char *p = tld; *p; p++)
    if (!g_ascii_isalpha(*p)) return FALSE; /* also rejects IPv4 literals */
  static const char *const blocked[] = {"local", "localhost", "internal", "lan",
                                        "home.arpa", "onion", "test", "invalid",
                                        "example", "localdomain"};
  for (guint i = 0; i < G_N_ELEMENTS(blocked); i++) {
    gsize bl = strlen(blocked[i]), dl = strlen(domain);
    if (dl >= bl && g_ascii_strcasecmp(domain + dl - bl, blocked[i]) == 0 &&
        (dl == bl || domain[dl - bl - 1] == '.'))
      return FALSE;
  }
  return TRUE;
}

static char **fold_words(const char *text) {
  g_autofree char *folded = nsp_text_fold(text);
  g_auto(GStrv) parts = g_strsplit(folded, " ", -1);
  GPtrArray *w = g_ptr_array_new();
  for (char **p = parts; *p && w->len < NSP_TEXT_MAX_WORDS; p++) {
    if (!**p) continue;
    gboolean dup = FALSE;
    for (guint i = 0; i < w->len && !dup; i++) dup = strcmp(w->pdata[i], *p) == 0;
    if (!dup) g_ptr_array_add(w, g_strdup(*p));
  }
  g_ptr_array_add(w, NULL);
  return (char **)g_ptr_array_free(w, FALSE);
}

/* Truncate @s in place to at most @max bytes on a UTF-8 boundary. */
static void utf8_clamp(char *s, gsize max) {
  if (strlen(s) <= max) return;
  const char *end = g_utf8_find_prev_char(s, s + max + 1);
  s[end ? (gsize)(end - s) : 0] = '\0';
}

/* Any token that looks like a secret key (valid bech32 or not) poisons
 * the whole query: it must not be sent to the relay as search text. */
static gboolean mentions_secret(const char *clean) {
  g_autofree char *lower = g_ascii_strdown(clean, -1);
  g_auto(GStrv) toks = g_strsplit(lower, " ", -1);
  for (char **t = toks; *t; t++) {
    const char *s = *t;
    if (g_str_has_prefix(s, "web+nostr:")) s += 10;
    else if (g_str_has_prefix(s, "nostr:")) s += 6;
    while (*s == '/') s++;
    if (g_str_has_prefix(s, "nsec1") || g_str_has_prefix(s, "ncryptsec1")) return TRUE;
  }
  return FALSE;
}

NspQuery *nsp_query_classify(const char *const *terms) {
  NspQuery *q = g_new0(NspQuery, 1);
  if (!terms) return q;
  g_autofree char *joined = g_strjoinv(" ", (char **)terms);
  g_autofree char *clean = nsp_text_sanitize(joined, 0);
  if (!*clean || mentions_secret(clean)) return q;

  if (!strchr(clean, ' ')) {
    g_autofree char *lower = g_ascii_strdown(clean, -1);
    switch (bech_kind(lower)) {
    case B_SECRET:
      return q; /* NONE: never search for, log or forward a secret key */
    case B_ID: {
      NdTarget *t = nd_target_parse_uri(lower, NULL);
      if (!t) return q; /* partially typed identifier */
      q->target = t;
      q->type = t->entity == ND_ENTITY_PROFILE   ? NSP_QUERY_PROFILE
                : t->entity == ND_ENTITY_ADDRESS ? NSP_QUERY_ADDRESS
                                                 : NSP_QUERY_EVENT;
      return q;
    }
    case B_NOT:
      break;
    }
    if (is_hex64(lower)) {
      q->type = NSP_QUERY_HEX;
      q->hex = g_steal_pointer(&lower);
      return q;
    }
    nh_nip05_address a;
    if (strchr(lower, '@') && nh_nip05_parse(lower, &a) == 0 &&
        nsp_nip05_domain_plausible(a.domain)) {
      q->type = NSP_QUERY_NIP05;
      q->nip05_local = g_ascii_strdown(a.local, -1);
      q->nip05_domain = g_ascii_strdown(a.domain, -1);
      q->nip05 = g_strdup_printf("%s@%s", q->nip05_local, q->nip05_domain);
      q->words = fold_words(q->nip05);
      return q;
    }
  }

  if ((gsize)g_utf8_strlen(clean, -1) < NSP_TEXT_MIN_CHARS) return q;
  utf8_clamp(clean, NSP_TEXT_MAX_BYTES);
  q->type = NSP_QUERY_TEXT;
  q->text = g_steal_pointer(&clean);
  q->words = fold_words(q->text);
  return q;
}

/* ---- filters ---------------------------------------------------------- */

static void add_int_array(JsonBuilder *b, const char *name, const gint *v, guint n) {
  json_builder_set_member_name(b, name);
  json_builder_begin_array(b);
  for (guint i = 0; i < n; i++) json_builder_add_int_value(b, v[i]);
  json_builder_end_array(b);
}

static void add_str_array(JsonBuilder *b, const char *name, const char *const *v) {
  json_builder_set_member_name(b, name);
  json_builder_begin_array(b);
  for (; v && *v; v++) json_builder_add_string_value(b, *v);
  json_builder_end_array(b);
}

static void add_limit(JsonBuilder *b, gint64 limit) {
  json_builder_set_member_name(b, "limit");
  json_builder_add_int_value(b, limit);
}

static void kind_author_filter(JsonBuilder *b, gint kind, const char *pk, const char *d) {
  json_builder_begin_object(b);
  add_int_array(b, "kinds", &kind, 1);
  const char *authors[] = {pk, NULL};
  add_str_array(b, "authors", authors);
  if (d) {
    const char *ds[] = {d, NULL};
    add_str_array(b, "#d", ds);
  }
  add_limit(b, 1);
  json_builder_end_object(b);
}

static void id_filter(JsonBuilder *b, const char *id) {
  json_builder_begin_object(b);
  const char *ids[] = {id, NULL};
  add_str_array(b, "ids", ids);
  add_limit(b, 1);
  json_builder_end_object(b);
}

static void search_filter(JsonBuilder *b, const gint *kinds, guint nk, const char *search,
                          gint64 limit) {
  json_builder_begin_object(b);
  add_int_array(b, "kinds", kinds, nk);
  json_builder_set_member_name(b, "search");
  json_builder_add_string_value(b, search);
  add_limit(b, limit);
  json_builder_end_object(b);
}

static JsonNode *finish(JsonBuilder *b) {
  json_builder_end_array(b);
  return json_builder_get_root(b);
}

JsonNode *nsp_query_filters(const NspQuery *q) {
  if (!q) return NULL;
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_array(b);
  switch (q->type) {
  case NSP_QUERY_PROFILE:
    kind_author_filter(b, 0, q->target->pubkey_hex, NULL);
    break;
  case NSP_QUERY_EVENT:
    id_filter(b, q->target->id_hex);
    break;
  case NSP_QUERY_ADDRESS:
    kind_author_filter(b, q->target->kind, q->target->pubkey_hex,
                       q->target->identifier ? q->target->identifier : "");
    break;
  case NSP_QUERY_HEX:
    id_filter(b, q->hex);
    kind_author_filter(b, 0, q->hex, NULL);
    break;
  case NSP_QUERY_NIP05: {
    gint k0 = 0;
    search_filter(b, &k0, 1, q->nip05, NSP_NIP05_CLAIM_LIMIT);
    break;
  }
  case NSP_QUERY_TEXT: {
    static const gint kinds[] = {0, 1, 30023};
    search_filter(b, kinds, G_N_ELEMENTS(kinds), q->text, NSP_TEXT_SEARCH_LIMIT);
    break;
  }
  default:
    return NULL;
  }
  return finish(b);
}

JsonNode *nsp_filters_profiles(const char *const *pubkeys) {
  if (!pubkeys || !pubkeys[0]) return NULL;
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_array(b);
  json_builder_begin_object(b);
  gint k0 = 0;
  add_int_array(b, "kinds", &k0, 1);
  add_str_array(b, "authors", pubkeys);
  add_limit(b, g_strv_length((char **)pubkeys));
  json_builder_end_object(b);
  return finish(b);
}

JsonNode *nsp_filters_nip05_claim(const char *address) {
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_array(b);
  gint k0 = 0;
  search_filter(b, &k0, 1, address, NSP_NIP05_CLAIM_LIMIT);
  return finish(b);
}

JsonNode *nsp_filters_text_scan(void) {
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_array(b);
  json_builder_begin_object(b);
  gint k0 = 0;
  add_int_array(b, "kinds", &k0, 1);
  add_limit(b, NSP_SCAN_PROFILE_LIMIT);
  json_builder_end_object(b);
  json_builder_begin_object(b);
  static const gint notes[] = {1, 30023};
  add_int_array(b, "kinds", notes, G_N_ELEMENTS(notes));
  add_limit(b, NSP_SCAN_NOTE_LIMIT);
  json_builder_end_object(b);
  return finish(b);
}

char *nsp_json_to_string(JsonNode *node) {
  return node ? json_to_string(node, FALSE) : NULL;
}
