#define _GNU_SOURCE
#include "auth_porthome_stage.h"
#include "nh_porthome_manifest.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WORKSPACE ".nostr-fetch"

int nh_auth_porthome_stage_open(int stage_fd) {
  struct stat st;
  if (fstat(stage_fd, &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0 ||
      (st.st_mode & 07777) != 0700 || mkdirat(stage_fd, WORKSPACE, 0700))
    return -1;
  return openat(stage_fd, WORKSPACE, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

int nh_auth_porthome_stage_reclaim(int fd) {
  return fchown(fd, 0, 0) || fchmod(fd, 0700) ? -1 : 0;
}

typedef struct {
  uid_t uid;
  gid_t gid;
  dev_t device;
  uint64_t remaining;
  unsigned entries;
  char buffer[65536];
} import_ctx;

static int copy_file(int in, int out, import_ctx *ctx) {
  for (;;) {
    ssize_t n = read(in, ctx->buffer, sizeof ctx->buffer);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) return -1;
    if (!n) return 0;
    if ((uint64_t)n > ctx->remaining) return -1;
    ctx->remaining -= (uint64_t)n;
    for (ssize_t off = 0; off < n;) {
      ssize_t w = write(out, ctx->buffer + off, (size_t)(n - off));
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) return -1;
      off += w;
    }
  }
}

/* Every traversal uses an opened directory and a single dirent component.
 * Links are copied as links, never followed; special files and hard links
 * are not portable-home objects. Destination entries are always exclusive. */
static int import_tree(int from, int to, import_ctx *ctx, unsigned depth) {
  if (depth > 64) return -1;
  int scan = openat(from, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (scan < 0) return -1;
  DIR *dir = fdopendir(scan);
  if (!dir) { close(scan); return -1; }
  int rc = -1;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) { if (!errno) rc = 0; break; }
    const char *name = entry->d_name;
    if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
    if (++ctx->entries > NH_PORTHOME_MAX_ENTRIES || (!depth && !strcmp(name, WORKSPACE))) break;
    struct stat st;
    if (fstatat(from, name, &st, AT_SYMLINK_NOFOLLOW) ||
        st.st_dev != ctx->device || (st.st_mode & 07000)) break;
    const struct timespec times[2] = {st.st_atim, st.st_mtim};
    if (S_ISLNK(st.st_mode)) {
      char target[NH_PORTHOME_MAX_SYMLINK_LEN + 1];
      ssize_t n = readlinkat(from, name, target, sizeof target);
      if (n <= 0 || n >= (ssize_t)sizeof target || target[0] == '/') break;
      target[n] = 0;
      if (symlinkat(target, to, name) ||
          fchownat(to, name, ctx->uid, ctx->gid, AT_SYMLINK_NOFOLLOW) ||
          utimensat(to, name, times, AT_SYMLINK_NOFOLLOW) ||
          unlinkat(from, name, 0)) break;
      continue;
    }
    int is_dir = S_ISDIR(st.st_mode);
    if (!is_dir && (!S_ISREG(st.st_mode) || st.st_nlink != 1)) break;
    int in = openat(from, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
                   O_NONBLOCK | (is_dir ? O_DIRECTORY : 0));
    if (in < 0) break;
    struct stat opened;
    if (fstat(in, &opened) || opened.st_dev != st.st_dev ||
        opened.st_ino != st.st_ino || opened.st_mode != st.st_mode ||
        (!is_dir && opened.st_nlink != 1)) { close(in); break; }
    if (is_dir && mkdirat(to, name, 0700)) { close(in); break; }
    int out = openat(to, name, O_NOFOLLOW | O_CLOEXEC |
                     (is_dir ? O_RDONLY | O_DIRECTORY : O_WRONLY | O_CREAT | O_EXCL), 0600);
    if (out < 0) { close(in); break; }
    int bad = is_dir ? import_tree(in, out, ctx, depth + 1) : copy_file(in, out, ctx);
    if (!bad) bad = fchown(out, ctx->uid, ctx->gid) ||
                    fchmod(out, st.st_mode & 0777) ||
                    futimens(out, times) || fsync(out);
    close(out);
    close(in);
    if (bad || unlinkat(from, name, is_dir ? AT_REMOVEDIR : 0)) break;
  }
  closedir(dir);
  return rc;
}

int nh_auth_porthome_stage_import(int stage_fd, int workspace_fd,
                                  uid_t uid, gid_t gid, uint64_t max_bytes) {
  struct stat named, opened;
  if (nh_auth_porthome_stage_reclaim(workspace_fd) ||
      fstat(workspace_fd, &opened) ||
      fstatat(stage_fd, WORKSPACE, &named, AT_SYMLINK_NOFOLLOW) ||
      !S_ISDIR(named.st_mode) || named.st_dev != opened.st_dev ||
      named.st_ino != opened.st_ino) return -1;
  import_ctx ctx = {.uid = uid, .gid = gid, .device = opened.st_dev,
                    .remaining = max_bytes};
  if (import_tree(workspace_fd, stage_fd, &ctx, 0) ||
      unlinkat(stage_fd, WORKSPACE, AT_REMOVEDIR) || fsync(stage_fd)) return -1;
  return 0;
}
