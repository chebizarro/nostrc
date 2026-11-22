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
nip29_json_for_group(NostrTags *tags, const gchar *group_id, gint kind)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, kind);
  nostr_event_set_pubkey(event, ACCOUNT);
  nostr_event_set_created_at(event, 1700000000);
  nostr_event_set_content(event, "hello");
  NostrTags *full = tags ? tags : nostr_tags_new(0);
  nostr_tags_append(full, nostr_tag_new("h", group_id, NULL));
  nostr_event_set_tags(event, full);
  g_assert_cmpint(nostr_event_sign(event, SECRET), ==, 0);
  gchar *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  return json;
}

static gchar *
nip29_json_with_tags(NostrTags *tags)
{
  return nip29_json_for_group(tags, "test-group", 9);
}

static void
test_nip29_opaque_group_ids(void)
{
  const gchar *ids[] = { "Tnq7x2ZTgrPZWFrC", "Mixed Case/Group: café'東京" };
  for (guint i = 0; i < G_N_ELEMENTS(ids); i++) {
    for (gint kind = 9; kind <= 12; kind++) {
      g_autofree gchar *json = nip29_json_for_group(NULL, ids[i], kind);
      g_autoptr(GError) error = NULL;
      g_autoptr(GhMessage) message = gh_message_new_from_nip29_event(
        ACCOUNT, "wss://nos.lol", json, &error);
      g_assert_no_error(error);
      g_assert_nonnull(message);
      g_assert_cmpstr(gh_message_get_group_id(message), ==, ids[i]);
      g_autofree gchar *relay = NULL, *id = NULL;
      g_assert_true(gh_message_nip29_room_split(gh_message_get_room_id(message), &relay, &id));
      g_assert_cmpstr(relay, ==, "wss://nos.lol");
      g_assert_cmpstr(id, ==, ids[i]);
      g_autofree gchar *lower = g_utf8_strdown(ids[i], -1);
      g_autofree gchar *other_room = gh_message_nip29_room_id(relay, lower);
      g_assert_cmpstr(gh_message_get_room_id(message), !=, other_room);
    }
  }
}

static void
test_nip29_invalid_group_ids(void)
{
  const gchar *ids[] = { "", "Room\nA", "Room\x1f" "A", "Room\x7f" "A",
                         "Room\xc2\x80" "A", "Room\xc2\x9f" "A" };
  g_autofree gchar *too_long = g_strnfill(GH_MESSAGE_MAX_GROUP_ID + 1, 'A');
  for (guint i = 0; i <= G_N_ELEMENTS(ids); i++) {
    const gchar *id = i < G_N_ELEMENTS(ids) ? ids[i] : too_long;
    g_autofree gchar *json = nip29_json_for_group(NULL, id, 9);
    g_autoptr(GError) error = NULL;
    g_autoptr(GhMessage) message = gh_message_new_from_nip29_event(
      ACCOUNT, "wss://nos.lol", json, &error);
    g_assert_null(message);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_autofree gchar *room = gh_message_nip29_room_id("wss://nos.lol", id);
    g_assert_false(gh_message_nip29_room_split(room, NULL, NULL));
  }
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

static void
test_mls_future_time(void)
{
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  for (guint future = 0; future < 2; future++) {
    gint64 timestamp = future ? now + GH_MESSAGE_MLS_MAX_FUTURE_SKEW_SECONDS + 60 : now;
    g_autofree gchar *json = g_strdup_printf(
      "{\"pubkey\":\"%s\",\"kind\":9,\"content\":\"clock test\",\"tags\":[],\"created_at\":%" G_GINT64_FORMAT "}",
      ACCOUNT, timestamp);
    g_autoptr(GError) error = NULL;
    g_autoptr(GhMessage) message = gh_message_new_from_mls(ACCOUNT, "aabbccdd", json, &error);
    if (future) {
      g_assert_null(message);
      g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
      g_assert_nonnull(strstr(error->message, "future"));
    } else {
      g_assert_no_error(error);
      g_assert_nonnull(message);
      g_assert_cmpint(gh_message_get_created_at(message), ==, now);
    }
  }
}

int
main(int argc, char **argv)
{
  char *pub = nostr_key_get_public(SECRET);
  ACCOUNT = g_strdup(pub);
  free(pub);
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/message/nip29/opaque-group-ids", test_nip29_opaque_group_ids);
  g_test_add_func("/message/nip29/invalid-group-ids", test_nip29_invalid_group_ids);
  g_test_add_func("/message/reply/nip29-no-reply", test_nip29_no_reply);
  g_test_add_func("/message/reply/nip29-e-marked", test_nip29_reply_e_marked);
  g_test_add_func("/message/reply/nip29-e-bare", test_nip29_reply_e_bare);
  g_test_add_func("/message/reply/nip29-q-tag", test_nip29_reply_q_tag);
  g_test_add_func("/message/reply/nip29-marked-over-bare", test_nip29_reply_marked_over_bare);
  g_test_add_func("/message/reply/mls-future-time", test_mls_future_time);
  g_test_add_func("/message/reply/mls-no-reply", test_mls_no_reply);
  g_test_add_func("/message/reply/mls-e-tag", test_mls_reply_e_tag);
  g_test_add_func("/message/reply/mls-q-over-bare", test_mls_reply_q_over_bare);

  g_test_add_func("/message/poll/nip29-semantic-kinds", test_nip29_poll_semantic_kinds);
  g_test_add_func("/message/mention/extract", test_extract_mentions);

  /* Review finding 8: e-mention must not produce a reply. */
  g_test_add_func("/message/reply/nip29-e-mention-no-reply", test_nip29_e_mention_no_reply);

  return g_test_run();
}
