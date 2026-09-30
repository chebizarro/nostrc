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
#include <json-glib/json-glib.h>
#include <sodium.h>

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

int signet_cli_handle_delivery_result(const char *reply_json,
                                      const char *out_path) {
  g_autoptr(JsonParser) p = json_parser_new();
  if (!reply_json || !out_path ||
      !json_parser_load_from_data(p, reply_json, -1, NULL)) {
    fprintf(stderr, "signetctl: malformed credential delivery reply\n");
    return 1;
  }
  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return 1;
  JsonObject *res = json_node_get_object(root);
  if (json_object_has_member(res, "result")) {
    JsonNode *rn = json_object_get_member(res, "result");
    if (rn && JSON_NODE_HOLDS_OBJECT(rn)) res = json_node_get_object(rn);
  }
  const char *encoded = json_object_has_member(res, "payload_b64")
      ? json_object_get_string_member(res, "payload_b64") : NULL;
  if (!encoded || !encoded[0]) {
    fprintf(stderr, "signetctl: credential delivery carried no payload\n");
    return 1;
  }
  gsize len = 0;
  guchar *decoded = g_base64_decode(encoded, &len);
  if (!decoded || len == 0 ||
      signet_cli_write_secret_bytes(out_path, decoded, len) != 0) {
    if (decoded) { sodium_memzero(decoded, len); g_free(decoded); }
    fprintf(stderr, "signetctl: failed to write protected credential output\n");
    return 1;
  }
  sodium_memzero(decoded, len);
  g_free(decoded);
  printf("credential: written to %s\n", out_path);
  return 0;
}
