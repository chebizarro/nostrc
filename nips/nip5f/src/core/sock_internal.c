#include "sock_internal.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *join_path(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  int need_slash = (la > 0 && a[la-1] != '/');
  size_t L = la + need_slash + lb + 1;
  char *out = (char*)malloc(L);
  if (!out) return NULL;
  memcpy(out, a, la);
  size_t pos = la;
  if (need_slash) out[pos++] = '/';
  memcpy(out + pos, b, lb);
  out[pos + lb] = '\0';
  return out;
}

char *nip5f_resolve_socket_path(void) {
  const char *env = getenv("NOSTR_SIGNER_SOCK");
  if (env && *env) return strdup(env);
  const char *home = getenv("HOME");
  if (!home || !*home) return NULL;
  char *share = join_path(home, ".local/share/nostr");
  if (!share) return NULL;
  char *sock = join_path(share, "signer.sock");
  free(share);
  return sock;
}

int nip5f_ensure_socket_dirs(const char *socket_path) {
  if (!socket_path) return -1;
  // derive parent directory path
  const char *slash = strrchr(socket_path, '/');
  if (!slash) return -1;
  size_t dlen = (size_t)(slash - socket_path);
  char *dir = (char*)malloc(dlen + 1);
  if (!dir) return -1;
  memcpy(dir, socket_path, dlen);
  dir[dlen] = '\0';
  struct stat st;
  if (stat(dir, &st) == 0) {
    // exists; attempt to chmod to 0700 if directory
    if (S_ISDIR(st.st_mode)) {
      chmod(dir, 0700);
      free(dir);
      return 0;
    }
    free(dir);
    return -1;
  }
  // create recursively minimal (we only expect 3 levels to exist already)
  // Best-effort: mkpath-like for two components
  // Create parent of dir if needed
  char *parent = strdup(dir);
  if (!parent) { free(dir); return -1; }
  char *p_slash = strrchr(parent, '/');
  if (p_slash) {
    *p_slash = '\0';
    mkdir(parent, 0700);
  }
  free(parent);
  // create dir
  int rc = mkdir(dir, 0700);
  free(dir);
  return rc == 0 ? 0 : -1;
}
