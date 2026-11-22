#include "signet/bootstrap_server.h"
#include "signet/health_server.h"
#include "signet/key_store.h"
#include "signet/store.h"
#include "test_http_client.h"
#include <sodium.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <json-glib/json-glib.h>
#include <time.h>

static char *agent_pubkey(const char *agent, void *data) {
  SignetAgentMeta meta = {0};
  if (signet_store_get_agent_meta(signet_key_store_get_store(data), agent, &meta) != 0) return NULL;
  char *pk = g_strdup(meta.pubkey);
  signet_agent_meta_clear(&meta);
  return pk;
}
static void auth_route(unsigned short port, SignetChallengeStore *challenges,
                       const char *agent, const char *sk, int expected) {
  char *challenge = signet_challenge_issue(challenges, agent, time(NULL)); CHECK(challenge);
  NostrEvent *event = nostr_event_new(); CHECK(event);
  nostr_event_set_kind(event, SIGNET_AUTH_KIND);
  nostr_event_set_content(event, ""); nostr_event_set_created_at(event, time(NULL));
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("challenge", challenge, NULL));
  nostr_tags_append(tags, nostr_tag_new("agent", agent, NULL));
  nostr_tags_append(tags, nostr_tag_new("purpose", "signet-auth", NULL));
  nostr_event_set_tags(event, tags); CHECK(!nostr_event_sign(event, sk));
  char *json = nostr_event_serialize_compact(event); CHECK(json);
  char response[8192];
  int status = http_post(port, "/auth", "X-Forwarded-Proto: https\r\n", json,
    response, sizeof(response));
  if (status != expected) fprintf(stderr, "auth route expected %d got %d: %s\n", expected, status, response);
  CHECK(status == expected);
  if (expected == 200) {
    const char *body = strstr(response, "\r\n\r\n"); CHECK(body);
    JsonParser *parser = json_parser_new();
    CHECK(json_parser_load_from_data(parser, body + 4, -1, NULL));
    JsonObject *o = json_node_get_object(json_parser_get_root(parser)); CHECK(o);
    CHECK(!strcmp(json_object_get_string_member(o, "agent_id"), agent));
    CHECK(strlen(json_object_get_string_member(o, "session_token")) == 64);
    g_object_unref(parser);
  }
  free(json); nostr_event_free(event); g_free(challenge);
}

int main(void) {
  CHECK(sodium_init() >= 0);
  SignetKeyStoreConfig kc = {.db_path = ":memory:",
    .master_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
  SignetKeyStore *keys = signet_key_store_new(NULL, &kc);
  CHECK(keys);
  SignetChallengeStore *challenges = signet_challenge_store_new();
  CHECK(challenges);
  const char *trusted[] = {"127.0.0.0/8", "::1/128"};
  SignetBootstrapServerConfig cfg = {.listen = "127.0.0.1:0", .keys = keys,
    .store = signet_key_store_get_store(keys), .challenges = challenges,
    .require_tls = true, .trusted_proxy_cidrs = trusted, .n_trusted_proxy_cidrs = 2};
  SignetFleetRegistry fleet = {.get_agent_pubkey = agent_pubkey, .user_data = keys};
  cfg.fleet = &fleet;
  SignetBootstrapServer *bs = signet_bootstrap_server_new(&cfg);
  CHECK(bs && signet_bootstrap_server_start(bs) == 0);
  CHECK(signet_bootstrap_server_start(bs) == 0);
  unsigned short port = signet_bootstrap_server_bound_port(bs);
  CHECK(port);
  const char *agent = "agent\"\\\nname";
  const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";
  char *pk = nostr_key_get_public(sk); CHECK(pk);
  uint8_t raw[32] = {0}; raw[31] = 1;
  CHECK(signet_store_put_agent_ex(signet_key_store_get_store(keys), agent, raw, 32,
    "fixture", pk, "provisioned", time(NULL)) == 0);
  auth_route(port, challenges, "unknown-agent", sk, 403);
  auth_route(port, challenges, agent, sk, 200);
  free(pk);
  char response[8192];
  CHECK(http_get(port, "/missing", "X-Forwarded-Proto: https\r\n", response, sizeof(response)) == 404);
  CHECK(strstr(response, "no-store"));
  CHECK(http_get(port, "/missing", "x-forwarded-proto: \tHTTPS \t\r\n", response, sizeof(response)) == 404);
  const char *bad[] = {"", "X-Forwarded-Proto: http\r\n", "X-Forwarded-Proto: https,http\r\n",
    "X-Forwarded-Proto: https\r\nX-Forwarded-Proto: https\r\n",
    "X-Forwarded-Proto: http\r\nX-Forwarded-Proto: https\r\n",
    "X-Forwarded-Proto: https trailing\r\n"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
    CHECK(http_get(port, "/missing", bad[i], response, sizeof(response)) == 403);
    CHECK(!strstr(response, "set X-Forwarded-Proto"));
  }
  signet_bootstrap_server_stop(bs);
  CHECK(signet_bootstrap_server_bound_port(bs) == 0);
  signet_bootstrap_server_stop(bs);
  signet_bootstrap_server_free(bs);
  const char *untrusted[] = {"192.0.2.0/24"};
  cfg.trusted_proxy_cidrs = untrusted; cfg.n_trusted_proxy_cidrs = 1;
  bs = signet_bootstrap_server_new(&cfg);
  CHECK(bs && signet_bootstrap_server_start(bs) == 0);
  CHECK(http_get(signet_bootstrap_server_bound_port(bs), "/missing",
                 "X-Forwarded-Proto: https\r\n", response, sizeof(response)) == 403);
  signet_bootstrap_server_free(bs);

  SignetHealthServerConfig hc = {.listen = "127.0.0.1:0"};
  SignetHealthServer *hs = signet_health_server_new(&hc);
  CHECK(hs && signet_health_server_start(hs) == 0);
  SignetHealthSnapshot snap = {.db_open = true, .key_store_available = true,
    .policy_store_loaded = true, .relay_connected = true, .fleet_synced = true,
    .provisioner_writeback_failed = true, .provisioner_writeback_failures = 3};
  signet_health_server_set_snapshot(hs, &snap);
  port = signet_health_server_bound_port(hs);
  CHECK(http_get(port, "/ready", NULL, response, sizeof(response)) == 200);
  CHECK(strstr(response, "\"provisioner_writeback_failed\":true"));
  CHECK(http_get(port, "/metrics", NULL, response, sizeof(response)) == 200);
  CHECK(strstr(response, "signet_provisioner_writeback_failures_total 3\n"));
  CHECK(strstr(response, "# TYPE signet_nip46_reply_ack_total counter\n"));
  CHECK(strstr(response, "signet_nip46_reply_ack_total{result=\"accept\"} 0\n"));

  snap.nip46_reply_ack_accept = UINT64_C(4294967297);
  snap.nip46_reply_ack_reject = 2;
  snap.nip46_reply_ack_timeout = 3;
  snap.nip46_reply_ack_transport_aborted = 4;
  signet_health_server_set_snapshot(hs, &snap);
  CHECK(http_get(port, "/metrics", NULL, response, sizeof(response)) == 200);
  CHECK(strstr(response, "signet_nip46_reply_ack_total{result=\"accept\"} 4294967297\n"));
  CHECK(strstr(response, "signet_nip46_reply_ack_total{result=\"reject\"} 2\n"));
  CHECK(strstr(response, "signet_nip46_reply_ack_total{result=\"timeout\"} 3\n"));
  CHECK(strstr(response, "signet_nip46_reply_ack_total{result=\"transport-aborted\"} 4\n"));
  /* Reply publication outcomes neither revoke readiness nor manufacture it. */
  CHECK(http_get(port, "/ready", NULL, response, sizeof(response)) == 200);
  CHECK(strstr(response, "\"ready\":true"));
  snap.fleet_synced = false;
  signet_health_server_set_snapshot(hs, &snap);
  CHECK(http_get(port, "/ready", NULL, response, sizeof(response)) == 503);
  CHECK(strstr(response, "\"ready\":false"));
  signet_health_server_free(hs);
  /* Listener free precedes every borrowed dependency. */
  signet_challenge_store_free(challenges);
  signet_key_store_free(keys);
  return 0;
}
