/* SPDX-License-Identifier: MIT */

#include "signet/cli_secret_output.h"
#include "test_check.h"

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
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

  /* Exercise the exact result handler used by signetctl while capturing both
   * output streams. The payload must reach only the protected file. */
  char *delivery_path = g_build_filename(dir, "delivered", NULL);
  char *stdout_path = g_build_filename(dir, "stdout", NULL);
  char *stderr_path = g_build_filename(dir, "stderr", NULL);
  char *b64 = g_base64_encode(first, sizeof(first) - 1);
  char *reply = g_strdup_printf(
      "{\"jsonrpc\":\"2.0\",\"result\":{\"payload_b64\":\"%s\"}}", b64);
  int saved_out = dup(STDOUT_FILENO), saved_err = dup(STDERR_FILENO);
  int out_fd = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  int err_fd = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(saved_out >= 0 && saved_err >= 0 && out_fd >= 0 && err_fd >= 0);
  CHECK(dup2(out_fd, STDOUT_FILENO) >= 0);
  CHECK(dup2(err_fd, STDERR_FILENO) >= 0);
  close(out_fd); close(err_fd);
  CHECK(signet_cli_handle_delivery_result(reply, delivery_path) == 0);
  fflush(stdout); fflush(stderr);
  CHECK(dup2(saved_out, STDOUT_FILENO) >= 0);
  CHECK(dup2(saved_err, STDERR_FILENO) >= 0);
  close(saved_out); close(saved_err);
  char *captured_out = NULL, *captured_err = NULL;
  gsize captured_len = 0;
  CHECK(g_file_get_contents(stdout_path, &captured_out, &captured_len, NULL));
  CHECK(strstr(captured_out, (const char *)first) == NULL);
  CHECK(g_file_get_contents(stderr_path, &captured_err, &captured_len, NULL));
  CHECK(strstr(captured_err, (const char *)first) == NULL);
  CHECK(g_file_get_contents(delivery_path, &contents, &len, NULL));
  CHECK(len == sizeof(first) - 1 && memcmp(contents, first, len) == 0);
  g_free(contents); g_free(captured_out); g_free(captured_err);
  g_free(reply); g_free(b64);

  unlink(link);
  unlink(target);
  unlink(path);
  unlink(delivery_path);
  unlink(stdout_path);
  unlink(stderr_path);
  rmdir(dir);
  g_free(link);
  g_free(target);
  g_free(path);
  g_free(delivery_path);
  g_free(stdout_path);
  g_free(stderr_path);
  puts("test_cli_secret_output: PASS");
  return 0;
}
