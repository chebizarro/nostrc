#define _GNU_SOURCE
#include "fscrypt_copy.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>

static int mount_id(int fd, unsigned long long *id) {
  struct statx s;
  if (statx(fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &s) || !(s.stx_mask & STATX_MNT_ID)) return -1;
  *id = s.stx_mnt_id;
  return 0;
}
static int attributes(int src, int dst, const struct stat *st) {
  ssize_t n = flistxattr(src, NULL, 0);
  if (n < 0) return -1;
  char *names = malloc((size_t)n + 1);
  if (!names) return -1;
  int rc = -1;
  if (flistxattr(src, names, (size_t)n) != n || fchown(dst, st->st_uid, st->st_gid) || fchmod(dst, st->st_mode & 07777)) goto out;
  for (char *p = names; p < names + n; p += strlen(p) + 1) {
    ssize_t size = fgetxattr(src, p, NULL, 0);
    if (size < 0) goto out;
    void *value = malloc((size_t)size + 1);
    if (!value) goto out;
    int bad = fgetxattr(src, p, value, (size_t)size) != size || fsetxattr(dst, p, value, (size_t)size, 0);
    free(value);
    if (bad) goto out;
  }
  struct timespec times[2] = {st->st_atim, st->st_mtim};
  if (futimens(dst, times) || fsync(dst)) goto out;
  rc = 0;
out:
  free(names);
  return rc;
}
static int unchanged(int fd, const struct stat *old) {
  struct stat now;
  return !fstat(fd, &now) && now.st_dev == old->st_dev && now.st_ino == old->st_ino &&
      now.st_size == old->st_size && now.st_mtim.tv_sec == old->st_mtim.tv_sec &&
      now.st_mtim.tv_nsec == old->st_mtim.tv_nsec && now.st_ctim.tv_sec == old->st_ctim.tv_sec &&
      now.st_ctim.tv_nsec == old->st_ctim.tv_nsec;
}
static int copy_file(int src, int dst, const struct stat *st) {
  char buf[65536];
  off_t offset = 0;
  while (offset < st->st_size) {
    ssize_t n = pread(src, buf, sizeof buf, offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    /* Avoid expanding wholly-zero blocks; preserve contents without copying
     * inherited fscrypt context or requiring a reflink across policies. */
    int zero = 1;
    for (ssize_t i = 0; i < n; i++) if (buf[i]) { zero = 0; break; }
    if (!zero) {
      ssize_t done = 0;
      while (done < n) {
        ssize_t wrote = pwrite(dst, buf + done, (size_t)(n - done), offset + done);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) return -1;
        done += wrote;
      }
    }
    offset += n;
  }
  if (ftruncate(dst, st->st_size) || !unchanged(src, st)) return -1;
  return attributes(src, dst, st);
}
static int walk(int src, int dst, unsigned long long mount, unsigned depth, int remove) {
  if (depth > 256) { errno = ELOOP; return -1; }
  struct stat before;
  if (fstat(src, &before)) return -1;
  int scan = openat(src, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (scan < 0) return -1;
  DIR *dir = fdopendir(scan);
  if (!dir) { close(scan); return -1; }
  int rc = 0;
  struct dirent *entry;
  for (;;) {
    errno = 0; entry = readdir(dir);
    if (!entry) { if (errno) rc = -1; break; }
    const char *name = entry->d_name;
    if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
    struct stat st;
    if (fstatat(src, name, &st, AT_SYMLINK_NOFOLLOW)) { rc = -1; break; }
    if (!remove && !S_ISDIR(st.st_mode) && st.st_nlink != 1) { errno = EMLINK; rc = -1; break; }
    if (S_ISLNK(st.st_mode)) {
      if (remove) rc = unlinkat(src, name, 0);
      else {
        char target[4096], path[8192];
        ssize_t n = readlinkat(src, name, target, sizeof target - 1);
        if (n < 0 || n == sizeof target - 1) { rc = -1; break; }
        target[n] = 0;
        /* Linux generally forbids symlink user xattrs. Refuse any that exist
         * rather than silently dropping a security label or other metadata. */
        snprintf(path, sizeof path, "/proc/self/fd/%d/%s", src, name);
        if (llistxattr(path, NULL, 0) != 0) { errno = ENOTSUP; rc = -1; break; }
        struct timespec times[2] = {st.st_atim, st.st_mtim};
        rc = symlinkat(target, dst, name) || fchownat(dst, name, st.st_uid, st.st_gid, AT_SYMLINK_NOFOLLOW) ||
            utimensat(dst, name, times, AT_SYMLINK_NOFOLLOW);
      }
    } else if (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode)) {
      int child = openat(src, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (S_ISDIR(st.st_mode) ? O_DIRECTORY : 0));
      unsigned long long child_mount;
      struct stat opened;
      if (child < 0 || fstat(child, &opened) || opened.st_dev != st.st_dev || opened.st_ino != st.st_ino ||
          mount_id(child, &child_mount) || child_mount != mount) {
        if (child >= 0) close(child);
        rc = -1; break;
      }
      if (remove) {
        if (S_ISDIR(st.st_mode)) rc = walk(child, -1, mount, depth + 1, 1);
        close(child);
        if (!rc) rc = unlinkat(src, name, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0);
      } else {
        int dest = -1;
        if (S_ISDIR(st.st_mode)) {
          if (!mkdirat(dst, name, 0700)) dest = openat(dst, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        } else dest = openat(dst, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (dest < 0) rc = -1;
        else if (S_ISDIR(st.st_mode)) rc = walk(child, dest, mount, depth + 1, 0);
        else rc = copy_file(child, dest, &st);
        if (dest >= 0) close(dest);
        close(child);
      }
    } else { errno = ENOTSUP; rc = -1; }
    if (rc) break;
  }
  closedir(dir);
  if (!rc && !remove && (!unchanged(src, &before) || attributes(src, dst, &before))) rc = -1;
  if (!rc && remove) rc = fsync(src);
  return rc;
}
int nh_fscrypt_copy_tree(int source, int destination) {
  unsigned long long mount;
  if (geteuid() || mount_id(source, &mount)) return -1;
  return walk(source, destination, mount, 0, 0);
}
int nh_fscrypt_remove_tree(int directory) {
  unsigned long long mount;
  if (geteuid() || mount_id(directory, &mount)) return -1;
  return walk(directory, -1, mount, 0, 1);
}
