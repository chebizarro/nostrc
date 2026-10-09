#include "gh-agent-event.h"
#include "gh-message.h"

#include <json-glib/json-glib.h>
#include <nostr-event.h>
#include <nostr-tag.h>
#include <string.h>

static gchar *
compact_text(const gchar *raw, guint max_chars)
{
  if (!raw || !g_utf8_validate(raw, -1, NULL))
    return NULL;
  GString *out = g_string_sized_new(MIN(strlen(raw), max_chars * 4));
  gboolean space = FALSE;
  guint chars = 0;
  for (const gchar *p = raw; *p && chars < max_chars; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    GUnicodeType type = g_unichar_type(c);
    if (g_unichar_isspace(c)) {
      space = out->len > 0;
      continue;
    }
    /* No control, bidi override/isolate, or zero-width formatting codepoint
     * from peer content may change the direction or apparent text of a row. */
    if (type == G_UNICODE_FORMAT || type == G_UNICODE_CONTROL ||
        type == G_UNICODE_SURROGATE || type == G_UNICODE_UNASSIGNED)
      continue;
    if (space) {
      g_string_append_c(out, ' ');
      chars++;
      space = FALSE;
      if (chars == max_chars)
        break;
    }
    g_string_append_unichar(out, c);
    chars++;
  }
  if (!out->len) {
    g_string_free(out, TRUE);
    return NULL;
  }
  return g_string_free(out, FALSE);
}

static gchar *
field(JsonObject *object, const gchar *key, guint max_chars)
{
  if (!json_object_has_member(object, key) ||
      !JSON_NODE_HOLDS_VALUE(json_object_get_member(object, key)) ||
      json_node_get_value_type(json_object_get_member(object, key)) != G_TYPE_STRING)
    return NULL;
  return compact_text(json_object_get_string_member(object, key), max_chars);
}

static const gchar *
icon_for(gint kind, const gchar *type)
{
  if (kind == GH_AGENT_ACTIVITY_KIND) {
    if (g_strcmp0(type, "thinking") == 0)
      return "content-loading-symbolic";
    if (g_strcmp0(type, "running") == 0 || g_strcmp0(type, "started") == 0 ||
        g_strcmp0(type, "in_progress") == 0)
      return "media-playback-start-symbolic";
    return "content-loading-symbolic";
  }
  if (g_strcmp0(type, "tool_call") == 0)
    return "computer-symbolic";
  if (g_strcmp0(type, "approval") == 0)
    return "dialog-warning-symbolic";
  if (g_strcmp0(type, "hook") == 0)
    return "document-open-recent-symbolic";
  if (g_strcmp0(type, "handoff") == 0)
    return "go-next-symbolic";
  if (g_strcmp0(type, "delivery") == 0)
    return "send-symbolic";
  return "computer-symbolic";
}

gboolean
gh_agent_event_is_kind(gint kind)
{
  return kind >= GH_AGENT_STREAM_START_KIND && kind <= GH_AGENT_OPERATION_KIND;
}

GhAgentEvent *
gh_agent_event_parse(gint kind, const gchar *content)
{
  if ((kind != GH_AGENT_ACTIVITY_KIND && kind != GH_AGENT_OPERATION_KIND) ||
      !content || strnlen(content, GH_AGENT_EVENT_MAX_JSON_BYTES + 1) >
                    GH_AGENT_EVENT_MAX_JSON_BYTES ||
      !g_utf8_validate(content, -1, NULL))
    return NULL;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, content, -1, NULL))
    return NULL;
  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root))
    return NULL;
  JsonObject *object = json_node_get_object(root);
  g_autofree gchar *text = field(object, "text", 140);
  if (!text)
    text = field(object, "preview", 140);
  if (!text)
    return NULL;
  g_autofree gchar *type = field(object,
    kind == GH_AGENT_ACTIVITY_KIND ? "status" : "event_type", 64);
  GhAgentEvent *event = g_new0(GhAgentEvent, 1);
  event->text = g_steal_pointer(&text);
  event->type = g_strdup(type);
  event->icon_name = icon_for(kind, type);
  event->detail = kind == GH_AGENT_OPERATION_KIND ? field(object, "name", 64) : NULL;
  if (!event->detail)
    event->detail = g_steal_pointer(&type);
  return event;
}

static const gchar *
tag_value(NostrTags *tags, const gchar *key)
{
  if (!tags)
    return NULL;
  for (size_t i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (tag && g_strcmp0(nostr_tag_get_key(tag), key) == 0)
      return nostr_tag_get_value(tag);
  }
  return NULL;
}

GhAgentEvent *
gh_agent_event_parse_message(GhMessage *message)
{
  g_return_val_if_fail(GH_IS_MESSAGE(message), NULL);
  gint kind = gh_message_get_kind(message);
  GhAgentEvent *display = gh_agent_event_parse(kind, gh_message_get_content(message));
  if (!display)
    return NULL;
  NostrEvent *inner = nostr_event_new();
  if (!inner || nostr_event_deserialize_unsigned(inner,
        gh_message_get_rumor_json(message), NULL) != NOSTR_EVENT_VALIDATION_OK) {
    if (inner)
      nostr_event_free(inner);
    return display; /* the checked body remains usable without optional tags */
  }
  NostrTags *tags = (NostrTags *)nostr_event_get_tags(inner);
  if (!display->type) {
    display->type = compact_text(tag_value(tags,
      kind == GH_AGENT_ACTIVITY_KIND ? "status" : "operation"), 64);
    display->icon_name = icon_for(kind, display->type);
    if (!display->detail)
      display->detail = g_strdup(display->type);
  }
  if (kind == GH_AGENT_OPERATION_KIND) {
    g_autofree gchar *name = compact_text(tag_value(tags, "operation-name"), 64);
    if (name && (!display->detail || g_strcmp0(display->detail, display->type) == 0)) {
      g_free(display->detail);
      display->detail = g_steal_pointer(&name);
    }
  }
  nostr_event_free(inner);
  return display;
}

gboolean
gh_agent_event_is_visible(gint kind, const gchar *content)
{
  if (!gh_agent_event_is_kind(kind))
    return TRUE;
  g_autoptr(GhAgentEvent) event = gh_agent_event_parse(kind, content);
  return event != NULL;
}

void
gh_agent_event_free(GhAgentEvent *event)
{
  if (!event)
    return;
  g_free(event->text);
  g_free(event->detail);
  g_free(event->type);
  g_free(event);
}
