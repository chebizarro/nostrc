/* Reply-target parsing for NIP-29 and MLS group messages (nostrc-zjkv).
 *
 * Verifies that gh_message_get_reply_to_id() extracts the correct event id
 * from e/q tags in both NIP-29 and MLS (kind-9) inner events. */
#include "gh-message.h"

#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-tag.h"
#include "nostr-utils.h"
#include <nostr/nip19/nip19.h>

#include <stdlib.h>
#include <string.h>

static const gchar *SECRET =
    "0000000000000000000000000000000000000000000000000000000000000001";
static gchar *ACCOUNT;

/* A 64-character lowercase hex string for a fake event id. */
#define FAKE_EVENT_ID "aabbccddee00112233445566778899aabbccddee00112233445566778899aabb"
#define FAKE_EVENT_ID2 "1122334455667788990011223344556677889900112233445566778899001122"
#define FAKE_EVENT_ID3 "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"

static gchar *
nip29_json_with_tags(NostrTags *tags)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 9);
  nostr_event_set_pubkey(event, ACCOUNT);
  nostr_event_set_created_at(event, 1700000000);
  nostr_event_set_content(event, "hello");
  NostrTags *full = tags ? tags : nostr_tags_new(0);
  nostr_tags_append(full, nostr_tag_new("h", "test-group", NULL));
  nostr_event_set_tags(event, full);
  g_assert_cmpint(nostr_event_sign(event, SECRET), ==, 0);
  gchar *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  return json;
}

static gchar *
mls_json_with_tags(NostrTags *tags)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 9);
  nostr_event_set_pubkey(event, ACCOUNT);
  nostr_event_set_created_at(event, 1700000000);
  nostr_event_set_content(event, "hello");
  nostr_event_set_tags(event, tags ? tags : nostr_tags_new(0));
  /* MLS inner events are unsigned. Compute the id manually. */
  event->id = nostr_event_get_id(event);
  gchar *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  return json;
}

/* ---- NIP-29 reply tests ---------------------------------------------------- */

static void
test_nip29_no_reply(void)
{
  g_autofree gchar *json = nip29_json_with_tags(NULL);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) msg = gh_message_new_from_nip29_event(
      ACCOUNT, "wss://relay.example.com", json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(msg);
  g_assert_null(gh_message_get_reply_to_id(msg));
}

static void
test_nip29_reply_e_marked(void)
{
  NostrTags *tags = nostr_tags_new(0);
  /* e tag with "reply" marker (NIP-10). */
  NostrTag *tag = nostr_tag_new("e", FAKE_EVENT_ID, "", "reply", NULL);
  nostr_tags_append(tags, tag);
  g_autofree gchar *json = nip29_json_with_tags(tags);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) msg = gh_message_new_from_nip29_event(
      ACCOUNT, "wss://relay.example.com", json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(msg);
  g_assert_cmpstr(gh_message_get_reply_to_id(msg), ==, FAKE_EVENT_ID);
}

static void
test_nip29_reply_e_bare(void)
{
  /* Bare e tag (deprecated NIP-10: last e tag is the reply). */
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("e", FAKE_EVENT_ID, NULL));
  nostr_tags_append(tags, nostr_tag_new("e", FAKE_EVENT_ID2, NULL));
  g_autofree gchar *json = nip29_json_with_tags(tags);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) msg = gh_message_new_from_nip29_event(
      ACCOUNT, "wss://relay.example.com", json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(msg);
  /* Last e tag wins. */
  g_assert_cmpstr(gh_message_get_reply_to_id(msg), ==, FAKE_EVENT_ID2);
}

static void
test_nip29_reply_q_tag(void)
{
  /* Quote (q tag). */
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("q", FAKE_EVENT_ID3, NULL));
  g_autofree gchar *json = nip29_json_with_tags(tags);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) msg = gh_message_new_from_nip29_event(
      ACCOUNT, "wss://relay.example.com", json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(msg);
  g_assert_cmpstr(gh_message_get_reply_to_id(msg), ==, FAKE_EVENT_ID3);
}

static void
test_nip29_reply_marked_over_bare(void)
{
  /* A marked e-reply tag wins over a bare e tag and a q tag. */
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("e", FAKE_EVENT_ID, NULL));
  nostr_tags_append(tags, nostr_tag_new("q", FAKE_EVENT_ID3, NULL));
  nostr_tags_append(tags, nostr_tag_new("e", FAKE_EVENT_ID2, "", "reply", NULL));
  g_autofree gchar *json = nip29_json_with_tags(tags);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) msg = gh_message_new_from_nip29_event(
      ACCOUNT, "wss://relay.example.com", json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(msg);
  g_assert_cmpstr(gh_message_get_reply_to_id(msg), ==, FAKE_EVENT_ID2);
}

/* ---- MLS reply tests ------------------------------------------------------- */

static void
test_mls_no_reply(void)
{
  g_autofree gchar *json = mls_json_with_tags(NULL);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) msg = gh_message_new_from_mls(
      ACCOUNT, "aabbccdd", json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(msg);
  g_assert_null(gh_message_get_reply_to_id(msg));
}

static void
test_mls_reply_e_tag(void)
{
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("e", FAKE_EVENT_ID, "", "reply", NULL));
  g_autofree gchar *json = mls_json_with_tags(tags);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) msg = gh_message_new_from_mls(
      ACCOUNT, "aabbccdd", json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(msg);
  g_assert_cmpstr(gh_message_get_reply_to_id(msg), ==, FAKE_EVENT_ID);
}

static void
test_mls_reply_q_over_bare(void)
{
  /* q tag wins over bare e tag (no marked reply). */
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("e", FAKE_EVENT_ID, NULL));
  nostr_tags_append(tags, nostr_tag_new("q", FAKE_EVENT_ID3, NULL));
  g_autofree gchar *json = mls_json_with_tags(tags);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) msg = gh_message_new_from_mls(
      ACCOUNT, "aabbccdd", json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(msg);
  g_assert_cmpstr(gh_message_get_reply_to_id(msg), ==, FAKE_EVENT_ID3);
}


/* Review finding 8: an e tag with marker "mention" must NOT produce a reply
 * header — it is an inline reference, not a reply target. */
static void
test_nip29_e_mention_no_reply(void)
{
  NostrTags *tags = nostr_tags_new(0);
  /* An e tag with "mention" marker only — no unmarked e tags, no q tags. */
  nostr_tags_append(tags, nostr_tag_new("e", FAKE_EVENT_ID, "", "mention", NULL));
  g_autofree gchar *json = nip29_json_with_tags(tags);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) msg = gh_message_new_from_nip29_event(
      ACCOUNT, "wss://relay.example.com", json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(msg);
  g_assert_null(gh_message_get_reply_to_id(msg));
}

static void
test_nip29_poll_semantic_kinds(void)
{
  const gchar *markers[] = { "1068", "1018" };
  const gint expected[] = { GH_MESSAGE_MLS_POLL_KIND, GH_MESSAGE_MLS_POLL_VOTE_KIND };
  for (guint i = 0; i < G_N_ELEMENTS(markers); i++) {
    NostrTags *tags = nostr_tags_new(0);
    nostr_tags_append(tags, nostr_tag_new("poll", markers[i], NULL));
    g_autofree gchar *json = nip29_json_with_tags(tags);
    g_autoptr(GError) error = NULL;
    g_autoptr(GhMessage) message = gh_message_new_from_nip29_event(
      ACCOUNT, "wss://relay.example.com", json, &error);
    g_assert_no_error(error);
    g_assert_nonnull(message);
    g_assert_cmpint(gh_message_get_kind(message), ==, expected[i]);
  }
}

static void
test_extract_mentions(void)
{
  guint8 key[32];
  g_assert_true(nostr_hex2bin(key, ACCOUNT, sizeof key));
  char *npub = NULL;
  g_assert_cmpint(nostr_nip19_encode_npub(key, &npub), ==, 0);
  g_autofree gchar *content = g_strdup_printf("Hi nostr:%s and nostr:%s", npub, npub);
  free(npub);
  g_auto(GStrv) mentions = gh_message_extract_mentions(content);
  g_assert_cmpuint(g_strv_length(mentions), ==, 1);
  g_assert_cmpstr(mentions[0], ==, ACCOUNT);
}

int
main(int argc, char **argv)
{
  char *pub = nostr_key_get_public(SECRET);
  ACCOUNT = g_strdup(pub);
  free(pub);
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/message/reply/nip29-no-reply", test_nip29_no_reply);
  g_test_add_func("/message/reply/nip29-e-marked", test_nip29_reply_e_marked);
  g_test_add_func("/message/reply/nip29-e-bare", test_nip29_reply_e_bare);
  g_test_add_func("/message/reply/nip29-q-tag", test_nip29_reply_q_tag);
  g_test_add_func("/message/reply/nip29-marked-over-bare", test_nip29_reply_marked_over_bare);
  g_test_add_func("/message/reply/mls-no-reply", test_mls_no_reply);
  g_test_add_func("/message/reply/mls-e-tag", test_mls_reply_e_tag);
  g_test_add_func("/message/reply/mls-q-over-bare", test_mls_reply_q_over_bare);

  g_test_add_func("/message/poll/nip29-semantic-kinds", test_nip29_poll_semantic_kinds);
  g_test_add_func("/message/mention/extract", test_extract_mentions);

  /* Review finding 8: e-mention must not produce a reply. */
  g_test_add_func("/message/reply/nip29-e-mention-no-reply", test_nip29_e_mention_no_reply);

  return g_test_run();
}
