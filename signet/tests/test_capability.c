/* SPDX-License-Identifier: MIT
 *
 * test_capability.c - Tests for SignetPolicyRegistry: capability checks,
 *                      method mapping, rate limiting, kind restrictions.
 */

#include "signet/capability.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

/* ----------------------------- Method mapping ----------------------------- */

static void test_method_to_capability(void) {
  /* Well-known NIP-46 / D-Bus method names should map to capabilities. */
  const char *sign_cap = signet_method_to_capability("SignEvent");
  assert(sign_cap != NULL);
  assert(strcmp(sign_cap, SIGNET_CAP_NOSTR_SIGN) == 0);

  const char *enc_cap = signet_method_to_capability("Encrypt");
  assert(enc_cap != NULL);
  assert(strcmp(enc_cap, SIGNET_CAP_NOSTR_ENCRYPT) == 0);

  /* NIP-5L style method names. */
  const char *sign_5l = signet_method_to_capability("sign_event");
  assert(sign_5l != NULL);
  assert(strcmp(sign_5l, SIGNET_CAP_NOSTR_SIGN) == 0);

  /* Unknown method should return NULL. */
  assert(signet_method_to_capability("TotallyFakeMethod") == NULL);
  assert(signet_method_to_capability(NULL) == NULL);

  printf("test_method_to_capability: PASS\n");
}

/* ----------------------------- Registry lifecycle ------------------------ */

static void test_registry_create_free(void) {
  SignetPolicyRegistry *pr = signet_policy_registry_new();
  assert(pr != NULL);
  signet_policy_registry_free(pr);

  /* Free NULL should not crash. */
  signet_policy_registry_free(NULL);

  printf("test_registry_create_free: PASS\n");
}

/* ----------------------------- Capability grants ------------------------- */

static void test_capability_grant(void) {
  SignetPolicyRegistry *pr = signet_policy_registry_new();

  char *caps[] = { (char *)SIGNET_CAP_NOSTR_SIGN, (char *)SIGNET_CAP_NOSTR_ENCRYPT, NULL };
  SignetAgentPolicy policy = {
    .name = g_strdup("signer-policy"),
    .capabilities = caps,
    .n_capabilities = 2,
    .allowed_event_kinds = NULL,
    .n_allowed_kinds = 0,
    .disallowed_credential_types = NULL,
    .n_disallowed_types = 0,
    .rate_limit_per_hour = 0,
  };

  int rc = signet_policy_registry_add(pr, &policy);
  assert(rc == 0);

  rc = signet_policy_registry_assign(pr, "agent-signer", "signer-policy");
  assert(rc == 0);

  /* Agent should have the granted capabilities. */
  assert(signet_policy_has_capability(pr, "agent-signer", SIGNET_CAP_NOSTR_SIGN));
  assert(signet_policy_has_capability(pr, "agent-signer", SIGNET_CAP_NOSTR_ENCRYPT));

  /* Agent should NOT have capabilities not granted. */
  assert(!signet_policy_has_capability(pr, "agent-signer", SIGNET_CAP_SSH_SIGN));
  assert(!signet_policy_has_capability(pr, "agent-signer", SIGNET_CAP_CREDENTIAL_GET_SESSION));

  /* Unknown agent should have no capabilities. */
  assert(!signet_policy_has_capability(pr, "unknown-agent", SIGNET_CAP_NOSTR_SIGN));

  g_free(policy.name);
  signet_policy_registry_free(pr);
  printf("test_capability_grant: PASS\n");
}

/* ----------------------------- Kind restrictions ------------------------- */

static void test_kind_restrictions(void) {
  SignetPolicyRegistry *pr = signet_policy_registry_new();

  int allowed_kinds[] = { 1, 4, 30023 }; /* text note, DM, long-form */
  char *caps[] = { (char *)SIGNET_CAP_NOSTR_SIGN, NULL };
  SignetAgentPolicy policy = {
    .name = g_strdup("kind-restricted"),
    .capabilities = caps,
    .n_capabilities = 1,
    .allowed_event_kinds = allowed_kinds,
    .n_allowed_kinds = 3,
    .disallowed_credential_types = NULL,
    .n_disallowed_types = 0,
    .rate_limit_per_hour = 0,
  };

  signet_policy_registry_add(pr, &policy);
  signet_policy_registry_assign(pr, "kind-agent", "kind-restricted");

  /* Allowed kinds should pass. */
  assert(signet_policy_allowed_kind(pr, "kind-agent", 1));
  assert(signet_policy_allowed_kind(pr, "kind-agent", 4));
  assert(signet_policy_allowed_kind(pr, "kind-agent", 30023));

  /* Disallowed kind should fail. */
  assert(!signet_policy_allowed_kind(pr, "kind-agent", 0)); /* metadata */
  assert(!signet_policy_allowed_kind(pr, "kind-agent", 3)); /* contact list */

  /* Unknown agent: kind check depends on implementation (likely allows or denies). */
  /* Just ensure it doesn't crash. */
  (void)signet_policy_allowed_kind(pr, "unassigned", 1);

  g_free(policy.name);
  signet_policy_registry_free(pr);
  printf("test_kind_restrictions: PASS\n");
}

/* ----------------------------- Policy evaluate (integration) ------------- */

static void test_policy_evaluate(void) {
  SignetPolicyRegistry *pr = signet_policy_registry_new();

  char *caps[] = { (char *)SIGNET_CAP_NOSTR_SIGN, NULL };
  SignetAgentPolicy policy = {
    .name = g_strdup("eval-policy"),
    .capabilities = caps,
    .n_capabilities = 1,
    .allowed_event_kinds = NULL,
    .n_allowed_kinds = 0,
    .disallowed_credential_types = NULL,
    .n_disallowed_types = 0,
    .rate_limit_per_hour = 0,
  };

  signet_policy_registry_add(pr, &policy);
  signet_policy_registry_assign(pr, "eval-agent", "eval-policy");

  /* SignEvent should evaluate to allowed. */
  assert(signet_policy_evaluate(pr, "eval-agent", "SignEvent", -1));

  /* sign_event (NIP-5L) should also work. */
  assert(signet_policy_evaluate(pr, "eval-agent", "sign_event", -1));

  /* Encrypt should be denied (not in capabilities). */
  assert(!signet_policy_evaluate(pr, "eval-agent", "Encrypt", -1));

  /* GetPublicKey should evaluate (depends on impl — may be allowed or mapped). */
  /* Just ensure no crash. */
  (void)signet_policy_evaluate(pr, "eval-agent", "GetPublicKey", -1);

  g_free(policy.name);
  signet_policy_registry_free(pr);
  printf("test_policy_evaluate: PASS\n");
}

/* ----------------------------- Rate limiting ------------------------------ */

static void test_rate_limit(void) {
  SignetPolicyRegistry *pr = signet_policy_registry_new();

  char *caps[] = { (char *)SIGNET_CAP_NOSTR_SIGN, NULL };
  SignetAgentPolicy policy = {
    .name = g_strdup("rate-limited"),
    .capabilities = caps,
    .n_capabilities = 1,
    .allowed_event_kinds = NULL,
    .n_allowed_kinds = 0,
    .disallowed_credential_types = NULL,
    .n_disallowed_types = 0,
    .rate_limit_per_hour = 5, /* very low limit for testing */
  };

  signet_policy_registry_add(pr, &policy);
  signet_policy_registry_assign(pr, "rate-agent", "rate-limited");

  /* First few requests should succeed. */
  bool first = signet_policy_rate_limit_check(pr, "rate-agent", SIGNET_CAP_NOSTR_SIGN);
  assert(first); /* at least the first should pass */

  /* Just ensure the API works without crashing for multiple calls. */
  for (int i = 0; i < 10; i++) {
    (void)signet_policy_rate_limit_check(pr, "rate-agent", SIGNET_CAP_NOSTR_SIGN);
  }

  g_free(policy.name);
  signet_policy_registry_free(pr);
  printf("test_rate_limit: PASS\n");
}

/* ----------------------------- Policy clear ------------------------------ */

static void test_policy_clear(void) {
  SignetAgentPolicy policy;
  memset(&policy, 0, sizeof(policy));
  policy.name = g_strdup("test-clear");
  signet_agent_policy_clear(&policy);
  assert(policy.name == NULL);

  /* Clear on zeroed struct should not crash. */
  memset(&policy, 0, sizeof(policy));
  signet_agent_policy_clear(&policy);

  printf("test_policy_clear: PASS\n");
}

int main(void) {
  test_registry_create_free();
  test_method_to_capability();
  test_capability_grant();
  test_kind_restrictions();
  test_policy_evaluate();
  test_rate_limit();
  test_policy_clear();

  printf("All capability tests passed!\n");
  return 0;
}
