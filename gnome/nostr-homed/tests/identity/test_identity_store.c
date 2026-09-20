#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "nostr_identity.h"

#include <assert.h>
#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static nh_identity_ownership_result ownership_probe(void *context,
    const char *username, uint32_t uid, uint32_t gid) {
  (void)context; (void)username;
  if (uid != gid) return NH_IDENTITY_OWNERSHIP_ERROR;
  return uid == 200000u ? NH_IDENTITY_OWNERSHIP_IN_USE
                        : NH_IDENTITY_OWNERSHIP_FREE;
}

static void fill_hex(char out[65], char value) {
  memset(out, value, 64); out[64] = '\0';
}

static void sql_expect_failure(const char *path, const char *sql) {
  sqlite3 *db = NULL;
  char *error = NULL;
  assert(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
  assert(sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK);
  sqlite3_free(error);
  sqlite3_close(db);
}

static void remove_tree(const char *dir, const char *db, const char *lock) {
  char sidecar[512];
  unlink(db); unlink(lock);
  snprintf(sidecar, sizeof(sidecar), "%s-wal", db); unlink(sidecar);
  snprintf(sidecar, sizeof(sidecar), "%s-shm", db); unlink(sidecar);
  rmdir(dir);
}

int main(void) {
  char temporary[] = "/tmp/nostr-identity-store-XXXXXX";
  char db_path[256], projection_path[256], lock_path[256];
  char key_a[65], key_b[65], key_c[65];
  nh_identity_config config;
  nh_identity_store_options options;
  nh_identity_store *store = NULL, *second = NULL;
  nh_identity_store_info initial_info, after_enroll, after_replay;
  nh_identity_enroll_request enroll;
  nh_identity_operation_state operation, replay;
  nh_identity_account account, retired, next;
  nh_identity_home_evidence staged = {11, 101}, installed = {11, 202};
  nh_identity_proof_attestation attestation;
  nh_identity_provider_record provider;
  nh_identity_reader *reader = NULL;
  nh_identity_passwd_record passwd_record;
  nh_identity_group_record group_record;
  char reader_buffer[NH_IDENTITY_READER_BUF_MAX];
  size_t reader_required = 0;
  uint64_t projection_generation = 0;
  char provider_id[NH_IDENTITY_UUID_CAP];
  struct stat st;
  pid_t child;
  int child_status;
  const uint8_t secret[] = {0xde, 0xad, 0xbe, 0xef};
  const char *op_enroll = "00000000-0000-4000-8000-000000000001";

  assert(mkdtemp(temporary));
  assert(chmod(temporary, 0700) == 0);
  snprintf(db_path, sizeof(db_path), "%s/authority.db", temporary);
  snprintf(projection_path, sizeof(projection_path), "%s/nss.db", temporary);
  snprintf(lock_path, sizeof(lock_path), "%s/authority.lock", temporary);
  nh_identity_config_defaults(&config);
  snprintf(config.authority_path, sizeof(config.authority_path), "%s", db_path);
  snprintf(config.projection_path, sizeof(config.projection_path), "%s", projection_path);
  snprintf(config.home_root, sizeof(config.home_root), "%s/home", temporary);
  config.uid_min = 200000u; config.uid_max = 200004u;
  assert(nh_identity_config_validate(&config) == NH_IDENTITY_OK);
  memset(&options, 0, sizeof(options));
  options.config = &config;
  options.ownership_probe = ownership_probe;
  options.flags = NH_IDENTITY_STORE_CREATE;

  assert(nh_identity_store_open(&options, &store) == NH_IDENTITY_OK);
  assert(stat(db_path, &st) == 0 && (st.st_mode & 0777) == 0600);
  assert(nh_identity_store_get_info(store, &initial_info) == NH_IDENTITY_OK);
  assert(initial_info.schema_version == 1 && initial_info.authority_generation == 1);
  assert(nh_identity_store_open(&options, &second) == NH_IDENTITY_BUSY);
  assert(second == NULL);

  child = fork();
  assert(child >= 0);
  if (child == 0) {
    nh_identity_store *child_store = NULL;
    nh_identity_rc rc = nh_identity_store_open(&options, &child_store);
    if (child_store) nh_identity_store_close(child_store);
    _exit(rc == NH_IDENTITY_BUSY ? 0 : 1);
  }
  assert(waitpid(child, &child_status, 0) == child);
  assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);

  fill_hex(key_a, 'a'); fill_hex(key_b, 'b'); fill_hex(key_c, 'c');
  memset(&enroll, 0, sizeof(enroll));
  enroll.username = "n_alice"; enroll.pubkey_hex = key_a;
  enroll.home_mode = NH_IDENTITY_HOME_CREATE;
  assert(nh_identity_operation_begin_enroll(store, op_enroll, &enroll,
                                             &operation) == NH_IDENTITY_OK);
  assert(!operation.replayed && operation.phase == NH_IDENTITY_PHASE_RESERVED);
  assert(nh_identity_store_lookup_by_name(store, "n_alice", &account) == NH_IDENTITY_OK);
  assert(account.uid == 200001u && account.gid == 200001u);
  assert(account.status == NH_IDENTITY_STATUS_ENROLLING && !account.projectable);
  assert(strcmp(account.pubkey_hex, key_a) == 0);
  assert(nh_identity_store_get_info(store, &after_enroll) == NH_IDENTITY_OK);
  assert(after_enroll.authority_generation == initial_info.authority_generation + 1);

  assert(nh_identity_operation_begin_enroll(store, op_enroll, &enroll,
                                             &replay) == NH_IDENTITY_OK);
  assert(replay.replayed && strcmp(replay.account_id, operation.account_id) == 0);
  assert(nh_identity_store_get_info(store, &after_replay) == NH_IDENTITY_OK);
  assert(after_replay.authority_generation == after_enroll.authority_generation);
  enroll.username = "n_changed";
  assert(nh_identity_operation_begin_enroll(store, op_enroll, &enroll,
    &replay) == NH_IDENTITY_OPERATION_MISMATCH);

  enroll.username = "n_alice"; enroll.pubkey_hex = key_b;
  assert(nh_identity_operation_begin_enroll(store,
    "00000000-0000-4000-8000-000000000002", &enroll, &replay) ==
    NH_IDENTITY_CONFLICT);
  enroll.username = "n_other"; enroll.pubkey_hex = key_a;
  assert(nh_identity_operation_begin_enroll(store,
    "00000000-0000-4000-8000-000000000003", &enroll, &replay) ==
    NH_IDENTITY_CONFLICT);

  assert(nh_identity_operation_advance_home(store, op_enroll,
    NH_IDENTITY_PHASE_RESERVED, NH_IDENTITY_PHASE_STAGED, &staged,
    &operation) == NH_IDENTITY_OK);
  assert(nh_identity_operation_advance_home(store, op_enroll,
    NH_IDENTITY_PHASE_RESERVED, NH_IDENTITY_PHASE_STAGED, &staged,
    &operation) == NH_IDENTITY_OK && operation.replayed);
  {
    nh_identity_home_evidence wrong = {11, 999};
    assert(nh_identity_operation_advance_home(store, op_enroll,
      NH_IDENTITY_PHASE_RESERVED, NH_IDENTITY_PHASE_STAGED, &wrong,
      &operation) == NH_IDENTITY_BAD_STATE);
  }
  assert(nh_identity_operation_advance_home(store, op_enroll,
    NH_IDENTITY_PHASE_STAGED, NH_IDENTITY_PHASE_INSTALLED, &installed,
    &operation) == NH_IDENTITY_OK);
  assert(nh_identity_store_lookup_by_id(store, account.account_id, &account) == NH_IDENTITY_OK);
  assert(account.projectable && account.status == NH_IDENTITY_STATUS_ENROLLING);

  assert(nh_identity_provider_stage(store,
    "00000000-0000-4000-8000-000000000004", account.account_id,
    NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1, "{\"cipher\":\"v1\"}",
    secret, sizeof(secret), provider_id) == NH_IDENTITY_OK);
  assert(nh_identity_store_provider_get(store, account.account_id,
    NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, false, &provider) == NH_IDENTITY_OK);
  assert(provider.secret_blob_len == sizeof(secret) &&
         memcmp(provider.secret_blob, secret, sizeof(secret)) == 0);
  memset(&attestation, 0, sizeof(attestation));
  snprintf(attestation.pubkey_hex, sizeof(attestation.pubkey_hex), "%s", key_a);
  attestation.key_generation = account.key_generation + 1;
  assert(nh_identity_provider_activate(store,
    "00000000-0000-4000-8000-000000000005", provider_id, &attestation) ==
    NH_IDENTITY_STALE_GENERATION);
  attestation.key_generation = account.key_generation;
  assert(nh_identity_provider_activate(store,
    "00000000-0000-4000-8000-000000000005", provider_id, &attestation) ==
    NH_IDENTITY_OK);
  assert(nh_identity_store_provider_get(store, account.account_id,
    NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, true, &provider) == NH_IDENTITY_OK);
  {
    char replayed_provider[NH_IDENTITY_UUID_CAP];
    assert(nh_identity_provider_stage(store,
      "00000000-0000-4000-8000-000000000004", account.account_id,
      NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1, "{\"cipher\":\"v1\"}",
      secret, sizeof(secret), replayed_provider) == NH_IDENTITY_OK);
    assert(strcmp(replayed_provider, provider_id) == 0);
  }
  assert(nh_identity_store_recheck(store, account.account_id,
    account.key_generation, account.authority_generation, NULL, NULL) ==
    NH_IDENTITY_NOT_ACTIVE);

  {
    nh_identity_rc publish_result = nh_identity_store_publish_projection(
      store, &projection_generation);
    if (publish_result != NH_IDENTITY_OK)
      fprintf(stderr, "publish failed: %s (%s)\n",
        nh_identity_rc_name(publish_result),
        nh_identity_store_error_detail(store));
    assert(publish_result == NH_IDENTITY_OK);
  }
  assert(projection_generation == 1);
  assert(nh_identity_operation_get(store, op_enroll, &operation) == NH_IDENTITY_OK);
  assert(operation.phase == NH_IDENTITY_PHASE_PROJECTED);
  assert(nh_identity_reader_open(projection_path, &reader) ==
    NH_IDENTITY_READER_FOUND);
  assert(nh_identity_reader_getpwnam(reader, "n_alice", &passwd_record,
    reader_buffer, 2, &reader_required) == NH_IDENTITY_READER_TOO_SMALL);
  assert(reader_required > 2);
  assert(nh_identity_reader_getpwnam(reader, "n_alice", &passwd_record,
    reader_buffer, sizeof(reader_buffer), &reader_required) ==
    NH_IDENTITY_READER_FOUND);
  assert(passwd_record.uid == account.uid &&
         strcmp(passwd_record.home, account.home) == 0);
  assert(nh_identity_reader_getpwnam(reader, "invalid", &passwd_record,
    reader_buffer, sizeof(reader_buffer), &reader_required) ==
    NH_IDENTITY_READER_NOT_FOUND);
  assert(nh_identity_reader_getgrgid(reader, account.gid, &group_record,
    reader_buffer, sizeof(reader_buffer), &reader_required) ==
    NH_IDENTITY_READER_FOUND);
  assert(strcmp(group_record.name, "n_alice") == 0);
  nh_identity_reader_close(reader); reader = NULL;
  assert(stat(projection_path, &st) == 0 && (st.st_mode & 0777) == 0644);
  {
    char journal_path[300], wal_path[300];
    snprintf(journal_path, sizeof(journal_path), "%s-journal", projection_path);
    snprintf(wal_path, sizeof(wal_path), "%s-wal", projection_path);
    assert(access(journal_path, F_OK) != 0 && access(wal_path, F_OK) != 0);
  }
  {
    ino_t old_inode = st.st_ino;
    assert(chmod(temporary, 0500) == 0);
    assert(nh_identity_store_publish_projection(store, NULL) ==
      NH_IDENTITY_STORAGE_ERROR);
    assert(chmod(temporary, 0700) == 0);
    assert(stat(projection_path, &st) == 0 && st.st_ino == old_inode);
    assert(nh_identity_reader_open(projection_path, &reader) ==
      NH_IDENTITY_READER_FOUND);
    assert(nh_identity_reader_getpwnam(reader, "n_alice", &passwd_record,
      reader_buffer, sizeof(reader_buffer), &reader_required) ==
      NH_IDENTITY_READER_FOUND);
    nh_identity_reader_close(reader); reader = NULL;
  }
  assert(nh_identity_operation_activate(store, op_enroll, &operation) ==
    NH_IDENTITY_OK);
  assert(nh_identity_store_lookup_by_id(store, account.account_id, &account) ==
    NH_IDENTITY_OK);
  assert(account.status == NH_IDENTITY_STATUS_ACTIVE);
  assert(nh_identity_store_recheck(store, account.account_id,
    account.key_generation, account.authority_generation, NULL, NULL) ==
    NH_IDENTITY_OK);

  assert(nh_identity_account_replace_identity(store,
    "00000000-0000-4000-8000-000000000006", account.account_id, key_c, true) ==
    NH_IDENTITY_OK);
  assert(nh_identity_store_lookup_by_id(store, account.account_id, &account) == NH_IDENTITY_OK);
  assert(account.status == NH_IDENTITY_STATUS_DISABLED &&
         account.key_generation == 2 && account.enabled_providers == 0 &&
         strcmp(account.pubkey_hex, key_c) == 0);
  assert(nh_identity_store_publish_projection(store, NULL) == NH_IDENTITY_OK);
  assert(nh_identity_reader_open(projection_path, &reader) ==
    NH_IDENTITY_READER_FOUND);
  assert(nh_identity_reader_getpwnam(reader, "n_alice", &passwd_record,
    reader_buffer, sizeof(reader_buffer), &reader_required) ==
    NH_IDENTITY_READER_FOUND);
  nh_identity_reader_close(reader); reader = NULL;
  assert(nh_identity_store_recheck(store, account.account_id,
    account.key_generation, account.authority_generation, NULL, NULL) ==
    NH_IDENTITY_NOT_ACTIVE);
  assert(nh_identity_account_set_status(store,
    "00000000-0000-4000-8000-000000000007", account.account_id,
    NH_IDENTITY_STATUS_RETIRED) == NH_IDENTITY_OK);
  assert(nh_identity_store_lookup_by_id(store, account.account_id, &retired) == NH_IDENTITY_OK);
  assert(retired.status == NH_IDENTITY_STATUS_RETIRED && retired.pubkey_hex[0] == '\0');
  assert(nh_identity_store_publish_projection(store, NULL) == NH_IDENTITY_OK);
  assert(nh_identity_reader_open(projection_path, &reader) ==
    NH_IDENTITY_READER_FOUND);
  assert(nh_identity_reader_getpwnam(reader, "n_alice", &passwd_record,
    reader_buffer, sizeof(reader_buffer), &reader_required) ==
    NH_IDENTITY_READER_FOUND);
  nh_identity_reader_close(reader); reader = NULL;

  enroll.username = "n_bob"; enroll.pubkey_hex = key_b;
  assert(nh_identity_operation_begin_enroll(store,
    "00000000-0000-4000-8000-000000000008", &enroll, &operation) == NH_IDENTITY_OK);
  assert(nh_identity_store_lookup_by_name(store, "n_bob", &next) == NH_IDENTITY_OK);
  assert(next.uid == 200002u && next.uid != retired.uid);

  nh_identity_store_close(store); store = NULL;
  sql_expect_failure(db_path, "UPDATE accounts SET uid=299999 WHERE username='n_bob'");
  sql_expect_failure(db_path, "UPDATE accounts SET username='n_taken' WHERE username='n_bob'");
  sql_expect_failure(db_path, "UPDATE accounts SET home='/tmp/takeover' WHERE username='n_bob'");
  sql_expect_failure(db_path, "DELETE FROM accounts WHERE username='n_bob'");

  {
    sqlite3 *db = NULL;
    assert(sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db, "PRAGMA user_version=2", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
    options.flags = 0;
    assert(nh_identity_store_open(&options, &store) == NH_IDENTITY_SCHEMA_UNSUPPORTED);
    assert(store == NULL);
    assert(sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db, "PRAGMA user_version=1;DELETE FROM metadata WHERE key='authority_id'",
                        NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
    assert(nh_identity_store_open(&options, &store) == NH_IDENTITY_STORAGE_ERROR);
    assert(store == NULL);
  }

  remove_tree(temporary, db_path, lock_path);
  puts("identity store tests: PASS");
  return 0;
}
