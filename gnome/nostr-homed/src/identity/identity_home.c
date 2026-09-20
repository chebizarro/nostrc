#define _GNU_SOURCE
#include "identity_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <dirent.h>
#include <linux/openat2.h>
#include <sys/syscall.h>

/* Fail closed on kernels without openat2/renameat2. The shipping kernel floor
 * supports both. st_dev alone cannot detect same-filesystem bind mounts. */
static int beneath(int parent, const char *name, int flags) {
  struct open_how how = {.flags = (uint64_t)(flags | O_CLOEXEC | O_NOFOLLOW),
    .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV};
  return (int)syscall(SYS_openat2, parent, name, &how, sizeof how);
}
static int trusted_directory(const char *path) {
  char copy[NH_IDENTITY_HOME_CAP], *save = NULL, *part;
  int fd, next;
  struct stat st;
  if (!path || path[0] != '/' || strlen(path) >= sizeof copy) return -1;
  strcpy(copy, path);
  fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return -1;
  for (part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
    if (!strcmp(part, ".") || !strcmp(part, "..")) { close(fd); return -1; }
    next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(fd); fd = next;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || st.st_uid != 0 || (st.st_mode & 0022)) { close(fd); return -1; }
  }
  return fd;
}
static int evidence(int fd, nh_identity_home_evidence *out) {
  struct stat st;
  if (fstat(fd, &st) || !S_ISDIR(st.st_mode)) return -1;
  out->filesystem_device = (uint64_t)st.st_dev;
  out->filesystem_inode = (uint64_t)st.st_ino;
  return 0;
}
static int same(const nh_identity_home_evidence *a, const nh_identity_home_evidence *b) {
  return a->filesystem_device == b->filesystem_device &&
         a->filesystem_inode == b->filesystem_inode;
}
static int owned(int fd, const nh_identity_account *account) {
  struct stat st;
  return fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) &&
    st.st_uid == account->uid && st.st_gid == account->gid &&
    (st.st_mode & 07777) == 0700;
}
/* Root-controlled skeleton only. No symbolic links, hard links, devices,
 * sockets, FIFOs, set-id bits or mount crossings. Bound recursion and size so
 * a provisioning request cannot become unbounded privileged copying. */
static int copy_tree(int source, int target, uid_t uid, gid_t gid,
    unsigned depth, size_t *entries, uint64_t *bytes) {
  DIR *directory;
  struct dirent *entry;
  int result = -1, scan = dup(source);
  if (scan < 0 || depth > 16) { if (scan >= 0) close(scan); return -1; }
  directory = fdopendir(scan);
  if (!directory) { close(scan); return -1; }
  errno = 0;
  while ((entry = readdir(directory))) {
    struct stat st;
    int in = -1, out = -1;
    char buffer[16384];
    ssize_t n;
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    if (++*entries > 4096) goto done;
    in = beneath(source, entry->d_name, O_RDONLY | O_NONBLOCK);
    if (in < 0) goto done;
    if (fstat(in, &st) || st.st_uid != 0 || (st.st_mode & 0022) ||
        (st.st_mode & 07000)) goto entry_fail;
    if (S_ISDIR(st.st_mode)) {
      if (mkdirat(target, entry->d_name, 0700)) goto entry_fail;
      out = beneath(target, entry->d_name, O_RDONLY | O_DIRECTORY);
      if (out < 0 || copy_tree(in, out, uid, gid, depth + 1, entries, bytes)) goto entry_fail;
    } else if (S_ISREG(st.st_mode) && st.st_nlink == 1) {
      if (st.st_size < 0 || (uint64_t)st.st_size > 64 * 1024 * 1024 - *bytes) goto entry_fail;
      out = openat(target, entry->d_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (out < 0) goto entry_fail;
      while ((n = read(in, buffer, sizeof buffer)) != 0) {
        ssize_t offset = 0;
        if (n < 0) { if (errno == EINTR) continue; goto entry_fail; }
        if ((uint64_t)n > 64 * 1024 * 1024 - *bytes) goto entry_fail;
        *bytes += (uint64_t)n;
        while (offset < n) {
          ssize_t written = write(out, buffer + offset, (size_t)(n - offset));
          if (written < 0 && errno == EINTR) continue;
          if (written <= 0) goto entry_fail;
          offset += written;
        }
      }
    } else goto entry_fail;
    if (fchown(out, uid, gid) || fchmod(out, st.st_mode & 0777) || fsync(out)) goto entry_fail;
    close(in); close(out); errno = 0; continue;
entry_fail:
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    goto done;
  }
  if (errno == 0 && fsync(target) == 0) result = 0;
done:
  closedir(directory); return result;
}

nh_identity_rc nh_identity_home_validate(nh_identity_store *store,
    const char *account_id, const nh_identity_home_evidence *expected,
    nh_identity_home_evidence *out) {
  nh_identity_account account;
  nh_identity_home_evidence found;
  nh_identity_rc rc;
  int root, home;
  if (!store || !out || !nh_identity_uuid_is_valid(account_id)) return NH_IDENTITY_INVALID;
  if (geteuid() != 0) return NH_IDENTITY_PERMISSION_DENIED;
  rc = nh_identity_store_lookup_by_id(store, account_id, &account);
  if (rc != NH_IDENTITY_OK) return rc;
  root = trusted_directory(store->config.home_root);
  if (root < 0) return NH_IDENTITY_OWNERSHIP_CHECK_FAILED;
  home = beneath(root, account.username, O_RDONLY | O_DIRECTORY); close(root);
  if (home < 0) return NH_IDENTITY_OWNERSHIP_CHECK_FAILED;
  rc = evidence(home, &found) || !owned(home, &account) ||
    (expected && !same(expected, &found)) ? NH_IDENTITY_OWNERSHIP_CHECK_FAILED : NH_IDENTITY_OK;
  close(home);
  if (rc == NH_IDENTITY_OK) *out = found;
  return rc;
}

nh_identity_rc nh_identity_home_prepare(nh_identity_store *store,
    const char *operation_id, const nh_identity_home_options *options,
    nh_identity_operation_state *out) {
  nh_identity_operation_state operation;
  nh_identity_account account;
  nh_identity_home_evidence found;
  nh_identity_rc rc;
  int root = -1, home = -1, skel = -1;
  char stage[64];
  struct stat st;
  size_t entries = 0;
  uint64_t bytes = 0;
  if (!store || !out || !nh_identity_uuid_is_valid(operation_id)) return NH_IDENTITY_INVALID;
  if (geteuid() != 0) return NH_IDENTITY_PERMISSION_DENIED;
  if (options && options->labeling_required && !options->label) return NH_IDENTITY_UNSUPPORTED;
  rc = nh_identity_operation_get(store, operation_id, &operation);
  if (rc != NH_IDENTITY_OK) return rc;
  if (operation.type != NH_IDENTITY_OPERATION_ENROLL ||
      (operation.outcome != NH_IDENTITY_OUTCOME_PENDING &&
       operation.outcome != NH_IDENTITY_OUTCOME_DONE)) return NH_IDENTITY_BAD_STATE;
  rc = nh_identity_store_lookup_by_id(store, operation.account_id, &account);
  if (rc != NH_IDENTITY_OK) return rc;
  if (operation.phase >= NH_IDENTITY_PHASE_INSTALLED) {
    rc = nh_identity_home_validate(store, account.account_id, &operation.installed_home, &found);
    if (rc == NH_IDENTITY_OK) { *out = operation; out->replayed = true; }
    return rc;
  }
  root = trusted_directory(store->config.home_root);
  if (root < 0) return NH_IDENTITY_OWNERSHIP_CHECK_FAILED;
  snprintf(stage, sizeof stage, ".nostr-%s", operation_id);
  if (operation.phase == NH_IDENTITY_PHASE_RESERVED) {
    if (operation.home_mode == NH_IDENTITY_HOME_ADOPT_EXISTING) {
      /* Explicit adopt never recursively chowns or rewrites existing content. */
      home = beneath(root, account.username, O_RDONLY | O_DIRECTORY);
      if (home < 0 || !owned(home, &account) || evidence(home, &found)) goto ambiguous;
    } else {
      if (fstatat(root, account.username, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) goto ambiguous;
      if (mkdirat(root, stage, 0700)) goto ambiguous;
      home = beneath(root, stage, O_RDONLY | O_DIRECTORY);
      if (home < 0 || fchmod(home, 0700) || fsync(root)) goto ambiguous;
      if (options && options->skel_path) {
        skel = trusted_directory(options->skel_path);
        if (skel < 0 || copy_tree(skel, home, account.uid, account.gid, 0, &entries, &bytes)) goto ambiguous;
        close(skel); skel = -1;
      }
      if (evidence(home, &found)) goto ambiguous;
    }
    if (options && options->label && options->label(options->label_context, home) != NH_IDENTITY_OK) goto ambiguous;
    if (fsync(home)) goto ambiguous;
    rc = nh_identity_operation_advance_home(store, operation_id, NH_IDENTITY_PHASE_RESERVED,
        NH_IDENTITY_PHASE_STAGED, &found, &operation);
    if (rc != NH_IDENTITY_OK) goto done;
    close(home); home = -1;
  }
  if (operation.phase != NH_IDENTITY_PHASE_STAGED) { rc = NH_IDENTITY_BAD_STATE; goto done; }
  if (operation.home_mode == NH_IDENTITY_HOME_CREATE) {
    home = beneath(root, stage, O_RDONLY | O_DIRECTORY);
    if (home >= 0) {
      if (evidence(home, &found) || !same(&found, &operation.staged_home) ||
          fstat(home, &st) || st.st_uid != 0 || (st.st_mode & 07777) != 0700) goto ambiguous;
      if (syscall(SYS_renameat2, root, stage, root, account.username, RENAME_NOREPLACE)) goto ambiguous;
      if (fsync(root)) goto ambiguous;
    } else if (errno == ENOENT) {
      /* Rename completed before the installed phase was durable. Only the
       * exact recorded inode is eligible for recovery. */
      home = beneath(root, account.username, O_RDONLY | O_DIRECTORY);
      if (home < 0 || evidence(home, &found) || !same(&found, &operation.staged_home)) goto ambiguous;
    } else goto ambiguous;
    if (fstat(home, &st) || (st.st_uid != 0 && st.st_uid != account.uid)) goto ambiguous;
    if (fchown(home, account.uid, account.gid) || fchmod(home, 0700)) goto ambiguous;
  } else {
    home = beneath(root, account.username, O_RDONLY | O_DIRECTORY);
    if (home < 0 || evidence(home, &found) || !same(&found, &operation.staged_home)) goto ambiguous;
  }
  if (!owned(home, &account) || fsync(home) || fsync(root) || evidence(home, &found)) goto ambiguous;
  rc = nh_identity_operation_advance_home(store, operation_id, NH_IDENTITY_PHASE_STAGED,
      NH_IDENTITY_PHASE_INSTALLED, &found, out);
  goto done;
ambiguous:
  /* Leave all existing content intact for administrator review, never delete
   * a directory based solely on its predictable operation name. */
  rc = nh_identity_operation_fail(store, operation_id, NH_IDENTITY_OUTCOME_REPAIR_REQUIRED,
      "home_filesystem_ambiguous", out);
  if (rc == NH_IDENTITY_OK) rc = NH_IDENTITY_OWNERSHIP_CHECK_FAILED;
done:
  if (skel >= 0) close(skel);
  if (home >= 0) close(home);
  if (root >= 0) close(root);
  return rc;
}
#else
nh_identity_rc nh_identity_home_prepare(nh_identity_store *store,
    const char *id, const nh_identity_home_options *options, nh_identity_operation_state *out) {
  (void)store; (void)id; (void)options; (void)out; return NH_IDENTITY_UNSUPPORTED;
}
nh_identity_rc nh_identity_home_validate(nh_identity_store *store,
    const char *id, const nh_identity_home_evidence *expected, nh_identity_home_evidence *out) {
  (void)store; (void)id; (void)expected; (void)out; return NH_IDENTITY_UNSUPPORTED;
}
#endif
