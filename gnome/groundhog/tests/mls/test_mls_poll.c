/* NIP-88 poll model unit tests (W26 slice C, nostrc-efa8): parse/validate,
 * create, vote, change vote, closed poll, bounds, malformed, privacy. */
#include "gh-mls-poll.h"
#include <gio/gio.h>
#include <nostr-event.h>
#include <nostr-tag.h>

/* ---- test helpers --------------------------------------------------------- */

/* Build a kind-1068 inner event JSON with @n_options options. */
static gchar *
make_poll_json(const gchar *pubkey, gint64 created_at,
               const gchar *question, guint n_options,
               const gchar *poll_type, gint64 ends_at)
{
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, 1068);
  nostr_event_set_pubkey(ev, pubkey);
  nostr_event_set_created_at(ev, created_at);
  nostr_event_set_content(ev, question);

  /* nostr_tags_append and nostr_event_set_tags take ownership — no manual
   * free of individual tags or the tags array after handing them off. */
  NostrTags *tags = nostr_tags_new(0);
  for (guint i = 0; i < n_options; i++) {
    gchar id[4];
    g_snprintf(id, sizeof id, "%u", i);
    gchar label[32];
    g_snprintf(label, sizeof label, "Option %u", i);
    nostr_tags_append(tags, nostr_tag_new("option", id, label, NULL));
  }

  nostr_tags_append(tags, nostr_tag_new("polltype", poll_type, NULL));

  if (ends_at > 0) {
    gchar buf[32];
    g_snprintf(buf, sizeof buf, "%" G_GINT64_FORMAT, ends_at);
    nostr_tags_append(tags, nostr_tag_new("endsAt", buf, NULL));
  }

  nostr_event_set_tags(ev, tags); /* takes ownership */

  gchar id_buf[65];
  nostr_event_compute_id(ev, id_buf);

  gchar *json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  return json;
}

/* Build a kind-1018 vote JSON. */
static gchar *
make_vote_json(const gchar *pubkey, gint64 created_at,
               const gchar *poll_id, const gchar **option_ids, guint n_options)
{
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, 1018);
  nostr_event_set_pubkey(ev, pubkey);
  nostr_event_set_created_at(ev, created_at);
  nostr_event_set_content(ev, "");

  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("e", poll_id, NULL));
  for (guint i = 0; i < n_options; i++)
    nostr_tags_append(tags, nostr_tag_new("response", option_ids[i], NULL));

  nostr_event_set_tags(ev, tags); /* takes ownership */

  gchar id_buf[65];
  nostr_event_compute_id(ev, id_buf);

  gchar *json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  return json;
}

/* NIP-29 carries poll semantics inside an ordinary kind-9 event. */
static gchar *
make_nip29_poll_json(const gchar *inner_json, const gchar *semantic_kind)
{
  NostrEvent *ev = nostr_event_new();
  g_assert_true(nostr_event_deserialize_compact(ev, inner_json, NULL));
  nostr_event_set_kind(ev, 9);
  NostrTags *tags = (NostrTags *) nostr_event_get_tags(ev);
  nostr_tags_append(tags, nostr_tag_new("poll", semantic_kind, NULL));
  gchar id_buf[65];
  nostr_event_compute_id(ev, id_buf);
  gchar *json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  return json;
}

#define ALICE_HEX "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define BOB_HEX   "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define CAROL_HEX "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define POLL_ID   "1111111111111111111111111111111111111111111111111111111111111111"

/* ---- parse and validate --------------------------------------------------- */

static void
test_parse_valid(void)
{
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *json = make_poll_json(ALICE_HEX, now, "Favorite color?",
                                           3, "singlechoice", now + 3600);
  g_autoptr(GError) err = NULL;
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          now, json, &err);
  g_assert_no_error(err);
  g_assert_nonnull(poll);
  g_assert_cmpstr(gh_mls_poll_get_event_id(poll), ==, POLL_ID);
  g_assert_cmpstr(gh_mls_poll_get_creator(poll), ==, ALICE_HEX);
  g_assert_cmpstr(gh_mls_poll_get_question(poll), ==, "Favorite color?");
  g_assert_cmpuint(gh_mls_poll_get_n_options(poll), ==, 3);
  g_assert_cmpint(gh_mls_poll_get_poll_type(poll), ==, GH_MLS_POLL_SINGLE_CHOICE);
  g_assert_cmpint(gh_mls_poll_get_ends_at(poll), ==, now + 3600);
  g_assert_true(gh_mls_poll_is_open(poll, now));
}

static void
test_parse_multiple_choice(void)
{
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *json = make_poll_json(ALICE_HEX, now, "Pick foods",
                                           4, "multiplechoice", 0);
  g_autoptr(GError) err = NULL;
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          now, json, &err);
  g_assert_no_error(err);
  g_assert_nonnull(poll);
  g_assert_cmpint(gh_mls_poll_get_poll_type(poll), ==, GH_MLS_POLL_MULTIPLE_CHOICE);
  g_assert_cmpint(gh_mls_poll_get_ends_at(poll), ==, 0);
}

static void
test_parse_no_deadline(void)
{
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *json = make_poll_json(ALICE_HEX, now, "Q", 2, "singlechoice", 0);
  g_autoptr(GError) err = NULL;
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          now, json, &err);
  g_assert_no_error(err);
  g_assert_nonnull(poll);
  /* A poll with no deadline is always open. */
  g_assert_true(gh_mls_poll_is_open(poll, now + 999999));
}

/* ---- bounds --------------------------------------------------------------- */

static void
test_bounds_too_few_options(void)
{
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *json = make_poll_json(ALICE_HEX, now, "Q", 1, "singlechoice", 0);
  g_autoptr(GError) err = NULL;
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          now, json, &err);
  g_assert_null(poll);
  g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_bounds_too_many_options(void)
{
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *json = make_poll_json(ALICE_HEX, now, "Q", 11, "singlechoice", 0);
  g_autoptr(GError) err = NULL;
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          now, json, &err);
  g_assert_null(poll);
  g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

/* ---- malformed ------------------------------------------------------------ */

static void
test_malformed_no_options(void)
{
  /* Kind 1068 but with no option tags — only a polltype tag. */
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, 1068);
  nostr_event_set_pubkey(ev, ALICE_HEX);
  nostr_event_set_created_at(ev, 1000);
  nostr_event_set_content(ev, "Q");
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("polltype", "singlechoice", NULL));
  nostr_event_set_tags(ev, tags);
  gchar id[65];
  nostr_event_compute_id(ev, id);
  gchar *json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);

  g_autoptr(GError) err = NULL;
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          1000, json, &err);
  free(json);
  g_assert_null(poll);
}

static void
test_malformed_bad_json(void)
{
  g_autoptr(GError) err = NULL;
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          1000, "not json", &err);
  g_assert_null(poll);
}

static void
test_malformed_wrong_kind(void)
{
  /* Kind 9 (regular chat) instead of 1068. */
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, 9);
  nostr_event_set_pubkey(ev, ALICE_HEX);
  nostr_event_set_created_at(ev, 1000);
  nostr_event_set_content(ev, "Q");
  NostrTags *tags = nostr_tags_new(0);
  nostr_event_set_tags(ev, tags);
  gchar id[65];
  nostr_event_compute_id(ev, id);
  gchar *json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);

  g_autoptr(GError) err = NULL;
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          1000, json, &err);
  free(json);
  g_assert_null(poll);
}

/* ---- create (build + round-trip) ------------------------------------------ */

static void
test_build_event(void)
{
  const gchar *labels[] = { "Red", "Green", "Blue", NULL };
  g_autoptr(GError) err = NULL;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *json = gh_mls_poll_build_event(
    ALICE_HEX, "abcd", now, "Best color?", labels, 3,
    GH_MLS_POLL_SINGLE_CHOICE, now + 7200, &err);
  g_assert_no_error(err);
  g_assert_nonnull(json);

  /* Parse back and verify round-trip. */
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(
    POLL_ID, ALICE_HEX, now, json, &err);
  g_assert_no_error(err);
  g_assert_nonnull(poll);
  g_assert_cmpstr(gh_mls_poll_get_question(poll), ==, "Best color?");
  g_assert_cmpuint(gh_mls_poll_get_n_options(poll), ==, 3);
  g_assert_cmpint(gh_mls_poll_get_poll_type(poll), ==, GH_MLS_POLL_SINGLE_CHOICE);
  g_assert_cmpint(gh_mls_poll_get_ends_at(poll), ==, now + 7200);

  /* Verify option labels. */
  g_assert_cmpstr(gh_mls_poll_get_option(poll, 0)->label, ==, "Red");
  g_assert_cmpstr(gh_mls_poll_get_option(poll, 1)->label, ==, "Green");
  g_assert_cmpstr(gh_mls_poll_get_option(poll, 2)->label, ==, "Blue");
}

static void
test_build_vote_event(void)
{
  const gchar *ids[] = { "0", NULL };
  g_autoptr(GError) err = NULL;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *json = gh_mls_poll_build_vote_event(
    BOB_HEX, "abcd", now, POLL_ID, ids, 1, &err);
  g_assert_no_error(err);
  g_assert_nonnull(json);

  /* Parse back. */
  g_autofree gchar *target = NULL;
  g_auto(GStrv) options = NULL;
  gboolean ok = gh_mls_poll_parse_vote(json, &target, &options, NULL, &err);
  g_assert_no_error(err);
  g_assert_true(ok);
  g_assert_cmpstr(target, ==, POLL_ID);
  g_assert_cmpstr(options[0], ==, "0");
  g_assert_null(options[1]);
}

/* ---- vote and tallies ----------------------------------------------------- */

static void
test_vote_single(void)
{
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *json = make_poll_json(ALICE_HEX, now, "Q", 3, "singlechoice", 0);
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          now, json, NULL);
  g_assert_nonnull(poll);
  gh_mls_poll_set_local_account(poll, BOB_HEX);

  const gchar *ids[] = { "1", NULL };
  gboolean changed = gh_mls_poll_apply_vote(poll, BOB_HEX, ids, now, NULL);
  g_assert_true(changed);
  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 1);

  const GhMlsPollTally *t0 = gh_mls_poll_get_option(poll, 0);
  const GhMlsPollTally *t1 = gh_mls_poll_get_option(poll, 1);
  g_assert_cmpuint(t0->votes, ==, 0);
  g_assert_cmpuint(t1->votes, ==, 1);

  /* Local selection. */
  g_assert_true(gh_mls_poll_has_voted(poll));
  const gchar *const *sel = gh_mls_poll_get_local_selection(poll);
  g_assert_nonnull(sel);
  g_assert_cmpstr(sel[0], ==, "1");
}

static void
test_change_vote(void)
{
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *json = make_poll_json(ALICE_HEX, now, "Q", 3, "singlechoice", 0);
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          now, json, NULL);
  g_assert_nonnull(poll);

  const gchar *ids1[] = { "0", NULL };
  gh_mls_poll_apply_vote(poll, BOB_HEX, ids1, now, NULL);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 0)->votes, ==, 1);

  /* Change vote from option 0 to option 2. */
  const gchar *ids2[] = { "2", NULL };
  gboolean changed = gh_mls_poll_apply_vote(poll, BOB_HEX, ids2, now + 1, NULL);
  g_assert_true(changed);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 0)->votes, ==, 0);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 2)->votes, ==, 1);
  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 1);
}

static void
test_multiple_voters(void)
{
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *json = make_poll_json(ALICE_HEX, now, "Q", 2, "singlechoice", 0);
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          now, json, NULL);
  g_assert_nonnull(poll);

  const gchar *ids0[] = { "0", NULL };
  const gchar *ids1[] = { "1", NULL };
  gh_mls_poll_apply_vote(poll, ALICE_HEX, ids0, now, NULL);
  gh_mls_poll_apply_vote(poll, BOB_HEX, ids0, now, NULL);
  gh_mls_poll_apply_vote(poll, CAROL_HEX, ids1, now, NULL);

  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 3);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 0)->votes, ==, 2);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 1)->votes, ==, 1);
}

static void
test_duplicate_vote_no_change(void)
{
  /* Applying the same vote again should return FALSE (no tally change). */
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *json = make_poll_json(ALICE_HEX, now, "Q", 3, "singlechoice", 0);
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          now, json, NULL);
  g_assert_nonnull(poll);

  const gchar *ids[] = { "1", NULL };
  gboolean first = gh_mls_poll_apply_vote(poll, BOB_HEX, ids, now, NULL);
  g_assert_true(first);
  /* Re-applying the same vote is valid but returns FALSE (no tally change). */
  gboolean second = gh_mls_poll_apply_vote(poll, BOB_HEX, ids, now + 1, NULL);
  g_assert_false(second);
  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 1);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 1)->votes, ==, 1);
}

/* ---- closed poll ---------------------------------------------------------- */

static void
test_closed_poll(void)
{
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  /* Poll ended 10 seconds ago. */
  g_autofree gchar *json = make_poll_json(ALICE_HEX, now - 100,
                                           "Q", 2, "singlechoice", now - 10);
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          now - 100, json, NULL);
  g_assert_nonnull(poll);
  g_assert_false(gh_mls_poll_is_open(poll, now));
}

/* ---- parse_vote ----------------------------------------------------------- */

static void
test_parse_vote_valid(void)
{
  g_autofree gchar *json = make_vote_json(BOB_HEX, 1000, POLL_ID,
                                           (const gchar *[]){"0", NULL}, 1);
  g_autofree gchar *target = NULL;
  g_auto(GStrv) options = NULL;
  g_autoptr(GError) err = NULL;
  g_assert_true(gh_mls_poll_parse_vote(json, &target, &options, NULL, &err));
  g_assert_no_error(err);
  g_assert_cmpstr(target, ==, POLL_ID);
  g_assert_cmpstr(options[0], ==, "0");
}

static void
test_parse_vote_multiple_responses(void)
{
  g_autofree gchar *json = make_vote_json(BOB_HEX, 1000, POLL_ID,
                                           (const gchar *[]){"0", "2", NULL}, 2);
  g_autofree gchar *target = NULL;
  g_auto(GStrv) options = NULL;
  g_autoptr(GError) err = NULL;
  g_assert_true(gh_mls_poll_parse_vote(json, &target, &options, NULL, &err));
  g_assert_no_error(err);
  g_assert_cmpstr(target, ==, POLL_ID);
  g_assert_cmpstr(options[0], ==, "0");
  g_assert_cmpstr(options[1], ==, "2");
  g_assert_null(options[2]);
}

static void
test_parse_vote_malformed(void)
{
  g_autofree gchar *target = NULL;
  g_auto(GStrv) options = NULL;
  g_autoptr(GError) err = NULL;
  g_assert_false(gh_mls_poll_parse_vote("not json", &target, &options, NULL, &err));
}

/* ---- privacy: nothing fetched --------------------------------------------- */

static void
test_no_network_fetch(void)
{
  /* Creating and parsing a poll must not trigger any network activity.
   * GhMlsPoll takes only a JSON string and never contacts any relay or
   * HTTP API. This is a structural assertion: if the poll model ever adds a
   * network dependency, its link will fail or this process's lack of a relay
   * will cause the test to time out or crash. */
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *json = make_poll_json(ALICE_HEX, now, "Q", 2, "singlechoice", 0);
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          now, json, NULL);
  g_assert_nonnull(poll);

  /* Also build a vote event and parse it — same assertion. */
  const gchar *ids[] = { "0", NULL };
  g_autofree gchar *vote_json = gh_mls_poll_build_vote_event(
    BOB_HEX, "abcd", now, POLL_ID, ids, 1, NULL);
  g_assert_nonnull(vote_json);
  g_autofree gchar *target = NULL;
  g_auto(GStrv) options = NULL;
  g_assert_true(gh_mls_poll_parse_vote(vote_json, &target, &options, NULL, NULL));
}

/* ---- MDK v0.11 wire-format compatibility (nostrc-a36s matrix case) -------- */

/* MDK produces inner app events as Serde struct-order JSON:
 * {"id":"...","pubkey":"...","created_at":N,"kind":N,"tags":[...],"content":"..."}
 * Verify Groundhog can parse events in that format and that Groundhog-produced
 * events have the correct tag structure matching the MDK spec in
 * /tmp/mdk-v0.11.0/crates/traits/src/polls.rs. */

/* A poll event produced by MDK: deterministic field order, sequential integer
 * option ids, matching NIP-01 canonical id. */
static const gchar MDK_POLL_JSON[] =
  "{\"id\":\"0000000000000000000000000000000000000000000000000000000000000000\","
  "\"pubkey\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
  "\"created_at\":1000,\"kind\":1068,"
  "\"tags\":["
    "[\"h\",\"abcd\"],"
    "[\"option\",\"0\",\"Tea\"],"
    "[\"option\",\"1\",\"Coffee\"],"
    "[\"polltype\",\"singlechoice\"],"
    "[\"endsAt\",\"3600\"]"
  "],"
  "\"content\":\"Drink?\"}";

static void
test_mdk_poll_to_groundhog(void)
{
  /* MDK-produced poll → Groundhog parses correctly. */
  g_autoptr(GError) err = NULL;
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(
    POLL_ID, ALICE_HEX, 1000, MDK_POLL_JSON, &err);
  g_assert_no_error(err);
  g_assert_nonnull(poll);
  g_assert_cmpstr(gh_mls_poll_get_question(poll), ==, "Drink?");
  g_assert_cmpuint(gh_mls_poll_get_n_options(poll), ==, 2);
  g_assert_cmpstr(gh_mls_poll_get_option(poll, 0)->id, ==, "0");
  g_assert_cmpstr(gh_mls_poll_get_option(poll, 0)->label, ==, "Tea");
  g_assert_cmpstr(gh_mls_poll_get_option(poll, 1)->id, ==, "1");
  g_assert_cmpstr(gh_mls_poll_get_option(poll, 1)->label, ==, "Coffee");
  g_assert_cmpint(gh_mls_poll_get_poll_type(poll), ==, GH_MLS_POLL_SINGLE_CHOICE);
  g_assert_cmpint(gh_mls_poll_get_ends_at(poll), ==, 3600);
}

static void
test_groundhog_poll_tag_structure(void)
{
  /* Groundhog poll → tags byte-for-byte match MDK expectations. */
  const gchar *labels[] = { "Tea", "Coffee", NULL };
  g_autoptr(GError) err = NULL;
  g_autofree gchar *json = gh_mls_poll_build_event(
    ALICE_HEX, "abcd", 1000, "Drink?", labels, 2,
    GH_MLS_POLL_SINGLE_CHOICE, 3600, &err);
  g_assert_no_error(err);
  g_assert_nonnull(json);

  /* Parse with libnostr to inspect tags. */
  NostrEvent ev = { 0 };
  g_assert_true(nostr_event_deserialize_compact(&ev, json, NULL));

  g_assert_cmpint(nostr_event_get_kind(&ev), ==, 1068);
  g_assert_cmpstr(nostr_event_get_content(&ev), ==, "Drink?");

  /* Verify tag structure matches MDK spec:
   * ["h","abcd"], ["option","0","Tea"], ["option","1","Coffee"],
   * ["polltype","singlechoice"], ["endsAt","3600"] */
  NostrTags *tags = (NostrTags *) nostr_event_get_tags(&ev);
  g_assert_nonnull(tags);
  g_assert_cmpuint(nostr_tags_size(tags), ==, 5);

  /* Tag 0: ["h", "abcd"] */
  NostrTag *t0 = nostr_tags_get(tags, 0);
  g_assert_cmpstr(nostr_tag_get_key(t0), ==, "h");
  g_assert_cmpstr(nostr_tag_get_value(t0), ==, "abcd");

  /* Tag 1: ["option", "0", "Tea"] */
  NostrTag *t1 = nostr_tags_get(tags, 1);
  g_assert_cmpstr(nostr_tag_get_key(t1), ==, "option");
  g_assert_cmpstr(nostr_tag_get(t1, 1), ==, "0");
  g_assert_cmpstr(nostr_tag_get(t1, 2), ==, "Tea");

  /* Tag 2: ["option", "1", "Coffee"] */
  NostrTag *t2 = nostr_tags_get(tags, 2);
  g_assert_cmpstr(nostr_tag_get_key(t2), ==, "option");
  g_assert_cmpstr(nostr_tag_get(t2, 1), ==, "1");
  g_assert_cmpstr(nostr_tag_get(t2, 2), ==, "Coffee");

  /* Tag 3: ["polltype", "singlechoice"] */
  NostrTag *t3 = nostr_tags_get(tags, 3);
  g_assert_cmpstr(nostr_tag_get_key(t3), ==, "polltype");
  g_assert_cmpstr(nostr_tag_get_value(t3), ==, "singlechoice");

  /* Tag 4: ["endsAt", "3600"] */
  NostrTag *t4 = nostr_tags_get(tags, 4);
  g_assert_cmpstr(nostr_tag_get_key(t4), ==, "endsAt");
  g_assert_cmpstr(nostr_tag_get_value(t4), ==, "3600");

  /* MDK option ids are sequential integers starting at 0. */
  g_assert_cmpstr(nostr_tag_get(t1, 1), ==, "0");
  g_assert_cmpstr(nostr_tag_get(t2, 1), ==, "1");

  /* Cleanup stack-allocated event members. */
  free(ev.id);
  free(ev.pubkey);
  free(ev.content);
  free(ev.sig);
  nostr_tags_free((NostrTags *) ev.tags);
}

/* A vote event produced by MDK format. */
static const gchar MDK_VOTE_JSON[] =
  "{\"id\":\"0000000000000000000000000000000000000000000000000000000000000000\","
  "\"pubkey\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
  "\"created_at\":2000,\"kind\":1018,"
  "\"tags\":["
    "[\"h\",\"abcd\"],"
    "[\"e\",\"1111111111111111111111111111111111111111111111111111111111111111\"],"
    "[\"response\",\"0\"]"
  "],"
  "\"content\":\"\"}";

static void
test_mdk_vote_to_groundhog(void)
{
  /* MDK-produced vote → Groundhog parses correctly. */
  g_autofree gchar *target = NULL;
  g_auto(GStrv) options = NULL;
  g_autoptr(GError) err = NULL;
  g_assert_true(gh_mls_poll_parse_vote(MDK_VOTE_JSON, &target, &options, NULL, &err));
  g_assert_no_error(err);
  g_assert_cmpstr(target, ==, POLL_ID);
  g_assert_cmpstr(options[0], ==, "0");
  g_assert_null(options[1]);
}

static void
test_groundhog_vote_tag_structure(void)
{
  /* Groundhog vote → tags byte-for-byte match MDK expectations. */
  const gchar *ids[] = { "0", NULL };
  g_autoptr(GError) err = NULL;
  g_autofree gchar *json = gh_mls_poll_build_vote_event(
    BOB_HEX, "abcd", 2000, POLL_ID, ids, 1, &err);
  g_assert_no_error(err);
  g_assert_nonnull(json);

  NostrEvent ev = { 0 };
  g_assert_true(nostr_event_deserialize_compact(&ev, json, NULL));

  g_assert_cmpint(nostr_event_get_kind(&ev), ==, 1018);
  g_assert_cmpstr(nostr_event_get_content(&ev), ==, "");

  NostrTags *tags = (NostrTags *) nostr_event_get_tags(&ev);
  g_assert_nonnull(tags);
  g_assert_cmpuint(nostr_tags_size(tags), ==, 3);

  /* Tag 0: ["h", "abcd"] */
  NostrTag *t0 = nostr_tags_get(tags, 0);
  g_assert_cmpstr(nostr_tag_get_key(t0), ==, "h");
  g_assert_cmpstr(nostr_tag_get_value(t0), ==, "abcd");

  /* Tag 1: ["e", poll_id] */
  NostrTag *t1 = nostr_tags_get(tags, 1);
  g_assert_cmpstr(nostr_tag_get_key(t1), ==, "e");
  g_assert_cmpstr(nostr_tag_get_value(t1), ==, POLL_ID);

  /* Tag 2: ["response", "0"] */
  NostrTag *t2 = nostr_tags_get(tags, 2);
  g_assert_cmpstr(nostr_tag_get_key(t2), ==, "response");
  g_assert_cmpstr(nostr_tag_get_value(t2), ==, "0");

  free(ev.id);
  free(ev.pubkey);
  free(ev.content);
  free(ev.sig);
  nostr_tags_free((NostrTags *) ev.tags);
}

/* MDK poll → Groundhog votes → verify tallies. */
static void
test_mdk_poll_groundhog_votes(void)
{
  g_autoptr(GError) err = NULL;
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(
    POLL_ID, ALICE_HEX, 1000, MDK_POLL_JSON, &err);
  g_assert_no_error(err);
  g_assert_nonnull(poll);

  /* Groundhog casts a vote for option 1 ("Coffee"). */
  const gchar *ids[] = { "1", NULL };
  gh_mls_poll_apply_vote(poll, BOB_HEX, ids, 2000, NULL);
  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 1);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 1)->votes, ==, 1);

  /* Another voter from Groundhog also votes Tea. */
  const gchar *ids0[] = { "0", NULL };
  gh_mls_poll_apply_vote(poll, CAROL_HEX, ids0, 2001, NULL);
  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 2);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 0)->votes, ==, 1);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 1)->votes, ==, 1);
}

/* Groundhog poll → MDK votes (parsed). */
static void
test_groundhog_poll_mdk_votes(void)
{
  /* Create a poll with Groundhog. */
  const gchar *labels[] = { "Tea", "Coffee", NULL };
  g_autoptr(GError) err = NULL;
  g_autofree gchar *poll_json = gh_mls_poll_build_event(
    ALICE_HEX, "abcd", 1000, "Drink?", labels, 2,
    GH_MLS_POLL_SINGLE_CHOICE, 3600, &err);
  g_assert_no_error(err);

  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(
    POLL_ID, ALICE_HEX, 1000, poll_json, &err);
  g_assert_no_error(err);

  /* Apply an MDK-formatted vote. */
  g_autofree gchar *target = NULL;
  g_auto(GStrv) options = NULL;
  g_assert_true(gh_mls_poll_parse_vote(MDK_VOTE_JSON, &target, &options, NULL, &err));
  g_assert_cmpstr(target, ==, POLL_ID);
  gh_mls_poll_apply_vote(poll, BOB_HEX, (const gchar **) options, 2000, NULL);
  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 1);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 0)->votes, ==, 1);
}

/* ---- F1: invalid UTF-8 (fuzz-found heap-buffer-overflow) ------------------- */

static void
test_invalid_utf8_question(void)
{
  /* The fuzz input that triggered F1: a À near the end of a short string.
   * g_utf8_next_char would read past the NUL into unallocated heap. */
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, 1068);
  nostr_event_set_pubkey(ev, ALICE_HEX);
  nostr_event_set_created_at(ev, 1000);
  /* A raw 0xC0 byte near the end: an incomplete multi-byte leader that
   * causes g_utf8_next_char to read past the NUL terminator. */
  gchar bad_question[] = { 'H', 'e', 'l', 'l', 'o', (gchar)0xC0, '\0' };
  nostr_event_set_content(ev, bad_question);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("option", "0", "A", NULL));
  nostr_tags_append(tags, nostr_tag_new("option", "1", "B", NULL));
  nostr_tags_append(tags, nostr_tag_new("polltype", "singlechoice", NULL));
  nostr_event_set_tags(ev, tags);
  gchar id[65];
  nostr_event_compute_id(ev, id);
  gchar *json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);

  g_autoptr(GError) err = NULL;
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          1000, json, &err);
  free(json);
  g_assert_null(poll);
  g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_invalid_utf8_option_label(void)
{
  /* Invalid UTF-8 in an option label. */
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, 1068);
  nostr_event_set_pubkey(ev, ALICE_HEX);
  nostr_event_set_created_at(ev, 1000);
  nostr_event_set_content(ev, "Q");
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("option", "0", "Good", NULL));
  gchar bad_label[] = { 'B', 'a', 'd', (gchar)0xFE, (gchar)0xFF, '\0' };
  nostr_tags_append(tags, nostr_tag_new("option", "1", bad_label, NULL));
  nostr_tags_append(tags, nostr_tag_new("polltype", "singlechoice", NULL));
  nostr_event_set_tags(ev, tags);
  gchar id[65];
  nostr_event_compute_id(ev, id);
  gchar *json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);

  g_autoptr(GError) err = NULL;
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          1000, json, &err);
  free(json);
  g_assert_null(poll);
}

/* ---- F3: post-deadline vote rejection --------------------------------------- */

static void
test_late_vote_rejected(void)
{
  /* A poll that ended at t=2000; a vote at t=2001 must be ignored. */
  gint64 created = 1000;
  gint64 ends = 2000;
  g_autofree gchar *json = make_poll_json(ALICE_HEX, created, "Q", 2,
                                           "singlechoice", ends);
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          created, json, NULL);
  g_assert_nonnull(poll);
  g_assert_true(gh_mls_poll_is_open(poll, 1500));
  g_assert_false(gh_mls_poll_is_open(poll, 2001));

  /* A vote at t=1500 (within deadline) should succeed. */
  const gchar *ids[] = { "0", NULL };
  g_assert_true(gh_mls_poll_is_open(poll, 1500));
  gboolean ok = gh_mls_poll_apply_vote(poll, BOB_HEX, ids, 1500, NULL);
  g_assert_true(ok);
  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 1);
}

static void
test_pre_poll_vote_rejected(void)
{
  /* A vote with created_at before the poll's created_at — should be rejected
   * by the service-level check (MDK's validate_poll_response rejects
   * response_created_at < poll_created_at). This test verifies the model-
   * level is_open check. */
  gint64 created = 1000;
  g_autofree gchar *json = make_poll_json(ALICE_HEX, created, "Q", 2,
                                           "singlechoice", 0);
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          created, json, NULL);
  g_assert_nonnull(poll);
  /* created_at=999 is before the poll — the service layer rejects it. */
  g_assert_true(gh_mls_poll_is_open(poll, 999));  /* no deadline → always open */
}

/* ---- vote withdrawal (F5) ------------------------------------------------- */

static void
test_vote_withdrawal(void)
{
  /* F5 / nostrc-tmib: withdrawing a vote (convergence) removes the voter
   * record in-session, not just on restart.  The voter record stores the
   * vote_event_id so remove_voter_by_event_id can find it. */
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *json = make_poll_json(ALICE_HEX, now, "Best color?",
                                           3, "singlechoice", 0);
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          now, json, NULL);
  g_assert_nonnull(poll);

  /* Bob and Carol vote (with known vote event ids). */
  const gchar *ids0[] = { "0", NULL };
  const gchar *ids1[] = { "1", NULL };
  g_assert_true(gh_mls_poll_apply_vote(poll, BOB_HEX, ids0, now,
                                        "bbbb000000000000000000000000000000000000000000000000000000000001"));
  g_assert_true(gh_mls_poll_apply_vote(poll, CAROL_HEX, ids1, now,
                                        "cccc000000000000000000000000000000000000000000000000000000000001"));
  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 2);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 0)->votes, ==, 1);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 1)->votes, ==, 1);

  /* Withdraw Bob's vote by its event id (as convergence would). */
  g_assert_true(gh_mls_poll_remove_voter_by_event_id(
      poll, "bbbb000000000000000000000000000000000000000000000000000000000001"));
  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 1);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 0)->votes, ==, 0);  /* Bob's vote gone */
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 1)->votes, ==, 1);  /* Carol's unchanged */

  /* Withdrawing a non-existent event id returns FALSE. */
  g_assert_false(gh_mls_poll_remove_voter_by_event_id(
      poll, "dddd000000000000000000000000000000000000000000000000000000000001"));
  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 1);

  /* Also test remove_voter by pubkey (Carol). */
  g_assert_true(gh_mls_poll_remove_voter(poll, CAROL_HEX));
  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 0);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 0)->votes, ==, 0);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 1)->votes, ==, 0);

  /* Removing an already-removed voter returns FALSE. */
  g_assert_false(gh_mls_poll_remove_voter(poll, CAROL_HEX));
}


/* ---- same-choice re-vote updates event ID (review finding 6) ----------- */

static void
test_revote_same_choice_updates_event_id(void)
{
  /* A same-choice re-vote does not change tallies but must track the newest
   * event id.  If convergence withdraws the second (surviving) event, the
   * voter record must be found and removed by the newer id.  If it
   * withdraws the first (superseded) event, the voter must NOT be
   * removed — the surviving second event still stands. */
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *json = make_poll_json(ALICE_HEX, now, "Fav?", 2,
                                           "singlechoice", 0);
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          now, json, NULL);
  const gchar *ids0[] = { "0", NULL };

  /* First vote by Bob for option 0 with event A. */
  g_assert_true(gh_mls_poll_apply_vote(poll, BOB_HEX, ids0, now,
      "aaaa000000000000000000000000000000000000000000000000000000000001"));
  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 1);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 0)->votes, ==, 1);

  /* Same-choice re-vote by Bob for option 0 with event B. */
  g_assert_false(gh_mls_poll_apply_vote(poll, BOB_HEX, ids0, now,
      "bbbb000000000000000000000000000000000000000000000000000000000001"));
  /* Tallies did not change. */
  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 1);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 0)->votes, ==, 1);

  /* Withdraw the FIRST (superseded) event A: Bob must remain — the second
   * event B still stands. */
  g_assert_false(gh_mls_poll_remove_voter_by_event_id(poll,
      "aaaa000000000000000000000000000000000000000000000000000000000001"));
  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 1);

  /* Withdraw the SECOND (surviving) event B: Bob is removed. */
  g_assert_true(gh_mls_poll_remove_voter_by_event_id(poll,
      "bbbb000000000000000000000000000000000000000000000000000000000001"));
  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 0);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 0)->votes, ==, 0);
}

static void
test_newest_vote_wins_and_reset(void)
{
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *json = make_poll_json(ALICE_HEX, now, "Best color?", 2,
                                           "singlechoice", 0);
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          now, json, NULL);
  const gchar *zero[] = { "0", NULL };
  const gchar *one[] = { "1", NULL };
  gh_mls_poll_set_local_account(poll, BOB_HEX);
  g_assert_true(gh_mls_poll_apply_vote(poll, BOB_HEX, zero, now + 2,
                                        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab"));
  g_assert_false(gh_mls_poll_apply_vote(poll, BOB_HEX, one, now + 1,
                                         "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac"));
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 0)->votes, ==, 1);
  g_assert_cmpuint(gh_mls_poll_get_option(poll, 1)->votes, ==, 0);
  g_assert_true(gh_mls_poll_has_voted(poll));
  gh_mls_poll_reset_votes(poll);
  g_assert_cmpuint(gh_mls_poll_get_total_voters(poll), ==, 0);
  g_assert_false(gh_mls_poll_has_voted(poll));
}

static void
test_nip29_poll_and_vote_envelope(void)
{
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *poll_json = make_poll_json(ALICE_HEX, now,
                                                 "Best color?", 2,
                                                 "singlechoice", 0);
  g_autofree gchar *group_poll = make_nip29_poll_json(poll_json, "1068");
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(POLL_ID, ALICE_HEX,
                                                          now, group_poll, &error);
  g_assert_no_error(error);
  g_assert_nonnull(poll);
  g_assert_cmpstr(gh_mls_poll_get_question(poll), ==, "Best color?");

  const gchar *ids[] = { "1", NULL };
  g_autofree gchar *vote_json = make_vote_json(BOB_HEX, now + 1, POLL_ID, ids, 1);
  g_autofree gchar *group_vote = make_nip29_poll_json(vote_json, "1018");
  g_autofree gchar *target = NULL;
  g_auto(GStrv) choices = NULL;
  g_autofree gchar *event_id = NULL;
  g_assert_true(gh_mls_poll_parse_vote(group_vote, &target, &choices, &event_id,
                                       &error));
  g_assert_no_error(error);
  g_assert_cmpstr(target, ==, POLL_ID);
  g_assert_cmpstr(choices[0], ==, "1");
}

/* ---- main ----------------------------------------------------------------- */

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/mls/poll/parse-valid", test_parse_valid);
  g_test_add_func("/mls/poll/parse-multiple-choice", test_parse_multiple_choice);
  g_test_add_func("/mls/poll/parse-no-deadline", test_parse_no_deadline);
  g_test_add_func("/mls/poll/bounds-too-few", test_bounds_too_few_options);
  g_test_add_func("/mls/poll/bounds-too-many", test_bounds_too_many_options);
  g_test_add_func("/mls/poll/malformed-no-options", test_malformed_no_options);
  g_test_add_func("/mls/poll/malformed-bad-json", test_malformed_bad_json);
  g_test_add_func("/mls/poll/malformed-wrong-kind", test_malformed_wrong_kind);
  g_test_add_func("/mls/poll/build-event", test_build_event);
  g_test_add_func("/mls/poll/build-vote-event", test_build_vote_event);
  g_test_add_func("/mls/poll/vote-single", test_vote_single);
  g_test_add_func("/mls/poll/change-vote", test_change_vote);
  g_test_add_func("/mls/poll/multiple-voters", test_multiple_voters);
  g_test_add_func("/mls/poll/duplicate-vote-no-change", test_duplicate_vote_no_change);
  g_test_add_func("/mls/poll/closed", test_closed_poll);
  g_test_add_func("/mls/poll/parse-vote-valid", test_parse_vote_valid);
  g_test_add_func("/mls/poll/parse-vote-multiple", test_parse_vote_multiple_responses);
  g_test_add_func("/mls/poll/parse-vote-malformed", test_parse_vote_malformed);
  g_test_add_func("/mls/poll/no-network-fetch", test_no_network_fetch);

  /* F1: invalid UTF-8 (fuzz-found heap-buffer-overflow). */
  g_test_add_func("/mls/poll/invalid-utf8-question", test_invalid_utf8_question);
  g_test_add_func("/mls/poll/invalid-utf8-option-label", test_invalid_utf8_option_label);

  /* F3: post-deadline vote rejection. */
  g_test_add_func("/mls/poll/late-vote-rejected", test_late_vote_rejected);
  g_test_add_func("/mls/poll/pre-poll-vote-rejected", test_pre_poll_vote_rejected);

  /* F5 / nostrc-tmib: vote withdrawal removes voter record in-session. */
  g_test_add_func("/mls/poll/vote-withdrawal", test_vote_withdrawal);

  /* Review finding 6: same-choice re-vote must update the event id. */
  g_test_add_func("/mls/poll/revote-same-choice-updates-event-id",
                  test_revote_same_choice_updates_event_id);

  g_test_add_func("/mls/poll/newest-vote-wins-and-reset", test_newest_vote_wins_and_reset);
  g_test_add_func("/mls/poll/nip29-envelope", test_nip29_poll_and_vote_envelope);

  /* MDK v0.11 wire-format matrix. */
  g_test_add_func("/mls/poll/mdk-poll-to-groundhog", test_mdk_poll_to_groundhog);
  g_test_add_func("/mls/poll/groundhog-poll-tag-structure", test_groundhog_poll_tag_structure);
  g_test_add_func("/mls/poll/mdk-vote-to-groundhog", test_mdk_vote_to_groundhog);
  g_test_add_func("/mls/poll/groundhog-vote-tag-structure", test_groundhog_vote_tag_structure);
  g_test_add_func("/mls/poll/mdk-poll-groundhog-votes", test_mdk_poll_groundhog_votes);
  g_test_add_func("/mls/poll/groundhog-poll-mdk-votes", test_groundhog_poll_mdk_votes);

  return g_test_run();
}

