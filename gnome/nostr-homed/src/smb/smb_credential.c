/*
 * SMB credential authority core.  See smb_credential.h for the
 * contract.  This file owns the SQLite issuance journal, the CSPRNG
 * password mint, and the coupling with the passdb adapter.  It never
 * touches Samba directly; that is the tdbsam adapter's job.
 *
 * beads nostrc-rb0e.6.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "smb_credential.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__) || defined(__GLIBC__)
#  include <sys/random.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#  include <sys/random.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#define NH_SMB_ERROR_DETAIL_CAP 256u

struct nh_smb_authority {
  sqlite3 *db;
  const nh_smb_passdb_ops *ops;
  void *ops_ctx;
  char error_detail[NH_SMB_ERROR_DETAIL_CAP];
};

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

const char *nh_smb_rc_name(nh_smb_rc rc) {
  switch (rc) {
    case NH_SMB_OK: return "ok";
    case NH_SMB_INVALID: return "invalid";
    case NH_SMB_NO_MEMORY: return "no-memory";
    case NH_SMB_STORAGE_ERROR: return "storage-error";
    case NH_SMB_PASSDB_ERROR: return "passdb-error";
    case NH_SMB_NOT_FOUND: return "not-found";
    case NH_SMB_INTERNAL: return "internal";
    case NH_SMB_RECONCILE_REQUIRED: return "reconcile-required";
  }
  return "unknown";
}

/* Forward decl — sweep-on-open (plan §4.1 A3) calls this from open()
 * which is defined before the wall-clock helper below. */
static uint64_t wall_ms_now(void);

static void set_error(nh_smb_authority *a, const char *fmt, ...) {
  if (!a) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(a->error_detail, sizeof a->error_detail, fmt, ap);
  va_end(ap);
}

const char *nh_smb_authority_error_detail(const nh_smb_authority *a) {
  if (!a) return "";
  return a->error_detail;
}

/* ------------------------------------------------------------------ */
/* Random primitives                                                   */
/* ------------------------------------------------------------------ */

/* Fill `buf` with `n` cryptographically random bytes.  Uses getrandom(2)
 * with a retry loop on EINTR; falls back to /dev/urandom on ENOSYS
 * (older kernels/musl).  Returns 0 on success, -1 on failure. */
static int csprng_bytes(void *buf, size_t n) {
  unsigned char *p = (unsigned char *)buf;
  size_t got = 0;
#if defined(__linux__) && defined(__GLIBC__) && defined(SYS_getrandom)
  /* Prefer getrandom(2) on Linux/glibc — no fd to leak, no /dev
   * dependency inside a chroot. */
  while (got < n) {
    ssize_t r = getrandom(p + got, n - got, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      if (errno == ENOSYS) break; /* older kernel: fall through */
      return -1;
    }
    got += (size_t)r;
  }
#elif defined(__APPLE__)
  /* macOS getentropy is capped at 256 bytes per call. */
  while (got < n) {
    size_t chunk = n - got > 256 ? 256 : n - got;
    if (getentropy(p + got, chunk) != 0) {
      if (errno == EINTR) continue;
      break;
    }
    got += chunk;
  }
#endif
  if (got == n) return 0;
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  while (got < n) {
    ssize_t r = read(fd, p + got, n - got);
    if (r < 0) {
      if (errno == EINTR) continue;
      close(fd);
      return -1;
    }
    if (r == 0) { close(fd); return -1; }
    got += (size_t)r;
  }
  close(fd);
  return 0;
}

static const char base64url_alphabet[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789-_";

/* base64url without padding.  `dst` must hold ceil(in_len*4/3)+1 bytes. */
static size_t base64url_encode(const uint8_t *in, size_t in_len, char *dst) {
  size_t i = 0, o = 0;
  while (i + 3 <= in_len) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
    dst[o++] = base64url_alphabet[(v >> 18) & 0x3f];
    dst[o++] = base64url_alphabet[(v >> 12) & 0x3f];
    dst[o++] = base64url_alphabet[(v >> 6) & 0x3f];
    dst[o++] = base64url_alphabet[v & 0x3f];
    i += 3;
  }
  if (i < in_len) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < in_len) v |= (uint32_t)in[i+1] << 8;
    dst[o++] = base64url_alphabet[(v >> 18) & 0x3f];
    dst[o++] = base64url_alphabet[(v >> 12) & 0x3f];
    if (i + 1 < in_len)
      dst[o++] = base64url_alphabet[(v >> 6) & 0x3f];
  }
  dst[o] = '\0';
  return o;
}

/* Draw a random UUIDv4-ish string (36 chars, canonical hyphenation) using
 * CSPRNG bytes.  The credential_id is not a security boundary — the
 * journal itself is — but a proper UUID keeps it collision-free. */
static int mint_credential_id(char out[NH_SMB_CREDENTIAL_ID_CAP]) {
  uint8_t b[16];
  if (csprng_bytes(b, sizeof b) != 0) return -1;
  b[6] = (uint8_t)((b[6] & 0x0f) | 0x40); /* version 4 */
  b[8] = (uint8_t)((b[8] & 0x3f) | 0x80); /* variant 10 */
  int n = snprintf(out, NH_SMB_CREDENTIAL_ID_CAP,
    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
    b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  if (n != (int)NH_SMB_CREDENTIAL_ID_LEN) return -1;
  return 0;
}

/* Mint a fresh SMB password.  Writes NUL-terminated ASCII into `out`
 * (base64url of NH_SMB_PASSWORD_ENTROPY_BYTES bytes).  Returns the
 * length written on success (excluding NUL), or 0 on failure. */
static size_t mint_password(char *out, size_t out_cap) {
  uint8_t raw[NH_SMB_PASSWORD_ENTROPY_BYTES];
  size_t need = ((sizeof raw + 2) / 3) * 4 + 1; /* worst-case */
  if (out_cap < need) return 0;
  if (csprng_bytes(raw, sizeof raw) != 0) {
    secure_wipe(raw, sizeof raw);
    return 0;
  }
  size_t n = base64url_encode(raw, sizeof raw, out);
  secure_wipe(raw, sizeof raw);
  if (n < NH_SMB_PASSWORD_MIN_LEN || n > NH_SMB_PASSWORD_MAX_LEN) {
    secure_wipe(out, out_cap);
    return 0;
  }
  return n;
}

/* ------------------------------------------------------------------ */
/* SQLite journal                                                      */
/* ------------------------------------------------------------------ */

static const char journal_schema[] =
    "BEGIN IMMEDIATE;"
    "CREATE TABLE IF NOT EXISTS metadata("
    " key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS credentials("
    " credential_id TEXT PRIMARY KEY CHECK(length(credential_id)=36),"
    " username TEXT NOT NULL CHECK(length(username) BETWEEN 1 AND 32),"
    " uid INTEGER NOT NULL CHECK(uid BETWEEN 1 AND 4294967295),"
    " pubkey_hex TEXT NOT NULL CHECK(length(pubkey_hex)=64),"
    " binding TEXT NOT NULL DEFAULT '',"
    " issued_at_ms INTEGER NOT NULL CHECK(issued_at_ms>=0),"
    " expires_at_ms INTEGER NOT NULL CHECK(expires_at_ms>issued_at_ms),"
    " revoked_at_ms INTEGER,"
    " revoke_reason INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS adopted_passdb("
    " username TEXT PRIMARY KEY CHECK(length(username) BETWEEN 1 AND 32))"
    " WITHOUT ROWID;"
    "CREATE UNIQUE INDEX IF NOT EXISTS credentials_one_active"
    " ON credentials(username) WHERE revoked_at_ms IS NULL;"
    "CREATE INDEX IF NOT EXISTS credentials_expiry"
    " ON credentials(expires_at_ms) WHERE revoked_at_ms IS NULL;"
    "INSERT OR IGNORE INTO metadata(key,value)"
    " VALUES('schema_version','1');"
    "COMMIT;";

static nh_smb_rc exec_sql(nh_smb_authority *a, const char *sql,
                          const char *op) {
  char *err = NULL;
  int rc = sqlite3_exec(a->db, sql, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    set_error(a, "%s: %s", op, err ? err : sqlite3_errmsg(a->db));
    sqlite3_free(err);
    return NH_SMB_STORAGE_ERROR;
  }
  return NH_SMB_OK;
}

/* ------------------------------------------------------------------ */
/* Envelope                                                            */
/* ------------------------------------------------------------------ */

void nh_smb_envelope_clear(nh_smb_envelope *e) {
  if (!e) return;
  if (e->password.ptr) {
    /* secure_free wipes and unlocks. */
    secure_free(&e->password);
  }
  secure_wipe(e->credential_id, sizeof e->credential_id);
  secure_wipe(e->username, sizeof e->username);
  e->password_len = 0;
  e->issued_at_ms = 0;
  e->expires_at_ms = 0;
  e->consumed = false;
}

/* ------------------------------------------------------------------ */
/* Passdb helpers                                                      */
/* ------------------------------------------------------------------ */

static int passdb_set_password(nh_smb_authority *a, const char *username,
                               const char *password) {
  if (!a->ops || !a->ops->set_password) return -1;
  return a->ops->set_password(a->ops_ctx, username, password);
}

static int passdb_disable(nh_smb_authority *a, const char *username) {
  if (!a->ops || !a->ops->disable) return -1;
  return a->ops->disable(a->ops_ctx, username);
}

static int passdb_remove(nh_smb_authority *a, const char *username) {
  if (!a->ops || !a->ops->remove) return -1;
  return a->ops->remove(a->ops_ctx, username);
}

/* ------------------------------------------------------------------ */
/* Journal helpers                                                     */
/* ------------------------------------------------------------------ */

static nh_smb_rc journal_mark_revoked(nh_smb_authority *a, const char *username,
                                      uint64_t now_ms,
                                      nh_smb_revoke_reason reason,
                                      bool *marked_out) {
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(a->db,
      "UPDATE credentials SET revoked_at_ms=?, revoke_reason=?"
      " WHERE username=? AND revoked_at_ms IS NULL",
      -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    set_error(a, "revoke prepare: %s", sqlite3_errmsg(a->db));
    return NH_SMB_STORAGE_ERROR;
  }
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)now_ms);
  sqlite3_bind_int(stmt, 2, (int)reason);
  sqlite3_bind_text(stmt, 3, username, -1, SQLITE_STATIC);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    set_error(a, "revoke step: %s", sqlite3_errmsg(a->db));
    return NH_SMB_STORAGE_ERROR;
  }
  if (marked_out) *marked_out = sqlite3_changes(a->db) > 0;
  return NH_SMB_OK;
}

static nh_smb_rc journal_forget_adopted(nh_smb_authority *a,
                                         const char *username) {
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(a->db,
      "DELETE FROM adopted_passdb WHERE username=?", -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    set_error(a, "forget adopted account %s: %s", username,
              sqlite3_errmsg(a->db));
    return NH_SMB_STORAGE_ERROR;
  }
  return NH_SMB_OK;
}

static nh_smb_rc journal_insert(nh_smb_authority *a,
                                const char *credential_id,
                                const nh_smb_issue_request *req,
                                uint64_t expires_at_ms) {
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(a->db,
      "INSERT INTO credentials(credential_id,username,uid,pubkey_hex,"
      "binding,issued_at_ms,expires_at_ms) VALUES(?,?,?,?,?,?,?)",
      -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    set_error(a, "insert prepare: %s", sqlite3_errmsg(a->db));
    return NH_SMB_STORAGE_ERROR;
  }
  sqlite3_bind_text(stmt, 1, credential_id, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, req->account.username, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)req->account.uid);
  sqlite3_bind_text(stmt, 4, req->account.pubkey_hex, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 5, req->binding ? req->binding : "", -1,
                    SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 6, (sqlite3_int64)req->now_monotonic_ms);
  sqlite3_bind_int64(stmt, 7, (sqlite3_int64)expires_at_ms);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    set_error(a, "insert step: %s", sqlite3_errmsg(a->db));
    return NH_SMB_STORAGE_ERROR;
  }
  return NH_SMB_OK;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Forward decl for the reconciliation helper defined further down. */
static nh_smb_rc reconcile_passdb_against_journal(nh_smb_authority *a);
static nh_smb_rc bootstrap_or_reconcile_passdb(nh_smb_authority *a);

nh_smb_rc nh_smb_authority_open(const char *journal_path,
                                const nh_smb_passdb_ops *passdb_ops,
                                void *passdb_ctx,
                                nh_smb_authority **out) {
  return nh_smb_authority_open_ex(journal_path, NULL, passdb_ops,
                                  passdb_ctx, out);
}

nh_smb_rc nh_smb_authority_open_ex(const char *journal_path,
                                   const char *smb_conf_path,
                                   const nh_smb_passdb_ops *passdb_ops,
                                   void *passdb_ctx,
                                   nh_smb_authority **out) {
  if (!journal_path || !passdb_ops || !passdb_ops->set_password ||
      !passdb_ops->disable || !passdb_ops->remove || !out)
    return NH_SMB_INVALID;
  *out = NULL;
  nh_smb_authority *a = calloc(1, sizeof *a);
  if (!a) return NH_SMB_NO_MEMORY;
  a->ops = passdb_ops;
  a->ops_ctx = passdb_ctx;

  int rc = sqlite3_open_v2(journal_path, &a->db,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
      NULL);
  if (rc != SQLITE_OK) {
    set_error(a, "sqlite3_open_v2(%s): %s", journal_path,
              a->db ? sqlite3_errmsg(a->db) : sqlite3_errstr(rc));
    if (a->db) sqlite3_close(a->db);
    free(a);
    return NH_SMB_STORAGE_ERROR;
  }
  sqlite3_busy_timeout(a->db, 250);
  (void)sqlite3_exec(a->db, "PRAGMA journal_mode=WAL;PRAGMA foreign_keys=ON;"
                            "PRAGMA synchronous=FULL;",
                     NULL, NULL, NULL);

  nh_smb_rc r = exec_sql(a, journal_schema, "install schema");
  if (r != NH_SMB_OK) {
    sqlite3_close(a->db);
    free(a);
    return r;
  }
  /* Plan §4.2 B3 (2026-09-25): reconcile the dedicated Samba passdb
   * against the SQLite issuance journal BEFORE we commit to the
   * "authority is ready" contract.  Drift is loud and non-recoverable
   * here (NH_SMB_RECONCILE_REQUIRED); an operator has to intervene.
   *
   * Skipped when smb_conf_path is NULL/empty (portable tests,
   * backwards-compat path via nh_smb_authority_open()) OR when the
   * adapter didn't provide the optional enumerate op (a mock without
   * anything to enumerate). */
  if (smb_conf_path && *smb_conf_path && passdb_ops->enumerate) {
    nh_smb_rc rr = bootstrap_or_reconcile_passdb(a);
    if (rr != NH_SMB_OK) {
      sqlite3_close(a->db);
      free(a);
      return rr;
    }
  }

  /* Plan §4.1 A3 (Finding 3 posture): the header contract says open()
   * revokes any already-expired credentials.  Do exactly that with one
   * idempotent sweep pass.  We intentionally do NOT fail open() on a
   * sweep error — the scheduled `nostr-authctl smb-sweep` timer is the
   * durable backstop; a transient passdb hiccup here should not brick
   * broker startup.  The error is retained in a->error via set_error()
   * inside sweep_expired() for callers who want to log it. */
  size_t swept = 0;
  (void)nh_smb_authority_sweep_expired(a, wall_ms_now(), &swept);
  *out = a;
  return NH_SMB_OK;
}

/* ------------------------------------------------------------------ */
/* Reconciliation                                                      */
/* ------------------------------------------------------------------ */

/* A first open can follow an operator's manual smbpasswd seed.  There is
 * no Nostr pubkey/UID/binding to reconstruct for such accounts, so retain
 * their names separately rather than forging credential issuance rows.  The
 * bootstrap marker makes this a one-time exception: subsequent opens always
 * run the normal two-way drift check, even while credentials is empty. */
static nh_smb_rc bootstrap_or_reconcile_passdb(nh_smb_authority *a) {
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(a->db,
      "SELECT value FROM metadata WHERE key='passdb_bootstrapped'", -1,
      &stmt, NULL);
  if (rc != SQLITE_OK) return NH_SMB_STORAGE_ERROR;
  rc = sqlite3_step(stmt);
  bool bootstrapped = rc == SQLITE_ROW;
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) return NH_SMB_STORAGE_ERROR;
  if (bootstrapped) return reconcile_passdb_against_journal(a);

  rc = sqlite3_prepare_v2(a->db, "SELECT EXISTS(SELECT 1 FROM credentials)",
                          -1, &stmt, NULL);
  if (rc != SQLITE_OK) return NH_SMB_STORAGE_ERROR;
  rc = sqlite3_step(stmt);
  bool has_history = rc == SQLITE_ROW && sqlite3_column_int(stmt, 0) != 0;
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW) return NH_SMB_STORAGE_ERROR;

  /* An older journal with issuance history is never a first-boot store. */
  if (has_history) {
    nh_smb_rc verdict = reconcile_passdb_against_journal(a);
    if (verdict != NH_SMB_OK) return verdict;
    return exec_sql(a, "INSERT INTO metadata(key,value)"
                       " VALUES('passdb_bootstrapped','1')", "mark passdb ready");
  }

  char **users = NULL;
  size_t count = 0;
  if (a->ops->enumerate(a->ops_ctx, &users, &count) != 0) {
    set_error(a, "passdb enumerate failed during bootstrap");
    return NH_SMB_PASSDB_ERROR;
  }
  nh_smb_rc verdict = exec_sql(a, "BEGIN IMMEDIATE", "begin passdb bootstrap");
  if (verdict != NH_SMB_OK) goto done;
  rc = sqlite3_prepare_v2(a->db,
      "INSERT OR IGNORE INTO adopted_passdb(username) VALUES(?)", -1,
      &stmt, NULL);
  if (rc != SQLITE_OK) {
    verdict = NH_SMB_STORAGE_ERROR;
    goto rollback;
  }
  for (size_t i = 0; i < count; i++) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, users[i], -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      verdict = NH_SMB_STORAGE_ERROR;
      break;
    }
  }
  sqlite3_finalize(stmt);
  stmt = NULL;
  if (verdict != NH_SMB_OK) goto rollback;
  verdict = exec_sql(a, "INSERT INTO metadata(key,value)"
                        " VALUES('passdb_bootstrapped','1')", "mark passdb bootstrap");
  if (verdict != NH_SMB_OK) goto rollback;
  verdict = exec_sql(a, "COMMIT", "commit passdb bootstrap");
  if (verdict == NH_SMB_OK)
    fprintf(stderr, "smb authority: first-boot passdb adoption: %zu account(s)\n", count);
  else
    (void)exec_sql(a, "ROLLBACK", "rollback passdb bootstrap");
  goto done;
rollback:
  if (stmt) sqlite3_finalize(stmt);
  (void)exec_sql(a, "ROLLBACK", "rollback passdb bootstrap");
done:
  for (size_t i = 0; i < count; i++) free(users[i]);
  free(users);
  return verdict;
}

/*
 * Diff the passdb enumeration (via the adapter's `enumerate` op) against
 * the set of currently-active usernames in the issuance journal.  Returns
 * NH_SMB_OK when the two sides match, NH_SMB_RECONCILE_REQUIRED on drift.
 *
 * Drift criteria (plan §4.2 B3):
 *   (a) A passdb user with no active journal row — either the passdb
 *       was restored from an unmatched backup or a rogue smbpasswd
 *       invocation added a user behind the authority's back.
 *   (b) A journal-active username missing from the passdb — the passdb
 *       lost a row and clients would silently fail auth without the
 *       authority realising.
 *
 * Either case is a bug-in-need-of-a-human, not an auto-repair moment.
 */
static nh_smb_rc reconcile_passdb_against_journal(nh_smb_authority *a) {
  char **passdb_users = NULL;
  size_t passdb_count = 0;
  int erc = a->ops->enumerate(a->ops_ctx, &passdb_users, &passdb_count);
  if (erc != 0) {
    set_error(a, "passdb enumerate failed (rc=%d)", erc);
    return NH_SMB_PASSDB_ERROR;
  }

  /* Load active-journal usernames into a comparable array. */
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(a->db,
      "SELECT username FROM credentials WHERE revoked_at_ms IS NULL "
      "UNION SELECT username FROM adopted_passdb",
      -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    set_error(a, "reconcile prepare: %s", sqlite3_errmsg(a->db));
    for (size_t i = 0; i < passdb_count; i++) free(passdb_users[i]);
    free(passdb_users);
    return NH_SMB_STORAGE_ERROR;
  }
  char **journal_users = NULL;
  size_t journal_count = 0, journal_cap = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *u = sqlite3_column_text(stmt, 0);
    if (!u) continue;
    if (journal_count == journal_cap) {
      size_t new_cap = journal_cap ? journal_cap * 2 : 8;
      char **grown = realloc(journal_users, new_cap * sizeof *journal_users);
      if (!grown) {
        sqlite3_finalize(stmt);
        for (size_t i = 0; i < passdb_count; i++) free(passdb_users[i]);
        free(passdb_users);
        for (size_t i = 0; i < journal_count; i++) free(journal_users[i]);
        free(journal_users);
        return NH_SMB_NO_MEMORY;
      }
      journal_users = grown;
      journal_cap = new_cap;
    }
    journal_users[journal_count] = strdup((const char *)u);
    if (!journal_users[journal_count]) {
      sqlite3_finalize(stmt);
      for (size_t i = 0; i < passdb_count; i++) free(passdb_users[i]);
      free(passdb_users);
      for (size_t i = 0; i < journal_count; i++) free(journal_users[i]);
      free(journal_users);
      return NH_SMB_NO_MEMORY;
    }
    journal_count++;
  }
  sqlite3_finalize(stmt);

  /* Two-way subset check.  Small lists (typically a handful of users),
   * so an O(n*m) scan beats hashing on both cognitive load and
   * dependency footprint. */
  nh_smb_rc verdict = NH_SMB_OK;
  for (size_t i = 0; i < passdb_count && verdict == NH_SMB_OK; i++) {
    bool found = false;
    for (size_t j = 0; j < journal_count; j++) {
      if (strcmp(passdb_users[i], journal_users[j]) == 0) { found = true; break; }
    }
    if (!found) {
      set_error(a, "reconcile drift: passdb user '%s' has no active journal row",
                passdb_users[i]);
      verdict = NH_SMB_RECONCILE_REQUIRED;
    }
  }
  for (size_t j = 0; j < journal_count && verdict == NH_SMB_OK; j++) {
    bool found = false;
    for (size_t i = 0; i < passdb_count; i++) {
      if (strcmp(journal_users[j], passdb_users[i]) == 0) { found = true; break; }
    }
    if (!found) {
      set_error(a, "reconcile drift: active journal user '%s' missing from passdb",
                journal_users[j]);
      verdict = NH_SMB_RECONCILE_REQUIRED;
    }
  }

  for (size_t i = 0; i < passdb_count; i++) free(passdb_users[i]);
  free(passdb_users);
  for (size_t i = 0; i < journal_count; i++) free(journal_users[i]);
  free(journal_users);
  return verdict;
}

void nh_smb_authority_close(nh_smb_authority *a) {
  if (!a) return;
  /* Service restart is a revocation event: pull any active credentials
   * from the passdb.  We do not touch the journal — the operator wants
   * a durable audit trail — but we mark them revoked so a subsequent
   * open() sees a consistent state.  Failures here are logged into the
   * error_detail buffer but do not prevent close. */
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(a->db,
        "SELECT username FROM credentials WHERE revoked_at_ms IS NULL",
        -1, &stmt, NULL) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const unsigned char *u = sqlite3_column_text(stmt, 0);
      if (u) (void)passdb_remove(a, (const char *)u);
    }
    sqlite3_finalize(stmt);
    (void)sqlite3_exec(a->db,
        "UPDATE credentials SET revoked_at_ms=strftime('%s','now')*1000,"
        " revoke_reason=5 WHERE revoked_at_ms IS NULL",
        NULL, NULL, NULL);
  }
  sqlite3_close(a->db);
  secure_wipe(a, sizeof *a);
  free(a);
}

/* ------------------------------------------------------------------ */
/* Issue                                                               */
/* ------------------------------------------------------------------ */

static bool valid_username(const char *s) {
  if (!s) return false;
  size_t n = strlen(s);
  return n > 0 && n <= NH_IDENTITY_USERNAME_MAX;
}

static bool valid_pubkey(const char *s) {
  if (!s) return false;
  if (strlen(s) != NH_IDENTITY_PUBKEY_HEX_LEN) return false;
  for (size_t i = 0; i < NH_IDENTITY_PUBKEY_HEX_LEN; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

nh_smb_rc nh_smb_credential_issue(nh_smb_authority *a,
                                  const nh_smb_issue_request *req,
                                  nh_smb_envelope *out) {
  if (!a || !req || !out) return NH_SMB_INVALID;
  if (!valid_username(req->account.username)) return NH_SMB_INVALID;
  if (!valid_pubkey(req->account.pubkey_hex)) return NH_SMB_INVALID;
  if (req->account.uid == 0) return NH_SMB_INVALID;
  if (req->lifetime_ms == 0) return NH_SMB_INVALID;
  if (req->binding && strlen(req->binding) > NH_SMB_BINDING_MAX)
    return NH_SMB_INVALID;

  /* Overflow-safe expiry computation. */
  if (req->lifetime_ms > (uint64_t)UINT64_MAX - req->now_monotonic_ms)
    return NH_SMB_INVALID;
  uint64_t expires_at_ms = req->now_monotonic_ms + req->lifetime_ms;

  memset(out, 0, sizeof *out);

  /* Serialize on username: rotate any previous outstanding credential
   * first.  Do this inside a single transaction; the passdb hop happens
   * before the new insert but after the revoke row is written, so a
   * mid-flight crash leaves the previous credential revoked and no new
   * one — the client can retry. */
  nh_smb_rc r = exec_sql(a, "BEGIN IMMEDIATE", "begin issue");
  if (r != NH_SMB_OK) return r;

  bool rotated = false;
  r = journal_mark_revoked(a, req->account.username, req->now_monotonic_ms,
                           NH_SMB_REVOKE_ROTATED, &rotated);
  if (r != NH_SMB_OK) {
    (void)exec_sql(a, "ROLLBACK", "rollback issue");
    return r;
  }

  char credential_id[NH_SMB_CREDENTIAL_ID_CAP];
  if (mint_credential_id(credential_id) != 0) {
    (void)exec_sql(a, "ROLLBACK", "rollback issue");
    set_error(a, "csprng: credential id");
    return NH_SMB_INTERNAL;
  }

  /* Allocate password buffer.  Cap = maximum encoded length + NUL. */
  size_t pass_cap = NH_SMB_PASSWORD_MAX_LEN + 1;
  nostr_secure_buf pw = secure_alloc(pass_cap);
  if (!pw.ptr) {
    (void)exec_sql(a, "ROLLBACK", "rollback issue");
    set_error(a, "secure_alloc failed");
    return NH_SMB_NO_MEMORY;
  }
  size_t pw_len = mint_password((char *)pw.ptr, pass_cap);
  if (pw_len == 0) {
    secure_free(&pw);
    (void)exec_sql(a, "ROLLBACK", "rollback issue");
    set_error(a, "csprng: password mint");
    return NH_SMB_INTERNAL;
  }

  /* Insert BEFORE hitting the passdb so a crash after passdb install
   * cannot orphan a passdb entry that the journal doesn't know about.
   * If the passdb install fails, we rollback the transaction. */
  r = journal_insert(a, credential_id, req, expires_at_ms);
  if (r != NH_SMB_OK) {
    secure_free(&pw);
    (void)exec_sql(a, "ROLLBACK", "rollback issue");
    return r;
  }

  /* A newly issued credential supersedes any first-boot adopted account. */
  r = journal_forget_adopted(a, req->account.username);
  if (r != NH_SMB_OK) {
    secure_free(&pw);
    (void)exec_sql(a, "ROLLBACK", "rollback issue");
    return r;
  }

  int ok = passdb_set_password(a, req->account.username, (const char *)pw.ptr);
  if (ok != 0) {
    secure_free(&pw);
    (void)exec_sql(a, "ROLLBACK", "rollback issue");
    /* Attempt best-effort passdb cleanup if the previous rotation had
     * been reflected there; the caller retries. */
    if (rotated) (void)passdb_remove(a, req->account.username);
    set_error(a, "passdb set_password failed for %s", req->account.username);
    return NH_SMB_PASSDB_ERROR;
  }

  r = exec_sql(a, "COMMIT", "commit issue");
  if (r != NH_SMB_OK) {
    /* Passdb succeeded but journal commit didn't: tear down passdb to
     * keep them in sync. */
    (void)passdb_remove(a, req->account.username);
    secure_free(&pw);
    return r;
  }

  /* Populate envelope. Both `credential_id` (mint_credential_id -> 36-char
   * UUID + NUL) and `req->account.username` (valid_username -> <=32 chars +
   * NUL) live in fixed-size buffers the same size as the destination fields.
   * strncpy(dst, src, sizeof(dst)-1) triggers -Werror=stringop-truncation
   * under -O2 (GCC 13) because when strlen(src) == sizeof(dst)-1 the NUL is
   * dropped; memcpy the whole buffer and NUL-terminate explicitly instead. */
  memcpy(out->credential_id, credential_id, sizeof out->credential_id);
  out->credential_id[sizeof out->credential_id - 1] = '\0';
  memcpy(out->username, req->account.username, sizeof out->username);
  out->username[sizeof out->username - 1] = '\0';
  out->issued_at_ms = req->now_monotonic_ms;
  out->expires_at_ms = expires_at_ms;
  out->password_len = pw_len;
  out->password = pw; /* ownership transfers */
  out->consumed = false;
  return NH_SMB_OK;
}

/* ------------------------------------------------------------------ */
/* Revoke                                                              */
/* ------------------------------------------------------------------ */

static uint64_t wall_ms_now(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

nh_smb_rc nh_smb_credential_revoke(nh_smb_authority *a, const char *username,
                                   nh_smb_revoke_reason reason) {
  if (!a || !valid_username(username)) return NH_SMB_INVALID;

  uint64_t now_ms = wall_ms_now();
  bool marked = false;
  nh_smb_rc r = exec_sql(a, "BEGIN IMMEDIATE", "begin revoke");
  if (r != NH_SMB_OK) return r;
  r = journal_mark_revoked(a, username, now_ms, reason, &marked);
  if (r != NH_SMB_OK) {
    (void)exec_sql(a, "ROLLBACK", "rollback revoke");
    return r;
  }
  r = exec_sql(a, "COMMIT", "commit revoke");
  if (r != NH_SMB_OK) return r;

  /* Passdb hop is outside the DB txn — passdb is not transactional. */
  int d = passdb_disable(a, username);
  int rm = passdb_remove(a, username);
  /* Forget an adopted baseline only after Samba actually removed it.
   * SQLite's single DELETE is atomic; a failure leaves a loud drift on
   * the next open rather than silently trusting a stale passdb state. */
  if (rm == 0) {
    r = journal_forget_adopted(a, username);
    if (r != NH_SMB_OK) return r;
  }
  if (!marked && d != 0 && rm != 0) return NH_SMB_NOT_FOUND;
  return NH_SMB_OK;
}

/* ------------------------------------------------------------------ */
/* Sweep expired                                                       */
/* ------------------------------------------------------------------ */

nh_smb_rc nh_smb_authority_sweep_expired(nh_smb_authority *a, uint64_t now_ms,
                                         size_t *revoked_out) {
  if (!a) return NH_SMB_INVALID;
  if (revoked_out) *revoked_out = 0;

  /* Collect names first so we don't hold the statement while calling
   * into the passdb adapter. */
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(a->db,
      "SELECT username FROM credentials"
      " WHERE revoked_at_ms IS NULL AND expires_at_ms<=?",
      -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    set_error(a, "sweep prepare: %s", sqlite3_errmsg(a->db));
    return NH_SMB_STORAGE_ERROR;
  }
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)now_ms);

  char **names = NULL;
  size_t cap = 0, count = 0;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const unsigned char *u = sqlite3_column_text(stmt, 0);
    if (!u) continue;
    if (count == cap) {
      size_t ncap = cap ? cap * 2 : 8;
      char **n = realloc(names, ncap * sizeof *n);
      if (!n) { sqlite3_finalize(stmt); goto oom; }
      names = n; cap = ncap;
    }
    names[count] = strdup((const char *)u);
    if (!names[count]) { sqlite3_finalize(stmt); goto oom; }
    count++;
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    set_error(a, "sweep step: %s", sqlite3_errmsg(a->db));
    goto err;
  }

  for (size_t i = 0; i < count; i++) {
    nh_smb_rc r = exec_sql(a, "BEGIN IMMEDIATE", "begin sweep-row");
    if (r != NH_SMB_OK) goto err;
    bool marked = false;
    r = journal_mark_revoked(a, names[i], now_ms, NH_SMB_REVOKE_EXPIRED,
                             &marked);
    if (r != NH_SMB_OK) {
      (void)exec_sql(a, "ROLLBACK", "rollback sweep-row");
      goto err;
    }
    r = exec_sql(a, "COMMIT", "commit sweep-row");
    if (r != NH_SMB_OK) goto err;
    (void)passdb_disable(a, names[i]);
    (void)passdb_remove(a, names[i]);
  }
  if (revoked_out) *revoked_out = count;
  for (size_t i = 0; i < count; i++) free(names[i]);
  free(names);
  return NH_SMB_OK;

oom:
  set_error(a, "sweep: out of memory");
  for (size_t i = 0; i < count; i++) free(names[i]);
  free(names);
  return NH_SMB_NO_MEMORY;
err:
  for (size_t i = 0; i < count; i++) free(names[i]);
  free(names);
  return NH_SMB_STORAGE_ERROR;
}

/* ------------------------------------------------------------------ */
/* Read-only accessors                                                 */
/* ------------------------------------------------------------------ */

nh_smb_rc nh_smb_authority_lookup_active(nh_smb_authority *a,
                                         const char *username,
                                         nh_smb_journal_row *out) {
  if (!a || !valid_username(username) || !out) return NH_SMB_INVALID;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(a->db,
      "SELECT credential_id,username,uid,pubkey_hex,issued_at_ms,"
      "       expires_at_ms,revoked_at_ms,revoke_reason"
      " FROM credentials"
      " WHERE username=? AND revoked_at_ms IS NULL"
      " LIMIT 1",
      -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    set_error(a, "lookup prepare: %s", sqlite3_errmsg(a->db));
    return NH_SMB_STORAGE_ERROR;
  }
  sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return NH_SMB_NOT_FOUND;
  }
  if (rc != SQLITE_ROW) {
    set_error(a, "lookup step: %s", sqlite3_errmsg(a->db));
    sqlite3_finalize(stmt);
    return NH_SMB_STORAGE_ERROR;
  }
  memset(out, 0, sizeof *out);
  const unsigned char *cid = sqlite3_column_text(stmt, 0);
  const unsigned char *un = sqlite3_column_text(stmt, 1);
  const unsigned char *pk = sqlite3_column_text(stmt, 3);
  /* SQLite text is a dynamically-sized C string; snprintf bounds the copy
   * AND always NUL-terminates. Replaces strncpy(dst, src, sizeof(dst)-1)
   * which could drop the terminator (same -Wstringop-truncation family). */
  if (cid) snprintf(out->credential_id, sizeof out->credential_id, "%s",
                    (const char *)cid);
  if (un) snprintf(out->username, sizeof out->username, "%s",
                   (const char *)un);
  out->uid = (uint32_t)sqlite3_column_int64(stmt, 2);
  if (pk) snprintf(out->pubkey_hex, sizeof out->pubkey_hex, "%s",
                   (const char *)pk);
  out->issued_at_ms = (uint64_t)sqlite3_column_int64(stmt, 4);
  out->expires_at_ms = (uint64_t)sqlite3_column_int64(stmt, 5);
  out->revoked_at_ms = sqlite3_column_type(stmt, 6) == SQLITE_NULL
      ? 0 : (uint64_t)sqlite3_column_int64(stmt, 6);
  out->revoke_reason = sqlite3_column_int(stmt, 7);
  sqlite3_finalize(stmt);
  return NH_SMB_OK;
}

nh_smb_rc nh_smb_authority_count(nh_smb_authority *a, size_t *rows_out) {
  if (!a || !rows_out) return NH_SMB_INVALID;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(a->db,
      "SELECT COUNT(*) FROM credentials", -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    set_error(a, "count prepare: %s", sqlite3_errmsg(a->db));
    return NH_SMB_STORAGE_ERROR;
  }
  rc = sqlite3_step(stmt);
  size_t n = 0;
  if (rc == SQLITE_ROW) n = (size_t)sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  *rows_out = n;
  return NH_SMB_OK;
}
