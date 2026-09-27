/* hanami_test_fs.h - recursive remove for test scratch dirs (nostrc-xw95)
 *
 * SPDX-License-Identifier: MIT
 *
 * Replaces system("rm -rf ...") in tests: no shell (so no quoting hazards on
 * odd paths) and a checkable result instead of an ignored system() status.
 * Missing paths are not an error; failures are reported on stderr and
 * returned so callers that care can assert on them.
 */
#ifndef HANAMI_TEST_FS_H
#define HANAMI_TEST_FS_H

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static inline int
hanami_test_rm_rf(const char *path)
{
  struct stat st;
  if (path == NULL || *path == '\0')
    return 0;
  if (lstat(path, &st) != 0)
    return errno == ENOENT ? 0 : -1;
  int rc = 0;
  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    if (d == NULL) {
      fprintf(stderr, "rm_rf: opendir(%s): %s\n", path, strerror(errno));
      return -1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
        continue;
      size_t n = strlen(path) + strlen(e->d_name) + 2;
      char *child = malloc(n);
      if (child == NULL) {
        rc = -1;
        break;
      }
      snprintf(child, n, "%s/%s", path, e->d_name);
      if (hanami_test_rm_rf(child) != 0)
        rc = -1;
      free(child);
    }
    closedir(d);
    if (rmdir(path) != 0) {
      fprintf(stderr, "rm_rf: rmdir(%s): %s\n", path, strerror(errno));
      rc = -1;
    }
  } else if (unlink(path) != 0) {
    fprintf(stderr, "rm_rf: unlink(%s): %s\n", path, strerror(errno));
    rc = -1;
  }
  return rc;
}

#endif /* HANAMI_TEST_FS_H */
