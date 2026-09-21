/* nostr-authd: minimal auth broker daemon (stage 1).
 *
 * Listens on a SOCK_SEQPACKET auth.sock, and for each connection enforces the
 * SO_PEERCRED/endpoint ACL and dispatches one request via the broker. Currently
 * serves CHECK_ACCOUNT against the identity authority; the login proof flow and
 * concurrency/deadline/receipt machinery arrive in later stages. Tracks
 * nostrc-zcll.2. Not installed yet.
 *
 * Usage: nostr-authd <socket-path> <authority-dir>
 */
#define _GNU_SOURCE
#include "auth_broker.h"
#include "nostr_identity.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static nh_identity_ownership_result probe(void *c, const char *n, uint32_t u,
                                          uint32_t g) {
  (void)c; (void)n; (void)u; (void)g;
  return NH_IDENTITY_OWNERSHIP_FREE;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <socket-path> <authority-dir>\n", argv[0]);
    return 2;
  }
  const char *socket_path = argv[1];
  const char *dir = argv[2];

  nh_identity_config config;
  nh_identity_config_defaults(&config);
  snprintf(config.authority_path, sizeof config.authority_path, "%s/authority.db", dir);
  snprintf(config.projection_path, sizeof config.projection_path, "%s/nss.db", dir);
  snprintf(config.home_root, sizeof config.home_root, "%s/home", dir);
  nh_identity_store_options options = {0};
  options.config = &config;
  options.ownership_probe = probe;
  options.flags = 0;
  nh_identity_store *store = NULL;
  if (nh_identity_store_open(&options, &store) != NH_IDENTITY_OK) {
    fprintf(stderr, "nostr-authd: cannot open authority at %s\n", config.authority_path);
    return 1;
  }
  nh_auth_broker *broker = nh_auth_broker_new(store);
  if (!broker) { nh_identity_store_close(store); return 1; }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  if (strlen(socket_path) >= sizeof addr.sun_path) {
    fprintf(stderr, "nostr-authd: socket path too long\n");
    nh_auth_broker_free(broker); nh_identity_store_close(store); return 1;
  }
  strcpy(addr.sun_path, socket_path);
  int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (listener < 0) { perror("socket"); nh_auth_broker_free(broker); nh_identity_store_close(store); return 1; }
  unlink(socket_path);
  mode_t old = umask(0077); /* auth.sock is privileged: 0600 */
  if (bind(listener, (struct sockaddr *)&addr, sizeof addr) != 0) {
    perror("bind"); umask(old); close(listener);
    nh_auth_broker_free(broker); nh_identity_store_close(store); return 1;
  }
  umask(old);
  if (listen(listener, 16) != 0) {
    perror("listen"); close(listener); unlink(socket_path);
    nh_auth_broker_free(broker); nh_identity_store_close(store); return 1;
  }

  struct sigaction sa = {0};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);

  fprintf(stderr, "nostr-authd: listening on %s\n", socket_path);
  while (!g_stop) {
    int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) continue; /* EINTR on signal -> loop checks g_stop */
    (void)nh_auth_broker_handle_connection(broker, fd);
    close(fd);
  }

  close(listener);
  unlink(socket_path);
  nh_auth_broker_free(broker);
  nh_identity_store_close(store);
  fprintf(stderr, "nostr-authd: stopped\n");
  return 0;
}
