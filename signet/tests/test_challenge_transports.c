/* SPDX-License-Identifier: MIT */
/* Include the production D-Bus implementation only to discover its ephemeral
 * listener address. All requests below cross real TCP/Unix/HTTP connections. */
#include "../src/dbus_tcp.c"
#include "signet/nip5l_transport.h"
#include "signet/bootstrap_server.h"
#include "test_http_client.h"
#include <glib/gstdio.h>
#include <nostr-event.h>
#include <nostr-keys.h>

static const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";
typedef struct {
  GMainLoop *loop;
  const char *address, *path, *public_challenge;
  unsigned short port;
} Fixture;
static char *get_pubkey(const char *agent, void *data) {
  char pk[65];
  return signet_key_store_get_agent_pubkey(data, agent, pk, sizeof(pk)) ? g_strdup(pk) : NULL;
}
static gboolean quit_loop(gpointer data) { g_main_loop_quit(data); return G_SOURCE_REMOVE; }
static gboolean watchdog(gpointer data) { (void)data; CHECK(!"transport test deadline"); return G_SOURCE_REMOVE; }
static gpointer client(gpointer data) {
  Fixture *f = data;
  GError *error = NULL;
  GDBusConnection *dbus = g_dbus_connection_new_for_address_sync(f->address,
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, NULL, NULL, &error);
  CHECK(dbus && !error);
  GSocketClient *sc = g_socket_client_new(); g_socket_client_set_timeout(sc, 5);
  GSocketAddress *address = g_unix_socket_address_new(f->path);
  GSocketConnection *conn = g_socket_client_connect(sc, G_SOCKET_CONNECTABLE(address), NULL, &error);
  CHECK(conn && !error); g_object_unref(address); g_object_unref(sc);
  GDataInputStream *input = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
  GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(conn));
  for (int i = 0; i < 1100; i++) {
    char *id = g_strdup_printf("unprovisioned-%d", i);
    GVariant *reply = g_dbus_connection_call_sync(dbus, NULL, SIGNET_TCP_OBJECT_PATH,
        "net.signet.Auth", "GetChallenge", g_variant_new("(s)", id), G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (reply || !error || !g_dbus_error_is_remote_error(error)) fprintf(stderr,"dbus result: reply=%p error=%s\n",(void *)reply,error ? error->message : "none");
    CHECK(!reply && error && g_dbus_error_is_remote_error(error));
    char *name = g_dbus_error_get_remote_error(error);
    CHECK(!strcmp(name, "net.signet.Error.NotProvisioned")); g_free(name); g_clear_error(&error);
    char *line = g_strdup_printf("{\"method\":\"get_challenge\",\"params\":[\"%s\"]}\n", id);
    CHECK(g_output_stream_write_all(output, line, strlen(line), NULL, NULL, &error)); CHECK(!error);
    char *result = g_data_input_stream_read_line(input, NULL, NULL, &error);
    CHECK(result && !error && strstr(result, "agent not provisioned"));
    g_free(result); g_free(line); g_free(id);
  }
  /* Repeated known-agent local requests cannot evict the public proof either. */
  for (int i = 0; i < 32; i++) {
  GVariant *reply = g_dbus_connection_call_sync(dbus, NULL, SIGNET_TCP_OBJECT_PATH,
      "net.signet.Auth", "GetChallenge", g_variant_new("(s)", "known"), G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
  CHECK(reply && !error); const char *local_challenge; g_variant_get(reply, "(&s)", &local_challenge);
  CHECK(strlen(local_challenge) == 64); g_variant_unref(reply);
  const char *line = "{\"method\":\"get_challenge\",\"params\":[\"known\"]}\n";
  CHECK(g_output_stream_write_all(output, line, strlen(line), NULL, NULL, &error));
  char *result = g_data_input_stream_read_line(input, NULL, NULL, &error);
  CHECK(result && !error && !strstr(result, "error")); g_free(result);

  }

  /* The public proof obtained before the local flood still authenticates. */
  NostrEvent *event = nostr_event_new(); CHECK(event);
  nostr_event_set_kind(event, SIGNET_AUTH_KIND); nostr_event_set_created_at(event, time(NULL));
  nostr_event_set_content(event, ""); NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("agent", "known", NULL));
  nostr_tags_append(tags, nostr_tag_new("purpose", "signet-auth", NULL));
  nostr_tags_append(tags, nostr_tag_new("challenge", f->public_challenge, NULL));
  nostr_event_set_tags(event, tags); CHECK(!nostr_event_sign(event, sk));
  char *auth = nostr_event_serialize_compact(event); CHECK(auth); char response[8192];
  CHECK(http_post(f->port, "/auth", "X-Forwarded-Proto: https\r\n", auth, response, sizeof(response)) == 200);
  free(auth); nostr_event_free(event);
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL); g_object_unref(input); g_object_unref(conn);
  g_dbus_connection_close_sync(dbus, NULL, NULL); g_object_unref(dbus);
  g_main_context_invoke(NULL, quit_loop, f->loop);
  return NULL;
}
int main(void) {
  CHECK(sodium_init() >= 0);
  SignetKeyStoreConfig kc = {.db_path=":memory:",
    .master_key="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
  SignetKeyStore *keys = signet_key_store_new(NULL, &kc); CHECK(keys);
  uint8_t raw[32] = {0}; raw[31]=1; char pk[65];
  CHECK(signet_key_store_adopt_agent(keys,"known",raw,NULL,"initial",NULL,NULL,0,pk,NULL)==SIGNET_ADOPT_OK);
  SignetChallengeStore *public = signet_challenge_store_new(), *tcp = signet_challenge_store_new(),
                       *unix_store = signet_challenge_store_new();
  SignetStore *store = signet_key_store_get_store(keys);
  SignetFleetRegistry fleet = {.get_agent_pubkey=get_pubkey,.user_data=keys};
  SignetDbusTcpServerConfig dc = {.listen_address="tcp:host=127.0.0.1,port=0",.keys=keys,
    .store=store,.challenges=tcp,.fleet=&fleet};
  SignetDbusTcpServer *ds = signet_dbus_tcp_server_new(&dc); CHECK(ds && !signet_dbus_tcp_server_start(ds));
  char *dir = g_dir_make_tmp("signet-local-challenge-XXXXXX",NULL); CHECK(dir);
  char *path = g_build_filename(dir,"local.sock",NULL);
  SignetNip5lServerConfig nc = {.socket_path=path,.keys=keys,.store=store,.challenges=unix_store,.fleet=&fleet};
  SignetNip5lServer *ns = signet_nip5l_server_new(&nc); CHECK(ns && !signet_nip5l_server_start(ns));
  const char *trusted[]={"127.0.0.0/8"};
  SignetBootstrapServerConfig bc = {.listen="127.0.0.1:0",.keys=keys,.store=store,.challenges=public,
    .fleet=&fleet,.require_tls=true,.trusted_proxy_cidrs=trusted,.n_trusted_proxy_cidrs=1};
  SignetBootstrapServer *bs = signet_bootstrap_server_new(&bc); CHECK(bs && !signet_bootstrap_server_start(bs));
  char *challenge = signet_challenge_issue(public,"known",time(NULL)); CHECK(challenge);
  Fixture f = {.loop=g_main_loop_new(NULL,FALSE),.address=g_dbus_server_get_client_address(ds->server),
    .path=path,.port=signet_bootstrap_server_bound_port(bs),.public_challenge=challenge};
  guint deadline=g_timeout_add_seconds(30,watchdog,NULL);
  GThread *thread=g_thread_new("challenge-client",client,&f);
  g_main_loop_run(f.loop); g_thread_join(thread); g_source_remove(deadline); g_main_loop_unref(f.loop);
  signet_bootstrap_server_free(bs); signet_nip5l_server_free(ns); signet_dbus_tcp_server_free(ds);
  signet_challenge_store_free(public); signet_challenge_store_free(tcp); signet_challenge_store_free(unix_store);
  signet_key_store_free(keys); g_unlink(path); CHECK(!g_rmdir(dir)); g_free(path); g_free(dir); g_free(challenge);
  puts("local unknown-agent flood refused; public proof and known transports work: PASS"); return 0;
}
