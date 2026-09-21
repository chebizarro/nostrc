#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "auth_ratelimit.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

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
  /* Persistence: NULL for in-memory mode. */
  sqlite3 *db;
  nh_auth_ratelimit_clock_fn clock_fn;
  void *clock_ctx;
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
  rl->db = NULL;
  rl->clock_fn = NULL;
  rl->clock_ctx = NULL;
  return rl;
}

/* ------------------------------------------------------------------ */
/* Persistence: SQLite backing                                        */
/* ------------------------------------------------------------------ */

/* Schema mirrors the identity_store / smb_credential idiom: WITHOUT ROWID
 * primary key, CHECK constraints on ranges, and a metadata table for the
 * schema version. The persisted timestamps are wall-clock milliseconds — see
 * auth_ratelimit.h for the rationale (the broker's monotonic clock resets
 * across reboots and can't be compared to prior-run values). */
static const char ratelimit_schema[] =
    "BEGIN IMMEDIATE;"
    "CREATE TABLE IF NOT EXISTS metadata("
    " key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS ratelimit_entries("
    " key TEXT PRIMARY KEY CHECK(length(key) BETWEEN 1 AND 255),"
    " failures INTEGER NOT NULL DEFAULT 0 CHECK(failures>=0),"
    " window_start_ms INTEGER NOT NULL DEFAULT 0 CHECK(window_start_ms>=0),"
    " cooldown_until_ms INTEGER NOT NULL DEFAULT 0 CHECK(cooldown_until_ms>=0)"
    ") WITHOUT ROWID;"
    "INSERT OR IGNORE INTO metadata(key,value)"
    " VALUES('schema_version','1');"
    "COMMIT;";

static uint64_t default_wall_ms(void *ctx) {
  (void)ctx;
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return (uint64_t)time(NULL) * 1000u;
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Resolve the "current time" for a mutation. Persistent mode has its own
 * wall-clock hook because the caller's now_ms is expressed in the broker's
 * monotonic domain and would be nonsensical across a restart. In-memory mode
 * still passes the caller's value straight through — that's the pre-existing
 * contract and tests rely on it. */
static uint64_t resolve_now_ms(const nh_auth_ratelimit *rl, uint64_t caller_now_ms) {
  if (!rl || !rl->db) return caller_now_ms;
  if (rl->clock_fn) return rl->clock_fn(rl->clock_ctx);
  return default_wall_ms(NULL);
}

static int persist_upsert(nh_auth_ratelimit *rl, const entry *e) {
  if (!rl || !rl->db || !e) return 0;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(rl->db,
      "INSERT INTO ratelimit_entries(key,failures,window_start_ms,cooldown_until_ms)"
      " VALUES(?,?,?,?)"
      " ON CONFLICT(key) DO UPDATE SET"
      "  failures=excluded.failures,"
      "  window_start_ms=excluded.window_start_ms,"
      "  cooldown_until_ms=excluded.cooldown_until_ms",
      -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;
  sqlite3_bind_text(stmt, 1, e->key, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)e->failures);
  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)e->window_start_ms);
  sqlite3_bind_int64(stmt, 4, (sqlite3_int64)e->cooldown_until_ms);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

static int persist_load_all(nh_auth_ratelimit *rl) {
  if (!rl || !rl->db) return 0;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(rl->db,
      "SELECT key,failures,window_start_ms,cooldown_until_ms FROM ratelimit_entries",
      -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const unsigned char *k = sqlite3_column_text(stmt, 0);
    if (!k) continue;
    entry *e = calloc(1, sizeof *e);
    if (!e) { sqlite3_finalize(stmt); return -1; }
    e->key = strdup((const char *)k);
    if (!e->key) { free(e); sqlite3_finalize(stmt); return -1; }
    e->failures = (unsigned)sqlite3_column_int64(stmt, 1);
    e->window_start_ms = (uint64_t)sqlite3_column_int64(stmt, 2);
    e->cooldown_until_ms = (uint64_t)sqlite3_column_int64(stmt, 3);
    e->next = rl->head;
    rl->head = e;
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

int nh_auth_ratelimit_open_persistent(const char *path,
                                      const nh_auth_ratelimit_config *cfg,
                                      nh_auth_ratelimit_clock_fn clock_fn,
                                      void *clock_ctx,
                                      nh_auth_ratelimit **out) {
  if (!out) return -1;
  *out = NULL;
  if (!path || !*path) return -1;
  nh_auth_ratelimit *rl = nh_auth_ratelimit_new(cfg);
  if (!rl) return -1;
  rl->clock_fn = clock_fn;
  rl->clock_ctx = clock_ctx;
  int rc = sqlite3_open_v2(path, &rl->db,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL);
  if (rc != SQLITE_OK) {
    if (rl->db) sqlite3_close(rl->db);
    rl->db = NULL;
    nh_auth_ratelimit_free(rl);
    return -1;
  }
  sqlite3_busy_timeout(rl->db, 250);
  (void)sqlite3_exec(rl->db,
      "PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;",
      NULL, NULL, NULL);
  if (sqlite3_exec(rl->db, ratelimit_schema, NULL, NULL, NULL) != SQLITE_OK) {
    nh_auth_ratelimit_free(rl);
    return -1;
  }
  if (persist_load_all(rl) != 0) {
    nh_auth_ratelimit_free(rl);
    return -1;
  }
  *out = rl;
  return 0;
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
  if (rl->db) sqlite3_close(rl->db);
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
  uint64_t now = resolve_now_ms(rl, now_ms);
  if (e->cooldown_until_ms != 0 && now < e->cooldown_until_ms) return 0;
  return 1;
}

void nh_auth_ratelimit_record_failure(nh_auth_ratelimit *rl, const char *key,
                                      uint64_t now_ms) {
  if (!rl || !key) return;
  entry *e = find_or_create(rl, key);
  if (!e) return; /* OOM: fail open — the broker will simply keep serving. */
  uint64_t now = resolve_now_ms(rl, now_ms);
  /* If we are already in a cooldown, this failure re-arms the cooldown from
   * the new now. Rate-limited requests are gated before this record is even
   * called, so in practice this fires only on genuine budget-exceeding fresh
   * failures; but arming from `now` again ensures a burst that races the
   * cooldown boundary does not immediately unlock. */
  if (e->cooldown_until_ms != 0 && now < e->cooldown_until_ms) {
    e->cooldown_until_ms = now + rl->config.cooldown_ms;
    (void)persist_upsert(rl, e);
    return;
  }
  /* Reset the window if we walked out of it or this is the first failure. */
  if (e->failures == 0 || now >= e->window_start_ms + rl->config.window_ms) {
    e->window_start_ms = now;
    e->failures = 0;
    e->cooldown_until_ms = 0;
  }
  e->failures++;
  if (e->failures >= rl->config.max_failures) {
    e->cooldown_until_ms = now + rl->config.cooldown_ms;
  }
  (void)persist_upsert(rl, e);
}

void nh_auth_ratelimit_reset(nh_auth_ratelimit *rl, const char *key) {
  if (!rl || !key) return;
  entry *e = find(rl, key);
  if (!e) return;
  e->failures = 0;
  e->window_start_ms = 0;
  e->cooldown_until_ms = 0;
  (void)persist_upsert(rl, e);
}
