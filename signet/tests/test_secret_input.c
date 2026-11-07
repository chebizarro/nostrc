#include "test_check.h"
#include "../src/secret_input.h"
#include <sodium.h>
#include <glib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

static void put(const char *path, const void *value, size_t len) {
  int fd = open(path, O_WRONLY | O_TRUNC | O_CREAT, 0600);
  CHECK(fd >= 0);
  CHECK(write(fd, value, len) == (ssize_t)len);
  close(fd);
}

int main(void) {
  CHECK(sodium_init() >= 0);
  char *dir = g_dir_make_tmp("signet-secret-XXXXXX", NULL);
  CHECK(dir);
  char *path = g_build_filename(dir, "key", NULL);
  char *link = g_build_filename(dir, "link", NULL);
  char *out = NULL;
  unsetenv("SIGNET_TEST_KEY"); unsetenv("SIGNET_TEST_KEY_FILE");
  CHECK(signet_secret_input_resolve("SIGNET_TEST_KEY", "SIGNET_TEST_KEY_FILE", &out) == 1);
  put(path, "legacy master string\r\n", 22);
  setenv("SIGNET_TEST_KEY_FILE", path, 1);
  CHECK(signet_secret_input_resolve("SIGNET_TEST_KEY", "SIGNET_TEST_KEY_FILE", &out) == 0);
  CHECK(strcmp(out, "legacy master string") == 0);
  sodium_free(out);
  setenv("SIGNET_TEST_KEY", "", 1);
  CHECK(signet_secret_input_resolve("SIGNET_TEST_KEY", "SIGNET_TEST_KEY_FILE", &out) == 0);
  sodium_free(out);
  setenv("SIGNET_TEST_KEY", "ambiguous", 1);
  CHECK(signet_secret_input_resolve("SIGNET_TEST_KEY", "SIGNET_TEST_KEY_FILE", &out) == -1);
  CHECK(out == NULL);
  unsetenv("SIGNET_TEST_KEY");
  CHECK(chmod(path, 0640) == 0);
  CHECK(signet_secret_input_file(path, &out) == -1);
  CHECK(chmod(path, 0400) == 0);
  CHECK(signet_secret_input_file(path, &out) == 0);
  sodium_free(out);
  CHECK(symlink(path, link) == 0);
  CHECK(signet_secret_input_file(link, &out) == -1);
  CHECK(signet_secret_input_file(dir, &out) == -1);
  CHECK(chmod(path, 0600) == 0);
  const char *bad[] = {"", "\n", "a\nb", "a\r", "a\n\n"};
  for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
    put(path, bad[i], strlen(bad[i]));
    CHECK(signet_secret_input_file(path, &out) == -1);
  }
  put(path, "a\0b", 3);
  CHECK(signet_secret_input_file(path, &out) == -1);
  char max[4097]; memset(max, 'x', sizeof(max));
  put(path, max, 4096);
  CHECK(signet_secret_input_file(path, &out) == 0);
  CHECK(strlen(out) == 4096); sodium_free(out);
  put(path, max, sizeof(max));
  CHECK(signet_secret_input_file(path, &out) == -1);
  if (geteuid() == 0) {
    CHECK(chown(path, 1, (gid_t)-1) == 0);
    CHECK(signet_secret_input_file(path, &out) == -1);
  }
  unlink(link); unlink(path); rmdir(dir);
  g_free(link); g_free(path); g_free(dir);
  return 0;
}
