#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "nostr/nip5f/nip5f.h"

static char *make_min_event(const char *content) {
  size_t len = strlen(content);
  char *esc = (char*)malloc(len*2 + 1);
  if (!esc) return NULL;
  size_t j=0; for (size_t i=0;i<len;i++){ char c=content[i]; if (c=='"' || c=='\\') esc[j++]='\\'; esc[j++]=c; }
  esc[j]='\0';
  char *json = (char*)malloc(j + 128);
  if (!json) { free(esc); return NULL; }
  snprintf(json, j+128, "{\"kind\":1,\"created_at\":%ld,\"tags\":[],\"content\":\"%s\"}", (long)time(NULL), esc);
  free(esc);
  return json;
}

int main(int argc, char **argv) {
  const char *sock_path = getenv("NOSTR_SIGNER_SOCK");
  const char *peer = NULL;
  const char *msg = "hello from example";
  for (int i=1;i<argc;i++) {
    if (!strncmp(argv[i], "--sock=", 7)) sock_path = argv[i]+7;
    else if (!strncmp(argv[i], "--peer=", 7)) peer = argv[i]+7;
    else if (!strncmp(argv[i], "--msg=", 6)) msg = argv[i]+6;
  }

  void *cli=NULL; if (nostr_nip5f_client_connect(sock_path, &cli)!=0) { fprintf(stderr, "connect failed\n"); return 1; }

  char *pub=NULL; if (nostr_nip5f_client_get_public_key(cli, &pub)!=0) { fprintf(stderr, "get_public_key failed\n"); nostr_nip5f_client_close(cli); return 1; }
  printf("pubkey: %s\n", pub);

  char *ev = make_min_event(msg);
  if (!ev) { fprintf(stderr, "event build failed\n"); free(pub); nostr_nip5f_client_close(cli); return 1; }
  char *signed_ev=NULL; if (nostr_nip5f_client_sign_event(cli, ev, NULL, &signed_ev)!=0) { fprintf(stderr, "sign_event failed\n"); free(ev); free(pub); nostr_nip5f_client_close(cli); return 1; }
  printf("signed event: %s\n", signed_ev);

  const char *peer_pk = peer ? peer : pub;
  char *cipher=NULL; if (nostr_nip5f_client_nip44_encrypt(cli, peer_pk, msg, &cipher)!=0) { fprintf(stderr, "nip44_encrypt failed\n"); free(ev); free(signed_ev); free(pub); nostr_nip5f_client_close(cli); return 1; }
  printf("cipher (b64): %s\n", cipher);

  char *plain=NULL; if (nostr_nip5f_client_nip44_decrypt(cli, peer_pk, cipher, &plain)!=0) { fprintf(stderr, "nip44_decrypt failed\n"); free(cipher); free(ev); free(signed_ev); free(pub); nostr_nip5f_client_close(cli); return 1; }
  printf("decrypted: %s\n", plain);

  char *keys_json=NULL; if (nostr_nip5f_client_list_public_keys(cli, &keys_json)==0) {
    printf("keys: %s\n", keys_json);
    free(keys_json);
  }

  free(plain); free(cipher); free(signed_ev); free(ev); free(pub);
  nostr_nip5f_client_close(cli);
  return 0;
}
