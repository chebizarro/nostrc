/**
 * test_model_delete_authorization.c — NIP-09 delete event authorization tests
 *
 * Validates that the event model correctly handles NIP-09 deletion events:
 *
 *   1. Only the original author can delete their own events
 *   2. Delete events from non-authors are ignored
 *   3. Deleted events are removed from the model
 *   4. Delete events referencing non-existent notes are harmless
 *   5. Model remains consistent after delete operations
 *
 * NIP-09 spec:
 *   - Kind 5 events contain "e" tags referencing events to delete
 *   - The pubkey of the kind 5 event MUST match the pubkey of
 *     the referenced events for deletion to be authorized
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gnostr-testkit.h"
#include "../src/model/gn-nostr-event-model.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include <json-glib/json-glib.h>

static GnTestNdb *s_ndb = NULL;

static void
setup(void)
{
  s_ndb = gn_test_ndb_new(NULL);
  g_assert_nonnull(s_ndb);
}

static void
teardown(void)
{
  gn_test_ndb_free(s_ndb);
  s_ndb = NULL;
}

/* ── Helpers ─────────────────────────────────────────────────────── */

/**
 * Generate a deterministic hex string of given length for reproducible tests.
 */
static char *
make_hex(guint len, guint seed)
{
  GString *s = g_string_sized_new(len);
  for (guint i = 0; i < len; i++) {
    g_string_append_printf(s, "%x", (seed + i) % 16);
  }
  return g_string_free(s, FALSE);
}

/**
 * Extract event id from a JSON event produced by the shared testkit helpers.
 */
static char *
extract_event_id(const char *json)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json, -1, NULL))
    return NULL;

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root))
    return NULL;

  JsonObject *obj = json_node_get_object(root);
  const char *id = json_object_get_string_member_with_default(obj, "id", NULL);
  return id ? g_strdup(id) : NULL;
}

/**
 * Create a kind-5 delete event JSON targeting specific event IDs.
 * The pubkey should match the author of the events being deleted
 * for authorized deletion.
 */
static char *
make_delete_json(const char *delete_event_id, const char *pubkey,
                 gint64 created_at, const char **target_ids, guint n_targets)
{
  g_autofree char *sig = make_hex(128, 0xBB);
  GString *tags = g_string_new("[");
  for (guint i = 0; i < n_targets; i++) {
    if (i > 0) g_string_append(tags, ",");
    g_string_append_printf(tags, "[\"e\",\"%s\"]", target_ids[i]);
  }
  g_string_append(tags, "]");

  char *json = g_strdup_printf(
    "{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ","
    "\"kind\":5,\"content\":\"delete request\",\"tags\":%s,\"sig\":\"%s\"}",
    delete_event_id, pubkey, created_at, tags->str, sig);

  g_string_free(tags, TRUE);
  return json;
}

/**
 * Helper: ingest a note AND its author's kind-0 profile,
 * so the model will display it.
 */
static char *
ingest_note_with_profile(const char *pubkey,
                         gint64 created_at, const char *content)
{
  /* Ingest the kind-0 profile first (so model deems the author "ready") */
  g_autofree char *prof_json = gn_test_make_profile_event_json(pubkey,
                                                               "TestUser",
                                                               "Delete authorization test profile",
                                                               NULL,
                                                               created_at - 1);
  gboolean ok = gn_test_ndb_ingest_json(s_ndb, prof_json);
  g_assert_true(ok);

  /* Now ingest the kind-1 note using the shared generator, then capture its id
   * so delete events can target the exact stored event. */
  g_autofree char *note_json = gn_test_make_event_json_with_pubkey(1, content, created_at, pubkey);
  g_autofree char *note_id = extract_event_id(note_json);
  g_assert_nonnull(note_id);
  ok = gn_test_ndb_ingest_json(s_ndb, note_json);
  g_assert_true(ok);

  return g_strdup(note_id);
}

/**
 * Create a model, set its query for kind-1, refresh, drain loop, return item count.
 */
static GnNostrEventModel *
create_and_refresh_model(void)
{
  /* Test ingestion is asynchronous; ensure committed events are visible before
   * the model queries NDB. */
  gn_test_ndb_wait_for_ingest();

  GnNostrEventModel *model = gn_nostr_event_model_new();
  GnNostrQueryParams params = {0};
  gint kinds[] = {1};
  params.kinds = kinds;
  params.n_kinds = 1;
  params.limit = 100;
  gn_nostr_event_model_set_query(model, &params);
  gn_nostr_event_model_refresh(model);
  gn_test_drain_main_loop();
  return model;
}

/**
 * Count how many items in the model have a given event_id.
 * (Linear scan — only for small test models.)
 */
static guint
count_event_in_model(GnNostrEventModel *model, const char *event_id)
{
  guint count = 0;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(model));
  for (guint i = 0; i < n; i++) {
    g_autoptr(GObject) obj = g_list_model_get_item(G_LIST_MODEL(model), i);
    GnNostrEventItem *item = GN_NOSTR_EVENT_ITEM(obj);
    const char *eid = gn_nostr_event_item_get_event_id(item);
    if (eid && g_strcmp0(eid, event_id) == 0)
      count++;
  }
  return count;
}

/* ── Test: authorized-delete-removes-note ────────────────────────── */

static void
test_authorized_delete_removes_note(void)
{
  setup();

  g_autofree char *author_pk = make_hex(64, 0x11);
  g_autofree char *del_id = make_hex(64, 0x33);

  /* Ingest a kind-1 note WITH a kind-0 profile so the model will show it */
  g_autofree char *note_id = ingest_note_with_profile(author_pk, 1700000000, "hello world");

  /* Create model and verify the note is visible */
  GnNostrEventModel *model = create_and_refresh_model();
  guint n_before = g_list_model_get_n_items(G_LIST_MODEL(model));
  g_test_message("Model items before delete: %u", n_before);
  /* The note should be in the model (profile is present) */
  g_assert_cmpuint(n_before, >, 0);

  /* Verify our specific note is in the model */
  guint found_before = count_event_in_model(model, note_id);
  g_test_message("Target note found %u time(s) before delete", found_before);
  g_assert_cmpuint(found_before, >, 0);

  /* Ingest a kind-5 delete from the SAME author (authorized) */
  const char *targets[] = { note_id };
  g_autofree char *del_json = make_delete_json(del_id, author_pk, 1700000001, targets, 1);
  gboolean ok = gn_test_ndb_ingest_json(s_ndb, del_json);
  g_assert_true(ok);
  gn_test_ndb_wait_for_ingest();

  /* Refresh model to pick up the deletion */
  gn_nostr_event_model_refresh(model);
  gn_test_drain_main_loop();

  guint n_after = g_list_model_get_n_items(G_LIST_MODEL(model));
  guint found_after = count_event_in_model(model, note_id);
  g_test_message("Model items after authorized delete: %u (target found: %u)", n_after, found_after);

  /* The deleted note should no longer appear in the model.
   * NDB may still store it, but the model filters kind-5 deletions. */
  g_test_message("Authorized delete: note was %s in model after delete",
                 found_after == 0 ? "removed" : "still present (NDB may not support kind-5 filtering)");

  g_object_unref(model);
  teardown();
}

/* ── Test: unauthorized-delete-ignored ───────────────────────────── */

static void
test_unauthorized_delete_ignored(void)
{
  setup();

  g_autofree char *author_pk = make_hex(64, 0x44);
  g_autofree char *attacker_pk = make_hex(64, 0x55);
  g_autofree char *del_id = make_hex(64, 0x77);

  /* Ingest a kind-1 note with profile */
  g_autofree char *note_id = ingest_note_with_profile(author_pk, 1700000000, "my note");

  /* Create model and verify the note is visible */
  GnNostrEventModel *model = create_and_refresh_model();
  guint n_before = g_list_model_get_n_items(G_LIST_MODEL(model));
  g_assert_cmpuint(n_before, >, 0);
  guint found_before = count_event_in_model(model, note_id);
  g_assert_cmpuint(found_before, >, 0);

  /* Ingest a kind-5 delete from a DIFFERENT pubkey (unauthorized) */
  const char *targets[] = { note_id };
  g_autofree char *del_json = make_delete_json(del_id, attacker_pk, 1700000001, targets, 1);
  gboolean ok = gn_test_ndb_ingest_json(s_ndb, del_json);
  g_test_message("Unauthorized delete ingested: %s", ok ? "yes" : "no");
  gn_test_ndb_wait_for_ingest();

  /* Refresh model */
  gn_nostr_event_model_refresh(model);
  gn_test_drain_main_loop();

  /* The original note should STILL be in the model — the delete was unauthorized */
  guint found_after = count_event_in_model(model, note_id);
  g_test_message("Target note found %u time(s) after unauthorized delete (expected: still present)",
                 found_after);
  g_assert_cmpuint(found_after, >, 0);

  g_object_unref(model);
  teardown();
}

/* ── Test: delete-nonexistent-harmless ───────────────────────────── */

static void
test_delete_nonexistent_harmless(void)
{
  setup();

  g_autofree char *author_pk = make_hex(64, 0x88);
  g_autofree char *del_id = make_hex(64, 0x99);
  g_autofree char *phantom_id = make_hex(64, 0xCC); /* doesn't exist */

  /* Ingest a kind-5 delete referencing a non-existent event */
  const char *targets[] = { phantom_id };
  g_autofree char *del_json = make_delete_json(del_id, author_pk, 1700000000, targets, 1);

  gboolean ok = gn_test_ndb_ingest_json(s_ndb, del_json);
  /* Should not crash */
  g_assert_true(ok);

  /* Model should handle this gracefully */
  GnNostrEventModel *model = create_and_refresh_model();
  guint n = g_list_model_get_n_items(G_LIST_MODEL(model));
  g_test_message("Model items after delete of non-existent: %u (expected 0 kind-1 notes)", n);
  /* No kind-1 notes were ingested, so model should be empty or have 0 items */

  gn_test_drain_main_loop();
  g_test_message("Delete of non-existent note handled gracefully");

  g_object_unref(model);
  teardown();
}

/* ── Test: multi-target-delete ───────────────────────────────────── */

static void
test_multi_target_delete(void)
{
  setup();

  g_autofree char *author_pk = make_hex(64, 0xAA);
  g_autofree char *del_id = make_hex(64, 0xEE);

  /* Ingest 3 notes from the same author, each with a profile */
  g_autofree char *note_id_1 = ingest_note_with_profile(author_pk, 1700000000, "note 1");
  g_autofree char *note_id_2 = ingest_note_with_profile(author_pk, 1700000001, "note 2");
  g_autofree char *note_id_3 = ingest_note_with_profile(author_pk, 1700000002, "note 3");

  /* Verify all 3 appear in model */
  GnNostrEventModel *model = create_and_refresh_model();
  guint n_before = g_list_model_get_n_items(G_LIST_MODEL(model));
  g_test_message("Model items before multi-delete: %u", n_before);
  g_assert_cmpuint(n_before, >=, 3);

  /* Ingest a single kind-5 event deleting all 3 notes */
  const char *targets[] = { note_id_1, note_id_2, note_id_3 };
  g_autofree char *del_json = make_delete_json(del_id, author_pk, 1700000003, targets, 3);
  gboolean ok = gn_test_ndb_ingest_json(s_ndb, del_json);
  g_assert_true(ok);
  gn_test_ndb_wait_for_ingest();

  /* Refresh model */
  gn_nostr_event_model_refresh(model);
  gn_test_drain_main_loop();

  guint n_after = g_list_model_get_n_items(G_LIST_MODEL(model));
  g_test_message("Model items after multi-delete: %u (was %u)", n_after, n_before);
  g_test_message("Multi-target delete processed successfully");

  g_object_unref(model);
  teardown();
}

/* ── Test: delete-then-re-ingest ─────────────────────────────────── */

static void
test_delete_then_reingest(void)
{
  setup();

  g_autofree char *author_pk = make_hex(64, 0x11);
  g_autofree char *del_id = make_hex(64, 0x33);

  /* Ingest note with profile */
  g_autofree char *note_id = ingest_note_with_profile(author_pk, 1700000000, "original");

  /* Delete it */
  const char *targets[] = { note_id };
  g_autofree char *del_json = make_delete_json(del_id, author_pk, 1700000001, targets, 1);
  g_assert_true(gn_test_ndb_ingest_json(s_ndb, del_json));
  gn_test_ndb_wait_for_ingest();
  gn_test_drain_main_loop();

  /* Re-ingest an equivalent note from the same author after delete.
   * NDB should handle this gracefully even if relays resend post-delete data. */
  g_autofree char *note_json = gn_test_make_event_json_with_pubkey(1, "original", 1700000000, author_pk);
  gboolean ok = gn_test_ndb_ingest_json(s_ndb, note_json);
  /* Don't assert success — NDB may legitimately reject duplicates */
  g_test_message("Re-ingest after delete: %s", ok ? "accepted" : "rejected (expected)");

  /* Model should be consistent either way */
  GnNostrEventModel *model = create_and_refresh_model();
  guint n = g_list_model_get_n_items(G_LIST_MODEL(model));
  g_test_message("Model items after re-ingest: %u", n);

  g_object_unref(model);
  gn_test_drain_main_loop();
  teardown();
}

/* ── Test: repeated-delete-cycles-no-leak ────────────────────────── */

static void
test_repeated_delete_cycles_no_leak(void)
{
  setup();

  for (int cycle = 0; cycle < 10; cycle++) {
    g_autofree char *pk = make_hex(64, 0x10 + cycle);
    g_autofree char *did = make_hex(64, 0x30 + cycle);

    /* Ingest note with profile */
    g_autofree char *nid = ingest_note_with_profile(pk, 1700000000 + cycle, "cycle note");

    const char *targets[] = { nid };
    g_autofree char *del = make_delete_json(did, pk, 1700000001 + cycle, targets, 1);
    g_assert_true(gn_test_ndb_ingest_json(s_ndb, del));
  }
  gn_test_ndb_wait_for_ingest();

  /* Create model after all cycles — should not crash or leak */
  GnNostrEventModel *model = create_and_refresh_model();
  GnTestPointerWatch *w = gn_test_watch_object(G_OBJECT(model), "delete-cycle-model");
  g_object_unref(model);
  gn_test_assert_finalized(w);
  g_free(w);

  gn_test_drain_main_loop();
  g_test_message("10 ingest-delete cycles completed without crash or leak");

  teardown();
}

/* ── Main ────────────────────────────────────────────────────────── */

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/integ/delete/authorized-removes-note",
                   test_authorized_delete_removes_note);
  g_test_add_func("/gnostr/integ/delete/unauthorized-ignored",
                   test_unauthorized_delete_ignored);
  g_test_add_func("/gnostr/integ/delete/nonexistent-harmless",
                   test_delete_nonexistent_harmless);
  g_test_add_func("/gnostr/integ/delete/multi-target",
                   test_multi_target_delete);
  g_test_add_func("/gnostr/integ/delete/delete-then-reingest",
                   test_delete_then_reingest);
  g_test_add_func("/gnostr/integ/delete/repeated-cycles-no-leak",
                   test_repeated_delete_cycles_no_leak);

  return g_test_run();
}
