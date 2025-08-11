#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
 #include <unistd.h>
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
  char *socket_path;
  int listen_fd;
  pthread_t accept_thr;
  int stop;
};

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
    // Handshake: send banner and read client hello (ignore content for now)
    const char *banner = "{\"name\":\"nostr-signer\",\"supported_methods\":[\"get_public_key\",\"sign_event\",\"nip44_encrypt\",\"nip44_decrypt\",\"list_public_keys\"]}";
    (void)nip5f_write_frame(cfd, banner, strlen(banner));
    char *hello = NULL; size_t hlen = 0;
    (void)nip5f_read_frame(cfd, &hello, &hlen);
    if (hello) free(hello);
    // Spawn a detached thread to handle requests for this connection
    pthread_t thr;
    struct Nip5fConnArg *carg = (struct Nip5fConnArg*)calloc(1, sizeof(*carg));
    if (!carg) { close(cfd); continue; }
    carg->fd = cfd;
    carg->ud = s->ud;
    carg->get_pub = s->get_pub;
    carg->sign_event = s->sign_event;
    carg->enc44 = s->enc44;
    carg->dec44 = s->dec44;
    carg->list_keys = s->list_keys;
    extern void *nip5f_conn_thread(void *arg); // implemented in sock_conn.c
    if (pthread_create(&thr, NULL, nip5f_conn_thread, carg) == 0) {
      pthread_detach(thr);
    } else {
      free(carg);
      close(cfd);
    }
  }
  return NULL;
}

int nostr_nip5f_server_start(const char *socket_path, void **out_handle) {
  // Make sure JSON interface is configured (jansson) before any connections.
  ensure_json();
  if (!out_handle) return -1;
  struct Nip5fServer *s = (struct Nip5fServer*)calloc(1, sizeof(*s));
  if (!s) return -1;
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
  s->get_pub = get_pub;
  s->sign_event = sign_event;
  s->enc44 = enc44;
  s->dec44 = dec44;
  s->list_keys = list_keys;
  s->ud = user_data;
  return 0;
}
