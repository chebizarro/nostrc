#define _GNU_SOURCE
#include "nostr_identity.h"
#include <assert.h>
#include "../nh_test.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static nh_identity_ownership_result available(void *ctx, const char *name, uint32_t uid, uint32_t gid) {
  (void)ctx; (void)name; (void)uid; (void)gid; return NH_IDENTITY_OWNERSHIP_FREE;
}
static void reserve(nh_identity_store *store, unsigned index, nh_identity_home_mode mode,
    char operation[37], nh_identity_account *account) {
  char name[33], key[65];
  nh_identity_operation_state state;
  nh_identity_enroll_request request;
  snprintf(operation, 37, "00000000-0000-4000-8000-%012u", index);
  snprintf(name, sizeof name, "n_test%u", index);
  snprintf(key, sizeof key, "%064x", index);
  request = (nh_identity_enroll_request){name, key, NULL, mode};
  NH_CHECK(nh_identity_operation_begin_enroll(store, operation, &request, &state) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_store_lookup_by_name(store, name, account) == NH_IDENTITY_OK);
}
static void recorded_stage(nh_identity_store *store, const char *op, const char *stage) {
  struct stat st;
  nh_identity_operation_state state;
  nh_identity_home_evidence ev;
  NH_CHECK(mkdir(stage, 0700) == 0 && stat(stage, &st) == 0);
  ev = (nh_identity_home_evidence){st.st_dev, st.st_ino};
  NH_CHECK(nh_identity_operation_advance_home(store, op, NH_IDENTITY_PHASE_RESERVED,
      NH_IDENTITY_PHASE_STAGED, &ev, &state) == NH_IDENTITY_OK);
}
int main(void) {
  char root[] = "/run/nostrc-home-test-XXXXXX", path[512], stage[512], op[37];
  nh_identity_config config;
  nh_identity_store *store;
  nh_identity_store_options options;
  nh_identity_home_options homes = {0};
  nh_identity_operation_state state;
  nh_identity_account account;
  nh_identity_home_evidence ev;
  struct stat st;
  FILE *file;
  if (geteuid() != 0) { fprintf(stderr, "Requires root in disposable Linux container\n"); return 77; }
  NH_CHECK(mkdtemp(root));
  nh_identity_config_defaults(&config);
  snprintf(config.authority_path, sizeof config.authority_path, "%s/authority.db", root);
  snprintf(config.projection_path, sizeof config.projection_path, "%s/nss.db", root);
  snprintf(config.home_root, sizeof config.home_root, "%s/home", root);
  NH_CHECK(mkdir(config.home_root, 0755) == 0);
  snprintf(path, sizeof path, "%s/skel", root); NH_CHECK(mkdir(path, 0755) == 0);
  homes.skel_path = strdup(path);
  strcat(path, "/welcome"); file = fopen(path, "w"); NH_CHECK(file); fputs("hello\n", file); NH_CHECK(fclose(file) == 0);
  options = (nh_identity_store_options){&config, available, NULL, NH_IDENTITY_STORE_CREATE};
  NH_CHECK(nh_identity_store_open(&options, &store) == NH_IDENTITY_OK);
  reserve(store, 1, NH_IDENTITY_HOME_CREATE, op, &account);
  NH_CHECK(nh_identity_home_prepare(store, op, &homes, &state) == NH_IDENTITY_OK);
  NH_CHECK(state.phase == NH_IDENTITY_PHASE_INSTALLED);
  NH_CHECK(stat(account.home, &st) == 0 && st.st_uid == account.uid && st.st_gid == account.gid && (st.st_mode & 07777) == 0700);
  snprintf(path, sizeof path, "%s/welcome", account.home);
  file = fopen(path, "r"); NH_CHECK(file && fgetc(file) == 'h'); fclose(file);
  NH_CHECK(stat(path, &st) == 0 && st.st_uid == account.uid);
  NH_CHECK(nh_identity_home_prepare(store, op, &homes, &state) == NH_IDENTITY_OK && state.replayed);
  NH_CHECK(nh_identity_store_lookup_by_name(store, account.username, &account) == NH_IDENTITY_OK && account.status == NH_IDENTITY_STATUS_ENROLLING);
  NH_CHECK(nh_identity_home_validate(store, account.account_id, &state.installed_home, &ev) == NH_IDENTITY_OK);
  NH_CHECK(chmod(account.home, 0755) == 0);
  NH_CHECK(nh_identity_home_validate(store, account.account_id, &state.installed_home, &ev) == NH_IDENTITY_OWNERSHIP_CHECK_FAILED);
  NH_CHECK(chmod(account.home, 0700) == 0);
  /* A crash after staging commit, and after rename but before installed commit. */
  reserve(store, 2, NH_IDENTITY_HOME_CREATE, op, &account);
  snprintf(stage, sizeof stage, "%s/.nostr-%s", config.home_root, op);
  recorded_stage(store, op, stage);
  NH_CHECK(nh_identity_home_prepare(store, op, NULL, &state) == NH_IDENTITY_OK);
  reserve(store, 3, NH_IDENTITY_HOME_CREATE, op, &account);
  snprintf(stage, sizeof stage, "%s/.nostr-%s", config.home_root, op);
  recorded_stage(store, op, stage); NH_CHECK(rename(stage, account.home) == 0);
  NH_CHECK(nh_identity_home_prepare(store, op, NULL, &state) == NH_IDENTITY_OK);
  /* Unrecorded stage is ambiguous, retained, not trusted on restart. */
  reserve(store, 4, NH_IDENTITY_HOME_CREATE, op, &account);
  snprintf(stage, sizeof stage, "%s/.nostr-%s", config.home_root, op); NH_CHECK(mkdir(stage, 0700) == 0);
  NH_CHECK(nh_identity_home_prepare(store, op, NULL, &state) == NH_IDENTITY_OWNERSHIP_CHECK_FAILED);
  NH_CHECK(state.outcome == NH_IDENTITY_OUTCOME_REPAIR_REQUIRED && stat(stage, &st) == 0);
  /* Existing target never replaced. */
  reserve(store, 5, NH_IDENTITY_HOME_CREATE, op, &account);
  NH_CHECK(mkdir(account.home, 0700) == 0);
  NH_CHECK(nh_identity_home_prepare(store, op, NULL, &state) == NH_IDENTITY_OWNERSHIP_CHECK_FAILED);
  NH_CHECK(stat(account.home, &st) == 0 && st.st_uid == 0);
  /* An explicitly adopted valid home is not recursively chowned. */
  reserve(store, 6, NH_IDENTITY_HOME_ADOPT_EXISTING, op, &account);
  NH_CHECK(mkdir(account.home, 0700) == 0 && chown(account.home, account.uid, account.gid) == 0);
  snprintf(path, sizeof path, "%s/root-owned", account.home); file = fopen(path, "w"); NH_CHECK(file); fclose(file);
  NH_CHECK(nh_identity_home_prepare(store, op, NULL, &state) == NH_IDENTITY_OK);
  NH_CHECK(stat(path, &st) == 0 && st.st_uid == 0);
  /* Missing mandatory labeling cannot silently proceed. */
  reserve(store, 7, NH_IDENTITY_HOME_CREATE, op, &account);
  homes.labeling_required = true;
  NH_CHECK(nh_identity_home_prepare(store, op, &homes, &state) == NH_IDENTITY_UNSUPPORTED);
  NH_CHECK(access(account.home, F_OK) != 0); homes.labeling_required = false;
  /* Skeleton symlink is never followed, and incomplete stage cannot activate. */
  snprintf(path, sizeof path, "%s/escape", homes.skel_path); NH_CHECK(symlink("/etc/passwd", path) == 0);
  NH_CHECK(nh_identity_home_prepare(store, op, &homes, &state) == NH_IDENTITY_OWNERSHIP_CHECK_FAILED);
  NH_CHECK(nh_identity_operation_activate(store, op, &state) == NH_IDENTITY_BAD_STATE);
  /* Replaced staged inode must not be mistaken for a recoverable rename. */
  reserve(store, 8, NH_IDENTITY_HOME_CREATE, op, &account);
  snprintf(stage, sizeof stage, "%s/.nostr-%s", config.home_root, op); recorded_stage(store, op, stage);
  snprintf(path, sizeof path, "%s/saved", root); NH_CHECK(rename(stage, path) == 0); NH_CHECK(mkdir(stage, 0700) == 0);
  NH_CHECK(nh_identity_home_prepare(store, op, NULL, &state) == NH_IDENTITY_OWNERSHIP_CHECK_FAILED);
  /* Privilege and root-controlled-parent checks. */
  reserve(store, 9, NH_IDENTITY_HOME_CREATE, op, &account);
  NH_CHECK(chmod(config.home_root, 0777) == 0);
  NH_CHECK(nh_identity_home_prepare(store, op, NULL, &state) == NH_IDENTITY_OWNERSHIP_CHECK_FAILED);
  NH_CHECK(chmod(config.home_root, 0755) == 0);
  pid_t child = fork(); NH_CHECK(child >= 0);
  if (!child) { NH_CHECK(setuid(65534) == 0); _exit(nh_identity_home_prepare(store, op, NULL, &state) == NH_IDENTITY_PERMISSION_DENIED ? 0 : 1); }
  int status; NH_CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  free((char *)homes.skel_path); nh_identity_store_close(store);
  /* The disposable container owns cleanup; avoid recursive root deletion in a test. */
  puts("identity home: provisioning, recovery and rejection cases passed"); return 0;
}
