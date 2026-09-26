/* nd-test-harness.c - Spawn a real NdDavServer for tests
 *
 * SPDX-License-Identifier: MIT
 */

#include "nd-test-harness.h"

#include <glib/gstdio.h>
#include <string.h>

gchar *
nd_test_make_tmpdir(void)
{
  GError *err = NULL;
  gchar *dir = g_dir_make_tmp("nostr-dav-test-XXXXXX", &err);
  g_assert_no_error(err);
  g_assert_nonnull(dir);
  return dir;
}

void
nd_test_rm_rf(const gchar *dir)
{
  GDir *d = g_dir_open(dir, 0, NULL);
  if (d != NULL) {
    const gchar *name;
    while ((name = g_dir_read_name(d)) != NULL) {
      g_autofree gchar *child = g_build_filename(dir, name, NULL);
      if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
          !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
        nd_test_rm_rf(child);
      else
        g_unlink(child);
    }
    g_dir_close(d);
  }
  g_rmdir(dir);
}

gchar *
nd_test_config_dir(const gchar *dir)
{
  return g_build_filename(dir, "config", "nostr-dav", NULL);
}

gchar *
nd_test_db_path(const gchar *dir)
{
  return g_build_filename(dir, "data", "nostr-dav", "store.sqlite", NULL);
}

gchar *
nd_test_basic_auth(const gchar *user, const gchar *password)
{
  g_autofree gchar *creds = g_strdup_printf("%s:%s", user, password);
  g_autofree gchar *b64 = g_base64_encode((const guchar *)creds, strlen(creds));
  return g_strdup_printf("Basic %s", b64);
}

static void
on_request_queued(SoupSession *session, SoupMessage *msg, gpointer user_data)
{
  (void)session;
  NdTestServer *ts = user_data;
  SoupMessageHeaders *hdrs = soup_message_get_request_headers(msg);
  if (soup_message_headers_get_one(hdrs, "Authorization") == NULL) {
    g_autofree gchar *auth = nd_test_basic_auth("nostr", ts->token);
    soup_message_headers_replace(hdrs, "Authorization", auth);
  }
}

/* Runs inside the server loop, so a quit issued after readiness can
 * never be lost to a not-yet-running loop. */
static gboolean
signal_ready(gpointer data)
{
  NdTestServer *ts = data;
  g_mutex_lock(&ts->mutex);
  ts->ready = TRUE;
  g_cond_signal(&ts->cond);
  g_mutex_unlock(&ts->mutex);
  return G_SOURCE_REMOVE;
}

static gpointer
server_thread_func(gpointer data)
{
  NdTestServer *ts = data;

  /* Create everything on this thread so the thread-default context is
   * the one libsoup attaches its sources to. */
  GMainContext *ctx = g_main_context_new();
  g_main_context_push_thread_default(ctx);

  GError *err = NULL;
  g_autofree gchar *config_dir = nd_test_config_dir(ts->dir);
  g_autofree gchar *db_path = nd_test_db_path(ts->dir);

  ts->token_store = nd_token_store_new(config_dir, FALSE);
  ts->token = nd_token_store_ensure_token(ts->token_store,
                                          ND_TEST_ACCOUNT_ID, &err);
  g_assert_no_error(err);

  ts->db = nd_store_db_open(db_path, &err);
  g_assert_no_error(err);

  ts->server = nd_dav_server_new(ts->token_store, ts->db);
  nd_dav_server_set_account_id(ts->server, ND_TEST_ACCOUNT_ID);
  g_assert_true(nd_dav_server_start(ts->server, "127.0.0.1", 0, &err));
  g_assert_no_error(err);
  ts->port = nd_dav_server_get_port(ts->server);
  g_assert_cmpuint(ts->port, >, 0);

  ts->loop = g_main_loop_new(ctx, FALSE);

  GSource *idle = g_idle_source_new();
  g_source_set_callback(idle, signal_ready, ts, NULL);
  g_source_attach(idle, ctx);
  g_source_unref(idle);

  g_main_loop_run(ts->loop);

  /* Tear down on the owning thread/context. */
  nd_dav_server_stop(ts->server);
  g_clear_object(&ts->server);
  g_clear_pointer(&ts->db, nd_store_db_unref);
  g_clear_pointer(&ts->token_store, nd_token_store_free);

  g_main_context_pop_thread_default(ctx);
  g_main_loop_unref(ts->loop);
  ts->loop = NULL;
  g_main_context_unref(ctx);
  return NULL;
}

NdTestServer *
nd_test_server_start(const gchar *dir)
{
  NdTestServer *ts = g_new0(NdTestServer, 1);
  if (dir != NULL) {
    ts->dir = g_strdup(dir);
  } else {
    ts->dir = nd_test_make_tmpdir();
    ts->owns_dir = TRUE;
  }

  g_mutex_init(&ts->mutex);
  g_cond_init(&ts->cond);

  ts->thread = g_thread_new("dav-server", server_thread_func, ts);

  g_mutex_lock(&ts->mutex);
  while (!ts->ready)
    g_cond_wait(&ts->cond, &ts->mutex);
  g_mutex_unlock(&ts->mutex);

  ts->base_url = g_strdup_printf("http://127.0.0.1:%u", ts->port);
  ts->session = soup_session_new();
  g_signal_connect(ts->session, "request-queued",
                   G_CALLBACK(on_request_queued), ts);
  return ts;
}

void
nd_test_server_stop(NdTestServer *ts)
{
  if (ts == NULL)
    return;

  g_clear_object(&ts->session);
  g_main_loop_quit(ts->loop);
  g_thread_join(ts->thread);

  g_mutex_clear(&ts->mutex);
  g_cond_clear(&ts->cond);

  if (ts->owns_dir)
    nd_test_rm_rf(ts->dir);
  g_free(ts->dir);
  g_free(ts->token);
  g_free(ts->base_url);
  g_free(ts);
}
