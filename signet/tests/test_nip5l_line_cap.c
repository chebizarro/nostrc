/* SPDX-License-Identifier: MIT
 *
 * test_nip5l_line_cap.c - NIP-5L request framing on the real socket
 * (nostrc-fpbvg): pipelined requests and blank lines keep working, and a
 * request that never ends is refused instead of buffered without limit.
 */
#include "signet/nip5l_transport.h"
#include "signet/key_store.h"
#include "signet/nostr_auth.h"
#include "test_check.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  const char *path;
  GMainLoop *loop;
} Fixture;

static gboolean quit_loop(gpointer data) { g_main_loop_quit(data); return G_SOURCE_REMOVE; }
static gboolean watchdog(gpointer data) { (void)data; CHECK(!"nip5l line-cap test deadline"); return G_SOURCE_REMOVE; }

static void send_all(GOutputStream *out, const char *buf, gsize len) {
  GError *error = NULL;
  CHECK(g_output_stream_write_all(out, buf, len, NULL, NULL, &error));
  CHECK(!error);
}

static char *read_reply(GDataInputStream *in) {
  GError *error = NULL;
  char *line = g_data_input_stream_read_line(in, NULL, NULL, &error);
  if (error) {
    fprintf(stderr, "read: %s\n", error->message);
    g_error_free(error);
  }
  return line;
}

static gpointer client(gpointer data) {
  Fixture *f = data;
  GError *error = NULL;
  GSocketClient *sc = g_socket_client_new();
  g_socket_client_set_timeout(sc, 5); /* a server that never answers fails the read */
  GSocketAddress *addr = g_unix_socket_address_new(f->path);
  GSocketConnection *conn = g_socket_client_connect(sc, G_SOCKET_CONNECTABLE(addr), NULL, &error);
  CHECK(conn && !error);
  g_object_unref(addr);
  g_object_unref(sc);
  GDataInputStream *in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));

  /* Two requests in one write, with a blank line between them. */
  const char *pipelined = "{\"method\":\"sign_event\"}\n\n{\"method\":\"ping\"}\n";
  send_all(out, pipelined, strlen(pipelined));
  for (int i = 0; i < 2; i++) {
    char *r = read_reply(in);
    CHECK(r && strstr(r, "authenticate first"));
    g_free(r);
  }

  /* 1 MiB + 64 KiB with no newline. */
  gsize big = 1024u * 1024u + 65536u;
  char *junk = g_malloc(big);
  memset(junk, 'a', big);
  /* The daemon may refuse and close before the whole write lands, so a short
   * or failed write here is expected; the reply below is what is checked. */
  GError *werr = NULL;
  (void)g_output_stream_write_all(out, junk, big, NULL, NULL, &werr);
  g_clear_error(&werr);
  g_free(junk);
  char *r = read_reply(in);
  CHECK(r && strstr(r, "request too large"));
  g_free(r);
  r = read_reply(in);
  CHECK(r == NULL); /* the daemon closed the connection */

  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(in);
  g_object_unref(conn);
  g_main_context_invoke(NULL, quit_loop, f->loop);
  return NULL;
}

int main(void) {
  CHECK(sodium_init() >= 0);
  SignetKeyStoreConfig kc = {.db_path = ":memory:",
    .master_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
  SignetKeyStore *keys = signet_key_store_new(NULL, &kc);
  CHECK(keys);
  SignetChallengeStore *challenges = signet_challenge_store_new();
  char *dir = g_dir_make_tmp("signet-nip5l-cap-XXXXXX", NULL);
  CHECK(dir);
  char *path = g_build_filename(dir, "nip5l.sock", NULL);
  SignetNip5lServerConfig nc = {.socket_path = path, .keys = keys,
    .store = signet_key_store_get_store(keys), .challenges = challenges};
  SignetNip5lServer *ns = signet_nip5l_server_new(&nc);
  CHECK(ns && signet_nip5l_server_start(ns) == 0);

  Fixture f = {.path = path, .loop = g_main_loop_new(NULL, FALSE)};
  guint deadline = g_timeout_add_seconds(30, watchdog, NULL);
  GThread *t = g_thread_new("nip5l-cap-client", client, &f);
  g_main_loop_run(f.loop);
  g_thread_join(t);
  g_source_remove(deadline);
  g_main_loop_unref(f.loop);

  signet_nip5l_server_free(ns);
  signet_challenge_store_free(challenges);
  signet_key_store_free(keys);
  g_unlink(path);
  CHECK(g_rmdir(dir) == 0);
  g_free(path);
  g_free(dir);
  puts("test_nip5l_line_cap: all passed");
  return 0;
}
