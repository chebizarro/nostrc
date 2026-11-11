#include "gh-call-signalling.h"
#include <json-glib/json-glib.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>
#include <string.h>

static gboolean hex_key(const gchar *s)
{
  if (!s || strlen(s) != 64) return FALSE;
  for (; *s; s++) if (!g_ascii_isxdigit(*s) || g_ascii_isupper(*s)) return FALSE;
  return TRUE;
}

const gchar *
gh_call_event_tag(const NostrEvent *event, const gchar *name)
{
  NostrTags *tags = nostr_event_get_tags(event);
  const gchar *value = NULL;
  for (gsize i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get_key(tag), name)) continue;
    /* Duplicate correlation tags are ambiguous, even with equal values. */
    if (value || nostr_tag_size(tag) != 2) return NULL;
    value = nostr_tag_get(tag, 1);
  }
  return value;
}

gboolean gh_call_id_valid(const gchar *id)
{
  if (!id) return FALSE;
  if (g_uuid_string_is_valid(id)) return TRUE;
  if (strlen(id) != 32) return FALSE;
  for (const gchar *p = id; *p; p++) if (!g_ascii_isxdigit(*p)) return FALSE;
  return TRUE;
}

static const gchar *string_member(JsonObject *o, const gchar *key)
{
  JsonNode *n = json_object_get_member(o, key);
  return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING
    ? json_node_get_string(n) : NULL;
}

gboolean
gh_call_ice_parse(const gchar *json, gchar **candidate, gchar **mid, guint *index)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json || strlen(json) > 8192 || !json_parser_load_from_data(parser, json, -1, NULL)) return FALSE;
  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) return FALSE;
  JsonObject *o = json_node_get_object(root);
  const gchar *c = string_member(o, "candidate"), *m = string_member(o, "sdpMid");
  JsonNode *mn = json_object_get_member(o, "sdpMid");
  JsonNode *n = json_object_get_member(o, "sdpMLineIndex");
  /* Nospeak emits nullable WebRTC fields; the backend uses the bundled first
   * media section when neither an index nor a mid was supplied. */
  if (!c || strlen(c) > 4096 || !mn || (!m && !JSON_NODE_HOLDS_NULL(mn)) ||
      (m && strlen(m) > 256) || !n || (!JSON_NODE_HOLDS_NULL(n) &&
      (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64 ||
       json_node_get_int(n) < 0 || json_node_get_int(n) > 65535))) return FALSE;
  if (candidate) *candidate = g_strdup(c);
  if (mid) *mid = g_strdup(m ? m : "");
  if (index) *index = JSON_NODE_HOLDS_NULL(n) ? 0 : (guint)json_node_get_int(n);
  return TRUE;
}

gchar *gh_call_ice_json(const gchar *candidate, const gchar *mid, guint index)
{
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "candidate"); json_builder_add_string_value(b, candidate);
  json_builder_set_member_name(b, "sdpMid"); json_builder_add_string_value(b, mid ? mid : "");
  json_builder_set_member_name(b, "sdpMLineIndex"); json_builder_add_int_value(b, index);
  json_builder_end_object(b);
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  return json_to_string(root, FALSE);
}

static gboolean has_tag(const NostrEvent *event, const gchar *key)
{
  NostrTags *tags = nostr_event_get_tags(event);
  for (gsize i = 0; tags && i < nostr_tags_size(tags); i++)
    if (!g_strcmp0(nostr_tag_get_key(nostr_tags_get(tags, i)), key)) return TRUE;
  return FALSE;
}

NostrEvent *
gh_call_event_parse(const gchar *json, const gchar *recipient, gint64 now, GError **error)
{
  NostrEvent *event = nostr_event_new();
  if (!json || strlen(json) > 65536 ||
      nostr_event_deserialize_signed(event, json, NULL) != NOSTR_EVENT_VALIDATION_OK ||
      nostr_event_validate(event, NULL) != NOSTR_EVENT_VALIDATION_OK) goto invalid;
  int kind = nostr_event_get_kind(event);
  gint64 time = nostr_event_get_created_at(event);
  const gchar *p = gh_call_event_tag(event, "p");
  const gchar *id = gh_call_event_tag(event, "call-id");
  const gchar *content = nostr_event_get_content(event);
  if (kind < NOSTR_KIND_CALL_OFFER || kind > NOSTR_KIND_CALL_RENEGOTIATE ||
      !hex_key(p) || (recipient && g_strcmp0(p, recipient)) || !id ||
      !gh_call_id_valid(id) || time < now - GH_CALL_FRESHNESS_SECONDS ||
      time > now + GH_CALL_FRESHNESS_SECONDS || !content ||
      has_tag(event, "group-call-id") || has_tag(event, "participants") ||
      has_tag(event, "conversation-id") || has_tag(event, "initiator")) goto invalid;
  if (kind == NOSTR_KIND_CALL_OFFER) {
    const gchar *type = gh_call_event_tag(event, "call-type");
    if ((has_tag(event, "call-type") && !type) ||
        (type && g_strcmp0(type, "voice") && g_strcmp0(type, "video"))) goto invalid;
  }
  if ((kind == NOSTR_KIND_CALL_OFFER || kind == NOSTR_KIND_CALL_ANSWER ||
       kind == NOSTR_KIND_CALL_RENEGOTIATE) && !g_str_has_prefix(content, "v=0")) goto invalid;
  if (kind == NOSTR_KIND_CALL_ICE_CANDIDATE && !gh_call_ice_parse(content, NULL, NULL, NULL)) goto invalid;
  return event;
invalid:
  nostr_event_free(event);
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid or stale NIP-AC event");
  return NULL;
}

NostrEvent *
gh_call_event_new(const gchar *author, const gchar *recipient, const gchar *call_id,
                  int kind, GhCallType type, const gchar *content, gint64 now)
{
  if (kind < NOSTR_KIND_CALL_OFFER || kind > NOSTR_KIND_CALL_RENEGOTIATE ||
      !hex_key(author) || !hex_key(recipient) || !gh_call_id_valid(call_id) ||
      (content && strlen(content) > 60000)) return NULL;
  NostrEvent *event = nostr_event_new();
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("p", recipient, NULL));
  nostr_tags_append(tags, nostr_tag_new("call-id", call_id, NULL));
  static const gchar *alts[] = { "WebRTC call offer", "WebRTC call answer",
    "WebRTC ICE candidate", "WebRTC call hangup", "WebRTC call rejection", "WebRTC call renegotiation" };
  nostr_tags_append(tags, nostr_tag_new("alt", alts[kind - NOSTR_KIND_CALL_OFFER], NULL));
  if (kind == NOSTR_KIND_CALL_OFFER)
    nostr_tags_append(tags, nostr_tag_new("call-type", type == GH_CALL_VIDEO ? "video" : "voice", NULL));
  nostr_event_set_tags(event, tags);
  nostr_event_set_pubkey(event, author);
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, now);
  nostr_event_set_content(event, content ? content : "");
  return event;
}
