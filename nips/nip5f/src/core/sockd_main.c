#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include "nostr/nip5f/nip5f.h"

static volatile int g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

/* Built-in forwarding handlers (C, not C++ lambdas) */
static int fwd_get_pub(void *ud, char **out_pub) {
  (void)ud; return nostr_nip5f_builtin_get_public_key(out_pub);
}
static int fwd_sign_event(void *ud, const char *ev, const char *pub, char **out_js) {
  (void)ud; return nostr_nip5f_builtin_sign_event(ev, pub, out_js);
}
static int fwd_enc44(void *ud, const char *peer, const char *pt, char **out_b64) {
  (void)ud; return nostr_nip5f_builtin_nip44_encrypt(peer, pt, out_b64);
}
static int fwd_dec44(void *ud, const char *peer, const char *ct, char **out_pt) {
  (void)ud; return nostr_nip5f_builtin_nip44_decrypt(peer, ct, out_pt);
}
static int fwd_list_keys(void *ud, char **out_arr) {
  (void)ud; return nostr_nip5f_builtin_list_public_keys(out_arr);
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  signal(SIGINT, on_sigint);
  signal(SIGTERM, on_sigint);

  void *srv = NULL;
  const char *sock = getenv("NOSTR_SIGNER_SOCK");
  int rc = nostr_nip5f_server_start(sock, &srv);
  if (rc != 0) {
    fprintf(stderr, "nostr-signer-sockd: failed to start server (rc=%d)\n", rc);
    return 1;
  }
  // Default to built-in handlers
  nostr_nip5f_server_set_handlers(srv,
    fwd_get_pub,
    fwd_sign_event,
    fwd_enc44,
    fwd_dec44,
    fwd_list_keys,
    /*user_data*/ NULL);

  // TODO: implement accept loop; for stub, just sleep until signal
  while (!g_stop) {
    usleep(100000);
  }

  nostr_nip5f_server_stop(srv);
  return 0;
}
