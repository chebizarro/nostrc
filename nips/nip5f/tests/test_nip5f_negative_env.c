#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include "nostr/nip5f/nip5f.h"

static char *unique_sock_path(void) {
  char buf[256];
  snprintf(buf, sizeof(buf), "/tmp/nostr-nip5f-neg-%ld-%d.sock", (long)time(NULL), (int)getpid());
  return strdup(buf);
}

int main(void) {
  // Ensure no key is present
  unsetenv("NOSTR_SIGNER_SECKEY_HEX");
  unsetenv("NOSTR_SIGNER_NSEC");

  // Start server
  void *srv = NULL; char *sock = unique_sock_path();
  if (nostr_nip5f_server_start(sock, &srv) != 0) { fprintf(stderr, "server start failed\n"); free(sock); return 1; }
  nostr_nip5f_server_set_handlers(srv, NULL, NULL, NULL, NULL, NULL, NULL);

  // Connect client
  void *cli = NULL;
  if (nostr_nip5f_client_connect(sock, &cli) != 0) {
    fprintf(stderr, "client connect failed\n");
    nostr_nip5f_server_stop(srv); unlink(sock); free(sock); return 1;
  }

  // get_public_key should fail because server cannot resolve key
  char *pub = NULL;
  int rc = nostr_nip5f_client_get_public_key(cli, &pub);
  if (rc == 0) {
    fprintf(stderr, "expected failure when no signing key env set\n");
    if (pub) free(pub);
    nostr_nip5f_client_close(cli); nostr_nip5f_server_stop(srv); unlink(sock); free(sock); return 1;
  }

  // Cleanup
  nostr_nip5f_client_close(cli);
  nostr_nip5f_server_stop(srv);
  unlink(sock);
  free(sock);
  printf("OK\n");
  return 0;
}
