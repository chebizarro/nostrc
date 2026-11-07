#include "secret_input.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sodium.h>

#define SECRET_LIMIT 4096

int signet_secret_input_file(const char *path, char **out) {
  if (!out || sodium_init() < 0) return -1;
  *out = NULL;
  if (!path || !path[0]) return 1;
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return -1;
  struct stat st;
  char *buf = NULL;
  int rc = -1;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & 0077) ||
      st.st_size < 1 || st.st_size > SECRET_LIMIT) goto done;
  buf = sodium_malloc(SECRET_LIMIT + 1);
  if (!buf) goto done;
  size_t n = 0;
  while (n < SECRET_LIMIT + 1) {
    ssize_t got = read(fd, buf + n, SECRET_LIMIT + 1 - n);
    if (got < 0 && errno == EINTR) continue;
    if (got < 0) goto done;
    if (!got) break;
    n += (size_t)got;
  }
  if (!n || n > SECRET_LIMIT) goto done;
  if (buf[n - 1] == '\n') {
    --n;
    if (n && buf[n - 1] == '\r') --n;
  }
  if (!n || memchr(buf, '\0', n) || memchr(buf, '\n', n) ||
      memchr(buf, '\r', n)) goto done;
  buf[n] = '\0';
  *out = buf;
  buf = NULL;
  rc = 0;
done:
  if (buf) sodium_free(buf);
  close(fd);
  return rc;
}

int signet_secret_input_resolve(const char *value_env, const char *file_env,
                                char **out) {
  if (!out || sodium_init() < 0) return -1;
  *out = NULL;
  const char *value = getenv(value_env);
  const char *path = getenv(file_env);
  bool have_value = value && value[0];
  bool have_file = path && path[0];
  if (have_value && have_file) {
    fprintf(stderr, "signet: ambiguous secret inputs %s and %s\n", value_env, file_env);
    return -1;
  }
  if (have_file) {
    int rc = signet_secret_input_file(path, out);
    if (rc != 0) fprintf(stderr, "signet: invalid protected file for %s\n", file_env);
    return rc;
  }
  if (!have_value) return 1;
  size_t len = strlen(value);
  *out = sodium_malloc(len + 1);
  if (!*out) return -1;
  memcpy(*out, value, len + 1);
  return 0;
}
