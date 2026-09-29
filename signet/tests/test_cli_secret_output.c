/* SPDX-License-Identifier: MIT */

#include "signet/cli_secret_output.h"
#include "test_check.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>

int main(void) {
  char dir_template[] = "/tmp/signet-cli-output-XXXXXX";
  char *dir = mkdtemp(dir_template);
  CHECK(dir != NULL);
  char *path = g_build_filename(dir, "credential", NULL);
  char *target = g_build_filename(dir, "target", NULL);
  char *link = g_build_filename(dir, "link", NULL);
  const uint8_t first[] = "delivery-secret-value";
  const uint8_t second[] = "rotated";

  mode_t old_umask = umask(0);
  CHECK(signet_cli_write_secret_bytes(path, first, sizeof(first) - 1) == 0);
  umask(old_umask);

  struct stat st;
  CHECK(lstat(path, &st) == 0);
  CHECK(S_ISREG(st.st_mode));
  CHECK((st.st_mode & 0777) == 0600);
  CHECK(st.st_uid == geteuid());
  char *contents = NULL;
  gsize len = 0;
  CHECK(g_file_get_contents(path, &contents, &len, NULL));
  CHECK(len == sizeof(first) - 1);
  CHECK(memcmp(contents, first, len) == 0);
  g_free(contents);

  CHECK(signet_cli_write_secret_bytes(path, second, sizeof(second) - 1) == 0);
  CHECK(lstat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
  CHECK(g_file_get_contents(path, &contents, &len, NULL));
  CHECK(len == sizeof(second) - 1);
  CHECK(memcmp(contents, second, len) == 0);
  g_free(contents);

  CHECK(g_file_set_contents(target, "x", 1, NULL));
  CHECK(symlink(target, link) == 0);
  CHECK(signet_cli_write_secret_bytes(link, first, sizeof(first) - 1) == -1);
  CHECK(signet_cli_write_secret_bytes(path, first, 0) == -1);

  unlink(link);
  unlink(target);
  unlink(path);
  rmdir(dir);
  g_free(link);
  g_free(target);
  g_free(path);
  puts("test_cli_secret_output: PASS");
  return 0;
}
