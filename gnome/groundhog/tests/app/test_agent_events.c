#include "gh-agent-event.h"
#include "gh-conversation-store.h"
#include "gh-message.h"

#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <stdlib.h>
#include <string.h>

static gchar *account;
static gchar *agent;

static gchar *
inner_json(gint kind, const gchar *content, gint64 at, gboolean stream_tags)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, kind);
  nostr_event_set_pubkey(event, agent);
  nostr_event_set_created_at(event, at);
  nostr_event_set_content(event, content);
  NostrTags *tags = nostr_tags_new(0);
  if (stream_tags) {
    nostr_tags_append(tags, nostr_tag_new("stream", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL));
    nostr_tags_append(tags, nostr_tag_new("stream-start", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", NULL));
    nostr_tags_append(tags, nostr_tag_new("stream-hash", "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc", NULL));
    nostr_tags_append(tags, nostr_tag_new("stream-chunks", "2", NULL));
  }
  nostr_event_set_tags(event, tags);
  event->id = nostr_event_get_id(event);
  gchar *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  return json;
}

static GhMessage *
message(gint kind, const gchar *content, gint64 at, gboolean stream_tags)
{
  g_autofree gchar *json = inner_json(kind, content, at, stream_tags);
  g_autoptr(GError) error = NULL;
  GhMessage *msg = gh_message_new_from_mls(account, "aabb", json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(msg);
  return msg;
}

static void
test_parser(void)
{
  g_autoptr(GhAgentEvent) activity = gh_agent_event_parse(1201,
    "{\"text\":\" Thinking \\u202e now \\n  \",\"status\":\"thinking\"}");
  g_assert_nonnull(activity);
  g_assert_cmpstr(activity->text, ==, "Thinking now");
  g_autoptr(GhAgentEvent) multiline = gh_agent_event_parse(1201,
    "{\"text\":\"one\\ntwo\"}");
  g_assert_nonnull(multiline);
  g_assert_cmpstr(multiline->text, ==, "one two");
  g_assert_cmpstr(activity->detail, ==, "thinking");
  g_autoptr(GhAgentEvent) operation = gh_agent_event_parse(1202,
    "{\"preview\":\"Reading files\",\"event_type\":\"tool_call\",\"name\":\"read_file\"}");
  g_assert_nonnull(operation);
  g_assert_cmpstr(operation->text, ==, "Reading files");
  g_assert_cmpstr(operation->detail, ==, "read_file");
  g_assert_null(gh_agent_event_parse(1201, "not JSON"));
  g_assert_null(gh_agent_event_parse(1202, "{\"text\":1}"));
  g_autofree gchar *huge = g_strnfill(GH_AGENT_EVENT_MAX_JSON_BYTES + 1, 'x');
  g_assert_null(gh_agent_event_parse(1201, huge));
  g_assert_false(gh_agent_event_is_visible(1200, "{}"));
  g_assert_false(gh_agent_event_is_visible(1201, "{}"));
}

static void
test_tag_metadata(void)
{
  NostrEvent *inner = nostr_event_new();
  nostr_event_set_kind(inner, 1202);
  nostr_event_set_pubkey(inner, agent);
  nostr_event_set_created_at(inner, 1700000000);
  nostr_event_set_content(inner, "{\"text\":\"Using a tool\"}");
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("operation", "tool_call", NULL));
  nostr_tags_append(tags, nostr_tag_new("operation-name", "read_file", NULL));
  nostr_event_set_tags(inner, tags);
  inner->id = nostr_event_get_id(inner);
  g_autofree gchar *json = nostr_event_serialize_compact(inner);
  nostr_event_free(inner);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) message = gh_message_new_from_mls(account, "aabb", json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(message);
  g_autoptr(GhAgentEvent) display = gh_agent_event_parse_message(message);
  g_assert_nonnull(display);
  g_assert_cmpstr(display->detail, ==, "read_file");
  g_assert_cmpstr(display->icon_name, ==, "computer-symbolic");
}

static void
test_conversation_projection(void)
{
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, account, NULL, NULL, NULL);
  g_autoptr(GhMessage) chat = message(9, "hello", 1700000000, FALSE);
  g_assert_cmpint(gh_conversation_store_admit(store, chat, NULL, NULL), ==,
                  GH_CONVERSATION_ADD_NEW);
  GhConversation *room = gh_conversation_store_lookup(store, gh_message_get_room_id(chat));
  g_assert_nonnull(room);
  g_assert_cmpstr(gh_conversation_get_preview(room), ==, "hello");
  guint unread = gh_conversation_get_unread_count(room);
  gint64 last = gh_conversation_get_last_activity(room);
  g_autoptr(GhMessage) activity = message(1201, "{\"text\":\"Thinking\"}",
                                          1700000010, FALSE);
  g_assert_cmpint(gh_conversation_store_admit(store, activity, NULL, NULL), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_cmpstr(gh_conversation_get_preview(room), ==, "hello");
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, unread);
  g_assert_cmpint(gh_conversation_get_last_activity(room), ==, last);
  /* A chat can sort before later activity but still be the latest chat. */
  g_autoptr(GhMessage) late_chat = message(9, "later chat", 1700000005, FALSE);
  g_assert_cmpint(gh_conversation_store_admit(store, late_chat, NULL, NULL), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_cmpstr(gh_conversation_get_preview(room), ==, "later chat");
  unread++;
  g_autoptr(GhMessage) start = message(1200, "{}", 1700000020, FALSE);
  g_assert_cmpint(gh_conversation_store_admit(store, start, NULL, NULL), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_cmpstr(gh_conversation_get_preview(room), ==, "later chat");
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, unread);
  g_autoptr(GhMessage) final = message(9, "final answer", 1700000030, TRUE);
  g_assert_cmpint(gh_conversation_store_admit(store, final, NULL, NULL), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_cmpstr(gh_conversation_get_preview(room), ==, "final answer");
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, unread + 1);
  g_autoptr(GhMessage) incomplete = message(9, "ordinary chat", 1700000040, FALSE);
  g_assert_cmpint(gh_conversation_store_admit(store, incomplete, NULL, NULL), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_cmpstr(gh_conversation_get_preview(room), ==, "ordinary chat");
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  char *a = nostr_key_get_public("0000000000000000000000000000000000000000000000000000000000000001");
  char *b = nostr_key_get_public("0000000000000000000000000000000000000000000000000000000000000002");
  account = g_strdup(a);
  agent = g_strdup(b);
  free(a);
  free(b);
  g_test_add_func("/groundhog/agent/parser", test_parser);
  g_test_add_func("/groundhog/agent/tag-metadata", test_tag_metadata);
  g_test_add_func("/groundhog/agent/conversation", test_conversation_projection);
  int result = g_test_run();
  g_free(account);
  g_free(agent);
  return result;
}
