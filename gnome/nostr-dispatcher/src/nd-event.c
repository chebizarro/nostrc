/* nd-event.c — see nd-event.h. */
#include "nd-event.h"
#include "nd-error.h"

#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

#include "nostr-event.h"

void nd_event_free(NdEvent *ev) {
  if (!ev) return;
  g_free(ev->json);
  g_free(ev->id_hex);
  g_free(ev->pubkey_hex);
  g_free(ev->d_tag);
  g_free(ev);
}

static char *dup_string_member(JsonObject *o, const char *name) {
  JsonNode *n = json_object_get_member(o, name);
  if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING)
    return NULL;
  return g_strdup(json_node_get_string(n));
}

static char *first_d_tag(JsonObject *o) {
  JsonNode *tn = json_object_get_member(o, "tags");
  if (!tn || !JSON_NODE_HOLDS_ARRAY(tn)) return NULL;
  JsonArray *tags = json_node_get_array(tn);
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonNode *t = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(t)) continue;
    JsonArray *ta = json_node_get_array(t);
    if (json_array_get_length(ta) < 2) continue;
    JsonNode *k = json_array_get_element(ta, 0), *v = json_array_get_element(ta, 1);
    if (JSON_NODE_HOLDS_VALUE(k) && json_node_get_value_type(k) == G_TYPE_STRING &&
        g_strcmp0(json_node_get_string(k), "d") == 0 &&
        JSON_NODE_HOLDS_VALUE(v) && json_node_get_value_type(v) == G_TYPE_STRING)
      return g_strdup(json_node_get_string(v));
  }
  return NULL;
}

NdEvent *nd_event_parse(const char *json, gssize len, GError **error) {
  if (!json) {
    g_set_error_literal(error, ND_ERROR, ND_ERROR_INVALID_EVENT, "no event JSON");
    return NULL;
  }
  gsize n = len < 0 ? strlen(json) : (gsize)len;
  if (n == 0 || n > ND_EVENT_MAX_JSON) {
    g_set_error_literal(error, ND_ERROR, ND_ERROR_INVALID_EVENT,
                        "event JSON is empty or larger than 1 MiB");
    return NULL;
  }
  g_autofree char *buf = g_strndup(json, n);
  if (strlen(buf) != n || !g_utf8_validate(buf, (gssize)n, NULL)) {
    g_set_error_literal(error, ND_ERROR, ND_ERROR_INVALID_EVENT,
                        "event JSON is not valid UTF-8 text");
    return NULL;
  }

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, buf, (gssize)n, NULL) ||
      !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
    g_set_error_literal(error, ND_ERROR, ND_ERROR_INVALID_EVENT,
                        "event JSON is not an object");
    return NULL;
  }
  JsonObject *o = json_node_get_object(json_parser_get_root(parser));
  JsonNode *kn = json_object_get_member(o, "kind");
  if (!kn || !JSON_NODE_HOLDS_VALUE(kn) || json_node_get_value_type(kn) != G_TYPE_INT64 ||
      json_node_get_int(kn) < 0 || json_node_get_int(kn) > 65535) {
    g_set_error_literal(error, ND_ERROR, ND_ERROR_INVALID_EVENT,
                        "event has no integer kind in 0..65535");
    return NULL;
  }

  NdEvent *ev = g_new0(NdEvent, 1);
  ev->kind = (gint)json_node_get_int(kn);
  ev->id_hex = dup_string_member(o, "id");
  ev->pubkey_hex = dup_string_member(o, "pubkey");
  ev->d_tag = first_d_tag(o);

  NostrEvent *ne = nostr_event_new();
  if (ne && nostr_event_deserialize_signed(ne, buf, NULL) == NOSTR_EVENT_VALIDATION_OK) {
    char canon[65] = {0};
    if (nostr_event_validate(ne, canon) == NOSTR_EVENT_VALIDATION_OK &&
        nostr_event_get_kind(ne) == ev->kind) {
      char *compact = nostr_event_serialize_compact(ne);
      if (compact) {
        ev->validated = TRUE;
        ev->json = g_strdup(compact);
        free(compact);
        g_free(ev->id_hex);
        ev->id_hex = g_strdup(canon);
      }
    }
  }
  if (ne) nostr_event_free(ne);
  if (!ev->json) ev->json = g_steal_pointer(&buf);
  return ev;
}

gboolean nd_event_matches_target(const NdEvent *ev, const NdTarget *t) {
  if (!ev || !t || !ev->validated) return FALSE;
  switch (t->entity) {
  case ND_ENTITY_EVENT:
    if (g_strcmp0(ev->id_hex, t->id_hex) != 0) return FALSE;
    if (t->kind >= 0 && ev->kind != t->kind) return FALSE;
    if (t->pubkey_hex && g_strcmp0(ev->pubkey_hex, t->pubkey_hex) != 0) return FALSE;
    return TRUE;
  case ND_ENTITY_ADDRESS:
    return ev->kind == t->kind && g_strcmp0(ev->pubkey_hex, t->pubkey_hex) == 0 &&
           g_strcmp0(ev->d_tag ? ev->d_tag : "", t->identifier ? t->identifier : "") == 0;
  case ND_ENTITY_PROFILE:
    return ev->kind == 0 && g_strcmp0(ev->pubkey_hex, t->pubkey_hex) == 0;
  default:
    return FALSE;
  }
}
