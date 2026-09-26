/* test_publish_rollback.c - Outbox commit + failure classes
 *
 * SPDX-License-Identifier: MIT
 *
 * Named for continuity with the plan's earlier draft; the actual
 * behaviour is offline-first + failed_permanent on permanent reject
 * (plan Track 2 D5 revised). Scenarios:
 *
 *   1. Mock signer approves; 3 relays, 2 accept + 1 permanently
 *      rejects (`invalid:` reason) -> row transitions to
 *      `failed_permanent`, notification fires exactly once.
 *   2. 2 accept + 1 transient reject (`error:` reason) -> row stays
 *      `pending`, publish_next_ts pushed into the future, publish_
 *      attempts incremented. A subsequent retry completes -> row
 *      lands in `published`.
 *   3. Notification dedup: retrying a failed_permanent row does NOT
 *      fire a second GNotification.
 */

#include "nd-publisher.h"
#include "nd-signer.h"
#include "nd-relay-transport.h"
#include "nd-store-db.h"
#include "nd-calendar-store.h"
#include "nd-contact-store.h"
#include "nd-ical.h"
#include "nd-test-harness.h"

#include <sqlite3.h>
#include <json-glib/json-glib.h>
#include <glib.h>
#include <string.h>

#define ACCOUNT_PUBKEY "1111111111111111111111111111111111111111111111111111111111111111"

/* ---- Mock signer ---- */

typedef struct {
  guint    call_count;
  gboolean deny_next;
  gchar   *fixed_id;   /* returned as the signed event's id */
} MockSigner;

static gchar *
mock_sign(gpointer user_data, const gchar *unsigned_json,
          GCancellable *cancellable, GError **error)
{
  (void)cancellable;
  MockSigner *m = user_data;
  m->call_count++;

  if (m->deny_next) {
    m->deny_next = FALSE;
    g_set_error_literal(error, ND_SIGNER_ERROR, ND_SIGNER_ERROR_DENIED,
                        "user denied");
    return NULL;
  }

  /* Stamp id + pubkey + sig onto the unsigned payload. json-glib makes
   * "parse, add three fields, re-serialise" a five-liner. */
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, unsigned_json, -1, error))
    return NULL;
  JsonNode *root = json_parser_get_root(parser);
  JsonObject *obj = json_node_get_object(root);
  json_object_set_string_member(obj, "id", m->fixed_id);
  json_object_set_string_member(obj, "pubkey", ACCOUNT_PUBKEY);
  json_object_set_string_member(obj, "sig",
    "1111222233334444555566667777888899990000aaaabbbbccccddddeeeeffff"
    "1111222233334444555566667777888899990000aaaabbbbccccddddeeeeffff");

  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  return json_generator_to_data(gen, NULL);
}

static void
mock_free(gpointer user_data)
{
  MockSigner *m = user_data;
  g_free(m->fixed_id);
  g_free(m);
}

static NdSigner *
make_mock_signer(const gchar *fixed_id, MockSigner **out)
{
  MockSigner *m = g_new0(MockSigner, 1);
  m->fixed_id = g_strdup(fixed_id);
  NdSignerVTable vt = {
    .sign_event_json   = mock_sign,
    .user_data_destroy = mock_free,
  };
  *out = m;
  return nd_signer_new_from_vtable(&vt, m);
}

/* ---- Notification recorder ---- */

typedef struct {
  guint  count;
  gchar *last_row_id;
  gchar *last_reason;
} NotifyRec;

static void
notify_cb(NdPublisher *self, NdPublisherNotifyKind kind,
          NdStoreCollection collection, const gchar *row_id,
          const gchar *reason, gpointer user_data)
{
  (void)self;
  (void)kind;
  (void)collection;
  NotifyRec *rec = user_data;
  rec->count++;
  g_free(rec->last_row_id);
  g_free(rec->last_reason);
  rec->last_row_id = g_strdup(row_id);
  rec->last_reason = g_strdup(reason ? reason : "");
}

static void
notify_rec_clear(NotifyRec *rec)
{
  rec->count = 0;
  g_clear_pointer(&rec->last_row_id, g_free);
  g_clear_pointer(&rec->last_reason, g_free);
}

/* ---- Test fixture ---- */

typedef struct {
  gchar             *tmpdir;
  NdStoreDb         *db;
  NdCalendarStore   *cal;
  NdSigner          *signer;
  MockSigner        *signer_state;
  NdRelayTransport  *r1;
  NdRelayTransport  *r2;
  NdRelayTransport  *r3;
  NdPublisher       *publisher;
  NotifyRec          notify;
} Fx;

static void
insert_calendar_row(Fx *fx, const gchar *uid, const gchar *summary)
{
  NdCalendarEvent ev = {0};
  ev.uid          = (gchar *)uid;
  ev.summary      = (gchar *)summary;
  ev.description  = (gchar *)"";
  ev.kind         = ND_NIP52_KIND_TIME;
  ev.is_date_only = FALSE;
  ev.dtstart_ts   = 1710000000;
  ev.dtend_ts     = 1710000100;
  ev.pubkey       = (gchar *)ACCOUNT_PUBKEY;
  ev.created_at   = 1710000000;

  GError *err = NULL;
  gboolean created = FALSE;
  g_assert_true(nd_calendar_store_put(fx->cal, &ev, &created, &err));
  g_assert_no_error(err);
  g_assert_true(created);
}

static void
fx_setup(Fx *fx, const gchar *fixed_id)
{
  fx->tmpdir = nd_test_make_tmpdir();
  g_autofree gchar *db_path = nd_test_db_path(fx->tmpdir);
  GError *err = NULL;
  fx->db = nd_store_db_open(db_path, &err);
  g_assert_no_error(err);

  fx->cal = nd_calendar_store_new(fx->db);

  fx->signer = make_mock_signer(fixed_id, &fx->signer_state);

  fx->r1 = nd_relay_transport_new_fixture("wss://r1.test");
  fx->r2 = nd_relay_transport_new_fixture("wss://r2.test");
  fx->r3 = nd_relay_transport_new_fixture("wss://r3.test");
  nd_relay_transport_connect_async(fx->r1);
  nd_relay_transport_connect_async(fx->r2);
  nd_relay_transport_connect_async(fx->r3);

  fx->publisher = nd_publisher_new(fx->db, fx->signer, NULL, NULL);
  nd_publisher_bind_transport(fx->publisher, "wss://r1.test", fx->r1);
  nd_publisher_bind_transport(fx->publisher, "wss://r2.test", fx->r2);
  nd_publisher_bind_transport(fx->publisher, "wss://r3.test", fx->r3);

  const gchar *urls[] = { "wss://r1.test", "wss://r2.test", "wss://r3.test",
                          NULL };
  nd_publisher_configure(fx->publisher, ACCOUNT_PUBKEY, (GStrv)urls,
                         ND_PUBLISH_QUORUM_DEFAULT);
  nd_publisher_set_notify_callback(fx->publisher, notify_cb, &fx->notify);
}

static void
fx_teardown(Fx *fx)
{
  nd_publisher_free(fx->publisher);
  nd_signer_unref(fx->signer);
  nd_relay_transport_unref(fx->r1);
  nd_relay_transport_unref(fx->r2);
  nd_relay_transport_unref(fx->r3);
  nd_calendar_store_free(fx->cal);
  nd_store_db_unref(fx->db);
  nd_test_rm_rf(fx->tmpdir);
  g_free(fx->tmpdir);
  notify_rec_clear(&fx->notify);
}

/* Reads publish_state for the calendar row keyed by @uid. */
static gchar *
get_publish_state(NdStoreDb *db, const gchar *uid)
{
  sqlite3 *h = nd_store_db_get_handle(db);
  sqlite3_stmt *stmt = NULL;
  g_assert_true(sqlite3_prepare_v2(h,
                  "SELECT publish_state FROM events WHERE uid = ?1",
                  -1, &stmt, NULL) == SQLITE_OK);
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_TRANSIENT);
  gchar *state = NULL;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    state = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return state;
}

/* Extracts the event id from the last EVENT frame that fx->r1 sent. */
static gchar *
sent_event_id(NdRelayTransport *t)
{
  gsize n = 0;
  gchar **frames = nd_relay_transport_fixture_take_sent(t, &n);
  g_assert_cmpuint(n, >, 0);
  /* First frame from the publisher is always ["EVENT",{...}]. */
  const gchar *f = frames[0];
  /* Not JSON-parsing here — pick the "id":"..." with a quick scan. */
  const gchar *needle = strstr(f, "\"id\":\"");
  g_assert_nonnull(needle);
  needle += strlen("\"id\":\"");
  const gchar *end = strchr(needle, '"');
  g_assert_nonnull(end);
  gchar *id = g_strndup(needle, end - needle);
  g_strfreev(frames);
  return id;
}

/* ---- Scenario: permanent reject fires exactly one notification ---- */

static void
test_permanent_reject_notifies_once(void)
{
  Fx fx = {0};
  fx_setup(&fx,
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

  insert_calendar_row(&fx, "evt-perm", "Fatal");
  GError *err = NULL;
  g_assert_true(nd_publisher_stage_calendar_put(fx.publisher, "evt-perm",
                                                &err));
  g_assert_no_error(err);

  /* Tick sends the EVENT frame to all three relays. */
  gint64 now = 2000000000;
  g_assert_true(nd_publisher_tick(fx.publisher, now));

  g_autofree gchar *event_id = sent_event_id(fx.r1);
  /* r2 + r3 fixtures also have queued frames; discard. */
  gchar **f2 = nd_relay_transport_fixture_take_sent(fx.r2, NULL);
  g_strfreev(f2);
  gchar **f3 = nd_relay_transport_fixture_take_sent(fx.r3, NULL);
  g_strfreev(f3);

  /* Two OKs then one permanent reject. */
  nd_publisher_record_ok(fx.publisher, "wss://r1.test", event_id, TRUE, "");
  nd_publisher_record_ok(fx.publisher, "wss://r2.test", event_id, TRUE, "");
  nd_publisher_record_ok(fx.publisher, "wss://r3.test", event_id, FALSE,
                         "invalid: banned author");

  g_autofree gchar *state = get_publish_state(fx.db, "evt-perm");
  g_assert_cmpstr(state, ==, "failed_permanent");
  g_assert_cmpuint(fx.notify.count, ==, 1);
  g_assert_cmpstr(fx.notify.last_row_id, ==, "evt-perm");

  /* A retry (later tick) MUST NOT fire another notification because the
   * DB row is already `failed_permanent`; nd_publisher_tick's SELECT
   * only picks up `pending`. */
  now += 3600;
  nd_publisher_tick(fx.publisher, now);
  g_assert_cmpuint(fx.notify.count, ==, 1);

  fx_teardown(&fx);
}

/* ---- Scenario: transient failure retries into a clean quorum ---- */

static void
test_transient_then_success(void)
{
  Fx fx = {0};
  fx_setup(&fx,
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

  insert_calendar_row(&fx, "evt-txn", "Retryable");
  GError *err = NULL;
  g_assert_true(nd_publisher_stage_calendar_put(fx.publisher, "evt-txn",
                                                &err));
  g_assert_no_error(err);

  gint64 now = 2000000000;
  g_assert_true(nd_publisher_tick(fx.publisher, now));

  g_autofree gchar *event_id = sent_event_id(fx.r1);
  gchar **f2 = nd_relay_transport_fixture_take_sent(fx.r2, NULL);
  g_strfreev(f2);
  gchar **f3 = nd_relay_transport_fixture_take_sent(fx.r3, NULL);
  g_strfreev(f3);

  /* Two OKs; r3 fails transiently ("error:" prefix). Quorum-all not
   * met -> row stays pending, gets rescheduled. */
  nd_publisher_record_ok(fx.publisher, "wss://r1.test", event_id, TRUE, "");
  nd_publisher_record_ok(fx.publisher, "wss://r2.test", event_id, TRUE, "");
  nd_publisher_record_ok(fx.publisher, "wss://r3.test", event_id, FALSE,
                         "error: temporarily overloaded");

  g_autofree gchar *state = get_publish_state(fx.db, "evt-txn");
  g_assert_cmpstr(state, ==, "pending");
  g_assert_cmpuint(fx.notify.count, ==, 0);

  /* Advance past the retry deadline and tick again. */
  now += 3600;
  g_assert_true(nd_publisher_tick(fx.publisher, now));
  /* The signer was called once for the first attempt; the retry re-uses
   * the stored signed_event_json so it does NOT hit the signer again. */
  g_assert_cmpuint(fx.signer_state->call_count, ==, 1);

  gchar **f1b = nd_relay_transport_fixture_take_sent(fx.r1, NULL);
  g_strfreev(f1b);
  gchar **f2b = nd_relay_transport_fixture_take_sent(fx.r2, NULL);
  g_strfreev(f2b);
  gchar **f3b = nd_relay_transport_fixture_take_sent(fx.r3, NULL);
  g_strfreev(f3b);

  nd_publisher_record_ok(fx.publisher, "wss://r1.test", event_id, TRUE, "");
  nd_publisher_record_ok(fx.publisher, "wss://r2.test", event_id, TRUE, "");
  nd_publisher_record_ok(fx.publisher, "wss://r3.test", event_id, TRUE, "");

  g_autofree gchar *state2 = get_publish_state(fx.db, "evt-txn");
  g_assert_cmpstr(state2, ==, "published");

  fx_teardown(&fx);
}

/* ---- Scenario: signer denial marks row permanent, no relay work ---- */

static void
test_signer_denial(void)
{
  Fx fx = {0};
  fx_setup(&fx,
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");

  insert_calendar_row(&fx, "evt-sign", "Denied");
  GError *err = NULL;
  g_assert_true(nd_publisher_stage_calendar_put(fx.publisher, "evt-sign",
                                                &err));
  g_assert_no_error(err);

  fx.signer_state->deny_next = TRUE;

  nd_publisher_tick(fx.publisher, 2000000000);

  g_autofree gchar *state = get_publish_state(fx.db, "evt-sign");
  g_assert_cmpstr(state, ==, "failed_permanent");
  g_assert_cmpuint(fx.notify.count, ==, 1);

  /* No EVENT frames should have gone out. */
  gsize sent_r1 = 0;
  gchar **f1 = nd_relay_transport_fixture_take_sent(fx.r1, &sent_r1);
  g_strfreev(f1);
  g_assert_cmpuint(sent_r1, ==, 0);

  fx_teardown(&fx);
}

/* ---- Scenario: DAV DELETE stages a kind-5 tombstone (nostrc-ls2c) ----
 *
 * A calendar row is staged and published (all three relays OK), then a
 * tombstone is staged directly against the publisher (the DAV server
 * calls the same API from its DELETE handlers). The next tick MUST:
 *   - build a NIP-09 kind-5 event with the `a` tag pointing at the
 *     addressable pointer (kind:pubkey:uid) of the deleted event,
 *   - emit an EVENT frame to every home relay,
 *   - transition the tombstone row to `published` once every relay
 *     ACKs. */

static void
assert_frame_is_kind_5_tombstone(const gchar *frame,
                                 int          expected_kind,
                                 const gchar *expected_pubkey,
                                 const gchar *expected_uid,
                                 gchar      **out_event_id)
{
  g_assert_nonnull(frame);
  g_assert_true(g_str_has_prefix(frame, "[\"EVENT\","));
  /* Skip the ['EVENT', prefix and trailing ]. */
  gsize len = strlen(frame);
  g_assert_cmpuint(len, >, strlen("[\"EVENT\",]"));
  g_autofree gchar *event_json =
    g_strndup(frame + strlen("[\"EVENT\","),
              len - strlen("[\"EVENT\",") - 1);

  g_autoptr(JsonParser) parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(parser, event_json, -1, NULL));
  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
  g_assert_true(json_object_has_member(obj, "kind"));
  g_assert_cmpint(json_object_get_int_member(obj, "kind"), ==, 5);

  JsonArray *tags = json_object_get_array_member(obj, "tags");
  g_assert_nonnull(tags);
  g_assert_cmpuint(json_array_get_length(tags), >=, 1);
  JsonArray *a_tag = json_array_get_array_element(tags, 0);
  g_assert_nonnull(a_tag);
  g_assert_cmpuint(json_array_get_length(a_tag), ==, 2);
  g_assert_cmpstr(json_array_get_string_element(a_tag, 0), ==, "a");
  g_autofree gchar *want_a =
    g_strdup_printf("%d:%s:%s", expected_kind, expected_pubkey, expected_uid);
  g_assert_cmpstr(json_array_get_string_element(a_tag, 1), ==, want_a);

  if (out_event_id != NULL)
    *out_event_id = g_strdup(json_object_get_string_member(obj, "id"));
}

static gchar *
get_tombstone_state(NdStoreDb *db)
{
  sqlite3 *h = nd_store_db_get_handle(db);
  sqlite3_stmt *stmt = NULL;
  g_assert_true(sqlite3_prepare_v2(h,
                  "SELECT publish_state FROM tombstones ORDER BY id DESC LIMIT 1",
                  -1, &stmt, NULL) == SQLITE_OK);
  gchar *state = NULL;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    state = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return state;
}

static void
test_dav_delete_stages_kind5_tombstone(void)
{
  Fx fx = {0};
  fx_setup(&fx,
    "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");

  const gchar *uid = "evt-to-delete";
  insert_calendar_row(&fx, uid, "To be deleted");
  GError *err = NULL;
  g_assert_true(nd_publisher_stage_calendar_put(fx.publisher, uid, &err));
  g_assert_no_error(err);

  /* First tick sends the primary event. Drain its frames and complete
   * the publish so the outbox is clean before we stage the tombstone. */
  gint64 now = 2000000000;
  g_assert_true(nd_publisher_tick(fx.publisher, now));
  g_autofree gchar *event_id = sent_event_id(fx.r1);
  gchar **f2 = nd_relay_transport_fixture_take_sent(fx.r2, NULL); g_strfreev(f2);
  gchar **f3 = nd_relay_transport_fixture_take_sent(fx.r3, NULL); g_strfreev(f3);
  nd_publisher_record_ok(fx.publisher, "wss://r1.test", event_id, TRUE, "");
  nd_publisher_record_ok(fx.publisher, "wss://r2.test", event_id, TRUE, "");
  nd_publisher_record_ok(fx.publisher, "wss://r3.test", event_id, TRUE, "");
  g_autofree gchar *state = get_publish_state(fx.db, uid);
  g_assert_cmpstr(state, ==, "published");

  /* Stage the tombstone — exactly what the DAV DELETE handler will do. */
  g_assert_true(nd_publisher_stage_tombstone(fx.publisher, ND_NIP52_KIND_TIME,
                                             ACCOUNT_PUBKEY, uid, &err));
  g_assert_no_error(err);

  now += 3600;
  g_assert_true(nd_publisher_tick(fx.publisher, now));

  /* The tick should have signed, framed, and shipped a kind-5 event to
   * every home relay. Verify the shape of the frame on r1 (r2 + r3 get
   * the same payload; drain them so the fixture bookkeeping resets). */
  gsize n1 = 0;
  gchar **tomb1 = nd_relay_transport_fixture_take_sent(fx.r1, &n1);
  g_assert_cmpuint(n1, ==, 1);
  g_autofree gchar *tomb_event_id = NULL;
  assert_frame_is_kind_5_tombstone(tomb1[0], ND_NIP52_KIND_TIME,
                                   ACCOUNT_PUBKEY, uid, &tomb_event_id);
  g_strfreev(tomb1);
  gchar **tomb2 = nd_relay_transport_fixture_take_sent(fx.r2, NULL);
  g_strfreev(tomb2);
  gchar **tomb3 = nd_relay_transport_fixture_take_sent(fx.r3, NULL);
  g_strfreev(tomb3);

  /* Every relay ACKs the tombstone → row transitions to `published`. */
  nd_publisher_record_ok(fx.publisher, "wss://r1.test", tomb_event_id, TRUE, "");
  nd_publisher_record_ok(fx.publisher, "wss://r2.test", tomb_event_id, TRUE, "");
  nd_publisher_record_ok(fx.publisher, "wss://r3.test", tomb_event_id, TRUE, "");

  g_autofree gchar *tstate = get_tombstone_state(fx.db);
  g_assert_cmpstr(tstate, ==, "published");

  fx_teardown(&fx);
}

/* ---- Scenario: silent relay times out and the row retries ---- */

static void
test_ok_wait_timeout(void)
{
  Fx fx = {0};
  fx_setup(&fx,
    "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");

  insert_calendar_row(&fx, "evt-silent", "Deaf relays");
  GError *err = NULL;
  g_assert_true(nd_publisher_stage_calendar_put(fx.publisher, "evt-silent",
                                                &err));
  g_assert_no_error(err);

  gint64 now = 2000000000;
  g_assert_true(nd_publisher_tick(fx.publisher, now));

  gchar **f1 = nd_relay_transport_fixture_take_sent(fx.r1, NULL);
  g_strfreev(f1);
  gchar **f2 = nd_relay_transport_fixture_take_sent(fx.r2, NULL);
  g_strfreev(f2);
  gchar **f3 = nd_relay_transport_fixture_take_sent(fx.r3, NULL);
  g_strfreev(f3);

  /* No OKs arrive. Advance past the OK-wait deadline (120 s) and tick. */
  now += 200;
  nd_publisher_tick(fx.publisher, now);

  g_autofree gchar *state = get_publish_state(fx.db, "evt-silent");
  g_assert_cmpstr(state, ==, "pending");
  g_assert_cmpuint(fx.notify.count, ==, 0);

  /* publish_attempts advanced so the next backoff is longer. */
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare_v2(nd_store_db_get_handle(fx.db),
                     "SELECT publish_attempts FROM events WHERE uid = ?1",
                     -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, "evt-silent", -1, SQLITE_TRANSIENT);
  g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
  gint attempts = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  g_assert_cmpint(attempts, >, 0);

  fx_teardown(&fx);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-dav/publish/permanent-reject-notifies-once",
                  test_permanent_reject_notifies_once);
  g_test_add_func("/nostr-dav/publish/transient-then-success",
                  test_transient_then_success);
  g_test_add_func("/nostr-dav/publish/signer-denial",
                  test_signer_denial);
  g_test_add_func("/nostr-dav/publish/ok-wait-timeout",
                  test_ok_wait_timeout);
  g_test_add_func("/nostr-dav/publish/dav-delete-stages-kind5-tombstone",
                  test_dav_delete_stages_kind5_tombstone);
  return g_test_run();
}
