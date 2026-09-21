/*
 * Headless integration test for the SMB credential authority.
 *
 * Uses an in-process mock passdb adapter to exercise the whole
 * lifecycle: issue -> journal row present (no password stored) ->
 * envelope delivers a policy-conforming password exactly once ->
 * sweep_expired revokes and disables -> revoke works end-to-end.
 *
 * beads nostrc-rb0e.6.
 */
#include "smb_credential.h"
#include "../nh_test.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* -------- Mock passdb -------- */

typedef struct mock_passdb {
  int set_calls;
  int disable_calls;
  int remove_calls;
  char last_username[64];
  char last_password[128];
  int fail_set;
} mock_passdb;

static int mock_set(void *ctx, const char *u, const char *p) {
  mock_passdb *m = ctx;
  if (m->fail_set) return -1;
  m->set_calls++;
  snprintf(m->last_username, sizeof m->last_username, "%s", u);
  snprintf(m->last_password, sizeof m->last_password, "%s", p);
  return 0;
}
static int mock_disable(void *ctx, const char *u) {
  (void)u;
  mock_passdb *m = ctx;
  m->disable_calls++;
  return 0;
}
static int mock_remove(void *ctx, const char *u) {
  (void)u;
  mock_passdb *m = ctx;
  m->remove_calls++;
  return 0;
}
static const nh_smb_passdb_ops mock_ops = { mock_set, mock_disable, mock_remove };

/* -------- Helpers -------- */

static bool password_is_base64url(const char *p, size_t n) {
  if (n == 0) return false;
  for (size_t i = 0; i < n; i++) {
    char c = p[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

static void fill_account(nh_identity_account *a) {
  memset(a, 0, sizeof *a);
  snprintf(a->account_id, sizeof a->account_id,
           "11111111-1111-1111-1111-111111111111");
  snprintf(a->username, sizeof a->username, "alice");
  snprintf(a->pubkey_hex, sizeof a->pubkey_hex,
           "0000000000000000000000000000000000000000000000000000000000000001");
  a->uid = 200042;
  a->gid = 200042;
  a->status = NH_IDENTITY_STATUS_ACTIVE;
  a->key_generation = 1;
  a->authority_generation = 1;
}

static char *tempdb_path(void) {
  char *tmpl = strdup("/tmp/nh-smb-XXXXXX");
  NH_CHECK(tmpl != NULL);
  int fd = mkstemp(tmpl);
  NH_CHECK(fd >= 0);
  close(fd);
  unlink(tmpl); /* let sqlite create fresh */
  return tmpl;
}

/* -------- Cases -------- */

static void case_issue_and_envelope(void) {
  char *db = tempdb_path();
  mock_passdb pdb = {0};
  nh_smb_authority *a = NULL;
  NH_CHECK(nh_smb_authority_open(db, &mock_ops, &pdb, &a) == NH_SMB_OK);

  nh_smb_issue_request req = {0};
  fill_account(&req.account);
  req.now_monotonic_ms = 1000000;
  req.lifetime_ms = 60000;
  req.binding = "receipt-token-digest";

  nh_smb_envelope env;
  memset(&env, 0, sizeof env);
  NH_CHECK(nh_smb_credential_issue(a, &req, &env) == NH_SMB_OK);

  /* Envelope invariants. */
  NH_CHECK(env.password.ptr != NULL);
  NH_CHECK(env.password_len >= NH_SMB_PASSWORD_MIN_LEN);
  NH_CHECK(env.password_len <= NH_SMB_PASSWORD_MAX_LEN);
  NH_CHECK(password_is_base64url((const char *)env.password.ptr,
                                 env.password_len));
  NH_CHECK(env.expires_at_ms == req.now_monotonic_ms + req.lifetime_ms);
  NH_CHECK(strlen(env.credential_id) == NH_SMB_CREDENTIAL_ID_LEN);
  NH_CHECK(strcmp(env.username, req.account.username) == 0);

  /* Mock adapter saw the exact password. */
  NH_CHECK(pdb.set_calls == 1);
  NH_CHECK(strcmp(pdb.last_username, req.account.username) == 0);
  NH_CHECK(strcmp(pdb.last_password, (const char *)env.password.ptr) == 0);

  /* Journal row: present but does NOT store the password. */
  size_t rows = 0;
  NH_CHECK(nh_smb_authority_count(a, &rows) == NH_SMB_OK);
  NH_CHECK(rows == 1);

  nh_smb_journal_row row;
  NH_CHECK(nh_smb_authority_lookup_active(a, req.account.username, &row) ==
           NH_SMB_OK);
  NH_CHECK(strcmp(row.username, req.account.username) == 0);
  NH_CHECK(row.uid == req.account.uid);
  NH_CHECK(strcmp(row.pubkey_hex, req.account.pubkey_hex) == 0);
  NH_CHECK(row.issued_at_ms == req.now_monotonic_ms);
  NH_CHECK(row.expires_at_ms == req.now_monotonic_ms + req.lifetime_ms);
  NH_CHECK(row.revoked_at_ms == 0);

  /* Grep the raw db file: the password bytes must NOT appear. */
  char password_copy[NH_SMB_PASSWORD_MAX_LEN + 1];
  size_t plen = env.password_len;
  memcpy(password_copy, env.password.ptr, plen);
  password_copy[plen] = '\0';
  /* Envelope is single-use: clearing wipes the buffer. */
  nh_smb_envelope_clear(&env);
  NH_CHECK(env.password.ptr == NULL);
  NH_CHECK(env.password_len == 0);

  /* Close authority so file handles are flushed, then scan. */
  nh_smb_authority_close(a);
  FILE *f = fopen(db, "rb");
  NH_CHECK(f != NULL);
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)sz);
  NH_CHECK(buf != NULL);
  NH_CHECK(fread(buf, 1, (size_t)sz, f) == (size_t)sz);
  fclose(f);
  int password_found = 0;
  if ((size_t)sz >= plen) {
    for (size_t i = 0; i + plen <= (size_t)sz; i++) {
      if (memcmp(buf + i, password_copy, plen) == 0) { password_found = 1; break; }
    }
  }
  free(buf);
  NH_CHECK(password_found == 0);

  unlink(db);
  /* Wipe copy. */
  volatile char *v = (volatile char *)password_copy;
  for (size_t i = 0; i < sizeof password_copy; i++) v[i] = 0;
  free(db);
}

static void case_sweep_expired(void) {
  char *db = tempdb_path();
  mock_passdb pdb = {0};
  nh_smb_authority *a = NULL;
  NH_CHECK(nh_smb_authority_open(db, &mock_ops, &pdb, &a) == NH_SMB_OK);

  nh_smb_issue_request req = {0};
  fill_account(&req.account);
  req.now_monotonic_ms = 2000000;
  req.lifetime_ms = 30000; /* 30 seconds */

  nh_smb_envelope env;
  memset(&env, 0, sizeof env);
  NH_CHECK(nh_smb_credential_issue(a, &req, &env) == NH_SMB_OK);
  NH_CHECK(pdb.set_calls == 1);
  nh_smb_envelope_clear(&env);

  /* Before deadline: sweep is a no-op. */
  size_t revoked = 999;
  NH_CHECK(nh_smb_authority_sweep_expired(a, req.now_monotonic_ms + 1000,
                                          &revoked) == NH_SMB_OK);
  NH_CHECK(revoked == 0);
  int disable_before = pdb.disable_calls;

  /* After deadline: sweep revokes it. */
  revoked = 0;
  NH_CHECK(nh_smb_authority_sweep_expired(
      a, req.now_monotonic_ms + req.lifetime_ms + 1, &revoked) == NH_SMB_OK);
  NH_CHECK(revoked == 1);
  NH_CHECK(pdb.disable_calls > disable_before);

  /* Journal row is now revoked -> lookup_active returns NOT_FOUND. */
  nh_smb_journal_row row;
  NH_CHECK(nh_smb_authority_lookup_active(a, req.account.username, &row) ==
           NH_SMB_NOT_FOUND);

  /* Idempotency: second sweep does nothing more. */
  size_t again = 999;
  NH_CHECK(nh_smb_authority_sweep_expired(
      a, req.now_monotonic_ms + req.lifetime_ms + 1, &again) == NH_SMB_OK);
  NH_CHECK(again == 0);

  nh_smb_authority_close(a);
  unlink(db);
  free(db);
}

static void case_manual_revoke(void) {
  char *db = tempdb_path();
  mock_passdb pdb = {0};
  nh_smb_authority *a = NULL;
  NH_CHECK(nh_smb_authority_open(db, &mock_ops, &pdb, &a) == NH_SMB_OK);

  nh_smb_issue_request req = {0};
  fill_account(&req.account);
  req.now_monotonic_ms = 3000000;
  req.lifetime_ms = 60000;

  nh_smb_envelope env;
  memset(&env, 0, sizeof env);
  NH_CHECK(nh_smb_credential_issue(a, &req, &env) == NH_SMB_OK);
  nh_smb_envelope_clear(&env);

  int disable_before = pdb.disable_calls;
  int remove_before = pdb.remove_calls;
  NH_CHECK(nh_smb_credential_revoke(a, req.account.username,
                                    NH_SMB_REVOKE_ADMIN) == NH_SMB_OK);
  NH_CHECK(pdb.disable_calls == disable_before + 1);
  NH_CHECK(pdb.remove_calls == remove_before + 1);

  nh_smb_journal_row row;
  NH_CHECK(nh_smb_authority_lookup_active(a, req.account.username, &row) ==
           NH_SMB_NOT_FOUND);

  nh_smb_authority_close(a);
  unlink(db);
  free(db);
}

static void case_rotate_replaces_previous(void) {
  char *db = tempdb_path();
  mock_passdb pdb = {0};
  nh_smb_authority *a = NULL;
  NH_CHECK(nh_smb_authority_open(db, &mock_ops, &pdb, &a) == NH_SMB_OK);

  nh_smb_issue_request req = {0};
  fill_account(&req.account);
  req.now_monotonic_ms = 4000000;
  req.lifetime_ms = 60000;

  nh_smb_envelope e1, e2;
  memset(&e1, 0, sizeof e1);
  memset(&e2, 0, sizeof e2);
  NH_CHECK(nh_smb_credential_issue(a, &req, &e1) == NH_SMB_OK);
  char first_id[NH_SMB_CREDENTIAL_ID_CAP];
  snprintf(first_id, sizeof first_id, "%s", e1.credential_id);
  nh_smb_envelope_clear(&e1);

  req.now_monotonic_ms = 4001000;
  NH_CHECK(nh_smb_credential_issue(a, &req, &e2) == NH_SMB_OK);
  NH_CHECK(strcmp(e2.credential_id, first_id) != 0);
  nh_smb_envelope_clear(&e2);

  /* One active credential; total rows = 2 (one revoked, one active). */
  size_t rows = 0;
  NH_CHECK(nh_smb_authority_count(a, &rows) == NH_SMB_OK);
  NH_CHECK(rows == 2);

  nh_smb_journal_row row;
  NH_CHECK(nh_smb_authority_lookup_active(a, req.account.username, &row) ==
           NH_SMB_OK);
  NH_CHECK(strcmp(row.credential_id, first_id) != 0);

  nh_smb_authority_close(a);
  unlink(db);
  free(db);
}

static void case_invalid_inputs(void) {
  char *db = tempdb_path();
  mock_passdb pdb = {0};
  nh_smb_authority *a = NULL;
  NH_CHECK(nh_smb_authority_open(db, &mock_ops, &pdb, &a) == NH_SMB_OK);

  nh_smb_envelope env;
  memset(&env, 0, sizeof env);

  /* NULL args */
  NH_CHECK(nh_smb_credential_issue(NULL, NULL, NULL) == NH_SMB_INVALID);

  /* Zero lifetime */
  nh_smb_issue_request req = {0};
  fill_account(&req.account);
  req.now_monotonic_ms = 5000000;
  req.lifetime_ms = 0;
  NH_CHECK(nh_smb_credential_issue(a, &req, &env) == NH_SMB_INVALID);

  /* Bad pubkey */
  req.lifetime_ms = 1000;
  snprintf(req.account.pubkey_hex, sizeof req.account.pubkey_hex, "not-hex");
  NH_CHECK(nh_smb_credential_issue(a, &req, &env) == NH_SMB_INVALID);

  nh_smb_authority_close(a);
  unlink(db);
  free(db);
}

static void case_passdb_failure_rolls_back(void) {
  char *db = tempdb_path();
  mock_passdb pdb = {0};
  pdb.fail_set = 1;
  nh_smb_authority *a = NULL;
  NH_CHECK(nh_smb_authority_open(db, &mock_ops, &pdb, &a) == NH_SMB_OK);

  nh_smb_issue_request req = {0};
  fill_account(&req.account);
  req.now_monotonic_ms = 6000000;
  req.lifetime_ms = 60000;

  nh_smb_envelope env;
  memset(&env, 0, sizeof env);
  NH_CHECK(nh_smb_credential_issue(a, &req, &env) == NH_SMB_PASSDB_ERROR);
  NH_CHECK(env.password.ptr == NULL);

  size_t rows = 0;
  NH_CHECK(nh_smb_authority_count(a, &rows) == NH_SMB_OK);
  NH_CHECK(rows == 0);

  nh_smb_authority_close(a);
  unlink(db);
  free(db);
}

int main(void) {
  case_issue_and_envelope();
  case_sweep_expired();
  case_manual_revoke();
  case_rotate_replaces_previous();
  case_invalid_inputs();
  case_passdb_failure_rolls_back();
  printf("test_smb_credential: OK\n");
  return 0;
}
