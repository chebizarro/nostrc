/* SPDX-License-Identifier: MIT */
#include "signet/bootstrap_server.h"
#include "signet/key_store.h"
#include "signet/store.h"
#include "signet/store_tokens.h"
#include "test_http_client.h"
#include <sodium.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <json-glib/json-glib.h>
#include <time.h>

#define HEADERS "X-Forwarded-Proto: https\r\n"
static const char *agent = "bootstrap-agent";
static const char *boot_sk = "0000000000000000000000000000000000000000000000000000000000000002";
static const char *wrong_sk = "0000000000000000000000000000000000000000000000000000000000000003";
static const char *token = "bootstrap-token";

static char *agent_pubkey(const char *id, void *data) {
  SignetAgentMeta meta = {0};
  if (signet_store_get_agent_meta(data, id, &meta)) return NULL;
  char *pk = g_strdup(meta.pubkey);
  signet_agent_meta_clear(&meta);
  return pk;
}

static char *proof(const char *sk, const char *challenge, const char *id,
                   const char *purpose, const char *token_hash) {
  NostrEvent *event = nostr_event_new(); CHECK(event);
  nostr_event_set_kind(event, SIGNET_AUTH_KIND);
  nostr_event_set_content(event, ""); nostr_event_set_created_at(event, time(NULL));
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("challenge", challenge, NULL));
  nostr_tags_append(tags, nostr_tag_new("agent", id, NULL));
  nostr_tags_append(tags, nostr_tag_new("purpose", purpose, NULL));
  nostr_tags_append(tags, nostr_tag_new("token", token_hash, NULL));
  nostr_event_set_tags(event, tags); CHECK(!nostr_event_sign(event, sk));
  char *json = nostr_event_serialize_compact(event); CHECK(json);
  nostr_event_free(event);
  return json;
}

static int redeem(unsigned short port, const char *pk, const char *auth, char *response) {
  char *body = g_strdup_printf("{\"token\":\"%s\",\"agent_id\":\"%s\","
                               "\"bootstrap_pubkey\":\"%s\"%s%s}", token, agent, pk,
                               auth ? ",\"auth_event\":" : "", auth ? auth : "");
  int status = http_post(port, "/bootstrap", HEADERS, body, response, 8192);
  g_free(body);
  return status;
}

static JsonParser *parse_response(const char *response) {
  const char *body = strstr(response, "\r\n\r\n"); CHECK(body);
  JsonParser *parser = json_parser_new();
  CHECK(json_parser_load_from_data(parser, body + 4, -1, NULL));
  return parser;
}

static void check_standing_secret(SignetStore *store) {
  SignetAgentRecord rec = {0};
  CHECK(!signet_store_get_agent(store, agent, &rec));
  CHECK(rec.connect_secret && !strcmp(rec.connect_secret, "standing-secret"));
  signet_agent_record_clear(&rec);
}

int main(void) {
  CHECK(sodium_init() >= 0);
  SignetKeyStoreConfig kc = {.db_path = ":memory:",
    .master_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
  SignetKeyStore *keys = signet_key_store_new(NULL, &kc); CHECK(keys);
  SignetStore *store = signet_key_store_get_store(keys); CHECK(store);
  SignetChallengeStore *cs = signet_challenge_store_new(); CHECK(cs);
  const char *agent_sk = "0000000000000000000000000000000000000000000000000000000000000001";
  char *agent_pk = nostr_key_get_public(agent_sk); CHECK(agent_pk);
  char *boot_pk = nostr_key_get_public(boot_sk); CHECK(boot_pk);
  uint8_t raw[32] = {0}; raw[31] = 1;
  char adopted_pk[65];
  CHECK(signet_key_store_adopt_agent(keys, agent, raw, agent_pk, "standing-secret",
      agent_pk, NULL, 0, adopted_pk, NULL) == SIGNET_ADOPT_OK);
  unsigned char hash[32]; char token_hash[65];
  crypto_hash_sha256(hash, (const unsigned char *)token, strlen(token));
  sodium_bin2hex(token_hash, sizeof(token_hash), hash, sizeof(hash));
  CHECK(!signet_store_put_bootstrap_token(store, token_hash, agent, boot_pk,
                                         time(NULL), time(NULL) + 600));
  const char *trusted[] = {"127.0.0.0/8"};
  const char *relays[] = {"wss://nos.lol"};
  SignetFleetRegistry fleet = {.get_agent_pubkey = agent_pubkey, .user_data = store};
  SignetBootstrapServerConfig cfg = {.listen = "127.0.0.1:0", .keys = keys,
    .store = store, .challenges = cs, .fleet = &fleet, .require_tls = true,
    .trusted_proxy_cidrs = trusted, .n_trusted_proxy_cidrs = 1,
    .bunker_pubkey_hex = agent_pk, .relay_urls = relays, .n_relay_urls = 1};
  SignetBootstrapServer *bs = signet_bootstrap_server_new(&cfg);
  CHECK(bs && !signet_bootstrap_server_start(bs));
  unsigned short port = signet_bootstrap_server_bound_port(bs); CHECK(port);
  char response[8192];
  /* A stolen bearer token and the public key alone must reveal no URI. */
  int no_proof = redeem(port, boot_pk, NULL, response);
  if (no_proof != 403) fprintf(stderr, "without proof: expected 403, got %d\n", no_proof);
  CHECK(no_proof == 403);
  CHECK(!strstr(response, "bunker://") && !strstr(response, "standing-secret"));
  check_standing_secret(store);

  const char *signers[] = {wrong_sk, boot_sk, boot_sk, boot_sk, boot_sk};
  const char *ids[] = {agent, "different-agent", agent, agent, agent};
  const char *purposes[] = {"signet-bootstrap", "signet-bootstrap", "signet-auth",
                           "signet-bootstrap", "signet-bootstrap"};
  for (int i = 0; i < 5; i++) {
    char *ch = signet_challenge_issue(cs, agent, time(NULL)); CHECK(ch);
    char *ev = proof(signers[i], ch, ids[i], purposes[i], i == 3 ? "wrong-token" : token_hash);
    if (i == 4) {
      char *sig = strstr(ev, "\"sig\":\""); CHECK(sig); sig += 7;
      *sig = *sig == 'a' ? 'b' : 'a';
    }
    CHECK(redeem(port, boot_pk, ev, response) == 403);
    CHECK(!strstr(response, "bunker://")); check_standing_secret(store);
    g_free(ev); g_free(ch);
    signet_challenge_store_cleanup(cs, time(NULL) + SIGNET_CHALLENGE_TTL_S);
  }
  char *ev = proof(boot_sk, "never-issued", agent, "signet-bootstrap", token_hash);
  CHECK(redeem(port, boot_pk, ev, response) == 403); g_free(ev);
  char *ch = signet_challenge_issue(cs, agent, time(NULL) - SIGNET_CHALLENGE_TTL_S); CHECK(ch);
  ev = proof(boot_sk, ch, agent, "signet-bootstrap", token_hash);
  CHECK(redeem(port, boot_pk, ev, response) == 403); g_free(ev); g_free(ch);
  check_standing_secret(store);

  /* Editing signed tags while keeping a genuine signature/id is not a proof. */
  ch = signet_challenge_issue(cs, agent, time(NULL)); CHECK(ch);
  const char *zero_hash = "0000000000000000000000000000000000000000000000000000000000000000";
  ev = proof(boot_sk, ch, agent, "signet-bootstrap", zero_hash);
  char *signed_hash = strstr(ev, zero_hash); CHECK(signed_hash);
  memcpy(signed_hash, token_hash, 64);
  CHECK(redeem(port, boot_pk, ev, response) == 403);
  check_standing_secret(store); g_free(ch); g_free(ev);

  /* A valid proof by an unbound key is not authority for this token. */
  ch = signet_challenge_issue(cs, agent, time(NULL)); CHECK(ch);
  ev = proof(wrong_sk, ch, agent, "signet-bootstrap", token_hash);
  char *wrong_pk = nostr_key_get_public(wrong_sk); CHECK(wrong_pk);
  CHECK(redeem(port, wrong_pk, ev, response) == 403);
  check_standing_secret(store); free(wrong_pk); g_free(ch); g_free(ev);

  CHECK(http_get(port, "/challenge?agent_id=bootstrap-agent", HEADERS, response, sizeof(response)) == 200);
  JsonParser *parser = parse_response(response);
  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
  ch = g_strdup(json_object_get_string_member(obj, "challenge")); CHECK(ch);
  g_object_unref(parser);
  ev = proof(boot_sk, ch, agent, "signet-bootstrap", token_hash);
  /* A bootstrap proof must not create a session lease through /auth. */
  CHECK(http_post(port, "/auth", HEADERS, ev, response, sizeof(response)) == 401);
  int status = redeem(port, boot_pk, ev, response);
  if (status != 200) fprintf(stderr, "bootstrap expected 200, got %d: %s\n", status, response);
  CHECK(status == 200);
  CHECK(strstr(response, "no-store"));
  parser = parse_response(response);
  obj = json_node_get_object(json_parser_get_root(parser));
  const char *uri = json_object_get_string_member(obj, "uri"); CHECK(uri);
  CHECK(!strcmp(json_object_get_string_member(obj, "pubkey"), agent_pk));
  const char *secret = strstr(uri, "secret="); CHECK(secret); secret += 7;
  CHECK(strlen(secret) == 64 && strcmp(secret, "standing-secret"));
  char *fresh = g_strdup(secret);
  g_object_unref(parser);
  CHECK(redeem(port, boot_pk, ev, response) == 403);
  /* Even a new proof cannot redeem the consumed token or rotate the pending secret. */
  g_free(ev); g_free(ch);
  ch = signet_challenge_issue(cs, agent, time(NULL)); CHECK(ch);
  ev = proof(boot_sk, ch, agent, "signet-bootstrap", token_hash);
  CHECK(redeem(port, boot_pk, ev, response) == 403);
  SignetAgentRecord rec = {0};
  CHECK(!signet_store_get_agent(store, agent, &rec));
  CHECK(rec.connect_secret && !strcmp(rec.connect_secret, fresh));
  signet_agent_record_clear(&rec);
  /* Exercise the actual persistent pairing consumer: old secret is invalid,
   * fresh secret binds once and cannot authorize another client afterwards. */
  char *resolved = NULL;
  CHECK(signet_store_consume_connect_secret_and_bind(store, "standing-secret", boot_pk,
                                                    time(NULL), &resolved) != 0);
  CHECK(!signet_store_consume_connect_secret_and_bind(store, fresh, boot_pk, time(NULL), &resolved));
  CHECK(resolved && !strcmp(resolved, agent)); g_free(resolved); resolved = NULL;
  CHECK(signet_store_consume_connect_secret_and_bind(store, fresh, agent_pk, time(NULL), &resolved) != 0);
  g_free(ev); g_free(ch); sodium_memzero(fresh, strlen(fresh)); g_free(fresh);
  signet_bootstrap_server_free(bs);

  /* A failure after token claim returns no URI and does not make the token
   * redeemable again. Exercise a real URI-construction failure, not a mock. */
  token = "bootstrap-failed-handoff";
  crypto_hash_sha256(hash, (const unsigned char *)token, strlen(token));
  sodium_bin2hex(token_hash, sizeof(token_hash), hash, sizeof(hash));
  CHECK(!signet_store_put_bootstrap_token(store, token_hash, agent, boot_pk,
                                         time(NULL), time(NULL) + 600));
  signet_challenge_store_cleanup(cs, time(NULL) + SIGNET_CHALLENGE_TTL_S);
  cfg.bunker_pubkey_hex = NULL;
  bs = signet_bootstrap_server_new(&cfg); CHECK(bs && !signet_bootstrap_server_start(bs));
  port = signet_bootstrap_server_bound_port(bs); CHECK(port);
  ch = signet_challenge_issue(cs, agent, time(NULL)); CHECK(ch);
  ev = proof(boot_sk, ch, agent, "signet-bootstrap", token_hash);
  CHECK(redeem(port, boot_pk, ev, response) == 500);
  CHECK(!strstr(response, "bunker://")); g_free(ch); g_free(ev);
  ch = signet_challenge_issue(cs, agent, time(NULL)); CHECK(ch);
  ev = proof(boot_sk, ch, agent, "signet-bootstrap", token_hash);
  CHECK(redeem(port, boot_pk, ev, response) == 403);
  CHECK(!strstr(response, "bunker://")); g_free(ch); g_free(ev);
  signet_bootstrap_server_free(bs);
  signet_challenge_store_free(cs); signet_key_store_free(keys);
  free(agent_pk); free(boot_pk);
  puts("bootstrap possession, replay, fresh single-use pairing: PASS");
  return 0;
}
