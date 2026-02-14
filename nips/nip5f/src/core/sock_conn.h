// Internal shared struct between server acceptor and connection thread
#ifndef NIPS_NIP5F_CORE_SOCK_CONN_H
#define NIPS_NIP5F_CORE_SOCK_CONN_H
#include "nostr/nip5f/nip5f.h"

struct Nip5fConnArg {
  int fd;
  void *ud;
  Nip5fGetPubFn get_pub;
  Nip5fSignEventFn sign_event;
  Nip5fNip44EncFn enc44;
  Nip5fNip44DecFn dec44;
  Nip5fListKeysFn list_keys;
};

void *nip5f_conn_thread(void *arg);
#endif /* NIPS_NIP5F_CORE_SOCK_CONN_H */
