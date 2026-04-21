/* SPDX-License-Identifier: MIT
 *
 * capability.c - Capability-based policy engine.
 *
 * Uses libnostr's token bucket (rate_limiter.h) for per-(agent, capability)
 * rate limiting.
 */

#include "signet/capability.h"

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <rate_limiter.h>

/* ----------------------------- internals --------------------------------- */

typedef struct {
  SignetAgentPolicy policy;
  char *agent_id;  /* NULL for policy templates, non-NULL for assigned */
} PolicyEntry;

/* Rate limiter key: "agent_id\0capability" */
typedef struct {
  nostr_token_bucket bucket;
} RateLimitEntry;

struct SignetPolicyRegistry {
  GHashTable *policies;     /* policy_name → SignetAgentPolicy* (templates) */
  GHashTable *assignments;  /* agent_id → policy_name */
  GHashTable *rate_limits;  /* "agent_id:capability" → RateLimitEntry* */
  GMutex mu;
};

static SignetAgentPolicy *signet_policy_copy(const SignetAgentPolicy *src) {
  SignetAgentPolicy *dst = g_new0(SignetAgentPolicy, 1);
  dst->name = g_strdup(src->name);
  dst->n_capabilities = src->n_capabilities;
  dst->capabilities = g_new0(char *, src->n_capabilities + 1);
  for (size_t i = 0; i < src->n_capabilities; i++)
    dst->capabilities[i] = g_strdup(src->capabilities[i]);
  if (src->n_allowed_kinds > 0 && src->allowed_event_kinds) {
    dst->allowed_event_kinds = g_new0(int, src->n_allowed_kinds);
    memcpy(dst->allowed_event_kinds, src->allowed_event_kinds, src->n_allowed_kinds * sizeof(int));
    dst->n_allowed_kinds = src->n_allowed_kinds;
  }
  if (src->n_disallowed_types > 0 && src->disallowed_credential_types) {
    dst->disallowed_credential_types = g_new0(char *, src->n_disallowed_types + 1);
    for (size_t i = 0; i < src->n_disallowed_types; i++)
      dst->disallowed_credential_types[i] = g_strdup(src->disallowed_credential_types[i]);
    dst->n_disallowed_types = src->n_disallowed_types;
  }
  dst->rate_limit_per_hour = src->rate_limit_per_hour;
  return dst;
}

static void signet_policy_free_cb(gpointer p) {
  SignetAgentPolicy *pol = (SignetAgentPolicy *)p;
  if (!pol) return;
  signet_agent_policy_clear(pol);
  g_free(pol);
}

/* ----------------------------- public API --------------------------------- */

SignetPolicyRegistry *signet_policy_registry_new(void) {
  SignetPolicyRegistry *pr = g_new0(SignetPolicyRegistry, 1);
  if (!pr) return NULL;
  g_mutex_init(&pr->mu);
  pr->policies = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        g_free, signet_policy_free_cb);
  pr->assignments = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, g_free);
  pr->rate_limits = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, g_free);
  return pr;
}

void signet_policy_registry_free(SignetPolicyRegistry *pr) {
  if (!pr) return;
  g_mutex_lock(&pr->mu);
  g_hash_table_destroy(pr->policies);
  g_hash_table_destroy(pr->assignments);
  g_hash_table_destroy(pr->rate_limits);
  g_mutex_unlock(&pr->mu);
  g_mutex_clear(&pr->mu);
  g_free(pr);
}

int signet_policy_registry_add(SignetPolicyRegistry *pr,
                                const SignetAgentPolicy *policy) {
  if (!pr || !policy || !policy->name) return -1;
  SignetAgentPolicy *copy = signet_policy_copy(policy);
  g_mutex_lock(&pr->mu);
  g_hash_table_replace(pr->policies, g_strdup(policy->name), copy);
  g_mutex_unlock(&pr->mu);
  return 0;
}

int signet_policy_registry_assign(SignetPolicyRegistry *pr,
                                   const char *agent_id,
                                   const char *policy_name) {
  if (!pr || !agent_id || !policy_name) return -1;
  g_mutex_lock(&pr->mu);
  if (!g_hash_table_contains(pr->policies, policy_name)) {
    g_mutex_unlock(&pr->mu);
    return -1; /* policy not found */
  }
  g_hash_table_replace(pr->assignments, g_strdup(agent_id), g_strdup(policy_name));
  g_mutex_unlock(&pr->mu);
  return 0;
}

/* Get agent's policy (must hold mu). Returns NULL if not assigned.
 * Falls back to wildcard assignment "*" if the specific agent_id is not found,
 * allowing a default policy for unprovisioned agents. */
static const SignetAgentPolicy *signet_policy_lookup_locked(SignetPolicyRegistry *pr,
                                                             const char *agent_id) {
  const char *name = (const char *)g_hash_table_lookup(pr->assignments, agent_id);
  if (!name) {
    /* Fallback: check for wildcard default assignment. */
    name = (const char *)g_hash_table_lookup(pr->assignments, "*");
  }
  if (!name) return NULL;
  return (const SignetAgentPolicy *)g_hash_table_lookup(pr->policies, name);
}

bool signet_policy_has_capability(SignetPolicyRegistry *pr,
                                   const char *agent_id,
                                   const char *capability) {
  if (!pr || !agent_id || !capability) return false;
  g_mutex_lock(&pr->mu);
  const SignetAgentPolicy *pol = signet_policy_lookup_locked(pr, agent_id);
  if (!pol) {
    g_mutex_unlock(&pr->mu);
    return false;
  }
  bool found = false;
  for (size_t i = 0; i < pol->n_capabilities; i++) {
    if (strcmp(pol->capabilities[i], capability) == 0) {
      found = true;
      break;
    }
  }
  g_mutex_unlock(&pr->mu);
  return found;
}

bool signet_policy_allowed_kind(SignetPolicyRegistry *pr,
                                 const char *agent_id,
                                 int event_kind) {
  if (!pr || !agent_id) return false;
  if (event_kind < 0) return true; /* wildcard / not applicable */
  g_mutex_lock(&pr->mu);
  const SignetAgentPolicy *pol = signet_policy_lookup_locked(pr, agent_id);
  if (!pol) {
    g_mutex_unlock(&pr->mu);
    return false;
  }
  /* Empty allowed_kinds = all kinds permitted. */
  if (pol->n_allowed_kinds == 0) {
    g_mutex_unlock(&pr->mu);
    return true;
  }
  bool found = false;
  for (size_t i = 0; i < pol->n_allowed_kinds; i++) {
    if (pol->allowed_event_kinds[i] == event_kind) {
      found = true;
      break;
    }
  }
  g_mutex_unlock(&pr->mu);
  return found;
}

bool signet_policy_rate_limit_check(SignetPolicyRegistry *pr,
                                     const char *agent_id,
                                     const char *capability) {
  if (!pr || !agent_id || !capability) return false;
  g_mutex_lock(&pr->mu);
  const SignetAgentPolicy *pol = signet_policy_lookup_locked(pr, agent_id);
  if (!pol || pol->rate_limit_per_hour == 0) {
    /* No rate limit configured. */
    g_mutex_unlock(&pr->mu);
    return true;
  }

  /* Look up or create rate limiter for this (agent_id, capability). */
  char *key = g_strdup_printf("%s:%s", agent_id, capability);
  RateLimitEntry *rle = (RateLimitEntry *)g_hash_table_lookup(pr->rate_limits, key);
  if (!rle) {
    rle = g_new0(RateLimitEntry, 1);
    /* rate = tokens/sec = limit_per_hour / 3600 */
    double rate = (double)pol->rate_limit_per_hour / 3600.0;
    /* burst = allow short bursts up to 10% of hourly limit, min 1 */
    double burst = (double)pol->rate_limit_per_hour / 10.0;
    if (burst < 1.0) burst = 1.0;
    tb_init(&rle->bucket, rate, burst);
    g_hash_table_replace(pr->rate_limits, g_strdup(key), rle);
  }

  bool allowed = tb_allow(&rle->bucket, 1.0);
  g_free(key);
  g_mutex_unlock(&pr->mu);
  return allowed;
}

const char *signet_method_to_capability(const char *method) {
  if (!method) return NULL;
  if (strcmp(method, "sign_event") == 0)    return SIGNET_CAP_NOSTR_SIGN;
  if (strcmp(method, "SignEvent") == 0)     return SIGNET_CAP_NOSTR_SIGN;
  if (strcmp(method, "get_public_key") == 0) return SIGNET_CAP_NOSTR_SIGN;
  if (strcmp(method, "GetPublicKey") == 0)  return SIGNET_CAP_NOSTR_SIGN;
  if (strcmp(method, "nip04_encrypt") == 0) return SIGNET_CAP_NOSTR_ENCRYPT;
  if (strcmp(method, "nip04_decrypt") == 0) return SIGNET_CAP_NOSTR_ENCRYPT;
  if (strcmp(method, "nip44_encrypt") == 0) return SIGNET_CAP_NOSTR_ENCRYPT;
  if (strcmp(method, "nip44_decrypt") == 0) return SIGNET_CAP_NOSTR_ENCRYPT;
  if (strcmp(method, "Encrypt") == 0)      return SIGNET_CAP_NOSTR_ENCRYPT;
  if (strcmp(method, "Decrypt") == 0)      return SIGNET_CAP_NOSTR_ENCRYPT;
  if (strcmp(method, "GetToken") == 0)     return SIGNET_CAP_CREDENTIAL_GET_TOKEN;
  if (strcmp(method, "GetSession") == 0)   return SIGNET_CAP_CREDENTIAL_GET_SESSION;
  if (strcmp(method, "connect") == 0)      return NULL; /* connect is always allowed */
  if (strcmp(method, "ping") == 0)         return NULL;
  if (strcmp(method, "get_relays") == 0)   return NULL;
  return NULL; /* unknown method — deny by default at call site */
}

bool signet_policy_evaluate(SignetPolicyRegistry *pr,
                             const char *agent_id,
                             const char *method,
                             int event_kind) {
  if (!pr || !agent_id || !method) return false;

  const char *cap = signet_method_to_capability(method);
  if (!cap) return true; /* Methods without capability requirement are open. */

  if (!signet_policy_has_capability(pr, agent_id, cap)) return false;

  /* For nostr.sign, also check event kind. */
  if (strcmp(cap, SIGNET_CAP_NOSTR_SIGN) == 0 && event_kind >= 0) {
    if (!signet_policy_allowed_kind(pr, agent_id, event_kind)) return false;
  }

  if (!signet_policy_rate_limit_check(pr, agent_id, cap)) return false;

  return true;
}

void signet_agent_policy_clear(SignetAgentPolicy *policy) {
  if (!policy) return;
  g_free(policy->name);
  policy->name = NULL;
  if (policy->capabilities) {
    for (size_t i = 0; i < policy->n_capabilities; i++)
      g_free(policy->capabilities[i]);
    g_free(policy->capabilities);
    policy->capabilities = NULL;
  }
  policy->n_capabilities = 0;
  g_free(policy->allowed_event_kinds);
  policy->allowed_event_kinds = NULL;
  policy->n_allowed_kinds = 0;
  if (policy->disallowed_credential_types) {
    for (size_t i = 0; i < policy->n_disallowed_types; i++)
      g_free(policy->disallowed_credential_types[i]);
    g_free(policy->disallowed_credential_types);
    policy->disallowed_credential_types = NULL;
  }
  policy->n_disallowed_types = 0;
  policy->rate_limit_per_hour = 0;
}
