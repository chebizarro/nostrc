#define _GNU_SOURCE
#include "auth_boundary.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

struct rate {
  int used;
  uid_t uid;
  uint64_t start;
  unsigned attempts;
};
struct nh_auth_boundary {
  struct nh_auth_connection *connections[NH_AUTH_BOUNDARY_CONNECTIONS];
  struct rate rates[NH_AUTH_BOUNDARY_RATE_BUCKETS];
  uint64_t last_now;
};
struct nh_auth_connection {
  nh_auth_boundary *owner;
  int fd, pidfd, closed;
  nh_auth_peer_snapshot peer;
  char transaction[65];
  nh_auth_disconnect_fn disconnect;
  void *context;
};
static void terminate(nh_auth_connection *c) {
  if (c->closed)
    return;
  c->closed = 1;
  close(c->fd);
  close(c->pidfd);
  c->fd = c->pidfd = -1;
  if (c->disconnect)
    c->disconnect(c->context);
}
void nh_auth_connection_close(nh_auth_connection *c) {
  if (!c)
    return;
  terminate(c);
  for (size_t i = 0; i < NH_AUTH_BOUNDARY_CONNECTIONS; i++)
    if (c->owner->connections[i] == c)
      c->owner->connections[i] = NULL;
  OPENSSL_cleanse(c, sizeof *c);
  free(c);
}
nh_auth_boundary *nh_auth_boundary_new(void) {
  return calloc(1, sizeof(nh_auth_boundary));
}
void nh_auth_boundary_free(nh_auth_boundary *b) {
  if (!b)
    return;
  for (size_t i = 0; i < NH_AUTH_BOUNDARY_CONNECTIONS; i++)
    nh_auth_connection_close(b->connections[i]);
  free(b);
}
static int alive(int fd) {
  struct pollfd p = {fd, POLLIN, 0};
  int r;
  do {
    r = poll(&p, 1, 0);
  } while (r < 0 && errno == EINTR);
  return r == 0;
}
static uint64_t process_start(pid_t pid) {
  char path[64], buf[4096];
  snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
  FILE *f = fopen(path, "re");
  if (!f)
    return 0;
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  char *p = strrchr(buf, ')');
  if (!p || p[1] != ' ')
    return 0;
  p += 2;
  for (unsigned field = 3; field < 22; field++) {
    p = strchr(p, ' ');
    if (!p)
      return 0;
    p++;
  }
  char *end = NULL;
  errno = 0;
  unsigned long long value = strtoull(p, &end, 10);
  return !errno && end != p && *end == ' ' ? (uint64_t)value : 0;
}
static int rate(nh_auth_boundary *b, uid_t uid, uint64_t now) {
  if (now < b->last_now)
    return 0;
  b->last_now = now;
  struct rate *slot = NULL;
  for (size_t i = 0; i < NH_AUTH_BOUNDARY_RATE_BUCKETS; i++) {
    struct rate *r = &b->rates[i];
    if (r->used && r->uid == uid) {
      slot = r;
      break;
    }
    if (!slot && (!r->used || now - r->start >= NH_AUTH_BOUNDARY_WINDOW_MS))
      slot = r;
  }
  if (!slot)
    return 0;
  if (!slot->used || slot->uid != uid ||
      now - slot->start >= NH_AUTH_BOUNDARY_WINDOW_MS)
    *slot = (struct rate){1, uid, now, 0};
  if (slot->attempts >= NH_AUTH_BOUNDARY_ATTEMPTS)
    return 0;
  slot->attempts++;
  return 1;
}
nh_auth_connection *nh_auth_boundary_accept(nh_auth_boundary *b, int fd,
                                            nh_auth_endpoint ep, uint64_t now) {
  int pidfd = -1, type = 0, enabled = 1;
  socklen_t size = sizeof type;
  struct sockaddr_un address;
  socklen_t address_size = sizeof address;
  struct ucred peer;
  socklen_t peer_size = sizeof peer;
  size_t index = 0;
  if (!b || (ep != NH_AUTH_ENDPOINT_AUTH && ep != NH_AUTH_ENDPOINT_USER))
    goto fail;
  for (; index < NH_AUTH_BOUNDARY_CONNECTIONS && b->connections[index]; index++)
    ;
  if (index == NH_AUTH_BOUNDARY_CONNECTIONS)
    goto fail;
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) ||
      type != SOCK_SEQPACKET ||
      getpeername(fd, (struct sockaddr *)&address, &address_size) ||
      address.sun_family != AF_UNIX ||
      getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) ||
      peer_size != sizeof peer || peer.pid <= 0 ||
      (ep == NH_AUTH_ENDPOINT_AUTH && peer.uid != 0))
    goto fail;
  pidfd = (int)syscall(SYS_pidfd_open, peer.pid, 0);
  if (pidfd < 0 || !alive(pidfd))
    goto fail;
  uint64_t start = process_start(peer.pid);
  if (!start || !alive(pidfd) || !rate(b, peer.uid, now))
    goto fail;
  if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof enabled) ||
      fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
    goto fail;
  nh_auth_connection *c = calloc(1, sizeof *c);
  if (!c)
    goto fail;
  c->owner = b;
  c->fd = fd;
  c->pidfd = pidfd;
  c->peer = (nh_auth_peer_snapshot){.endpoint = ep,
                                    .uid = peer.uid,
                                    .gid = peer.gid,
                                    .pid = peer.pid,
                                    .process_start_id = start};
  if (RAND_bytes(c->peer.connection_id, sizeof c->peer.connection_id) != 1) {
    free(c);
    goto fail;
  }
  b->connections[index] = c;
  return c;
fail:
  if (pidfd >= 0)
    close(pidfd);
  if (fd >= 0)
    close(fd);
  return NULL;
}
const nh_auth_peer_snapshot *nh_auth_connection_peer(nh_auth_connection *c) {
  return c && !c->closed ? &c->peer : NULL;
}
int nh_auth_connection_pidfd(nh_auth_connection *c) {
  return c && !c->closed ? c->pidfd : -1;
}
int nh_auth_connection_bind(nh_auth_connection *c, const char *id,
                            nh_auth_disconnect_fn fn, void *ctx) {
  if (!c || c->closed || c->transaction[0] || !id || strlen(id) != 64 || !fn)
    return -1;
  for (size_t i = 0; i < 64; i++)
    if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
      return -1;
  memcpy(c->transaction, id, 65);
  c->disconnect = fn;
  c->context = ctx;
  return 0;
}
int nh_auth_connection_receive(nh_auth_connection *c, nh_auth_message *out) {
  if (!c || !out || c->closed)
    return -1;
  memset(out, 0, sizeof *out);
  if (!alive(c->pidfd)) {
    terminate(c);
    return -1;
  }
  unsigned char packet[NH_AUTH_PACKET_MAX];
  union {
    struct cmsghdr align;
    unsigned char
        bytes[CMSG_SPACE(sizeof(struct ucred)) + CMSG_SPACE(16 * sizeof(int))];
  } control;
  struct iovec iov = {packet, sizeof packet};
  struct msghdr msg = {.msg_iov = &iov,
                       .msg_iovlen = 1,
                       .msg_control = control.bytes,
                       .msg_controllen = sizeof control};
  ssize_t n = recvmsg(c->fd, &msg, MSG_DONTWAIT | MSG_TRUNC | MSG_CMSG_CLOEXEC);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    return 0;
  int bad = n <= 0 || n > (ssize_t)sizeof packet ||
            (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
  unsigned credentials = 0;
  if (n >= 0)
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg, h)) {
      if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS) {
        size_t count = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        int *fds = (int *)CMSG_DATA(h);
        for (size_t i = 0; i < count; i++)
          close(fds[i]);
        bad = 1;
      } else if (h->cmsg_level == SOL_SOCKET &&
                 h->cmsg_type == SCM_CREDENTIALS &&
                 h->cmsg_len == CMSG_LEN(sizeof(struct ucred))) {
        struct ucred *p = (struct ucred *)CMSG_DATA(h);
        credentials++;
        if (p->pid != c->peer.pid || p->uid != c->peer.uid ||
            p->gid != c->peer.gid)
          bad = 1;
      } else
        bad = 1;
    }
  if (credentials != 1)
    bad = 1;
  if (!bad && nh_auth_message_parse(packet, (size_t)n, out))
    bad = 1;
  OPENSSL_cleanse(packet, sizeof packet);
  if (!bad) {
    int owns =
        c->transaction[0] && !strcmp(c->transaction, out->transaction_id);
    if (!nh_auth_operation_allowed(c->peer.endpoint, out->operation,
                                   c->peer.uid, owns))
      bad = 1;
    if (out->transaction_id[0] && !owns)
      bad = 1;
    if (c->transaction[0] && (out->operation == NH_AUTH_OP_BEGIN_LOGIN ||
                              out->operation == NH_AUTH_OP_BEGIN_SMB_PROOF))
      bad = 1;
  }
  if (bad) {
    nh_auth_message_clear(out);
    terminate(c);
    return -1;
  }
  return 1;
}
