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
                          ND_UPSTREAM_MODE_SESSION_RELAY_OR_DIRECT, NULL);
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

/* ---- Scenario: the sync layer actually subscribes (nostrc-wr3t) ---- */

static gchar **
take_sent(NdRelayTransport *t, gsize *n)
{
  return nd_relay_transport_fixture_take_sent(t, n);
}

static void
test_req_on_connect_and_reconnect(void)
{
  Fx fx = {0};
  fx_setup(&fx);

  /* A persisted cursor becomes since = cursor - 1 h overlap. */
  GError *err = NULL;
  g_assert_true(nd_store_db_set_relay_cursor(fx.db, RELAY_URL, 1710007200,
                                             &err));
  g_assert_no_error(err);

  nd_relay_sync_start(fx.sync);
  gsize n = 0;
  g_auto(GStrv) sent = take_sent(fx.transport, &n);
  g_assert_cmpuint(n, ==, 1);
  g_assert_cmpstr(sent[0], ==,
    "[\"REQ\",\"nd-sync\",{\"kinds\":[31922,31923,30085,5],"
    "\"authors\":[\"" AUTHOR_HEX "\"],\"since\":1710003600}]");

  /* EOSE for our sub id is consumed without error. */
  g_assert_true(nd_relay_sync_handle_envelope(fx.sync, RELAY_URL,
                                              "[\"EOSE\",\"nd-sync\"]", &err));
  g_assert_no_error(err);

  /* Drop + reconnect: the REQ is re-sent on the new connection. */
  nd_relay_transport_fixture_set_state(fx.transport, FALSE, NULL);
  nd_relay_transport_fixture_set_state(fx.transport, TRUE, NULL);
  g_auto(GStrv) again = take_sent(fx.transport, &n);
  g_assert_cmpuint(n, ==, 1);
  g_assert_true(g_str_has_prefix(again[0], "[\"REQ\",\"nd-sync\","));

  /* Stop CLOSEs the subscription before disconnecting. */
  nd_relay_sync_stop(fx.sync);
  g_auto(GStrv) closing = take_sent(fx.transport, &n);
  g_assert_cmpuint(n, ==, 1);
  g_assert_cmpstr(closing[0], ==, "[\"CLOSE\",\"nd-sync\"]");

  fx_teardown(&fx);
}

static void
test_no_req_without_pubkey(void)
{
  Fx fx = {0};
  fx_setup(&fx);
  const gchar *relays[] = { RELAY_URL, NULL };
  nd_relay_sync_configure(fx.sync, NULL, (GStrv)relays,
                          ND_UPSTREAM_MODE_DIRECT_ONLY, NULL);
  nd_relay_sync_start(fx.sync);
  gsize n = 0;
  g_auto(GStrv) sent = take_sent(fx.transport, &n);
  g_assert_cmpuint(n, ==, 0);

  /* Configuring the account on a running layer subscribes right away. */
  nd_relay_sync_configure(fx.sync, AUTHOR_HEX, (GStrv)relays,
                          ND_UPSTREAM_MODE_DIRECT_ONLY, NULL);
  g_auto(GStrv) later = take_sent(fx.transport, &n);
  g_assert_cmpuint(n, ==, 1);
  g_assert_true(g_str_has_prefix(later[0], "[\"REQ\",\"nd-sync\","));
  fx_teardown(&fx);
}

static gboolean
iterate_until_sent(NdRelayTransport *t, guint timeout_ms, gchar ***out)
{
  gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
  while (g_get_monotonic_time() < deadline) {
    g_main_context_iteration(NULL, FALSE);
    gsize n = 0;
    gchar **sent = nd_relay_transport_fixture_take_sent(t, &n);
    if (n > 0) {
      *out = sent;
      return TRUE;
    }
    g_strfreev(sent);
    g_usleep(20000);
  }
  return FALSE;
}

static void
test_closed_auth_required_resubscribes_once(void)
{
  Fx fx = {0};
  fx_setup(&fx);
  nd_relay_sync_start(fx.sync);
  g_auto(GStrv) first = take_sent(fx.transport, NULL);

  GError *err = NULL;
  g_assert_true(nd_relay_sync_handle_envelope(fx.sync, RELAY_URL,
    "[\"CLOSED\",\"nd-sync\",\"auth-required: please AUTH\"]", &err));
  g_assert_no_error(err);
  gchar **retry = NULL;
  g_assert_true(iterate_until_sent(fx.transport, 5000, &retry));
  g_assert_true(g_str_has_prefix(retry[0], "[\"REQ\",\"nd-sync\","));
  g_strfreev(retry);

  /* A second auth-required CLOSED on the same connection is not retried
   * (it is reported instead). */
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                        "*closed the sync subscription: auth-required: still no*");
  g_assert_true(nd_relay_sync_handle_envelope(fx.sync, RELAY_URL,
    "[\"CLOSED\",\"nd-sync\",\"auth-required: still no\"]", &err));
  g_test_assert_expected_messages();
  gchar **none = NULL;
  g_assert_false(iterate_until_sent(fx.transport, 2600, &none));
  fx_teardown(&fx);
}

/* ---- Scenario: nostr_dav_upstream_mode is enforced (nostrc-862u) ---- */

#define HOME_URL    "wss://home.test/"
#define SESSION_URL "ws://localhost/"

typedef struct {
  GHashTable *by_url;     /* url -> NdRelayTransport* (owned) */
  GPtrArray  *requested;  /* urls the factory was asked for */
} MultiFactory;

static NdRelayTransport *
multi_factory_new(const gchar *relay_url, gpointer user_data)
{
  MultiFactory *mf = user_data;
  g_ptr_array_add(mf->requested, g_strdup(relay_url));
  NdRelayTransport *t = g_hash_table_lookup(mf->by_url, relay_url);
  if (t == NULL) {
    t = nd_relay_transport_new_fixture(relay_url);
    g_hash_table_insert(mf->by_url, g_strdup(relay_url), t);
  }
  return nd_relay_transport_ref(t);
}

static gboolean
requested(MultiFactory *mf, const gchar *url)
{
  for (guint i = 0; i < mf->requested->len; i++)
    if (g_str_equal(g_ptr_array_index(mf->requested, i), url))
      return TRUE;
  return FALSE;
}

static void
run_upstream_case(NdUpstreamMode mode, const gchar *session_url,
                  gboolean expect_home, gboolean expect_session)
{
  g_autofree gchar *dir = nd_test_make_tmpdir();
  g_autofree gchar *db_path = nd_test_db_path(dir);
  GError *err = NULL;
  NdStoreDb *db = nd_store_db_open(db_path, &err);
  g_assert_no_error(err);
  NdCalendarStore *cal = nd_calendar_store_new(db);
  NdContactStore *contact = nd_contact_store_new(db);
  MultiFactory mf = {
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                          (GDestroyNotify)nd_relay_transport_unref),
    g_ptr_array_new_with_free_func(g_free),
  };
  NdRelaySync *sync = nd_relay_sync_new(db, cal, contact,
                                        multi_factory_new, &mf);
  const gchar *home[] = { HOME_URL, NULL };
  nd_relay_sync_configure(sync, AUTHOR_HEX, (GStrv)home, mode, session_url);
  nd_relay_sync_start(sync);

  g_assert_cmpint(requested(&mf, HOME_URL), ==, expect_home);
  g_assert_cmpint(requested(&mf, SESSION_URL), ==, expect_session);
  if (expect_session) {
    gsize n = 0;
    g_auto(GStrv) s = nd_relay_transport_fixture_take_sent(
      g_hash_table_lookup(mf.by_url, SESSION_URL), &n);
    g_assert_cmpuint(n, ==, 1);
    g_assert_true(g_str_has_prefix(s[0], "[\"REQ\",\"nd-sync\","));
  }

  nd_relay_sync_free(sync);
  g_hash_table_destroy(mf.by_url);
  g_ptr_array_unref(mf.requested);
  nd_calendar_store_free(cal);
  nd_contact_store_free(contact);
  nd_store_db_unref(db);
  nd_test_rm_rf(dir);
}

static void
test_upstream_mode_enforced(void)
{
  /* session_relay_only: never a home relay; nothing at all without one. */
  run_upstream_case(ND_UPSTREAM_MODE_SESSION_RELAY_ONLY, SESSION_URL, FALSE, TRUE);
  run_upstream_case(ND_UPSTREAM_MODE_SESSION_RELAY_ONLY, NULL, FALSE, FALSE);
  /* session_relay_or_direct: session relay when present, else home. */
  run_upstream_case(ND_UPSTREAM_MODE_SESSION_RELAY_OR_DIRECT, SESSION_URL, FALSE, TRUE);
  run_upstream_case(ND_UPSTREAM_MODE_SESSION_RELAY_OR_DIRECT, NULL, TRUE, FALSE);
  /* direct_only: home relays even when a session relay exists. */
  run_upstream_case(ND_UPSTREAM_MODE_DIRECT_ONLY, SESSION_URL, TRUE, FALSE);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-dav/relay-sync/req-on-connect-and-reconnect",
                  test_req_on_connect_and_reconnect);
  g_test_add_func("/nostr-dav/relay-sync/no-req-without-pubkey",
                  test_no_req_without_pubkey);
  g_test_add_func("/nostr-dav/relay-sync/closed-auth-required-resubscribes-once",
                  test_closed_auth_required_resubscribes_once);
  g_test_add_func("/nostr-dav/relay-sync/upstream-mode-enforced",
                  test_upstream_mode_enforced);
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
