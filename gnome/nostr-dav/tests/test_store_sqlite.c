/* test_store_sqlite.c - nostr-dav SQLite persistence
 *
 * SPDX-License-Identifier: MIT
 *
 * Plan Track 2 §2.7: put → daemon restart → get returns the row; ctag
 * is stable across a restart and strictly increases with every
 * mutation. Also pins the v1 schema (publish outbox columns +
 * publish_log), file permissions, publish-state preservation on
 * upsert, and corruption quarantine.
 */

#include "nd-test-harness.h"
#include "nd-calendar-store.h"
#include "nd-contact-store.h"
#include "nd-file-store.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <sqlite3.h>
#include <string.h>
#include <sys/stat.h>

#define SAMPLE_ICS \
  "BEGIN:VCALENDAR\r\n" \
  "VERSION:2.0\r\n" \
  "PRODID:-//Test//Test//EN\r\n" \
  "BEGIN:VEVENT\r\n" \
  "UID:persist-event\r\n" \
  "SUMMARY:Survives restart\r\n" \
  "DESCRIPTION:Stored in SQLite\r\n" \
  "DTSTART;VALUE=DATE:20260420\r\n" \
  "DTEND;VALUE=DATE:20260421\r\n" \
  "LOCATION:Berlin\r\n" \
  "END:VEVENT\r\n" \
  "END:VCALENDAR\r\n"

#define SAMPLE_VCARD \
  "BEGIN:VCARD\r\n" \
  "VERSION:4.0\r\n" \
  "UID:persist-contact\r\n" \
  "FN:Alice Nakamoto\r\n" \
  "N:Nakamoto;Alice;;;\r\n" \
  "EMAIL;TYPE=work:alice@example.com\r\n" \
  "END:VCARD\r\n"

/* ---- Helpers ---- */

static NdStoreDb *
open_db(const gchar *dir)
{
  g_autofree gchar *path = nd_test_db_path(dir);
  GError *err = NULL;
  NdStoreDb *db = nd_store_db_open(path, &err);
  g_assert_no_error(err);
  g_assert_nonnull(db);
  return db;
}

static gint64
ctag_value(const gchar *ctag)
{
  g_assert_nonnull(ctag);
  gchar *end = NULL;
  gint64 v = g_ascii_strtoll(ctag, &end, 10);
  g_assert_true(end != ctag && *end == '\0');
  return v;
}

static gchar *
query_text(NdStoreDb *db, const gchar *sql)
{
  sqlite3 *h = nd_store_db_get_handle(db);
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_prepare_v2(h, sql, -1, &stmt, NULL), ==, SQLITE_OK);
  gchar *out = NULL;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    out = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return out;
}

static void
exec_sql(NdStoreDb *db, const gchar *sql)
{
  char *errmsg = NULL;
  int rc = sqlite3_exec(nd_store_db_get_handle(db), sql, NULL, NULL, &errmsg);
  if (rc != SQLITE_OK)
    g_error("%s: %s", sql, errmsg);
}

static guint
send_status(NdTestServer *ts, SoupMessage *msg, GBytes **out_body)
{
  GError *err = NULL;
  GBytes *body = soup_session_send_and_read(ts->session, msg, NULL, &err);
  g_assert_no_error(err);
  if (out_body)
    *out_body = body;
  else
    g_bytes_unref(body);
  return soup_message_get_status(msg);
}

static guint
http_put(NdTestServer *ts, const gchar *path, const gchar *type,
         const gchar *payload)
{
  g_autofree gchar *url = g_strconcat(ts->base_url, path, NULL);
  g_autoptr(SoupMessage) msg = soup_message_new("PUT", url);
  g_autoptr(GBytes) body = g_bytes_new(payload, strlen(payload));
  soup_message_set_request_body_from_bytes(msg, type, body);
  return send_status(ts, msg, NULL);
}

/* GET @path; returns status, fills *out_etag / *out_body when non-NULL. */
static guint
http_get(NdTestServer *ts, const gchar *path, gchar **out_etag,
         gchar **out_body)
{
  g_autofree gchar *url = g_strconcat(ts->base_url, path, NULL);
  g_autoptr(SoupMessage) msg = soup_message_new("GET", url);
  g_autoptr(GBytes) body = NULL;
  guint status = send_status(ts, msg, &body);
  if (out_etag)
    *out_etag = g_strdup(soup_message_headers_get_one(
      soup_message_get_response_headers(msg), "ETag"));
  if (out_body) {
    gsize len = 0;
    const gchar *data = g_bytes_get_data(body, &len);
    *out_body = g_strndup(data, len);
  }
  return status;
}

/* PROPFIND depth 1 on @collection and extract its getctag value. */
static gint64
http_ctag(NdTestServer *ts, const gchar *collection)
{
  g_autofree gchar *url = g_strconcat(ts->base_url, collection, NULL);
  g_autoptr(SoupMessage) msg = soup_message_new("PROPFIND", url);
  soup_message_headers_replace(soup_message_get_request_headers(msg),
                               "Depth", "1");
  g_autoptr(GBytes) body = NULL;
  g_assert_cmpuint(send_status(ts, msg, &body), ==, 207);

  gsize len = 0;
  const gchar *data = g_bytes_get_data(body, &len);
  g_autofree gchar *xml = g_strndup(data, len);
  const gchar *tag = strstr(xml, "getctag>");
  g_assert_nonnull(tag);
  tag += strlen("getctag>");
  g_autofree gchar *value = g_strndup(tag, strcspn(tag, "<"));
  return ctag_value(value);
}

/* ---- Store-level persistence ---- */

static void
test_store_put_reopen_get(void)
{
  g_autofree gchar *dir = nd_test_make_tmpdir();
  GError *err = NULL;
  g_autofree gchar *etag_before = NULL;
  g_autofree gchar *vcard_etag_before = NULL;
  gint64 cal_ctag = 0, card_ctag = 0, file_ctag = 0;

  {
    g_autoptr(NdStoreDb) db = open_db(dir);
    NdCalendarStore *cal = nd_calendar_store_new(db);
    NdContactStore *cards = nd_contact_store_new(db);
    NdFileStore *files = nd_file_store_new(db);

    NdCalendarEvent *ev = nd_ical_parse_vevent(SAMPLE_ICS, &err);
    g_assert_no_error(err);
    ev->pubkey = g_strdup("ab12");
    ev->created_at = 1700000000;
    gboolean created = FALSE;
    g_assert_true(nd_calendar_store_put(cal, ev, &created, &err));
    g_assert_no_error(err);
    g_assert_true(created);
    etag_before = nd_ical_compute_etag(ev);
    nd_calendar_event_free(ev);

    NdContact *c = nd_vcard_parse(SAMPLE_VCARD, &err);
    g_assert_no_error(err);
    c->npub = g_strdup("npub1example");
    c->created_at = 1700000001;
    g_assert_true(nd_contact_store_put(cards, c, &created, &err));
    g_assert_no_error(err);
    g_assert_true(created);
    vcard_etag_before = nd_vcard_compute_etag(c);
    nd_contact_free(c);

    g_autoptr(GBytes) bytes = g_bytes_new_static("hello\0world", 11);
    NdFileEntry *fe = nd_file_entry_new("notes/bin.dat", bytes,
                                        "application/octet-stream");
    fe->modified_at = 1700000002;
    g_assert_true(nd_file_store_put(files, fe, &created, &err));
    g_assert_no_error(err);
    g_assert_true(created);
    nd_file_entry_free(fe);

    g_autofree gchar *c1 = nd_calendar_store_get_ctag(cal, &err);
    g_autofree gchar *c2 = nd_contact_store_get_ctag(cards, &err);
    g_autofree gchar *c3 = nd_file_store_get_ctag(files, &err);
    g_assert_no_error(err);
    cal_ctag = ctag_value(c1);
    card_ctag = ctag_value(c2);
    file_ctag = ctag_value(c3);
    g_assert_cmpint(cal_ctag, ==, 1);

    nd_calendar_store_free(cal);
    nd_contact_store_free(cards);
    nd_file_store_free(files);
  }

  /* Reopen: rows, etags, and ctags all survive. */
  {
    g_autoptr(NdStoreDb) db = open_db(dir);
    NdCalendarStore *cal = nd_calendar_store_new(db);
    NdContactStore *cards = nd_contact_store_new(db);
    NdFileStore *files = nd_file_store_new(db);

    NdCalendarEvent *ev = nd_calendar_store_get(cal, "persist-event", &err);
    g_assert_no_error(err);
    g_assert_nonnull(ev);
    g_assert_cmpstr(ev->summary, ==, "Survives restart");
    g_assert_cmpstr(ev->location, ==, "Berlin");
    g_assert_cmpstr(ev->pubkey, ==, "ab12");
    g_assert_cmpint(ev->created_at, ==, 1700000000);
    g_autofree gchar *etag_after = nd_ical_compute_etag(ev);
    g_assert_cmpstr(etag_after, ==, etag_before);
    nd_calendar_event_free(ev);

    NdContact *c = nd_contact_store_get(cards, "persist-contact", &err);
    g_assert_no_error(err);
    g_assert_nonnull(c);
    g_assert_cmpstr(c->fn, ==, "Alice Nakamoto");
    g_assert_cmpstr(c->npub, ==, "npub1example");
    g_autofree gchar *vcard_etag_after = nd_vcard_compute_etag(c);
    g_assert_cmpstr(vcard_etag_after, ==, vcard_etag_before);
    nd_contact_free(c);

    NdFileEntry *fe = nd_file_store_get(files, "notes/bin.dat", &err);
    g_assert_no_error(err);
    g_assert_nonnull(fe);
    g_assert_cmpuint(fe->size, ==, 11);
    g_assert_cmpmem(g_bytes_get_data(fe->content, NULL), 11, "hello\0world", 11);
    g_assert_cmpint(fe->modified_at, ==, 1700000002);
    nd_file_entry_free(fe);

    g_assert_null(nd_calendar_store_get(cal, "missing", &err));
    g_assert_no_error(err);

    g_autofree gchar *c1 = nd_calendar_store_get_ctag(cal, &err);
    g_autofree gchar *c2 = nd_contact_store_get_ctag(cards, &err);
    g_autofree gchar *c3 = nd_file_store_get_ctag(files, &err);
    g_assert_cmpint(ctag_value(c1), ==, cal_ctag);
    g_assert_cmpint(ctag_value(c2), ==, card_ctag);
    g_assert_cmpint(ctag_value(c3), ==, file_ctag);

    /* Removing a missing row is not a mutation; removing a real one is. */
    gboolean removed = TRUE;
    g_assert_true(nd_calendar_store_remove(cal, "missing", &removed, &err));
    g_assert_false(removed);
    g_autofree gchar *c1b = nd_calendar_store_get_ctag(cal, &err);
    g_assert_cmpint(ctag_value(c1b), ==, cal_ctag);

    g_assert_true(nd_calendar_store_remove(cal, "persist-event", &removed, &err));
    g_assert_true(removed);
    g_autofree gchar *c1c = nd_calendar_store_get_ctag(cal, &err);
    g_assert_cmpint(ctag_value(c1c), >, cal_ctag);

    guint count = 99;
    g_assert_true(nd_calendar_store_count(cal, &count, &err));
    g_assert_cmpuint(count, ==, 0);

    nd_calendar_store_free(cal);
    nd_contact_store_free(cards);
    nd_file_store_free(files);
  }

  nd_test_rm_rf(dir);
}

/* ---- Daemon-level restart over HTTP ---- */

static void
test_daemon_restart_http(void)
{
  g_autofree gchar *dir = nd_test_make_tmpdir();

  NdTestServer *ts = nd_test_server_start(dir);
  g_autofree gchar *token_before = g_strdup(ts->token);
  gint64 ctag0 = http_ctag(ts, "/calendars/nostr/");
  g_assert_cmpuint(http_put(ts, "/calendars/nostr/persist-event.ics",
                            "text/calendar", SAMPLE_ICS), ==, 201);
  g_assert_cmpuint(http_put(ts, "/contacts/nostr/persist-contact.vcf",
                            "text/vcard", SAMPLE_VCARD), ==, 201);
  gint64 ctag1 = http_ctag(ts, "/calendars/nostr/");
  g_assert_cmpint(ctag1, >, ctag0);

  g_autofree gchar *etag1 = NULL;
  g_assert_cmpuint(http_get(ts, "/calendars/nostr/persist-event.ics",
                            &etag1, NULL), ==, 200);
  nd_test_server_stop(ts);

  /* Restart against the same directory. */
  ts = nd_test_server_start(dir);
  g_assert_cmpstr(ts->token, ==, token_before);

  g_autofree gchar *etag2 = NULL;
  g_autofree gchar *ics = NULL;
  g_assert_cmpuint(http_get(ts, "/calendars/nostr/persist-event.ics",
                            &etag2, &ics), ==, 200);
  g_assert_cmpstr(etag2, ==, etag1);
  g_assert_nonnull(strstr(ics, "SUMMARY:Survives restart"));

  g_autofree gchar *vcard = NULL;
  g_assert_cmpuint(http_get(ts, "/contacts/nostr/persist-contact.vcf",
                            NULL, &vcard), ==, 200);
  g_assert_nonnull(strstr(vcard, "FN:Alice Nakamoto"));

  /* ctag is stable across restart, then strictly increases. */
  gint64 ctag2 = http_ctag(ts, "/calendars/nostr/");
  g_assert_cmpint(ctag2, ==, ctag1);

  g_assert_cmpuint(http_put(ts, "/calendars/nostr/persist-event.ics",
                            "text/calendar", SAMPLE_ICS), ==, 204);
  gint64 ctag3 = http_ctag(ts, "/calendars/nostr/");
  g_assert_cmpint(ctag3, >, ctag2);

  g_autofree gchar *del_url =
    g_strconcat(ts->base_url, "/calendars/nostr/persist-event.ics", NULL);
  g_autoptr(SoupMessage) del = soup_message_new("DELETE", del_url);
  g_assert_cmpuint(send_status(ts, del, NULL), ==, 204);
  gint64 ctag4 = http_ctag(ts, "/calendars/nostr/");
  g_assert_cmpint(ctag4, >, ctag3);
  nd_test_server_stop(ts);

  /* And the deletion persisted too. */
  ts = nd_test_server_start(dir);
  g_assert_cmpuint(http_get(ts, "/calendars/nostr/persist-event.ics",
                            NULL, NULL), ==, 404);
  g_assert_cmpint(http_ctag(ts, "/calendars/nostr/"), ==, ctag4);
  nd_test_server_stop(ts);

  nd_test_rm_rf(dir);
}

/* ---- Schema, permissions, outbox preservation ---- */

static void
test_schema_and_permissions(void)
{
  g_autofree gchar *dir = nd_test_make_tmpdir();
  g_autofree gchar *path = nd_test_db_path(dir);
  g_autoptr(NdStoreDb) db = open_db(dir);

  struct stat st;
  g_assert_cmpint(g_stat(path, &st), ==, 0);
  g_assert_cmpint(st.st_mode & 0077, ==, 0);
  g_autofree gchar *parent = g_path_get_dirname(path);
  g_assert_cmpint(g_stat(parent, &st), ==, 0);
  g_assert_cmpint(st.st_mode & 0077, ==, 0);

  g_autofree gchar *version = query_text(db, "PRAGMA user_version");
  g_assert_cmpstr(version, ==, "1");
  g_autofree gchar *mode = query_text(db, "PRAGMA journal_mode");
  g_assert_cmpstr(mode, ==, "wal");

  /* Publish outbox columns exist on every content table. */
  static const gchar *const outbox_selects[] = {
    "SELECT publish_state, publish_attempts, publish_next_ts, "
    "signed_event_json, nostr_event_id FROM events LIMIT 0",
    "SELECT publish_state, publish_attempts, publish_next_ts, "
    "signed_event_json, nostr_event_id FROM contacts LIMIT 0",
    "SELECT publish_state, publish_attempts, publish_next_ts, "
    "signed_event_json, nostr_event_id FROM files LIMIT 0",
    "SELECT id, target_kind, target_uid, relay_url, attempted_at, "
    "http_status, error FROM publish_log LIMIT 0",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(outbox_selects); i++) {
    sqlite3_stmt *stmt = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(nd_store_db_get_handle(db),
                                       outbox_selects[i], -1, &stmt, NULL),
                    ==, SQLITE_OK);
    sqlite3_finalize(stmt);
  }

  /* New rows start idle; an upsert must not clobber publish state. */
  GError *err = NULL;
  NdCalendarStore *cal = nd_calendar_store_new(db);
  NdCalendarEvent *ev = nd_ical_parse_vevent(SAMPLE_ICS, &err);
  g_assert_no_error(err);
  g_assert_true(nd_calendar_store_put(cal, ev, NULL, &err));
  g_autofree gchar *state0 = query_text(db,
    "SELECT publish_state FROM events WHERE uid = 'persist-event'");
  g_assert_cmpstr(state0, ==, "idle");

  exec_sql(db, "UPDATE events SET publish_state = 'pending', "
               "publish_attempts = 2, signed_event_json = '{}' "
               "WHERE uid = 'persist-event'");
  g_free(ev->summary);
  ev->summary = g_strdup("Edited");
  gboolean created = TRUE;
  g_assert_true(nd_calendar_store_put(cal, ev, &created, &err));
  g_assert_false(created);
  g_autofree gchar *state1 = query_text(db,
    "SELECT publish_state || ':' || publish_attempts || ':' || "
    "signed_event_json FROM events WHERE uid = 'persist-event'");
  g_assert_cmpstr(state1, ==, "pending:2:{}");

  /* The schema rejects unknown publish states. */
  char *errmsg = NULL;
  g_assert_cmpint(sqlite3_exec(nd_store_db_get_handle(db),
                               "UPDATE events SET publish_state = 'bogus'",
                               NULL, NULL, &errmsg), !=, SQLITE_OK);
  sqlite3_free(errmsg);

  nd_calendar_event_free(ev);
  nd_calendar_store_free(cal);
  g_clear_pointer(&db, nd_store_db_unref);
  nd_test_rm_rf(dir);
}

static void
test_newer_schema_refused(void)
{
  g_autofree gchar *dir = nd_test_make_tmpdir();
  g_autofree gchar *path = nd_test_db_path(dir);
  {
    g_autoptr(NdStoreDb) db = open_db(dir);
    exec_sql(db, "PRAGMA user_version = 99");
  }

  GError *err = NULL;
  NdStoreDb *db = nd_store_db_open(path, &err);
  g_assert_null(db);
  g_assert_error(err, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_SCHEMA);
  g_clear_error(&err);
  nd_test_rm_rf(dir);
}

static void
test_corrupt_store_quarantined(void)
{
  g_autofree gchar *dir = nd_test_make_tmpdir();
  g_autofree gchar *path = nd_test_db_path(dir);
  g_autofree gchar *parent = g_path_get_dirname(path);
  g_assert_cmpint(g_mkdir_with_parents(parent, 0700), ==, 0);

  GError *err = NULL;
  g_assert_true(g_file_set_contents(path,
    "this is definitely not an sqlite database, just some bytes that are "
    "long enough to fill a header and then some more padding here", -1, &err));
  g_assert_cmpint(g_chmod(path, 0600), ==, 0);

  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                        "*Integrity check failed*");
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*moved to*");
  g_autoptr(NdStoreDb) db = nd_store_db_open(path, &err);
  g_test_assert_expected_messages();
  g_assert_no_error(err);
  g_assert_nonnull(db);

  /* Fresh, usable, and the corrupt file was kept aside. */
  g_autofree gchar *version = query_text(db, "PRAGMA user_version");
  g_assert_cmpstr(version, ==, "1");

  gboolean found = FALSE;
  GDir *d = g_dir_open(parent, 0, NULL);
  const gchar *name;
  while ((name = g_dir_read_name(d)) != NULL)
    if (g_str_has_prefix(name, "store.sqlite.corrupt-"))
      found = TRUE;
  g_dir_close(d);
  g_assert_true(found);

  g_clear_pointer(&db, nd_store_db_unref);
  nd_test_rm_rf(dir);
}

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/store/put-reopen-get", test_store_put_reopen_get);
  g_test_add_func("/store/daemon-restart-http", test_daemon_restart_http);
  g_test_add_func("/store/schema-and-permissions", test_schema_and_permissions);
  g_test_add_func("/store/newer-schema-refused", test_newer_schema_refused);
  g_test_add_func("/store/corrupt-quarantined", test_corrupt_store_quarantined);

  return g_test_run();
}
