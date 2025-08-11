#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "nostr/nip5f/nip5f.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "json.h"

static char *unique_sock_path(void) {
  char buf[256];
  snprintf(buf, sizeof(buf), "/tmp/nostr-nip5f-test-%ld-%d.sock", (long)time(NULL), (int)getpid());
  return strdup(buf);
}

static char *make_min_event_json(const char *content, int kind, int64_t created_at) {
  char *esc = NULL;
  size_t len = strlen(content);
  esc = (char*)malloc(len*2 + 1);
  if (!esc) return NULL;
  size_t j=0; for (size_t i=0;i<len;i++){ char c=content[i]; if (c=='"' || c=='\\') esc[j++]='\\'; esc[j++]=c; }
  esc[j]='\0';
  char *json = (char*)malloc(j + 128);
  if (!json) { free(esc); return NULL; }
  snprintf(json, j+128, "{\"kind\":%d,\"created_at\":%lld,\"tags\":[],\"content\":\"%s\"}", kind, (long long)created_at, esc);
  free(esc);
  return json;
}

int main(void) {
  /* Ensure server bypasses ACL in test mode */
  setenv("NOSTR_TEST_MODE", "1", 1);
  // Generate a fresh secret and set env for built-ins
  char *sk = nostr_key_generate_private();
  if (!sk) { fprintf(stderr, "failed to gen sk\n"); return 1; }
  setenv("NOSTR_SIGNER_SECKEY_HEX", sk, 1);
  char *expected_pub = nostr_key_get_public(sk);
  if (!expected_pub) { fprintf(stderr, "failed to derive pk\n"); free(sk); return 1; }

  // Start server with built-in handlers
  void *srv = NULL;
  char *sock_path = unique_sock_path();
  if (nostr_nip5f_server_start(sock_path, &srv) != 0) {
    fprintf(stderr, "server start failed\n");
    free(sk); free(expected_pub); free(sock_path);
    return 1;
  }
  // Set default handlers (NULLs cause dispatcher to use built-ins in sockd_main normally), but set explicitly to be clear
  nostr_nip5f_server_set_handlers(srv,
    /*get_pub*/ (Nip5fGetPubFn)NULL,
    /*sign*/    (Nip5fSignEventFn)NULL,
    /*enc44*/   (Nip5fNip44EncFn)NULL,
    /*dec44*/   (Nip5fNip44DecFn)NULL,
    /*list*/    (Nip5fListKeysFn)NULL,
    NULL);

  // Connect client
  void *cli = NULL;
  if (nostr_nip5f_client_connect(sock_path, &cli) != 0) {
    fprintf(stderr, "client connect failed\n");
    nostr_nip5f_server_stop(srv);
    free(sk); free(expected_pub); free(sock_path);
    return 1;
  }

  // get_public_key
  char *pub = NULL;
  if (nostr_nip5f_client_get_public_key(cli, &pub) != 0) {
    fprintf(stderr, "get_public_key failed\n");
    goto fail;
  }
  if (strcmp(pub, expected_pub) != 0) {
    fprintf(stderr, "pubkey mismatch\n");
    goto fail;
  }
  free(pub); pub=NULL;

  // sign_event
  int64_t now = (int64_t)time(NULL);
  char *evjson = make_min_event_json("hello", 1, now);
  if (!evjson) { fprintf(stderr, "build event json failed\n"); goto fail; }
  char *signed_json = NULL;
  if (nostr_nip5f_client_sign_event(cli, evjson, NULL, &signed_json) != 0) {
    fprintf(stderr, "sign_event failed\n");
    free(evjson);
    goto fail;
  }
  NostrEvent *ev = nostr_event_new();
  if (!ev) { fprintf(stderr, "alloc ev failed\n"); free(evjson); free(signed_json); goto fail; }
  if (nostr_event_deserialize(ev, signed_json) != 0) {
    fprintf(stderr, "signed event deserialize failed\n");
    free(evjson); free(signed_json); nostr_event_free(ev);
    goto fail;
  }
  if (!nostr_event_check_signature(ev)) {
    fprintf(stderr, "signature verification failed\n");
    nostr_event_free(ev);
    free(evjson); free(signed_json);
    goto fail;
  }
  const char *evpk = nostr_event_get_pubkey(ev);
  if (!evpk || strcmp(evpk, expected_pub) != 0) {
    fprintf(stderr, "signed event pubkey mismatch\n");
    nostr_event_free(ev);
    free(evjson); free(signed_json);
    goto fail;
  }
  nostr_event_free(ev);
  free(evjson); free(signed_json);

  // nip44 encrypt/decrypt roundtrip (peer = self)
  const char *plaintext = "hello nip44";
  char *cipher_b64 = NULL;
  if (nostr_nip5f_client_nip44_encrypt(cli, expected_pub, plaintext, &cipher_b64) != 0) {
    fprintf(stderr, "nip44_encrypt failed\n");
    goto fail;
  }
  char *decrypted = NULL;
  if (nostr_nip5f_client_nip44_decrypt(cli, expected_pub, cipher_b64, &decrypted) != 0) {
    fprintf(stderr, "nip44_decrypt failed\n");
    free(cipher_b64);
    goto fail;
  }
  if (strcmp(decrypted, plaintext) != 0) {
    fprintf(stderr, "nip44 roundtrip mismatch\n");
    free(cipher_b64); free(decrypted);
    goto fail;
  }
  free(cipher_b64); free(decrypted);

  // list_public_keys
  char *keys_json = NULL;
  if (nostr_nip5f_client_list_public_keys(cli, &keys_json) != 0) {
    fprintf(stderr, "list_public_keys failed\n");
    goto fail;
  }
  if (!strstr(keys_json, expected_pub)) {
    fprintf(stderr, "list_public_keys missing expected pubkey\n");
    free(keys_json);
    goto fail;
  }
  free(keys_json);

  // Cleanup
  nostr_nip5f_client_close(cli);
  nostr_nip5f_server_stop(srv);
  unlink(sock_path);
  free(sock_path);
  free(expected_pub);
  free(sk);
  unsetenv("NOSTR_SIGNER_SECKEY_HEX");
  printf("OK\n");
  return 0;

fail:
  if (cli) nostr_nip5f_client_close(cli);
  if (srv) nostr_nip5f_server_stop(srv);
  if (sock_path) { unlink(sock_path); free(sock_path); }
  if (expected_pub) free(expected_pub);
  if (sk) free(sk);
  unsetenv("NOSTR_SIGNER_SECKEY_HEX");
  return 1;
}
