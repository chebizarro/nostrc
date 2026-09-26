/* test_relay_sync.c - Relay REQ ingest, fold, dedup, and tombstone
 *
 * SPDX-License-Identifier: MIT
 *
 * Exercises NdRelaySync via the fixture transport. No sockets touched.
 * The scenarios below hit the invariants the plan spells out for
 * Track 2 D4:
 *   - kind 31922/31923 events fold into the calendar store, ETag +
 *     ctag update on every mutation;
 *   - kind 30085 events fold into the contact store;
 *   - kind 5 tombstones remove the addressable row by `e`-tag (event
 *     id) or `a`-tag coordinates;
 *   - duplicate event ids are ignored on replay;
 *   - the relay cursor advances so a restart resumes without
 *     re-processing history.
 */

#include "nd-relay-sync.h"
#include "nd-store-db.h"
#include "nd-calendar-store.h"
#include "nd-contact-store.h"
#include "nd-test-harness.h"

#include <glib.h>

/* Realistic-looking hex64 event ids. Anything of the right length works
 * for the ingest path; a full signature check is out of scope for the
 * fold logic and is validated inside nd-signer/nostr_event_validate. */
#define EVENT_ID_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define EVENT_ID_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define EVENT_ID_C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define AUTHOR_HEX "1111111111111111111111111111111111111111111111111111111111111111"

#define RELAY_URL "wss://relay.test/fixture"

/* ---- Fixture factory ---- */

typedef struct {
  NdRelayTransport *transport;
} FactoryCtx;

static NdRelayTransport *
factory_new(const gchar *relay_url, gpointer user_data)
{
  FactoryCtx *ctx = user_data;
  /* Single-relay tests: return the same transport for every URL. */
  (void)relay_url;
  return ctx->transport ? nd_relay_transport_ref(ctx->transport) : NULL;
}

/* ---- Event builders (unsigned, but well-shaped) ---- */

static gchar *
build_calendar_event(const gchar *event_id,
                     gint         kind,
                     const gchar *uid,
                     gint64       created_at,
                     const gchar *summary)
{
  return g_strdup_printf(
    "{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ","
    "\"kind\":%d,\"content\":\"%s\","
    "\"tags\":[[\"d\",\"%s\"],[\"title\",\"%s\"],[\"start\",\"%" G_GINT64_FORMAT "\"]],"
    "\"sig\":\"deadbeef\"}",
    event_id, AUTHOR_HEX, created_at, kind, summary, uid, summary,
    (gint64)1710000000);
}

static gchar *
build_contact_event(const gchar *event_id,
                    const gchar *uid,
                    gint64       created_at,
                    const gchar *fn)
{
  return g_strdup_printf(
    "{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ","
    "\"kind\":30085,\"content\":\"BEGIN:VCARD\\nVERSION:4.0\\nFN:%s\\nUID:%s\\nEND:VCARD\","
    "\"tags\":[[\"d\",\"%s\"],[\"name\",\"%s\"]],\"sig\":\"deadbeef\"}",
    event_id, AUTHOR_HEX, created_at, fn, uid, uid, fn);
}

static gchar *
build_deletion_e(const gchar *event_id, const gchar *target_event_id)
{
  return g_strdup_printf(
    "{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":1710000100,"
    "\"kind\":5,\"content\":\"\","
    "\"tags\":[[\"e\",\"%s\"]],\"sig\":\"deadbeef\"}",
    event_id, AUTHOR_HEX, target_event_id);
}

static gchar *
build_deletion_a(const gchar *event_id, gint kind, const gchar *d_tag)
{
  return g_strdup_printf(
    "{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":1710000200,"
    "\"kind\":5,\"content\":\"\","
    "\"tags\":[[\"a\",\"%d:%s:%s\"]],\"sig\":\"deadbeef\"}",
    event_id, AUTHOR_HEX, kind, AUTHOR_HEX, d_tag);
}

static void
deliver_event(NdRelayTransport *t, const gchar *event_object_json)
{
  g_autofree gchar *envelope =
    g_strdup_printf("[\"EVENT\",\"sub-1\",%s]", event_object_json);
  nd_relay_transport_fixture_deliver_frame(t, "EVENT", envelope);
}

/* ---- Scenario: calendar + contact fold, dedup ---- */

typedef struct {
  gchar          *tmpdir;
  NdStoreDb      *db;
  NdCalendarStore *cal;
  NdContactStore  *contact;
  NdRelayTransport *transport;
  NdRelaySync     *sync;
  FactoryCtx      factory_ctx;
} Fx;

static void
fx_setup(Fx *fx)
{
  fx->tmpdir = nd_test_make_tmpdir();
  g_autofree gchar *db_path = nd_test_db_path(fx->tmpdir);

  GError *err = NULL;
  fx->db = nd_store_db_open(db_path, &err);
  g_assert_no_error(err);

  fx->cal     = nd_calendar_store_new(fx->db);
  fx->contact = nd_contact_store_new(fx->db);

  fx->transport = nd_relay_transport_new_fixture(RELAY_URL);
  fx->factory_ctx.transport = fx->transport;

  fx->sync = nd_relay_sync_new(fx->db, fx->cal, fx->contact,
                               factory_new, &fx->factory_ctx);

  const gchar *relays[] = { RELAY_URL, NULL };
  nd_relay_sync_configure(fx->sync, AUTHOR_HEX, (GStrv)relays,
                          ND_UPSTREAM_MODE_SESSION_RELAY_OR_DIRECT);
}

static void
fx_teardown(Fx *fx)
{
  nd_relay_sync_stop(fx->sync);
  nd_relay_sync_free(fx->sync);
  nd_calendar_store_free(fx->cal);
  nd_contact_store_free(fx->contact);
  nd_store_db_unref(fx->db);
  nd_relay_transport_unref(fx->transport);
  nd_test_rm_rf(fx->tmpdir);
  g_free(fx->tmpdir);
}

static void
test_ingest_calendar_and_contact(void)
{
  Fx fx = {0};
  fx_setup(&fx);
  nd_relay_sync_start(fx.sync);

  g_autofree gchar *cal_ev =
    build_calendar_event(EVENT_ID_A, 31923, "evt-1", 1710000010, "First");
  deliver_event(fx.transport, cal_ev);

  g_autofree gchar *contact_ev =
    build_contact_event(EVENT_ID_B, "person-1", 1710000020, "Alice");
  deliver_event(fx.transport, contact_ev);

  GError *err = NULL;
  guint cal_count = 0;
  g_assert_true(nd_calendar_store_count(fx.cal, &cal_count, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(cal_count, ==, 1);

  guint contact_count = 0;
  g_assert_true(nd_contact_store_count(fx.contact, &contact_count, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(contact_count, ==, 1);

  /* Cursor advanced past the newest created_at we saw. */
  gint64 since = -1;
  g_assert_true(nd_store_db_get_relay_cursor(fx.db, RELAY_URL, &since, &err));
  g_assert_no_error(err);
  g_assert_cmpint(since, ==, 1710000020);

  fx_teardown(&fx);
}

static void
test_dedup_by_event_id(void)
{
  Fx fx = {0};
  fx_setup(&fx);
  nd_relay_sync_start(fx.sync);

  g_autofree gchar *first =
    build_calendar_event(EVENT_ID_A, 31923, "evt-dup", 1710000010, "First");
  deliver_event(fx.transport, first);

  /* Replay the exact same event id twice more. */
  deliver_event(fx.transport, first);
  deliver_event(fx.transport, first);

  GError *err = NULL;
  guint count = 0;
  g_assert_true(nd_calendar_store_count(fx.cal, &count, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(count, ==, 1);

  fx_teardown(&fx);
}

static void
test_last_writer_wins_by_created_at(void)
{
  Fx fx = {0};
  fx_setup(&fx);
  nd_relay_sync_start(fx.sync);

  g_autofree gchar *v1 =
    build_calendar_event(EVENT_ID_A, 31923, "evt-lww", 1710000010, "Old");
  deliver_event(fx.transport, v1);

  g_autofree gchar *v2 =
    build_calendar_event(EVENT_ID_B, 31923, "evt-lww", 1710000020, "New");
  deliver_event(fx.transport, v2);

  /* And an older-created_at update on a THIRD event id — must NOT win. */
  g_autofree gchar *v3 =
    build_calendar_event(EVENT_ID_C, 31923, "evt-lww", 1710000005, "Older");
  deliver_event(fx.transport, v3);

  GError *err = NULL;
  NdCalendarEvent *evt = nd_calendar_store_get(fx.cal, "evt-lww", &err);
  g_assert_no_error(err);
  g_assert_nonnull(evt);
  g_assert_cmpstr(evt->summary, ==, "New");
  nd_calendar_event_free(evt);

  fx_teardown(&fx);
}

static void
test_tombstone_by_event_id(void)
{
  Fx fx = {0};
  fx_setup(&fx);
  nd_relay_sync_start(fx.sync);

  g_autofree gchar *cal =
    build_calendar_event(EVENT_ID_A, 31923, "evt-tomb", 1710000010, "Doomed");
  deliver_event(fx.transport, cal);

  g_autofree gchar *tomb = build_deletion_e(EVENT_ID_B, EVENT_ID_A);
  deliver_event(fx.transport, tomb);

  GError *err = NULL;
  guint count = 0;
  g_assert_true(nd_calendar_store_count(fx.cal, &count, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(count, ==, 0);

  fx_teardown(&fx);
}

static void
test_tombstone_by_a_tag(void)
{
  Fx fx = {0};
  fx_setup(&fx);
  nd_relay_sync_start(fx.sync);

  g_autofree gchar *contact =
    build_contact_event(EVENT_ID_A, "contact-tomb", 1710000010, "Bob");
  deliver_event(fx.transport, contact);

  g_autofree gchar *tomb = build_deletion_a(EVENT_ID_B, 30085, "contact-tomb");
  deliver_event(fx.transport, tomb);

  GError *err = NULL;
  guint count = 0;
  g_assert_true(nd_contact_store_count(fx.contact, &count, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(count, ==, 0);

  fx_teardown(&fx);
}

/* Foreign deletion (different pubkey) must NOT delete our rows. */
static void
test_tombstone_from_stranger_ignored(void)
{
  Fx fx = {0};
  fx_setup(&fx);
  nd_relay_sync_start(fx.sync);

  g_autofree gchar *cal =
    build_calendar_event(EVENT_ID_A, 31923, "evt-safe", 1710000010, "Keep");
  deliver_event(fx.transport, cal);

  /* Delivery with a stranger pubkey. */
  g_autofree gchar *stranger =
    g_strdup_printf(
      "{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":1710000030,"
      "\"kind\":5,\"content\":\"\","
      "\"tags\":[[\"e\",\"%s\"]],\"sig\":\"deadbeef\"}",
      EVENT_ID_C,
      "2222222222222222222222222222222222222222222222222222222222222222",
      EVENT_ID_A);
  deliver_event(fx.transport, stranger);

  GError *err = NULL;
  guint count = 0;
  g_assert_true(nd_calendar_store_count(fx.cal, &count, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(count, ==, 1);

  fx_teardown(&fx);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-dav/relay-sync/ingest",
                  test_ingest_calendar_and_contact);
  g_test_add_func("/nostr-dav/relay-sync/dedup",
                  test_dedup_by_event_id);
  g_test_add_func("/nostr-dav/relay-sync/last-writer-wins",
                  test_last_writer_wins_by_created_at);
  g_test_add_func("/nostr-dav/relay-sync/tombstone-e-tag",
                  test_tombstone_by_event_id);
  g_test_add_func("/nostr-dav/relay-sync/tombstone-a-tag",
                  test_tombstone_by_a_tag);
  g_test_add_func("/nostr-dav/relay-sync/tombstone-stranger",
                  test_tombstone_from_stranger_ignored);
  return g_test_run();
}
