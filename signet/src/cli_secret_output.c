/* SPDX-License-Identifier: MIT */

#include "signet/cli_secret_output.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib.h>

int signet_cli_write_secret_bytes(const char *path,
                                  const uint8_t *secret,
                                  size_t len) {
  if (!path || !path[0] || !secret || len == 0) return -1;
  struct stat current;
  if (lstat(path, &current) == 0) {
    if (!S_ISREG(current.st_mode) || current.st_uid != geteuid()) return -1;
  } else if (errno != ENOENT) {
    return -1;
  }

  char *tmp = g_strdup_printf("%s.tmp.%ld", path, (long)getpid());
  if (!tmp) return -1;
  int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0) {
    g_free(tmp);
    return -1;
  }
  size_t off = 0;
  bool ok = true;
  while (off < len) {
    ssize_t wr = write(fd, secret + off, len - off);
    if (wr < 0) {
      if (errno == EINTR) continue;
      ok = false;
      break;
    }
    off += (size_t)wr;
  }
  if (ok) ok = (fsync(fd) == 0 && fchmod(fd, 0600) == 0);
  if (close(fd) != 0) ok = false;
  if (ok && rename(tmp, path) != 0) ok = false;
  if (ok) {
    char *dir = g_path_get_dirname(path);
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
      if (fsync(dfd) != 0) ok = false;
      close(dfd);
    } else {
      ok = false;
    }
    g_free(dir);
  }
  if (!ok) unlink(tmp);
  g_free(tmp);
  return ok ? 0 : -1;
}
