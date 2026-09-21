#define _GNU_SOURCE
#include "auth_peer.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

/* Peer process start time (clock ticks since boot): field 22 of
 * /proc/<pid>/stat, read after the ")" that terminates the comm field so a
 * process name containing spaces or parentheses cannot shift the columns. */
static uint64_t peer_process_start(pid_t pid) {
  char path[64];
  snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  char buf[1024];
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0) return 0;
  buf[n] = '\0';
  char *rparen = strrchr(buf, ')');
  if (!rparen) return 0;
  /* Fields after comm start at (3). starttime is field 22 -> 19 fields on. */
  const char *p = rparen + 1;
  int field = 2; /* we are positioned just past field 2 (comm) */
  while (*p && field < 22) {
    if (*p == ' ') { field++; }
    p++;
  }
  if (field != 22) return 0;
  unsigned long long start = 0;
  if (sscanf(p, "%llu", &start) != 1) return 0;
  return (uint64_t)start;
}

static int fill_random(uint8_t *out, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = getrandom(out + off, len - off, 0);
    if (n < 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

int nh_auth_peer_from_fd(int fd, nh_auth_endpoint endpoint,
                         nh_auth_peer_snapshot *out) {
  if (!out) return -1;
  struct ucred cred;
  socklen_t len = sizeof cred;
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 ||
      len != sizeof cred)
    return -1;
  memset(out, 0, sizeof *out);
  out->endpoint = endpoint;
  out->uid = cred.uid;
  out->gid = cred.gid;
  out->pid = cred.pid;
  out->process_start_id = peer_process_start(cred.pid);
  if (fill_random(out->connection_id, sizeof out->connection_id) != 0)
    return -1;
  return 0;
}
