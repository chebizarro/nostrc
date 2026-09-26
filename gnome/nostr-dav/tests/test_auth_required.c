/* test_auth_required.c - nostr-dav fails closed on auth and bind policy
 *
 * SPDX-License-Identifier: MIT
 *
 * Plan Track 2 §2.7: with no account configured the server refuses to
 * start; a wrong token gets 401; OPTIONS and the .well-known redirects
 * stay unauthenticated but expose nothing mutable. Also covers the
 * token file contract (0600, O_EXCL mint, refuse on bad permissions),
 * the loopback-only bind, and the upstream-mode config gate.
 */

#include "nd-test-harness.h"
#include "nd-config.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SAMPLE_ICS \
  "BEGIN:VCALENDAR\r\n" \
  "VERSION:2.0\r\n" \
  "PRODID:-//Test//Test//EN\r\n" \
  "BEGIN:VEVENT\r\n" \
  "UID:auth-event\r\n" \
  "SUMMARY:Should not be stored\r\n" \
  "DTSTART;VALUE=DATE:20260420\r\n" \
  "DTEND;VALUE=DATE:20260421\r\n" \
  "END:VEVENT\r\n" \
  "END:VCALENDAR\r\n"

/* ---- Helpers ---- */

static guint
send_status(SoupSession *session, SoupMessage *msg)
{
  GError *err = NULL;
  g_autoptr(GBytes) body = soup_session_send_and_read(session, msg, NULL, &err);
  g_assert_no_error(err);
  return soup_message_get_status(msg);
}

static SoupMessage *
new_propfind(const gchar *url)
{
  SoupMessage *msg = soup_message_new("PROPFIND", url);
  soup_message_headers_replace(soup_message_get_request_headers(msg),
                               "Depth", "1");
  return msg;
}

static void
write_file_with_mode(const gchar *path, const gchar *contents, int mode)
{
  GError *err = NULL;
  g_assert_true(g_file_set_contents(path, contents, -1, &err));
  g_assert_no_error(err);
  g_assert_cmpint(g_chmod(path, mode), ==, 0);
}

/* ---- Server refuses to start when unconfigured ---- */

static void
test_start_refused_without_account(void)
{
  g_autofree gchar *dir = nd_test_make_tmpdir();
  g_autofree gchar *config_dir = nd_test_config_dir(dir);
  g_autofree gchar *db_path = nd_test_db_path(dir);
  GError *err = NULL;

  NdTokenStore *tokens = nd_token_store_new(config_dir, FALSE);
  g_autofree gchar *token = nd_token_store_ensure_token(tokens, "default", &err);
  g_assert_no_error(err);
  g_autoptr(NdStoreDb) db = nd_store_db_open(db_path, &err);
  g_assert_no_error(err);

  NdDavServer *server = nd_dav_server_new(tokens, db);
  /* No nd_dav_server_set_account_id(). */
  g_assert_false(nd_dav_server_start(server, "127.0.0.1", 0, &err));
  g_assert_error(err, ND_DAV_SERVER_ERROR, ND_DAV_SERVER_ERROR_NOT_CONFIGURED);
  g_clear_error(&err);
  g_assert_false(nd_dav_server_is_running(server));
  g_assert_cmpuint(nd_dav_server_get_port(server), ==, 0);

  g_object_unref(server);
  nd_token_store_free(tokens);
  g_clear_pointer(&db, nd_store_db_unref);
  nd_test_rm_rf(dir);
}

static void
test_start_refused_without_token(void)
{
  g_autofree gchar *dir = nd_test_make_tmpdir();
  g_autofree gchar *config_dir = nd_test_config_dir(dir);
  g_autofree gchar *db_path = nd_test_db_path(dir);
  GError *err = NULL;

  /* Token store never ensured: account set, but no token loaded. */
  NdTokenStore *tokens = nd_token_store_new(config_dir, FALSE);
  g_autoptr(NdStoreDb) db = nd_store_db_open(db_path, &err);
  g_assert_no_error(err);

  NdDavServer *server = nd_dav_server_new(tokens, db);
  nd_dav_server_set_account_id(server, "default");
  g_assert_false(nd_dav_server_start(server, "127.0.0.1", 0, &err));
  g_assert_error(err, ND_DAV_SERVER_ERROR, ND_DAV_SERVER_ERROR_NOT_CONFIGURED);
  g_clear_error(&err);
  g_assert_false(nd_dav_server_is_running(server));

  g_object_unref(server);
  nd_token_store_free(tokens);
  g_clear_pointer(&db, nd_store_db_unref);
  nd_test_rm_rf(dir);
}

static void
test_start_refused_off_loopback(void)
{
  g_autofree gchar *dir = nd_test_make_tmpdir();
  g_autofree gchar *config_dir = nd_test_config_dir(dir);
  g_autofree gchar *db_path = nd_test_db_path(dir);
  GError *err = NULL;

  NdTokenStore *tokens = nd_token_store_new(config_dir, FALSE);
  g_autofree gchar *token = nd_token_store_ensure_token(tokens, "default", &err);
  g_assert_no_error(err);
  g_autoptr(NdStoreDb) db = nd_store_db_open(db_path, &err);
  g_assert_no_error(err);

  NdDavServer *server = nd_dav_server_new(tokens, db);
  nd_dav_server_set_account_id(server, "default");

  static const gchar *const non_loopback[] = { "0.0.0.0", "::", "192.0.2.10" };
  for (gsize i = 0; i < G_N_ELEMENTS(non_loopback); i++) {
    g_assert_false(nd_dav_server_start(server, non_loopback[i], 0, &err));
    g_assert_error(err, ND_DAV_SERVER_ERROR, ND_DAV_SERVER_ERROR_NOT_LOOPBACK);
    g_clear_error(&err);
    g_assert_false(nd_dav_server_is_running(server));
  }

  g_object_unref(server);
  nd_token_store_free(tokens);
  g_clear_pointer(&db, nd_store_db_unref);
  nd_test_rm_rf(dir);
}

/* ---- HTTP auth on a running server ---- */

static void
test_no_credentials_401(void)
{
  NdTestServer *ts = nd_test_server_start(NULL);
  g_autoptr(SoupSession) anon = soup_session_new();

  static const gchar *const paths[] = {
    "/", "/calendars/", "/calendars/nostr/", "/contacts/nostr/",
    "/files/nostr/", "/principals/me/",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(paths); i++) {
    g_autofree gchar *url = g_strconcat(ts->base_url, paths[i], NULL);
    g_autoptr(SoupMessage) msg = new_propfind(url);
    g_assert_cmpuint(send_status(anon, msg), ==, 401);
    const gchar *challenge = soup_message_headers_get_one(
      soup_message_get_response_headers(msg), "WWW-Authenticate");
    g_assert_nonnull(challenge);
    g_assert_true(g_str_has_prefix(challenge, "Basic"));
  }

  nd_test_server_stop(ts);
}

static void
test_wrong_token_401(void)
{
  NdTestServer *ts = nd_test_server_start(NULL);
  g_autofree gchar *url = g_strconcat(ts->base_url, "/calendars/nostr/", NULL);

  g_autofree gchar *truncated = g_strndup(ts->token, strlen(ts->token) - 1);
  g_autofree gchar *extended = g_strconcat(ts->token, "x", NULL);
  g_autofree gchar *flipped = g_strdup(ts->token);
  flipped[0] = (flipped[0] == 'A') ? 'B' : 'A';

  const gchar *wrong[] = { "", "not-the-token", truncated, extended, flipped };
  for (gsize i = 0; i < G_N_ELEMENTS(wrong); i++) {
    g_autoptr(SoupMessage) msg = new_propfind(url);
    g_autofree gchar *auth = nd_test_basic_auth("nostr", wrong[i]);
    soup_message_headers_replace(soup_message_get_request_headers(msg),
                                 "Authorization", auth);
    g_assert_cmpuint(send_status(ts->session, msg), ==, 401);
  }

  /* Malformed Authorization headers. */
  const gchar *malformed[] = { "Bearer x", "Basic", "Basic !!!", "Basic Zm9v" };
  for (gsize i = 0; i < G_N_ELEMENTS(malformed); i++) {
    g_autoptr(SoupMessage) msg = new_propfind(url);
    soup_message_headers_replace(soup_message_get_request_headers(msg),
                                 "Authorization", malformed[i]);
    g_assert_cmpuint(send_status(ts->session, msg), ==, 401);
  }

  /* Username is ignored; the right token authenticates. */
  g_autoptr(SoupMessage) ok = new_propfind(url);
  g_autofree gchar *auth = nd_test_basic_auth("anyone", ts->token);
  soup_message_headers_replace(soup_message_get_request_headers(ok),
                               "Authorization", auth);
  g_assert_cmpuint(send_status(ts->session, ok), ==, 207);

  nd_test_server_stop(ts);
}

static void
test_unauthenticated_write_rejected(void)
{
  NdTestServer *ts = nd_test_server_start(NULL);
  g_autoptr(SoupSession) anon = soup_session_new();
  g_autofree gchar *url =
    g_strconcat(ts->base_url, "/calendars/nostr/auth-event.ics", NULL);

  g_autoptr(SoupMessage) put = soup_message_new("PUT", url);
  g_autoptr(GBytes) body = g_bytes_new_static(SAMPLE_ICS, strlen(SAMPLE_ICS));
  soup_message_set_request_body_from_bytes(put, "text/calendar", body);
  g_assert_cmpuint(send_status(anon, put), ==, 401);

  g_autoptr(SoupMessage) del = soup_message_new("DELETE", url);
  g_assert_cmpuint(send_status(anon, del), ==, 401);

  /* Nothing was written. */
  g_autoptr(SoupMessage) get = soup_message_new("GET", url);
  g_assert_cmpuint(send_status(ts->session, get), ==, 404);

  nd_test_server_stop(ts);
}

static void
test_options_and_wellknown_unauthenticated(void)
{
  NdTestServer *ts = nd_test_server_start(NULL);
  g_autoptr(SoupSession) anon = soup_session_new();

  g_autofree gchar *root = g_strconcat(ts->base_url, "/", NULL);
  g_autoptr(SoupMessage) opts = soup_message_new("OPTIONS", root);
  GError *err = NULL;
  g_autoptr(GBytes) opts_body =
    soup_session_send_and_read(anon, opts, NULL, &err);
  g_assert_no_error(err);
  g_assert_cmpuint(soup_message_get_status(opts), ==, 200);
  g_assert_cmpuint(g_bytes_get_size(opts_body), ==, 0);

  static const gchar *const wellknown[] = {
    "/.well-known/caldav", "/.well-known/carddav",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(wellknown); i++) {
    g_autofree gchar *url = g_strconcat(ts->base_url, wellknown[i], NULL);
    g_autoptr(SoupMessage) msg = soup_message_new("GET", url);
    soup_message_set_flags(msg, SOUP_MESSAGE_NO_REDIRECT);
    g_assert_cmpuint(send_status(anon, msg), ==, 301);

    /* The redirect target itself still requires auth. */
    const gchar *location = soup_message_headers_get_one(
      soup_message_get_response_headers(msg), "Location");
    g_assert_nonnull(location);
    g_autofree gchar *target = g_strconcat(ts->base_url, location, NULL);
    g_autoptr(SoupMessage) follow = new_propfind(target);
    g_assert_cmpuint(send_status(anon, follow), ==, 401);
  }

  nd_test_server_stop(ts);
}

/* ---- Token file contract ---- */

static void
test_token_minted_0600_and_stable(void)
{
  g_autofree gchar *dir = nd_test_make_tmpdir();
  g_autofree gchar *config_dir = nd_test_config_dir(dir);
  GError *err = NULL;

  NdTokenStore *a = nd_token_store_new(config_dir, FALSE);
  g_assert_false(nd_token_store_validate(a, "default", "anything"));

  g_autofree gchar *t1 = nd_token_store_ensure_token(a, "default", &err);
  g_assert_no_error(err);
  g_assert_cmpuint(strlen(t1), >=, 43);   /* 256 bits, base64url */

  struct stat st;
  g_assert_cmpint(g_stat(nd_token_store_get_path(a), &st), ==, 0);
  g_assert_true(S_ISREG(st.st_mode));
  g_assert_cmpint(st.st_mode & 0777, ==, 0600);
  g_assert_cmpint(g_stat(config_dir, &st), ==, 0);
  g_assert_cmpint(st.st_mode & 0077, ==, 0);

  g_assert_true(nd_token_store_validate(a, "default", t1));
  g_assert_false(nd_token_store_validate(a, "other-account", t1));

  /* A second account on the same store is refused (v1: single account). */
  g_autofree gchar *other = nd_token_store_ensure_token(a, "other", &err);
  g_assert_null(other);
  g_assert_error(err, ND_TOKEN_STORE_ERROR, ND_TOKEN_STORE_ERROR_ACCOUNT);
  g_clear_error(&err);

  /* A fresh store ("daemon restart") reads the same token back. */
  NdTokenStore *b = nd_token_store_new(config_dir, FALSE);
  g_autofree gchar *t2 = nd_token_store_ensure_token(b, "default", &err);
  g_assert_no_error(err);
  g_assert_cmpstr(t1, ==, t2);

  nd_token_store_free(a);
  nd_token_store_free(b);
  nd_test_rm_rf(dir);
}

static void
assert_token_refused(const gchar *config_dir, gint expected_code)
{
  GError *err = NULL;
  NdTokenStore *store = nd_token_store_new(config_dir, FALSE);
  g_autofree gchar *token = nd_token_store_ensure_token(store, "default", &err);
  g_assert_null(token);
  g_assert_error(err, ND_TOKEN_STORE_ERROR, expected_code);
  g_clear_error(&err);
  g_assert_false(nd_token_store_has_token(store, "default"));
  nd_token_store_free(store);
}

static void
test_token_refuses_bad_permissions(void)
{
  g_autofree gchar *dir = nd_test_make_tmpdir();
  g_autofree gchar *config_dir = nd_test_config_dir(dir);
  g_assert_cmpint(g_mkdir_with_parents(config_dir, 0700), ==, 0);
  g_autofree gchar *path = g_build_filename(config_dir, "token", NULL);
  const gchar *valid = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ\n";

  /* World-/group-readable token file: refuse, do not chmod. */
  write_file_with_mode(path, valid, 0644);
  assert_token_refused(config_dir, ND_TOKEN_STORE_ERROR_PERMISSIONS);
  struct stat st;
  g_assert_cmpint(g_stat(path, &st), ==, 0);
  g_assert_cmpint(st.st_mode & 0777, ==, 0644);

  write_file_with_mode(path, valid, 0640);
  assert_token_refused(config_dir, ND_TOKEN_STORE_ERROR_PERMISSIONS);

  /* Correct mode but group-writable directory. */
  g_assert_cmpint(g_chmod(path, 0600), ==, 0);
  g_assert_cmpint(g_chmod(config_dir, 0770), ==, 0);
  assert_token_refused(config_dir, ND_TOKEN_STORE_ERROR_PERMISSIONS);
  g_assert_cmpint(g_chmod(config_dir, 0700), ==, 0);

  /* Symlinked token file. */
  g_autofree gchar *real = g_build_filename(dir, "elsewhere", NULL);
  write_file_with_mode(real, valid, 0600);
  g_assert_cmpint(g_unlink(path), ==, 0);
  g_assert_cmpint(symlink(real, path), ==, 0);
  assert_token_refused(config_dir, ND_TOKEN_STORE_ERROR_PERMISSIONS);
  g_assert_cmpint(g_unlink(path), ==, 0);

  /* Malformed content. */
  write_file_with_mode(path, "short\n", 0600);
  assert_token_refused(config_dir, ND_TOKEN_STORE_ERROR_INVALID);

  /* Sanity: the same valid content with 0600 loads. */
  write_file_with_mode(path, valid, 0600);
  GError *err = NULL;
  NdTokenStore *store = nd_token_store_new(config_dir, FALSE);
  g_autofree gchar *token = nd_token_store_ensure_token(store, "default", &err);
  g_assert_no_error(err);
  g_assert_cmpstr(token, ==, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ");
  nd_token_store_free(store);

  nd_test_rm_rf(dir);
}

/* ---- Upstream-mode config ---- */

static void
test_config_upstream_mode(void)
{
  g_autofree gchar *dir = nd_test_make_tmpdir();
  g_autofree gchar *path = g_build_filename(dir, "nostr-dav.conf", NULL);
  NdConfig cfg;
  GError *err = NULL;

  /* Missing file → default. */
  g_assert_true(nd_config_load(path, &cfg, &err));
  g_assert_no_error(err);
  g_assert_cmpint(cfg.upstream_mode, ==, ND_UPSTREAM_MODE_SESSION_RELAY_OR_DIRECT);

  static const struct { const gchar *value; NdUpstreamMode mode; } cases[] = {
    { "session_relay_only",      ND_UPSTREAM_MODE_SESSION_RELAY_ONLY },
    { "session_relay_or_direct", ND_UPSTREAM_MODE_SESSION_RELAY_OR_DIRECT },
    { "direct_only",             ND_UPSTREAM_MODE_DIRECT_ONLY },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autofree gchar *contents = g_strdup_printf(
      "[nostr-dav]\nnostr_dav_upstream_mode=%s\n", cases[i].value);
    write_file_with_mode(path, contents, 0600);
    g_assert_true(nd_config_load(path, &cfg, &err));
    g_assert_no_error(err);
    g_assert_cmpint(cfg.upstream_mode, ==, cases[i].mode);
    g_assert_cmpstr(nd_upstream_mode_to_string(cfg.upstream_mode), ==,
                    cases[i].value);
  }

  /* A typo must not silently widen a privacy setting. */
  write_file_with_mode(path,
                       "[nostr-dav]\nnostr_dav_upstream_mode=session_only\n",
                       0600);
  g_assert_false(nd_config_load(path, &cfg, &err));
  g_assert_error(err, ND_CONFIG_ERROR, ND_CONFIG_ERROR_INVALID_VALUE);
  g_clear_error(&err);

  nd_test_rm_rf(dir);
}

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/auth/start-refused-without-account",
                  test_start_refused_without_account);
  g_test_add_func("/auth/start-refused-without-token",
                  test_start_refused_without_token);
  g_test_add_func("/auth/start-refused-off-loopback",
                  test_start_refused_off_loopback);
  g_test_add_func("/auth/no-credentials-401", test_no_credentials_401);
  g_test_add_func("/auth/wrong-token-401", test_wrong_token_401);
  g_test_add_func("/auth/unauthenticated-write-rejected",
                  test_unauthenticated_write_rejected);
  g_test_add_func("/auth/options-and-wellknown-unauthenticated",
                  test_options_and_wellknown_unauthenticated);
  g_test_add_func("/token/minted-0600-and-stable",
                  test_token_minted_0600_and_stable);
  g_test_add_func("/token/refuses-bad-permissions",
                  test_token_refuses_bad_permissions);
  g_test_add_func("/config/upstream-mode", test_config_upstream_mode);

  return g_test_run();
}
