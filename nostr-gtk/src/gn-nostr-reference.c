#include <nostr-gtk-1.0/gn-nostr-reference.h>
#include <nostr/nip19/nip19.h>
#include <json-glib/json-glib.h>
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

GnNostrReference *gn_nostr_reference_parse(const gchar *uri) {
  if (!uri || strlen(uri) > 2048) return NULL;
  const char *value = g_str_has_prefix(uri, "nostr:") ? uri + 6 : uri;
  if (strchr(value, '?') || strchr(value, '#') || strchr(value, '/')) return NULL;
  GnNostrReference *r = g_new0(GnNostrReference, 1);
  r->uri = g_strdup_printf("nostr:%s", value);
  r->kind = -1;
  guint8 raw[32];
  if (g_str_has_prefix(value, "npub1") && !nostr_nip19_decode_npub(value, raw)) {
    r->type = GN_NOSTR_REFERENCE_PERSON;
    r->author = bytes_hex(raw);
  } else if (g_str_has_prefix(value, "note1") && !nostr_nip19_decode_note(value, raw)) {
    r->type = GN_NOSTR_REFERENCE_EVENT;
    r->id = bytes_hex(raw);
  } else if (g_str_has_prefix(value, "nprofile1")) {
    NostrProfilePointer *p = NULL;
    if (nostr_nip19_decode_nprofile(value, &p) || !p || !hex64(p->public_key)) {
      nostr_profile_pointer_free(p);
      goto fail;
    }
    r->type = GN_NOSTR_REFERENCE_PERSON;
    r->author = g_ascii_strdown(p->public_key, -1);
    r->relay_hints = copy_relays(p->relays, p->relays_count);
    nostr_profile_pointer_free(p);
  } else if (g_str_has_prefix(value, "nevent1")) {
    NostrEventPointer *p = NULL;
    if (nostr_nip19_decode_nevent(value, &p) || !p || !hex64(p->id)) {
      nostr_event_pointer_free(p);
      goto fail;
    }
    r->type = GN_NOSTR_REFERENCE_EVENT;
    r->id = g_ascii_strdown(p->id, -1);
    if (hex64(p->author)) r->author = g_ascii_strdown(p->author, -1);
    r->kind = p->kind > 0 ? p->kind : -1;
    r->relay_hints = copy_relays(p->relays, p->relays_count);
    nostr_event_pointer_free(p);
  } else if (g_str_has_prefix(value, "naddr1")) {
    NostrEntityPointer *p = NULL;
    if (nostr_nip19_decode_naddr(value, &p) || !p || !hex64(p->public_key) || p->kind <= 0 || !p->identifier) {
      nostr_entity_pointer_free(p);
      goto fail;
    }
    r->type = GN_NOSTR_REFERENCE_ADDRESS;
    r->id = g_strdup(p->identifier);
    r->author = g_ascii_strdown(p->public_key, -1);
    r->kind = p->kind;
    r->relay_hints = copy_relays(p->relays, p->relays_count);
    nostr_entity_pointer_free(p);
  } else goto fail;
  return r;
fail:
  gn_nostr_reference_free(r);
  return NULL;
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
  if (embedded_event_verified && kind != 1 && json_object_has_member(obj, "content")) {
    const char *content = object_string(obj, "content");
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
