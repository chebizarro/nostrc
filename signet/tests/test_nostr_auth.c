/* SPDX-License-Identifier: MIT
 *
 * test_nostr_auth.c - Tests for SignetChallengeStore: issuance, TTL, cleanup.
 */

#include "signet/nostr_auth.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>

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
  test_auth_result_strings();

  printf("All nostr auth tests passed!\n");
  return 0;
}
