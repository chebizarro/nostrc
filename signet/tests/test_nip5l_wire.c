/* SPDX-License-Identifier: MIT
 *
 * test_nip5l_wire.c - NIP-5L replies over the real Unix socket (nostrc-vhzq1).
 *
 * Every reply line must be JSON a client can parse, and string results must
 * round-trip byte for byte, including non-ASCII plaintext.
 */
#include "signet/nip5l_transport.h"
#include "signet/key_store.h"
#include "signet/nostr_auth.h"
#include "signet/store.h"
#include "test_check.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <nostr-event.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";

static char *get_pubkey(const char *agent, void *data) {
  char pk[65];
  return signet_key_store_get_agent_pubkey(data, agent, pk, sizeof(pk)) ? g_strdup(pk) : NULL;
}

typedef struct {
  GDataInputStream *in;
  GOutputStream *out;
} Conn;

/* Send one request; return the reply's parsed root object (caller unrefs parser). */
static JsonObject *rpc(Conn *c, JsonParser *parser, const char *request) {
  GError *error = NULL;
  char *line = g_strconcat(request, "\n", NULL);
  CHECK(g_output_stream_write_all(c->out, line, strlen(line), NULL, NULL, &error));
  CHECK(!error);
  g_free(line);
  char *reply = g_data_input_stream_read_line_utf8(c->in, NULL, NULL, &error);
  CHECK(reply && !error);
  if (!json_parser_load_from_data(parser, reply, -1, &error)) {
    fprintf(stderr, "reply is not JSON: %s (%s)\n", reply, error ? error->message : "?");
    CHECK(!"NIP-5L reply must be valid JSON");
  }
  g_free(reply);
  JsonNode *root = json_parser_get_root(parser);
  CHECK(root && JSON_NODE_HOLDS_OBJECT(root));
  return json_node_get_object(root);
}

static char *request_json(const char *method, const char *p0, const char *p1) {
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "method");
  json_builder_add_string_value(b, method);
  json_builder_set_member_name(b, "params");
  json_builder_begin_array(b);
  if (p0) json_builder_add_string_value(b, p0);
  if (p1) json_builder_add_string_value(b, p1);
  json_builder_end_array(b);
  json_builder_end_object(b);
  JsonGenerator *g = json_generator_new();
  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  char *s = json_generator_to_data(g, NULL);
  json_node_unref(root);
  g_object_unref(g);
  g_object_unref(b);
  return s;
}

static char *call_result(Conn *c, const char *method, const char *p0, const char *p1) {
  JsonParser *parser = json_parser_new();
  char *req = request_json(method, p0, p1);
  JsonObject *o = rpc(c, parser, req);
  g_free(req);
  if (!json_object_has_member(o, "result"))
    fprintf(stderr, "%s failed: %s\n", method,
            json_object_has_member(o, "error") ? json_object_get_string_member(o, "error") : "?");
  CHECK(json_object_has_member(o, "result"));
  char *r = g_strdup(json_object_get_string_member(o, "result"));
  g_object_unref(parser);
  return r;
}

typedef struct {
  const char *path;
  const char *pubkey;
  GMainLoop *loop;
} Fixture;

static gboolean quit_loop(gpointer data) { g_main_loop_quit(data); return G_SOURCE_REMOVE; }
static gboolean watchdog(gpointer data) { (void)data; CHECK(!"nip5l test deadline"); return G_SOURCE_REMOVE; }

static gpointer client(gpointer data) {
  Fixture *f = data;
  GError *error = NULL;
  GSocketClient *sc = g_socket_client_new();
  g_socket_client_set_timeout(sc, 5);
  GSocketAddress *addr = g_unix_socket_address_new(f->path);
  GSocketConnection *conn = g_socket_client_connect(sc, G_SOCKET_CONNECTABLE(addr), NULL, &error);
  CHECK(conn && !error);
  g_object_unref(addr);
  g_object_unref(sc);
  Conn c = {
    .in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn))),
    .out = g_io_stream_get_output_stream(G_IO_STREAM(conn)),
  };

  char *challenge = call_result(&c, "get_challenge", "known", NULL);
  CHECK(strlen(challenge) == 64);

  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, SIGNET_AUTH_KIND);
  nostr_event_set_created_at(ev, time(NULL));
  nostr_event_set_content(ev, "");
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("agent", "known", NULL));
  nostr_tags_append(tags, nostr_tag_new("purpose", "signet-auth", NULL));
  nostr_tags_append(tags, nostr_tag_new("challenge", challenge, NULL));
  nostr_event_set_tags(ev, tags);
  CHECK(nostr_event_sign(ev, sk) == 0);
  char *auth = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  char *authed = call_result(&c, "authenticate", auth, NULL);
  CHECK(strcmp(authed, "authenticated") == 0);
  free(auth);

  /* Non-ASCII, quote, backslash and newline: g_strescape rendered the UTF-8
   * bytes as octal escapes, which no JSON parser accepts. */
  const char *plain = "caf\xc3\xa9 \xf0\x9f\x8e\x89 \"q\" \\ line\nend";
  char *ct = call_result(&c, "nip44_encrypt", f->pubkey, plain);
  char *back = call_result(&c, "nip44_decrypt", f->pubkey, ct);
  CHECK(strcmp(back, plain) == 0);

  g_free(challenge); g_free(authed); g_free(ct); g_free(back);
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(c.in);
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
  uint8_t raw[32] = {0};
  raw[31] = 1;
  char pk[65];
  CHECK(signet_key_store_adopt_agent(keys, "known", raw, NULL, "initial", NULL, NULL, 0, pk, NULL)
        == SIGNET_ADOPT_OK);
  SignetChallengeStore *challenges = signet_challenge_store_new();
  SignetFleetRegistry fleet = {.get_agent_pubkey = get_pubkey, .user_data = keys};
  char *dir = g_dir_make_tmp("signet-nip5l-wire-XXXXXX", NULL);
  CHECK(dir);
  char *path = g_build_filename(dir, "nip5l.sock", NULL);
  SignetNip5lServerConfig nc = {.socket_path = path, .keys = keys,
    .store = signet_key_store_get_store(keys), .challenges = challenges, .fleet = &fleet};
  SignetNip5lServer *ns = signet_nip5l_server_new(&nc);
  CHECK(ns && signet_nip5l_server_start(ns) == 0);

  Fixture f = {.path = path, .pubkey = pk, .loop = g_main_loop_new(NULL, FALSE)};
  guint deadline = g_timeout_add_seconds(30, watchdog, NULL);
  GThread *t = g_thread_new("nip5l-client", client, &f);
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
  puts("test_nip5l_wire: all passed");
  return 0;
}
