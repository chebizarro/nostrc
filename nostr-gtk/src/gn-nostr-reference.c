#include <nostr-gtk-1.0/gn-nostr-reference.h>
#include <nostr/nip19/nip19.h>
#include <json-glib/json-glib.h>
#include <nostr-event.h>
#include <nostr-tag.h>
#include <stdlib.h>
#include <string.h>

static gboolean hex64(const char *s) {
  if (!s || strlen(s) != 64) return FALSE;
  for (guint i = 0; i < 64; i++) if (!g_ascii_isxdigit(s[i])) return FALSE;
  return TRUE;
}

static gchar *bytes_hex(const guint8 bytes[32]) {
  gchar *s = g_malloc(65);
  for (guint i = 0; i < 32; i++) g_snprintf(s + 2 * i, 3, "%02x", bytes[i]);
  return s;
}

static gchar **copy_relays(char **relays, size_t count) {
  gchar **out = g_new0(gchar *, MIN(count, 8) + 1);
  for (size_t i = 0; i < count && i < 8; i++) out[i] = g_strdup(relays[i]);
  return out;
}

void gn_nostr_reference_free(GnNostrReference *r) {
  if (!r) return;
  g_free(r->uri); g_free(r->id); g_free(r->author);
  g_strfreev(r->relay_hints);
  g_free(r);
}

G_DEFINE_QUARK(gn-nostr-reference-error-quark, gn_nostr_reference_error)

#define REF_FAIL(code, ...) \
  do { g_set_error(error, GN_NOSTR_REFERENCE_ERROR, code, __VA_ARGS__); goto fail; } while (0)

/* Ported from gnostr-nostr-target / nip21_uri: case-insensitive scheme,
 * single-case bech32, typed refusal of secrets and nrelay. Kept from the
 * Groundhog reimplementation: length bound, no query/fragment/path, nsec is
 * never decoded, relay hints inert. */
GnNostrReference *gn_nostr_reference_parse_full(const gchar *uri, GError **error) {
  GnNostrReference *r = NULL;
  g_autofree gchar *value = NULL;
  g_autofree gchar *upper = NULL;
  if (!uri || !*uri || strlen(uri) > 2048) {
    g_set_error(error, GN_NOSTR_REFERENCE_ERROR, GN_NOSTR_REFERENCE_ERROR_INVALID,
                "Not a nostr: reference");
    return NULL;
  }
  const char *raw = g_ascii_strncasecmp(uri, "nostr:", 6) == 0 ? uri + 6 : uri;
  if (!*raw || strpbrk(raw, "?#/%")) {
    g_set_error(error, GN_NOSTR_REFERENCE_ERROR, GN_NOSTR_REFERENCE_ERROR_INVALID,
                "Not a NIP-21 nostr: reference");
    return NULL;
  }
  value = g_ascii_strdown(raw, -1);
  upper = g_ascii_strup(raw, -1);
  /* Bech32 forbids mixed case. */
  if (strcmp(raw, value) != 0 && strcmp(raw, upper) != 0) {
    g_set_error(error, GN_NOSTR_REFERENCE_ERROR, GN_NOSTR_REFERENCE_ERROR_INVALID,
                "Mixed-case nostr: reference");
    return NULL;
  }
  if (g_str_has_prefix(value, "nsec1") || g_str_has_prefix(value, "ncryptsec1")) {
    g_set_error(error, GN_NOSTR_REFERENCE_ERROR, GN_NOSTR_REFERENCE_ERROR_REFUSED,
                "Refusing to open a private key reference");
    return NULL;
  }
  if (g_str_has_prefix(value, "nrelay1")) {
    g_set_error(error, GN_NOSTR_REFERENCE_ERROR, GN_NOSTR_REFERENCE_ERROR_UNSUPPORTED,
                "nrelay references are not supported");
    return NULL;
  }
  r = g_new0(GnNostrReference, 1);
  r->uri = g_strdup_printf("nostr:%s", value);
  r->kind = -1;
  guint8 bytes[32];
  if (g_str_has_prefix(value, "npub1")) {
    if (nostr_nip19_decode_npub(value, bytes))
      REF_FAIL(GN_NOSTR_REFERENCE_ERROR_INVALID, "Invalid npub");
    r->type = GN_NOSTR_REFERENCE_PERSON;
    r->author = bytes_hex(bytes);
  } else if (g_str_has_prefix(value, "note1")) {
    if (nostr_nip19_decode_note(value, bytes))
      REF_FAIL(GN_NOSTR_REFERENCE_ERROR_INVALID, "Invalid note");
    r->type = GN_NOSTR_REFERENCE_EVENT;
    r->id = bytes_hex(bytes);
  } else if (g_str_has_prefix(value, "nprofile1")) {
    NostrProfilePointer *p = NULL;
    if (nostr_nip19_decode_nprofile(value, &p) || !p || !hex64(p->public_key)) {
      nostr_profile_pointer_free(p);
      REF_FAIL(GN_NOSTR_REFERENCE_ERROR_INVALID, "Invalid nprofile");
    }
    r->type = GN_NOSTR_REFERENCE_PERSON;
    r->author = g_ascii_strdown(p->public_key, -1);
    r->relay_hints = copy_relays(p->relays, p->relays_count);
    nostr_profile_pointer_free(p);
  } else if (g_str_has_prefix(value, "nevent1")) {
    NostrEventPointer *p = NULL;
    if (nostr_nip19_decode_nevent(value, &p) || !p || !hex64(p->id)) {
      nostr_event_pointer_free(p);
      REF_FAIL(GN_NOSTR_REFERENCE_ERROR_INVALID, "Invalid nevent");
    }
    r->type = GN_NOSTR_REFERENCE_EVENT;
    r->id = g_ascii_strdown(p->id, -1);
    if (hex64(p->author)) r->author = g_ascii_strdown(p->author, -1);
    /* A missing kind TLV decodes as 0: profiles are npub/nprofile, so a
     * nevent "kind 0" means "kind unknown". */
    r->kind = p->kind > 0 ? p->kind : -1;
    r->relay_hints = copy_relays(p->relays, p->relays_count);
    nostr_event_pointer_free(p);
  } else if (g_str_has_prefix(value, "naddr1")) {
    NostrEntityPointer *p = NULL;
    if (nostr_nip19_decode_naddr(value, &p) || !p || !hex64(p->public_key) ||
        p->kind <= 0 || p->kind > 65535) {
      nostr_entity_pointer_free(p);
      REF_FAIL(GN_NOSTR_REFERENCE_ERROR_INVALID, "Invalid naddr");
    }
    r->type = GN_NOSTR_REFERENCE_ADDRESS;
    /* An empty d tag is a valid replaceable-event coordinate. */
    r->id = g_strdup(p->identifier ? p->identifier : "");
    r->author = g_ascii_strdown(p->public_key, -1);
    r->kind = p->kind;
    r->relay_hints = copy_relays(p->relays, p->relays_count);
    nostr_entity_pointer_free(p);
  } else {
    NostrBech32Type t = NOSTR_B32_UNKNOWN;
    if (nostr_nip19_inspect(value, &t) == 0 && t != NOSTR_B32_UNKNOWN)
      REF_FAIL(GN_NOSTR_REFERENCE_ERROR_UNSUPPORTED, "Unsupported nostr: reference");
    REF_FAIL(GN_NOSTR_REFERENCE_ERROR_INVALID, "Invalid nostr: reference");
  }
  return r;
fail:
  gn_nostr_reference_free(r);
  return NULL;
}

GnNostrReference *gn_nostr_reference_parse(const gchar *uri) {
  return gn_nostr_reference_parse_full(uri, NULL);
}

/* ---- builders (ported from gnostr nip21_uri) ---------------------------- */

static gboolean hex_bytes(const char *hex, guint8 out[32]) {
  if (!hex64(hex)) return FALSE;
  for (guint i = 0; i < 32; i++)
    out[i] = (guint8)((g_ascii_xdigit_value(hex[2 * i]) << 4) |
                      g_ascii_xdigit_value(hex[2 * i + 1]));
  return TRUE;
}

static gchar *with_scheme(char *encoded) {
  if (!encoded) return NULL;
  gchar *uri = g_strdup_printf("nostr:%s", encoded);
  free(encoded);
  return uri;
}

static gsize relay_count(const gchar *const *relays) {
  gsize n = 0;
  while (relays && relays[n] && n < 8) n++;
  return n;
}

gchar *gn_nostr_reference_build_person(const gchar *pubkey_hex,
                                       const gchar *const *relays) {
  guint8 bytes[32];
  char *encoded = NULL;
  if (!hex_bytes(pubkey_hex, bytes)) return NULL;
  if (relay_count(relays) == 0) {
    if (nostr_nip19_encode_npub(bytes, &encoded)) return NULL;
  } else {
    g_autofree gchar *pk = g_ascii_strdown(pubkey_hex, -1);
    NostrProfilePointer p = { .public_key = pk, .relays = (char **)relays,
                              .relays_count = relay_count(relays) };
    if (nostr_nip19_encode_nprofile(&p, &encoded)) return NULL;
  }
  return with_scheme(encoded);
}

gchar *gn_nostr_reference_build_event(const gchar *id_hex,
                                      const gchar *author_hex,
                                      gint kind,
                                      const gchar *const *relays) {
  guint8 bytes[32];
  char *encoded = NULL;
  if (!hex_bytes(id_hex, bytes)) return NULL;
  if (author_hex && !hex64(author_hex)) return NULL;
  if (!author_hex && kind <= 0 && relay_count(relays) == 0) {
    if (nostr_nip19_encode_note(bytes, &encoded)) return NULL;
  } else {
    g_autofree gchar *id = g_ascii_strdown(id_hex, -1);
    g_autofree gchar *author = author_hex ? g_ascii_strdown(author_hex, -1) : NULL;
    NostrEventPointer p = { .id = id, .author = author, .kind = kind > 0 ? kind : 0,
                            .relays = (char **)relays, .relays_count = relay_count(relays) };
    if (nostr_nip19_encode_nevent(&p, &encoded)) return NULL;
  }
  return with_scheme(encoded);
}

gchar *gn_nostr_reference_build_address(const gchar *author_hex,
                                        gint kind,
                                        const gchar *identifier,
                                        const gchar *const *relays) {
  char *encoded = NULL;
  if (!hex64(author_hex) || kind <= 0 || kind > 65535 || !identifier) return NULL;
  g_autofree gchar *author = g_ascii_strdown(author_hex, -1);
  NostrEntityPointer p = { .identifier = (char *)identifier, .public_key = author,
                           .kind = kind, .relays = (char **)relays,
                           .relays_count = relay_count(relays) };
  if (nostr_nip19_encode_naddr(&p, &encoded)) return NULL;
  return with_scheme(encoded);
}

/* ---- event verification (ported from gnostr-nostr-target) -------------- */

void gn_nostr_event_info_free(GnNostrEventInfo *info) {
  if (!info) return;
  g_free(info->id); g_free(info->pubkey); g_free(info->d_tag);
  g_free(info->content);
  g_free(info);
}

static gchar *first_d_tag(NostrEvent *ev) {
  NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
  gsize n = tags ? nostr_tags_size(tags) : 0;
  for (gsize i = 0; i < n; i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag || nostr_tag_size(tag) < 2) continue;
    const char *key = nostr_tag_get(tag, 0);
    if (key && strcmp(key, "d") == 0) {
      const char *v = nostr_tag_get(tag, 1);
      return g_strdup(v ? v : "");
    }
  }
  return NULL;
}

static GnNostrEventInfo *event_info_copy(const GnNostrEventInfo *info) {
  GnNostrEventInfo *copy = g_new0(GnNostrEventInfo, 1);
  copy->id = g_strdup(info->id);
  copy->pubkey = g_strdup(info->pubkey);
  copy->kind = info->kind;
  copy->created_at = info->created_at;
  copy->d_tag = g_strdup(info->d_tag);
  copy->content = g_strdup(info->content);
  return copy;
}

/* Verification cache (nostrc-8xfib.6). Rows re-render the same embedded
 * originals and resolver results on every bind; a Schnorr verification costs
 * far more than hashing the bytes, so successful verifications are memoised
 * by the SHA-256 of the exact JSON. Bounded in entries and per-entry size;
 * a full cache is simply emptied. Thread-safe: renderers run off-thread. */
#define VERIFY_CACHE_MAX_ENTRIES 512
#define VERIFY_CACHE_MAX_JSON (64 * 1024)
static GMutex verify_cache_lock;
static GHashTable *verify_cache; /* sha256 hex -> GnNostrEventInfo */

static gchar *verify_cache_key(const gchar *event_json, gsize len) {
  return len <= VERIFY_CACHE_MAX_JSON
    ? g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)event_json, len)
    : NULL;
}

void gn_nostr_event_verify_cache_clear(void) {
  g_mutex_lock(&verify_cache_lock);
  g_clear_pointer(&verify_cache, g_hash_table_unref);
  g_mutex_unlock(&verify_cache_lock);
}

GnNostrEventInfo *gn_nostr_event_parse(const gchar *event_json, GError **error) {
  gsize len = event_json ? strlen(event_json) : 0;
  if (!len || len > 256 * 1024) {
    g_set_error(error, GN_NOSTR_REFERENCE_ERROR, GN_NOSTR_REFERENCE_ERROR_INVALID,
                "Empty or oversized event");
    return NULL;
  }
  g_autofree gchar *key = verify_cache_key(event_json, len);
  if (key) {
    GnNostrEventInfo *hit = NULL;
    g_mutex_lock(&verify_cache_lock);
    const GnNostrEventInfo *cached = verify_cache ? g_hash_table_lookup(verify_cache, key) : NULL;
    if (cached) hit = event_info_copy(cached);
    g_mutex_unlock(&verify_cache_lock);
    if (hit) return hit;
  }
  NostrEvent *ev = nostr_event_new();
  if (!ev) {
    g_set_error(error, GN_NOSTR_REFERENCE_ERROR, GN_NOSTR_REFERENCE_ERROR_INVALID,
                "Out of memory");
    return NULL;
  }
  char id[65] = { 0 };
  NostrEventValidationStatus st = nostr_event_deserialize_signed(ev, event_json, NULL);
  if (st == NOSTR_EVENT_VALIDATION_OK)
    st = nostr_event_validate(ev, id);
  if (st != NOSTR_EVENT_VALIDATION_OK) {
    g_set_error(error, GN_NOSTR_REFERENCE_ERROR, GN_NOSTR_REFERENCE_ERROR_INVALID,
                "Invalid event: %s", nostr_event_validation_status_string(st));
    nostr_event_free(ev);
    return NULL;
  }
  GnNostrEventInfo *info = g_new0(GnNostrEventInfo, 1);
  info->id = g_ascii_strdown(id, -1);
  info->pubkey = g_ascii_strdown(nostr_event_get_pubkey(ev), -1);
  info->kind = nostr_event_get_kind(ev);
  info->created_at = nostr_event_get_created_at(ev);
  info->d_tag = first_d_tag(ev);
  const char *content = nostr_event_get_content(ev);
  info->content = g_strdup(content ? content : "");
  nostr_event_free(ev);
  if (key) {
    g_mutex_lock(&verify_cache_lock);
    if (!verify_cache)
      verify_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           (GDestroyNotify)gn_nostr_event_info_free);
    if (g_hash_table_size(verify_cache) >= VERIFY_CACHE_MAX_ENTRIES)
      g_hash_table_remove_all(verify_cache);
    g_hash_table_replace(verify_cache, g_steal_pointer(&key), event_info_copy(info));
    g_mutex_unlock(&verify_cache_lock);
  }
  return info;
}

gboolean gn_nostr_event_verify(const gchar *event_json, GError **error) {
  g_autoptr(GnNostrEventInfo) info = gn_nostr_event_parse(event_json, error);
  return info != NULL;
}

gboolean gn_nostr_reference_matches_event_info(const GnNostrReference *r,
                                               const GnNostrEventInfo *info) {
  g_return_val_if_fail(r != NULL && info != NULL, FALSE);
  switch (r->type) {
    case GN_NOSTR_REFERENCE_EVENT:
      if (g_strcmp0(info->id, r->id) != 0) return FALSE;
      if (r->kind >= 0 && info->kind != r->kind) return FALSE;
      if (r->author && g_strcmp0(info->pubkey, r->author) != 0) return FALSE;
      return TRUE;
    case GN_NOSTR_REFERENCE_ADDRESS:
      return info->kind == r->kind && g_strcmp0(info->pubkey, r->author) == 0 &&
             g_strcmp0(info->d_tag ? info->d_tag : "", r->id ? r->id : "") == 0;
    case GN_NOSTR_REFERENCE_PERSON:
    default:
      return info->kind == 0 && g_strcmp0(info->pubkey, r->author) == 0;
  }
}

gboolean gn_nostr_reference_matches_event(const GnNostrReference *r,
                                          const gchar *event_json) {
  g_return_val_if_fail(r != NULL, FALSE);
  g_autoptr(GnNostrEventInfo) info = gn_nostr_event_parse(event_json, NULL);
  return info && gn_nostr_reference_matches_event_info(r, info);
}

void gn_nostr_repost_descriptor_free(GnNostrRepostDescriptor *d) {
  if (!d) return;
  gn_nostr_reference_free(d->target);
  g_free(d->original_json);
  g_free(d);
}

static const char *object_string(JsonObject *obj, const char *name) {
  JsonNode *n = json_object_get_member(obj, name);
  return n && JSON_NODE_HOLDS_VALUE(n) &&
    json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : NULL;
}

static const char *tag_at(JsonArray *a, guint index) {
  if (json_array_get_length(a) <= index) return NULL;
  JsonNode *n = json_array_get_element(a, index);
  return JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ?
    json_node_get_string(n) : NULL;
}

static gchar *address_from_tag(const char *tag) {
  if (!tag) return NULL;
  const char *one = strchr(tag, ':');
  if (!one) return NULL;
  const char *two = strchr(one + 1, ':');
  if (!two || two - one != 65) return NULL;
  g_autofree char *author = g_strndup(one + 1, 64);
  if (!hex64(author)) return NULL;
  char *end = NULL;
  gint64 kind = g_ascii_strtoll(tag, &end, 10);
  if (end != one || kind <= 0 || kind > G_MAXINT) return NULL;
  return g_strdup(tag);
}

GnNostrRepostDescriptor *gn_nostr_repost_descriptor_parse(const gchar *event_json,
                                                           gboolean embedded_event_verified) {
  if (!event_json || strlen(event_json) > 256 * 1024) return NULL;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, event_json, -1, NULL)) return NULL;
  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) return NULL;
  JsonObject *obj = json_node_get_object(root);
  JsonNode *kind_node = json_object_get_member(obj, "kind");
  JsonNode *tags_node = json_object_get_member(obj, "tags");
  if (!kind_node || !JSON_NODE_HOLDS_VALUE(kind_node) ||
      json_node_get_value_type(kind_node) != G_TYPE_INT64 ||
      !tags_node || !JSON_NODE_HOLDS_ARRAY(tags_node)) return NULL;
  gint64 kind64 = json_node_get_int(kind_node);
  if (kind64 != 1 && kind64 != 6 && kind64 != 16) return NULL;
  gint kind = (gint)kind64;
  JsonArray *tags = json_node_get_array(tags_node);
  if (!tags || json_array_get_length(tags) > 1024) return NULL;
  const char *target = NULL, *relay = NULL, *author = NULL, *kind_tag = NULL;
  gboolean address = FALSE;
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonNode *node = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(node)) continue;
    JsonArray *a = json_node_get_array(node);
    const char *name = tag_at(a, 0), *value = tag_at(a, 1);
    if (!name || !value) continue;
    if (!target && ((kind == 1 && !strcmp(name, "q")) ||
                    (kind != 1 && (!strcmp(name, "e") || !strcmp(name, "a"))))) {
      target = value; relay = tag_at(a, 2);
      address = !strcmp(name, "a") || (kind == 1 && !hex64(value));
      if (kind == 1) author = tag_at(a, 3);
    } else if (!author && !strcmp(name, "p")) author = value;
    else if (!kind_tag && !strcmp(name, "k")) kind_tag = value;
  }
  if (!target) return NULL;
  GnNostrRepostDescriptor *d = g_new0(GnNostrRepostDescriptor, 1);
  char *encoded = NULL;
  d->source_kind = kind;
  d->quote = kind == 1;
  d->target = g_new0(GnNostrReference, 1);
  d->target->type = address ? GN_NOSTR_REFERENCE_ADDRESS : GN_NOSTR_REFERENCE_EVENT;
  d->target->kind = kind == 6 ? 1 : -1;
  if (address) {
    g_autofree gchar *coordinate = address_from_tag(target);
    if (!coordinate) goto fail;
    const char *one = strchr(coordinate, ':'), *two = strchr(one + 1, ':');
    d->target->id = g_strdup(two + 1);
    d->target->author = g_ascii_strdown(one + 1, 64);
    d->target->kind = (gint)g_ascii_strtoll(coordinate, NULL, 10);
  } else {
    if (!hex64(target)) goto fail;
    d->target->id = g_ascii_strdown(target, -1);
    if (hex64(author)) d->target->author = g_ascii_strdown(author, -1);
    if (kind == 16 && kind_tag) {
      char *end = NULL;
      gint64 k = g_ascii_strtoll(kind_tag, &end, 10);
      if (*end == '\0' && k > 0 && k <= G_MAXINT) d->target->kind = (gint)k;
    }
  }
  if (relay && *relay) {
    d->target->relay_hints = g_new0(gchar *, 2);
    d->target->relay_hints[0] = g_strdup(relay);
  }
  /* The embedded original of a kind 6/16 is used only once it verifies. */
  const char *embedded_content = kind != 1 ? object_string(obj, "content") : NULL;
  if (!embedded_event_verified && embedded_content && *embedded_content)
    embedded_event_verified = gn_nostr_event_verify(embedded_content, NULL);
  if (embedded_event_verified && kind != 1 && json_object_has_member(obj, "content")) {
    const char *content = embedded_content;
    g_autoptr(JsonParser) embedded = json_parser_new();
    if (content && json_parser_load_from_data(embedded, content, -1, NULL) &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(embedded))) {
      JsonObject *original = json_node_get_object(json_parser_get_root(embedded));
      const char *id = object_string(original, "id");
      const char *pubkey = object_string(original, "pubkey");
      if (id && d->target->type == GN_NOSTR_REFERENCE_EVENT && !g_ascii_strcasecmp(id, d->target->id)) {
        d->original_json = g_strdup(content);
        if (hex64(pubkey)) {
          g_free(d->target->author);
          d->target->author = g_ascii_strdown(pubkey, -1);
          d->target->author_authenticated = TRUE;
        }
      }
    }
  }
  /* Descriptor consumers need a canonical shareable NIP-21 URI, not only
   * the raw e/a tag value. Include the verified author when one was supplied;
   * relay hints remain inert. */
  if (address) {
    NostrEntityPointer pointer = { .identifier = d->target->id,
      .public_key = d->target->author, .kind = d->target->kind,
      .relays = d->target->relay_hints,
      .relays_count = d->target->relay_hints ? g_strv_length(d->target->relay_hints) : 0 };
    if (nostr_nip19_encode_naddr(&pointer, &encoded)) goto fail;
  } else {
    NostrEventPointer pointer = { .id = d->target->id,
      .author = d->target->author,
      .kind = d->target->kind > 0 ? d->target->kind : 0,
      .relays = d->target->relay_hints,
      .relays_count = d->target->relay_hints ? g_strv_length(d->target->relay_hints) : 0 };
    if (nostr_nip19_encode_nevent(&pointer, &encoded)) goto fail;
  }
  d->target->uri = g_strdup_printf("nostr:%s", encoded);
  free(encoded);
  return d;
fail:
  free(encoded);
  gn_nostr_repost_descriptor_free(d);
  return NULL;
}

GnNostrRepostDescriptor *gn_nostr_repost_descriptor_parse_tags(gint kind,
                                                                const gchar *tags_json) {
  if ((kind != 1 && kind != 6 && kind != 16) || !tags_json ||
      strlen(tags_json) > 128 * 1024) return NULL;
  g_autofree gchar *event = g_strdup_printf("{\"kind\":%d,\"tags\":%s,\"content\":\"\"}",
                                           kind, tags_json);
  return gn_nostr_repost_descriptor_parse(event, FALSE);
}

static gchar *build_template(const GnNostrReference *target, gint kind,
                             const char *content, gboolean quote) {
  if (!target || target->type == GN_NOSTR_REFERENCE_PERSON || !target->id ||
      (target->type == GN_NOSTR_REFERENCE_EVENT && !hex64(target->id))) return NULL;
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "kind"); json_builder_add_int_value(b, kind);
  json_builder_set_member_name(b, "content"); json_builder_add_string_value(b, content ? content : "");
  json_builder_set_member_name(b, "tags"); json_builder_begin_array(b);
  json_builder_begin_array(b);
  json_builder_add_string_value(b, quote ? "q" : target->type == GN_NOSTR_REFERENCE_ADDRESS ? "a" : "e");
  g_autofree gchar *coordinate = target->type == GN_NOSTR_REFERENCE_ADDRESS ?
    g_strdup_printf("%d:%s:%s", target->kind, target->author ? target->author : "", target->id) : NULL;
  json_builder_add_string_value(b, coordinate ? coordinate : target->id);
  json_builder_add_string_value(b, target->relay_hints && target->relay_hints[0] ? target->relay_hints[0] : "");
  if (quote && target->author) json_builder_add_string_value(b, target->author);
  json_builder_end_array(b);
  if (target->author) {
    json_builder_begin_array(b); json_builder_add_string_value(b, "p");
    json_builder_add_string_value(b, target->author); json_builder_end_array(b);
  }
  if (kind == 16 && target->kind > 0) {
    g_autofree gchar *ks = g_strdup_printf("%d", target->kind);
    json_builder_begin_array(b); json_builder_add_string_value(b, "k");
    json_builder_add_string_value(b, ks); json_builder_end_array(b);
  }
  json_builder_end_array(b);
  json_builder_end_object(b);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  g_autoptr(JsonNode) node = json_builder_get_root(b);
  json_generator_set_root(gen, node);
  return json_generator_to_data(gen, NULL);
}

gchar *gn_nostr_build_repost_template(const GnNostrReference *target,
                                      const gchar *verified_original_json) {
  if (!target || target->kind < 0) return NULL;
  return build_template(target, target->kind == 1 ? 6 : 16,
                        verified_original_json, FALSE);
}

gchar *gn_nostr_build_quote_template(const GnNostrReference *target,
                                     const gchar *comment) {
  if (!target || target->type == GN_NOSTR_REFERENCE_PERSON) return NULL;
  const char *uri = target->uri;
  char *encoded = NULL;
  if (target->type == GN_NOSTR_REFERENCE_EVENT) {
    NostrEventPointer p = { .id = target->id, .author = target->author,
      .kind = target->kind > 0 ? target->kind : 0 };
    if (target->relay_hints) {
      p.relays = target->relay_hints;
      p.relays_count = g_strv_length(target->relay_hints);
    }
    if (nostr_nip19_encode_nevent(&p, &encoded)) return NULL;
  } else if (!uri && target->type == GN_NOSTR_REFERENCE_ADDRESS &&
             target->author && target->kind > 0) {
    NostrEntityPointer p = { .identifier = target->id, .public_key = target->author,
      .kind = target->kind };
    if (target->relay_hints) {
      p.relays = target->relay_hints;
      p.relays_count = g_strv_length(target->relay_hints);
    }
    if (nostr_nip19_encode_naddr(&p, &encoded)) return NULL;
  }
  g_autofree gchar *generated_uri = encoded ? g_strdup_printf("nostr:%s", encoded) : NULL;
  free(encoded);
  if (generated_uri) uri = generated_uri;
  if (!uri) return NULL;
  g_autofree gchar *content = g_strdup_printf("%s%s%s", comment ? comment : "",
    comment && *comment ? "\n\n" : "", uri);
  return build_template(target, 1, content, TRUE);
}
