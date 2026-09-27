#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* struct ucred */
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <pthread.h>
#include "json.h"
#include "nostr/nip5f/nip5f.h"
#include "sock_internal.h"
#include "sock_conn.h"

// Ensure a JSON implementation is installed for libnostr before handling requests.
// This avoids nostr_event_deserialize() returning -1 due to a NULL json_interface.
static void ensure_json(void)
{
  extern NostrJsonInterface *jansson_impl;
  nostr_set_json_interface(jansson_impl);
  nostr_json_init();
}

struct Nip5fServer {
  void *ud;
  Nip5fGetPubFn get_pub;
  Nip5fSignEventFn sign_event;
  Nip5fNip44EncFn enc44;
  Nip5fNip44DecFn dec44;
  Nip5fListKeysFn list_keys;
  int have_hooks;
  Nip5fServerHooks hooks;
  void *hooks_ud;
  char *socket_path;
  int listen_fd;
  pthread_t accept_thr;
  int stop;
};

void nip5f_peer_from_fd(int fd, Nip5fPeer *peer) {
  memset(peer, 0, sizeof *peer);
  peer->pidfd = -1;
  peer->conn_fd = fd;
#if defined(__linux__) && defined(SO_PEERCRED)
  struct ucred uc;
  socklen_t len = sizeof uc;
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &len) == 0 && len == sizeof uc) {
    peer->have_creds = 1;
    peer->uid = (unsigned int)uc.uid;
    peer->pid = (int)uc.pid;
  }
# ifdef SO_PEERPIDFD
  /* Linux >= 6.5: pins the connecting process, so a PID reused after it
   * exits cannot be mistaken for it. */
  int pidfd = -1;
  len = sizeof pidfd;
  if (getsockopt(fd, SOL_SOCKET, SO_PEERPIDFD, &pidfd, &len) == 0 && pidfd >= 0)
    peer->pidfd = pidfd;
# endif
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  uid_t euid; gid_t egid;
  if (getpeereid(fd, &euid, &egid) == 0) {
    peer->have_creds = 1;
    peer->uid = (unsigned int)euid;
  }
# if defined(__APPLE__) && defined(LOCAL_PEERPID)
  pid_t pid = 0;
  socklen_t len = sizeof pid;
  if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &len) == 0 && pid > 0)
    peer->pid = (int)pid;
# endif
#endif
}

/* A client that hangs up must not kill the server with SIGPIPE; sends use
 * MSG_NOSIGNAL where it exists (Linux), this covers macOS/BSD. */
static void no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int on = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
  (void)fd;
#endif
}

static void *accept_loop(void *arg) {
  struct Nip5fServer *s = (struct Nip5fServer*)arg;
  for (;;) {
    if (s->stop) break;
    int cfd = accept(s->listen_fd, NULL, NULL);
    if (cfd < 0) {
      if (errno == EINTR) continue;
      if (s->stop) break;
      // brief sleep to avoid busy loop on fatal error
      usleep(10000);
      continue;
    }
    /* Only our own uid: the socket is 0600 in a 0700 directory, but a
     * socket path handed to (or bind-mounted into) another user's process
     * must not reach the key. */
    struct Nip5fConnArg *carg = (struct Nip5fConnArg*)calloc(1, sizeof(*carg));
    if (!carg) { close(cfd); continue; }
    nip5f_peer_from_fd(cfd, &carg->peer);
    if (!carg->peer.have_creds || carg->peer.uid != (unsigned int)geteuid()) {
      if (carg->peer.pidfd >= 0) close(carg->peer.pidfd);
      free(carg);
      close(cfd);
      continue;
    }
    no_sigpipe(cfd);
    carg->fd = cfd;
    carg->handshake = 1; /* banner + hello on the connection's own thread */
    carg->ud = s->ud;
    carg->get_pub = s->get_pub;
    carg->sign_event = s->sign_event;
    carg->enc44 = s->enc44;
    carg->dec44 = s->dec44;
    carg->list_keys = s->list_keys;
    carg->have_hooks = s->have_hooks;
    carg->hooks = s->hooks;
    carg->hooks_ud = s->hooks_ud;
    pthread_t thr;
    if (pthread_create(&thr, NULL, nip5f_conn_thread, carg) == 0) {
      pthread_detach(thr);
    } else {
      if (carg->peer.pidfd >= 0) close(carg->peer.pidfd);
      free(carg);
      close(cfd);
    }
  }
  return NULL;
}

int nostr_nip5f_serve_connection(int fd, const Nip5fPeer *peer, const Nip5fServerHooks *hooks,
                                 void *user_data) {
  if (fd < 0 || !peer || !hooks || !hooks->request) return -1;
  ensure_json();
  struct Nip5fConnArg *carg = (struct Nip5fConnArg*)calloc(1, sizeof(*carg));
  if (!carg) return -1;
  no_sigpipe(fd);
  carg->fd = fd;
  carg->peer = *peer;
  carg->peer.conn_fd = fd;
  carg->have_hooks = 1;
  carg->hooks = *hooks;
  carg->hooks_ud = user_data;
  pthread_t thr;
  if (pthread_create(&thr, NULL, nip5f_conn_thread, carg) != 0) {
    free(carg);
    return -1;
  }
  pthread_detach(thr);
  return 0;
}

static int server_start(const char *socket_path, const Nip5fServerHooks *hooks, void *hooks_ud,
                        void **out_handle) {
  // Make sure JSON interface is configured (jansson) before any connections.
  ensure_json();
  if (!out_handle) return -1;
  struct Nip5fServer *s = (struct Nip5fServer*)calloc(1, sizeof(*s));
  if (!s) return -1;
  if (hooks) {
    if (!hooks->request) { free(s); return -1; }
    s->have_hooks = 1;
    s->hooks = *hooks;
    s->hooks_ud = hooks_ud;
  }
  char *resolved = NULL;
  if (socket_path && *socket_path) {
    resolved = strdup(socket_path);
  } else {
    resolved = nip5f_resolve_socket_path();
  }
  if (!resolved) { free(s); return -1; }
  s->socket_path = resolved;
  if (nip5f_ensure_socket_dirs(s->socket_path) != 0) {
    free(s->socket_path); free(s); return -1;
  }

  // Remove stale socket if present and not in use
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  size_t maxlen = sizeof(addr.sun_path)-1;
  strncpy(addr.sun_path, s->socket_path, maxlen);
  addr.sun_path[maxlen] = '\0';

  // Try connecting to detect active server; if not active, unlink
  int probe = socket(AF_UNIX, SOCK_STREAM, 0);
  if (probe >= 0) {
    if (connect(probe, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
      // not active, safe to unlink
      unlink(s->socket_path);
    }
    close(probe);
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) { free(s->socket_path); free(s); return -1; }
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    close(fd); free(s->socket_path); free(s); return -1;
  }
  // Set permissions to 0600
  chmod(s->socket_path, 0600);
  if (listen(fd, 64) != 0) {
    unlink(s->socket_path);
    close(fd); free(s->socket_path); free(s); return -1;
  }
  s->listen_fd = fd;
  s->stop = 0;
  if (pthread_create(&s->accept_thr, NULL, accept_loop, s) != 0) {
    unlink(s->socket_path);
    close(fd); free(s->socket_path); free(s); return -1;
  }
  *out_handle = s;
  return 0;
}

int nostr_nip5f_server_start(const char *socket_path, void **out_handle) {
  return server_start(socket_path, NULL, NULL, out_handle);
}

int nostr_nip5f_server_start_with_hooks(const char *socket_path, const Nip5fServerHooks *hooks,
                                        void *user_data, void **out_handle) {
  if (!hooks) return -1;
  return server_start(socket_path, hooks, user_data, out_handle);
}

int nostr_nip5f_server_stop(void *handle) {
  if (!handle) return 0;
  struct Nip5fServer *s = (struct Nip5fServer*)handle;
  s->stop = 1;
  if (s->listen_fd > 0) {
    // Wake accept by closing fd
    shutdown(s->listen_fd, SHUT_RDWR);
    close(s->listen_fd);
  }
  if (s->accept_thr) {
    pthread_join(s->accept_thr, NULL);
  }
  if (s->socket_path) {
    unlink(s->socket_path);
    free(s->socket_path);
  }
  free(s);
  return 0;
}

int nostr_nip5f_server_set_handlers(void *handle,
  Nip5fGetPubFn get_pub, Nip5fSignEventFn sign_event,
  Nip5fNip44EncFn enc44, Nip5fNip44DecFn dec44,
  Nip5fListKeysFn list_keys, void *user_data)
{
  if (!handle) return -1;
  struct Nip5fServer *s = (struct Nip5fServer*)handle;
  if (s->have_hooks) return -1; /* a gated server never falls back to these */
  s->get_pub = get_pub;
  s->sign_event = sign_event;
  s->enc44 = enc44;
  s->dec44 = dec44;
  s->list_keys = list_keys;
  s->ud = user_data;
  return 0;
}
