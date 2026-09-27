// Internal shared struct between server acceptor and connection thread
#ifndef NIPS_NIP5F_CORE_SOCK_CONN_H
#define NIPS_NIP5F_CORE_SOCK_CONN_H
#include "nostr/nip5f/nip5f.h"

struct Nip5fConnArg {
  int fd;
  int handshake; /* send the banner and read the client hello first */
  void *ud;
  Nip5fGetPubFn get_pub;
  Nip5fSignEventFn sign_event;
  Nip5fNip44EncFn enc44;
  Nip5fNip44DecFn dec44;
  Nip5fListKeysFn list_keys;
  /* Hooks mode (nostr_nip5f_server_start_with_hooks): every method goes
   * through hooks.request; the handlers above are unused. */
  int have_hooks;
  Nip5fServerHooks hooks;
  void *hooks_ud;
  Nip5fPeer peer;
};

/* Kernel-reported peer of @fd (pidfd = -1, have_creds = 0 when unknown). */
void nip5f_peer_from_fd(int fd, Nip5fPeer *peer);

void *nip5f_conn_thread(void *arg);
#endif /* NIPS_NIP5F_CORE_SOCK_CONN_H */
