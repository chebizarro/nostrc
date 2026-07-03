/* SPDX-License-Identifier: MIT
 *
 * test_nostr_auth.c - Tests for SignetChallengeStore: issuance, TTL, cleanup.
 */

#include "signet/nostr_auth.h"

#include <nostr-event.h>
#include <nostr-keys.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>

/* ----------------------- signed-challenge verification -------------------- */

#define BOGUS_PUBKEY "0000000000000000000000000000000000000000000000000000000000000000"

typedef struct {
  const char *pubkey;   /* real signer pubkey */
  bool in_fleet;
  bool denied;
  bool wrong_pubkey;    /* if true, get_agent_pubkey returns BOGUS_PUBKEY */
} FleetCtx;

static bool tf_in_fleet(const char *pk, void *ud) { (void)pk; return ((FleetCtx *)ud)->in_fleet; }
static bool tf_denied(const char *pk, void *ud)   { (void)pk; return ((FleetCtx *)ud)->denied; }
static char *tf_get_pubkey(const char *agent, void *ud) {
  (void)agent;
  FleetCtx *c = (FleetCtx *)ud;
  return g_strdup(c->wrong_pubkey ? BOGUS_PUBKEY : c->pubkey);
}

/* Build and sign a kind-28100 auth event with the given tags. */
static char *build_auth_event(const char *sk_hex, int kind, const char *challenge,
                              const char *agent_id, const char *purpose,
                              int64_t created_at) {
  NostrEvent *evt = nostr_event_new();
  if (!evt) return NULL;
  nostr_event_set_kind(evt, kind);
  nostr_event_set_created_at(evt, created_at);
  nostr_event_set_content(evt, "");
  NostrTags *tags = nostr_tags_new(0);
  if (challenge) nostr_tags_append(tags, nostr_tag_new("challenge", challenge, NULL));
  if (agent_id)  nostr_tags_append(tags, nostr_tag_new("agent", agent_id, NULL));
  if (purpose)   nostr_tags_append(tags, nostr_tag_new("purpose", purpose, NULL));
  nostr_event_set_tags(evt, tags);
  char *json = (nostr_event_sign(evt, sk_hex) == 0) ? nostr_event_serialize_compact(evt) : NULL;
  nostr_event_free(evt);
  return json;
}

/* Corrupt one hex char of the "sig" field to invalidate the signature. */
static char *tamper_sig(const char *json) {
  char *copy = g_strdup(json);
  char *p = strstr(copy, "\"sig\":\"");
  if (p) { p += 7; if (*p) *p = (*p == 'a') ? 'b' : 'a'; }
  return copy;
}

static void test_verify_valid_and_replay(void) {
  SignetChallengeStore *cs = signet_challenge_store_new();
  int64_t now = 1700000000;

  char *sk = nostr_key_generate_private();
  char *pk = nostr_key_get_public(sk);
  assert(sk && pk);

  char *challenge = signet_challenge_issue(cs, "agent-auth", now);
  assert(challenge);

  FleetCtx fc = { .pubkey = pk, .in_fleet = true, .denied = false, .wrong_pubkey = false };
  SignetFleetRegistry fleet = { .is_in_fleet = tf_in_fleet, .is_denied = tf_denied,
                                .get_agent_pubkey = tf_get_pubkey, .user_data = &fc };

  char *ev = build_auth_event(sk, SIGNET_AUTH_KIND, challenge, "agent-auth", "signet-auth", now);
  assert(ev);

  /* A correctly signed event for an issued challenge verifies OK and returns
   * the agent_id + pubkey. This is the positive path the old test never had. */
  char *out_agent = NULL, *out_pk = NULL;
  SignetAuthResult r = signet_auth_verify(cs, &fleet, ev, now, &out_agent, &out_pk);
  assert(r == SIGNET_AUTH_OK);
  assert(out_agent && strcmp(out_agent, "agent-auth") == 0);
  assert(out_pk && g_ascii_strcasecmp(out_pk, pk) == 0);
  g_free(out_agent); g_free(out_pk);

  /* Single-use: replaying the same event is rejected. */
  r = signet_auth_verify(cs, &fleet, ev, now, NULL, NULL);
  assert(r == SIGNET_AUTH_ERR_CHALLENGE_REPLAYED);

  g_free(ev); g_free(challenge); free(pk); free(sk);
  signet_challenge_store_free(cs);
  printf("test_verify_valid_and_replay: PASS\n");
}

static void test_verify_negatives(void) {
  int64_t now = 1700000000;
  char *sk = nostr_key_generate_private();
  char *pk = nostr_key_get_public(sk);
  assert(sk && pk);

  /* Bad signature. */
  {
    SignetChallengeStore *cs = signet_challenge_store_new();
    char *ch = signet_challenge_issue(cs, "a", now);
    FleetCtx fc = { pk, true, false, false };
    SignetFleetRegistry fl = { tf_in_fleet, tf_denied, tf_get_pubkey, &fc };
    char *ev = build_auth_event(sk, SIGNET_AUTH_KIND, ch, "a", "signet-auth", now);
    char *bad = tamper_sig(ev);
    assert(signet_auth_verify(cs, &fl, bad, now, NULL, NULL) == SIGNET_AUTH_ERR_BAD_SIGNATURE);
    g_free(bad); g_free(ev); g_free(ch); signet_challenge_store_free(cs);
  }

  /* Wrong kind. */
  {
    SignetChallengeStore *cs = signet_challenge_store_new();
    char *ch = signet_challenge_issue(cs, "a", now);
    FleetCtx fc = { pk, true, false, false };
    SignetFleetRegistry fl = { tf_in_fleet, tf_denied, tf_get_pubkey, &fc };
    char *ev = build_auth_event(sk, 1, ch, "a", "signet-auth", now);
    assert(signet_auth_verify(cs, &fl, ev, now, NULL, NULL) == SIGNET_AUTH_ERR_WRONG_KIND);
    g_free(ev); g_free(ch); signet_challenge_store_free(cs);
  }

  /* Wrong purpose. */
  {
    SignetChallengeStore *cs = signet_challenge_store_new();
    char *ch = signet_challenge_issue(cs, "a", now);
    FleetCtx fc = { pk, true, false, false };
    SignetFleetRegistry fl = { tf_in_fleet, tf_denied, tf_get_pubkey, &fc };
    char *ev = build_auth_event(sk, SIGNET_AUTH_KIND, ch, "a", "not-signet", now);
    assert(signet_auth_verify(cs, &fl, ev, now, NULL, NULL) == SIGNET_AUTH_ERR_WRONG_PURPOSE);
    g_free(ev); g_free(ch); signet_challenge_store_free(cs);
  }

  /* Unknown challenge (never issued). */
  {
    SignetChallengeStore *cs = signet_challenge_store_new();
    FleetCtx fc = { pk, true, false, false };
    SignetFleetRegistry fl = { tf_in_fleet, tf_denied, tf_get_pubkey, &fc };
    char *ev = build_auth_event(sk, SIGNET_AUTH_KIND,
                                "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef00",
                                "a", "signet-auth", now);
    assert(signet_auth_verify(cs, &fl, ev, now, NULL, NULL) == SIGNET_AUTH_ERR_CHALLENGE_MISMATCH);
    g_free(ev); signet_challenge_store_free(cs);
  }

  /* Expired challenge. */
  {
    SignetChallengeStore *cs = signet_challenge_store_new();
    char *ch = signet_challenge_issue(cs, "a", now);
    FleetCtx fc = { pk, true, false, false };
    SignetFleetRegistry fl = { tf_in_fleet, tf_denied, tf_get_pubkey, &fc };
    char *ev = build_auth_event(sk, SIGNET_AUTH_KIND, ch, "a", "signet-auth", now);
    int64_t later = now + SIGNET_CHALLENGE_TTL_S + 1;
    assert(signet_auth_verify(cs, &fl, ev, later, NULL, NULL) == SIGNET_AUTH_ERR_CHALLENGE_EXPIRED);
    g_free(ev); g_free(ch); signet_challenge_store_free(cs);
  }

  /* Deny list takes precedence. */
  {
    SignetChallengeStore *cs = signet_challenge_store_new();
    char *ch = signet_challenge_issue(cs, "a", now);
    FleetCtx fc = { pk, true, true /*denied*/, false };
    SignetFleetRegistry fl = { tf_in_fleet, tf_denied, tf_get_pubkey, &fc };
    char *ev = build_auth_event(sk, SIGNET_AUTH_KIND, ch, "a", "signet-auth", now);
    assert(signet_auth_verify(cs, &fl, ev, now, NULL, NULL) == SIGNET_AUTH_ERR_DENIED);
    g_free(ev); g_free(ch); signet_challenge_store_free(cs);
  }

  /* Pubkey does not match agent_id. */
  {
    SignetChallengeStore *cs = signet_challenge_store_new();
    char *ch = signet_challenge_issue(cs, "a", now);
    FleetCtx fc = { pk, true, false, true /*wrong_pubkey*/ };
    SignetFleetRegistry fl = { tf_in_fleet, tf_denied, tf_get_pubkey, &fc };
    char *ev = build_auth_event(sk, SIGNET_AUTH_KIND, ch, "a", "signet-auth", now);
    assert(signet_auth_verify(cs, &fl, ev, now, NULL, NULL) == SIGNET_AUTH_ERR_PUBKEY_MISMATCH);
    g_free(ev); g_free(ch); signet_challenge_store_free(cs);
  }

  free(pk); free(sk);
  printf("test_verify_negatives: PASS\n");
}

/* ----------------------------- Challenge store --------------------------- */

static void test_challenge_issue(void) {
  SignetChallengeStore *cs = signet_challenge_store_new();
  assert(cs != NULL);

  int64_t now = (int64_t)time(NULL);
  char *c1 = signet_challenge_issue(cs, "agent-1", now);
  assert(c1 != NULL);
  assert(strlen(c1) == 64); /* 32 bytes → 64 hex chars */

  /* A second challenge for same agent should be different. */
  char *c2 = signet_challenge_issue(cs, "agent-1", now + 1);
  assert(c2 != NULL);
  assert(strcmp(c1, c2) != 0);

  g_free(c1);
  g_free(c2);
  signet_challenge_store_free(cs);
  printf("test_challenge_issue: PASS\n");
}

static void test_challenge_different_agents(void) {
  SignetChallengeStore *cs = signet_challenge_store_new();
  int64_t now = (int64_t)time(NULL);

  char *c1 = signet_challenge_issue(cs, "agent-a", now);
  char *c2 = signet_challenge_issue(cs, "agent-b", now);
  assert(c1 != NULL && c2 != NULL);
  assert(strcmp(c1, c2) != 0); /* unique per agent */

  g_free(c1);
  g_free(c2);
  signet_challenge_store_free(cs);
  printf("test_challenge_different_agents: PASS\n");
}

static void test_challenge_cleanup(void) {
  SignetChallengeStore *cs = signet_challenge_store_new();
  int64_t now = 1000000;

  /* Issue challenges at 'now'. */
  char *c1 = signet_challenge_issue(cs, "cleanup-a", now);
  char *c2 = signet_challenge_issue(cs, "cleanup-b", now);
  assert(c1 != NULL && c2 != NULL);
  g_free(c1);
  g_free(c2);

  /* Cleanup with a time far in the future should purge them. */
  signet_challenge_store_cleanup(cs, now + SIGNET_CHALLENGE_TTL_S + 10);

  /* Issuing again should still work (store is functional after cleanup). */
  char *c3 = signet_challenge_issue(cs, "cleanup-a", now + SIGNET_CHALLENGE_TTL_S + 20);
  assert(c3 != NULL);
  g_free(c3);

  signet_challenge_store_free(cs);
  printf("test_challenge_cleanup: PASS\n");
}

/* ----------------------------- Auth verify error paths ------------------- */

static void test_verify_null_inputs(void) {
  SignetChallengeStore *cs = signet_challenge_store_new();
  int64_t now = (int64_t)time(NULL);

  char *agent_id = NULL, *pubkey_hex = NULL;

  /* NULL event JSON should return an error. */
  SignetAuthResult r = signet_auth_verify(cs, NULL, NULL, now, &agent_id, &pubkey_hex);
  assert(r != SIGNET_AUTH_OK);

  /* Empty JSON string. */
  r = signet_auth_verify(cs, NULL, "", now, &agent_id, &pubkey_hex);
  assert(r != SIGNET_AUTH_OK);

  /* Garbage JSON. */
  r = signet_auth_verify(cs, NULL, "{not valid json at all", now, &agent_id, &pubkey_hex);
  assert(r != SIGNET_AUTH_OK);

  signet_challenge_store_free(cs);
  printf("test_verify_null_inputs: PASS\n");
}

/* ----------------------------- Auth result strings ----------------------- */

static void test_auth_result_strings(void) {
  /* Every enum value should have a non-NULL string. */
  assert(signet_auth_result_string(SIGNET_AUTH_OK) != NULL);
  assert(signet_auth_result_string(SIGNET_AUTH_ERR_INVALID_EVENT) != NULL);
  assert(signet_auth_result_string(SIGNET_AUTH_ERR_WRONG_KIND) != NULL);
  assert(signet_auth_result_string(SIGNET_AUTH_ERR_BAD_SIGNATURE) != NULL);
  assert(signet_auth_result_string(SIGNET_AUTH_ERR_MISSING_CHALLENGE) != NULL);
  assert(signet_auth_result_string(SIGNET_AUTH_ERR_CHALLENGE_MISMATCH) != NULL);
  assert(signet_auth_result_string(SIGNET_AUTH_ERR_CHALLENGE_EXPIRED) != NULL);
  assert(signet_auth_result_string(SIGNET_AUTH_ERR_CHALLENGE_REPLAYED) != NULL);
  assert(signet_auth_result_string(SIGNET_AUTH_ERR_MISSING_AGENT) != NULL);
  assert(signet_auth_result_string(SIGNET_AUTH_ERR_MISSING_PURPOSE) != NULL);
  assert(signet_auth_result_string(SIGNET_AUTH_ERR_WRONG_PURPOSE) != NULL);
  assert(signet_auth_result_string(SIGNET_AUTH_ERR_PUBKEY_MISMATCH) != NULL);
  assert(signet_auth_result_string(SIGNET_AUTH_ERR_NOT_IN_FLEET) != NULL);
  assert(signet_auth_result_string(SIGNET_AUTH_ERR_DENIED) != NULL);
  assert(signet_auth_result_string(SIGNET_AUTH_ERR_INTERNAL) != NULL);

  /* OK string should be human-readable. */
  const char *ok_str = signet_auth_result_string(SIGNET_AUTH_OK);
  assert(strlen(ok_str) > 0);

  printf("test_auth_result_strings: PASS\n");
}

/* ----------------------------- Null safety -------------------------------- */

static void test_null_safety(void) {
  signet_challenge_store_free(NULL); /* should not crash */

  /* Cleanup on NULL should not crash. */
  signet_challenge_store_cleanup(NULL, 0);

  printf("test_null_safety: PASS\n");
}

int main(void) {
  test_null_safety();
  test_challenge_issue();
  test_challenge_different_agents();
  test_challenge_cleanup();
  test_verify_null_inputs();
  test_verify_valid_and_replay();
  test_verify_negatives();
  test_auth_result_strings();

  printf("All nostr auth tests passed!\n");
  return 0;
}
