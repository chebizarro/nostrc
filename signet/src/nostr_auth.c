/* SPDX-License-Identifier: MIT
 *
 * nostr_auth.c - Shared Nostr-signed challenge validator.
 *
 * Uses libnostr NostrEvent for deserialization and signature verification.
 * Challenge management is in-memory with TTL cleanup.
 */

#include "signet/nostr_auth.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <sodium.h>

/* libnostr */
#include <nostr-event.h>
#include <nostr-keys.h>
#include <json.h>

/* ----------------------------- challenge store --------------------------- */

struct SignetChallengeStore {
  GHashTable *challenges;  /* challenge_hex → SignetChallenge* */
  GMutex mu;
};

static void signet_challenge_free(gpointer p) {
  SignetChallenge *c = (SignetChallenge *)p;
  if (!c) return;
  g_free(c->challenge_hex);
  g_free(c->agent_id);
  g_free(c);
}

SignetChallengeStore *signet_challenge_store_new(void) {
  SignetChallengeStore *cs = g_new0(SignetChallengeStore, 1);
  if (!cs) return NULL;
  g_mutex_init(&cs->mu);
  cs->challenges = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, signet_challenge_free);
  return cs;
}

void signet_challenge_store_free(SignetChallengeStore *cs) {
  if (!cs) return;
  g_mutex_lock(&cs->mu);
  if (cs->challenges) {
    g_hash_table_destroy(cs->challenges);
    cs->challenges = NULL;
  }
  g_mutex_unlock(&cs->mu);
  g_mutex_clear(&cs->mu);
  g_free(cs);
}

char *signet_challenge_issue(SignetChallengeStore *cs,
                              const char *agent_id,
                              int64_t now) {
  if (!cs || !agent_id) return NULL;

  /* Generate 32 random bytes → 64-char hex. */
  uint8_t raw[32];
  randombytes_buf(raw, sizeof(raw));

  char *hex = g_malloc(65);
  for (int i = 0; i < 32; i++) {
    sprintf(hex + i * 2, "%02x", raw[i]);
  }
  hex[64] = '\0';
  sodium_memzero(raw, sizeof(raw));

  SignetChallenge *ch = g_new0(SignetChallenge, 1);
  ch->challenge_hex = g_strdup(hex);
  ch->agent_id = g_strdup(agent_id);
  ch->issued_at = now;
  ch->consumed = false;

  g_mutex_lock(&cs->mu);
  g_hash_table_replace(cs->challenges, g_strdup(hex), ch);
  g_mutex_unlock(&cs->mu);

  return hex;
}

/* ----------------------------- tag extraction ----------------------------- */

static const char *signet_event_get_tag_value(NostrEvent *evt, const char *tag_name) {
  if (!evt || !tag_name) return NULL;

  NostrTags *tags = nostr_event_get_tags(evt);
  if (!tags) return NULL;

  size_t n = nostr_tags_size(tags);
  for (size_t i = 0; i < n; i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag) continue;
    size_t tn = nostr_tag_size(tag);
    if (tn < 2) continue;
    const char *name = nostr_tag_get(tag, 0);
    if (name && strcmp(name, tag_name) == 0) {
      return nostr_tag_get(tag, 1);
    }
  }
  return NULL;
}

/* ----------------------------- verification ------------------------------ */

SignetAuthResult signet_auth_verify(SignetChallengeStore *cs,
                                    const SignetFleetRegistry *fleet,
                                    const char *auth_event_json,
                                    int64_t now,
                                    char **out_agent_id,
                                    char **out_pubkey_hex) {
  if (out_agent_id) *out_agent_id = NULL;
  if (out_pubkey_hex) *out_pubkey_hex = NULL;

  if (!cs || !auth_event_json) return SIGNET_AUTH_ERR_INVALID_EVENT;

  /* 1. Deserialize the event. */
  NostrEvent *evt = nostr_event_new();
  if (!evt) return SIGNET_AUTH_ERR_INTERNAL;

  if (!nostr_event_deserialize(evt, auth_event_json)) {
    nostr_event_free(evt);
    return SIGNET_AUTH_ERR_INVALID_EVENT;
  }

  /* 2. Check kind. */
  int kind = nostr_event_get_kind(evt);
  if (kind != SIGNET_AUTH_KIND) {
    nostr_event_free(evt);
    return SIGNET_AUTH_ERR_WRONG_KIND;
  }

  /* 3. Verify signature. */
  if (!nostr_event_check_signature(evt)) {
    nostr_event_free(evt);
    return SIGNET_AUTH_ERR_BAD_SIGNATURE;
  }

  /* 4. Extract tags. */
  const char *challenge = signet_event_get_tag_value(evt, "challenge");
  const char *agent_id = signet_event_get_tag_value(evt, "agent");
  const char *purpose = signet_event_get_tag_value(evt, "purpose");
  const char *pubkey = nostr_event_get_pubkey(evt);

  if (!challenge || !challenge[0]) {
    nostr_event_free(evt);
    return SIGNET_AUTH_ERR_MISSING_CHALLENGE;
  }
  if (!agent_id || !agent_id[0]) {
    nostr_event_free(evt);
    return SIGNET_AUTH_ERR_MISSING_AGENT;
  }
  if (!purpose || !purpose[0]) {
    nostr_event_free(evt);
    return SIGNET_AUTH_ERR_MISSING_PURPOSE;
  }
  if (strcmp(purpose, "signet-auth") != 0) {
    nostr_event_free(evt);
    return SIGNET_AUTH_ERR_WRONG_PURPOSE;
  }

  /* 5. Challenge lookup and validation. */
  g_mutex_lock(&cs->mu);
  SignetChallenge *ch = (SignetChallenge *)g_hash_table_lookup(cs->challenges, challenge);

  SignetAuthResult result = SIGNET_AUTH_OK;

  if (!ch) {
    result = SIGNET_AUTH_ERR_CHALLENGE_MISMATCH;
  } else if (ch->consumed) {
    result = SIGNET_AUTH_ERR_CHALLENGE_REPLAYED;
  } else if (now - ch->issued_at > SIGNET_CHALLENGE_TTL_S) {
    result = SIGNET_AUTH_ERR_CHALLENGE_EXPIRED;
  } else if (strcmp(ch->agent_id, agent_id) != 0) {
    result = SIGNET_AUTH_ERR_CHALLENGE_MISMATCH;
  }

  if (result == SIGNET_AUTH_OK && ch) {
    /* Mark consumed (single-use). */
    ch->consumed = true;
  }
  g_mutex_unlock(&cs->mu);

  if (result != SIGNET_AUTH_OK) {
    nostr_event_free(evt);
    return result;
  }

  /* 6. Fleet registry checks. */
  if (fleet) {
    /* Check deny list first (highest precedence). */
    if (fleet->is_denied && fleet->is_denied(pubkey, fleet->user_data)) {
      nostr_event_free(evt);
      return SIGNET_AUTH_ERR_DENIED;
    }

    /* Check pubkey matches agent_id. */
    if (fleet->get_agent_pubkey) {
      char *expected = fleet->get_agent_pubkey(agent_id, fleet->user_data);
      if (expected) {
        bool match = (g_ascii_strcasecmp(expected, pubkey) == 0);
        g_free(expected);
        if (!match) {
          nostr_event_free(evt);
          return SIGNET_AUTH_ERR_PUBKEY_MISMATCH;
        }
      }
    }

    /* Check fleet membership. */
    if (fleet->is_in_fleet && !fleet->is_in_fleet(pubkey, fleet->user_data)) {
      nostr_event_free(evt);
      return SIGNET_AUTH_ERR_NOT_IN_FLEET;
    }
  }

  /* Success — return agent_id and pubkey. */
  if (out_agent_id) *out_agent_id = g_strdup(agent_id);
  if (out_pubkey_hex) *out_pubkey_hex = g_strdup(pubkey);

  nostr_event_free(evt);
  return SIGNET_AUTH_OK;
}

void signet_challenge_store_cleanup(SignetChallengeStore *cs, int64_t now) {
  if (!cs) return;

  g_mutex_lock(&cs->mu);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, cs->challenges);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    SignetChallenge *ch = (SignetChallenge *)value;
    if (now - ch->issued_at > SIGNET_CHALLENGE_TTL_S * 2) {
      /* Remove challenges that are well past expiry. */
      g_hash_table_iter_remove(&iter);
    }
  }

  g_mutex_unlock(&cs->mu);
}

const char *signet_auth_result_string(SignetAuthResult r) {
  switch (r) {
    case SIGNET_AUTH_OK:                   return "ok";
    case SIGNET_AUTH_ERR_INVALID_EVENT:    return "invalid event";
    case SIGNET_AUTH_ERR_WRONG_KIND:       return "wrong event kind";
    case SIGNET_AUTH_ERR_BAD_SIGNATURE:    return "bad signature";
    case SIGNET_AUTH_ERR_MISSING_CHALLENGE:return "missing challenge tag";
    case SIGNET_AUTH_ERR_CHALLENGE_MISMATCH:return "challenge mismatch";
    case SIGNET_AUTH_ERR_CHALLENGE_EXPIRED:return "challenge expired";
    case SIGNET_AUTH_ERR_CHALLENGE_REPLAYED:return "challenge replayed";
    case SIGNET_AUTH_ERR_MISSING_AGENT:   return "missing agent tag";
    case SIGNET_AUTH_ERR_MISSING_PURPOSE: return "missing purpose tag";
    case SIGNET_AUTH_ERR_WRONG_PURPOSE:   return "wrong purpose (expected signet-auth)";
    case SIGNET_AUTH_ERR_PUBKEY_MISMATCH: return "pubkey does not match agent_id";
    case SIGNET_AUTH_ERR_NOT_IN_FLEET:    return "not in fleet registry";
    case SIGNET_AUTH_ERR_DENIED:          return "agent is in deny list";
    case SIGNET_AUTH_ERR_INTERNAL:        return "internal error";
    default:                              return "unknown error";
  }
}
