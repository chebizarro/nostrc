#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "auth_ratelimit.h"

#include <stdlib.h>
#include <string.h>

/* Small closed-hashing / linked-list hybrid is overkill for the expected
 * number of concurrently-active accounts on a home directory host. A plain
 * singly-linked list keeps the module trivial, avoids depending on any
 * hash-table helper library, and remains O(N) in the number of accounts
 * that have failed at least once in the current window — vanishingly small
 * in practice. */
typedef struct entry {
  struct entry *next;
  char *key;
  uint64_t window_start_ms;  /* First failure in the current window. */
  uint64_t cooldown_until_ms; /* 0 => not in cooldown. */
  unsigned failures;         /* Failures inside [window_start, window_start+window). */
} entry;

struct nh_auth_ratelimit {
  nh_auth_ratelimit_config config;
  entry *head;
};

void nh_auth_ratelimit_config_defaults(nh_auth_ratelimit_config *out) {
  if (!out) return;
  out->max_failures = NH_AUTH_RATELIMIT_DEFAULT_MAX_FAILURES;
  out->window_ms = NH_AUTH_RATELIMIT_DEFAULT_WINDOW_MS;
  out->cooldown_ms = NH_AUTH_RATELIMIT_DEFAULT_COOLDOWN_MS;
}

nh_auth_ratelimit *nh_auth_ratelimit_new(const nh_auth_ratelimit_config *cfg) {
  nh_auth_ratelimit_config c;
  if (cfg) {
    c = *cfg;
  } else {
    nh_auth_ratelimit_config_defaults(&c);
  }
  if (c.max_failures == 0) return NULL;
  nh_auth_ratelimit *rl = calloc(1, sizeof *rl);
  if (!rl) return NULL;
  rl->config = c;
  rl->head = NULL;
  return rl;
}

void nh_auth_ratelimit_free(nh_auth_ratelimit *rl) {
  if (!rl) return;
  entry *cur = rl->head;
  while (cur) {
    entry *next = cur->next;
    free(cur->key);
    free(cur);
    cur = next;
  }
  free(rl);
}

static entry *find(nh_auth_ratelimit *rl, const char *key) {
  for (entry *e = rl->head; e; e = e->next) {
    if (strcmp(e->key, key) == 0) return e;
  }
  return NULL;
}

static entry *find_or_create(nh_auth_ratelimit *rl, const char *key) {
  entry *e = find(rl, key);
  if (e) return e;
  e = calloc(1, sizeof *e);
  if (!e) return NULL;
  e->key = strdup(key);
  if (!e->key) { free(e); return NULL; }
  e->next = rl->head;
  rl->head = e;
  return e;
}

int nh_auth_ratelimit_check(nh_auth_ratelimit *rl, const char *key,
                            uint64_t now_ms) {
  if (!rl || !key) return 1;
  entry *e = find(rl, key);
  if (!e) return 1;
  if (e->cooldown_until_ms != 0 && now_ms < e->cooldown_until_ms) return 0;
  return 1;
}

void nh_auth_ratelimit_record_failure(nh_auth_ratelimit *rl, const char *key,
                                      uint64_t now_ms) {
  if (!rl || !key) return;
  entry *e = find_or_create(rl, key);
  if (!e) return; /* OOM: fail open — the broker will simply keep serving. */
  /* If we are already in a cooldown, this failure re-arms the cooldown from
   * the new now. Rate-limited requests are gated before this record is even
   * called, so in practice this fires only on genuine budget-exceeding fresh
   * failures; but arming from `now` again ensures a burst that races the
   * cooldown boundary does not immediately unlock. */
  if (e->cooldown_until_ms != 0 && now_ms < e->cooldown_until_ms) {
    e->cooldown_until_ms = now_ms + rl->config.cooldown_ms;
    return;
  }
  /* Reset the window if we walked out of it or this is the first failure. */
  if (e->failures == 0 || now_ms >= e->window_start_ms + rl->config.window_ms) {
    e->window_start_ms = now_ms;
    e->failures = 0;
    e->cooldown_until_ms = 0;
  }
  e->failures++;
  if (e->failures >= rl->config.max_failures) {
    e->cooldown_until_ms = now_ms + rl->config.cooldown_ms;
  }
}

void nh_auth_ratelimit_reset(nh_auth_ratelimit *rl, const char *key) {
  if (!rl || !key) return;
  entry *e = find(rl, key);
  if (!e) return;
  e->failures = 0;
  e->window_start_ms = 0;
  e->cooldown_until_ms = 0;
}
