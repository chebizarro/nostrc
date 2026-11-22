#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/un.h>
#include "nostr/nip5f/nip5f.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "json.h"
#include "nostr-utils.h"
#include "nostr/nip44/nip44.h"

static char *unique_sock_path(void) {
  char buf[256];
  snprintf(buf, sizeof(buf), "/tmp/nostr-nip5f-test-%ld-%d.sock", (long)time(NULL), (int)getpid());
  return strdup(buf);
}

static char *make_min_event_json(const char *content, int kind, int64_t created_at) {
  char *esc = nostr_escape_string(content);
  if (!esc) return NULL;
  size_t L = strlen(esc) + 128;
  char *json = (char*)malloc(L);
  if (json) snprintf(json, L, "{\"kind\":%d,\"created_at\":%lld,\"tags\":[],\"content\":\"%s\"}", kind, (long long)created_at, esc);
  free(esc);
  return json;
}

static int test_builtin_roundtrip(void);
static int test_escaped_payloads(void);
static int test_with_authorized_client(void);
static int test_with_unauthorized_client(void);

int main(void) {
  int failures = 0;
  char overlong[sizeof(((struct sockaddr_un *)0)->sun_path) + 1];
  memset(overlong, 'x', sizeof(overlong) - 1);
  overlong[sizeof(overlong) - 1] = '\0';
  void *connection = NULL;
  errno = 0;
  if (nostr_nip5f_client_connect(overlong, &connection) != -1 || errno != ENAMETOOLONG) {
    fprintf(stderr, "overlong socket path was not rejected with ENAMETOOLONG\n");
    return 1;
  }


  printf("Running NIP-5F loopback tests...\n");

  /* Test 1: built-in handlers (key from the server's environment) */
  printf("\n=== Test 1: built-in handlers round-trip ===\n");
  if (test_builtin_roundtrip() != 0) {
    fprintf(stderr, "FAILED: built-in round-trip test\n");
    failures++;
  } else {
    printf("PASSED: built-in round-trip test\n");
  }

  printf("\n=== Test 1b: JSON-escaped payloads round-trip ===\n");
  if (test_escaped_payloads() != 0) {
    fprintf(stderr, "FAILED: escaped payloads test\n");
    failures++;
  } else {
    printf("PASSED: escaped payloads test\n");
  }

  /* Test 2: With proper authorization (no test mode) */
  printf("\n=== Test 2: Authorized client (no test mode) ===\n");
  if (test_with_authorized_client() != 0) {
    fprintf(stderr, "FAILED: Authorized client test\n");
    failures++;
  } else {
    printf("PASSED: Authorized client test\n");
  }

  /* Test 3: Unauthorized client should fail */
  printf("\n=== Test 3: Unauthorized client (should fail) ===\n");
  if (test_with_unauthorized_client() != 0) {
    fprintf(stderr, "FAILED: Unauthorized client test (expected failure not observed)\n");
    failures++;
  } else {
    printf("PASSED: Unauthorized client test (correctly rejected)\n");
  }

  if (failures == 0) {
    printf("\n=== All NIP-5F loopback tests passed! ===\n");
    return 0;
  } else {
    printf("\n=== %d test(s) failed ===\n", failures);
    return 1;
  }
}

static int test_builtin_roundtrip(void) {
  /* No per-app ACL any more (nostrc-q23h): built-ins serve same-uid peers. */
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

/* Plaintexts whose JSON string form needs escaping (md-ebm). The client must hand
 * back exactly these bytes, not their escaped wire form. */
static const char *const escaped_samples[] = {
  /* A NIP-17 seal: every gift wrap decrypts to JSON like this. */
  "{\"id\":\"00ff\",\"pubkey\":\"ab\",\"kind\":13,\"tags\":[],\"content\":\"x\\\"y\"}",
  /* Member names a text scanner could mistake for the response's own. */
  "\"error\":{\"code\":1},\"result\":\"spoof\",\"id\":\"999\"",
  "back\\slash \\\\ \\\" \\u0041 \\n literal",
  "ctl \x01\x02\b\f\n\r\t\x1f\x7f end",
  "unicode h\xc3\xa9llo \xe2\x80\x94 \xe4\xb8\x96\xe7\x95\x8c \xf0\x9f\x9a\x80 \xe2\x80\xa8\xe2\x80\xa9",
};

static int test_escaped_payloads(void) {
  int rc = 1;
  char *sk = nostr_key_generate_private();
  char *pub = sk ? nostr_key_get_public(sk) : NULL;
  char *sock_path = unique_sock_path();
  void *srv = NULL, *cli = NULL;
  unsigned char skb[32], pkb[32];
  if (!pub || !nostr_hex2bin(skb, sk, 32) || !nostr_hex2bin(pkb, pub, 32)) {
    fprintf(stderr, "key setup failed\n");
    goto out;
  }
  setenv("NOSTR_SIGNER_SECKEY_HEX", sk, 1);
  if (nostr_nip5f_server_start(sock_path, &srv) != 0 ||
      nostr_nip5f_client_connect(sock_path, &cli) != 0) {
    fprintf(stderr, "server/client setup failed\n");
    goto out;
  }

  for (size_t i = 0; i < sizeof(escaped_samples) / sizeof(escaped_samples[0]); i++) {
    const char *pt = escaped_samples[i];
    char *cipher = NULL, *dec = NULL, *evjson = NULL, *signed_json = NULL;
    uint8_t *local = NULL; size_t local_len = 0;
    NostrEvent *ev = NULL;
    int ok = 0;

    /* Request direction: the server must encrypt the unescaped plaintext. */
    if (nostr_nip5f_client_nip44_encrypt(cli, pub, pt, &cipher) != 0 || !cipher) {
      fprintf(stderr, "sample %zu: nip44_encrypt failed\n", i);
    } else if (nostr_nip44_decrypt_v2(skb, pkb, cipher, &local, &local_len) != 0 ||
               local_len != strlen(pt) || memcmp(local, pt, local_len) != 0) {
      fprintf(stderr, "sample %zu: server encrypted different bytes\n", i);
    /* Response direction: the client must unescape the result string. */
    } else if (nostr_nip5f_client_nip44_decrypt(cli, pub, cipher, &dec) != 0 || !dec) {
      fprintf(stderr, "sample %zu: nip44_decrypt failed\n", i);
    } else if (strcmp(dec, pt) != 0) {
      fprintf(stderr, "sample %zu: decrypt mismatch: got [%s]\n", i, dec);
    /* Object results: the signed event keeps the same content. */
    } else if (!(evjson = make_min_event_json(pt, 1, (int64_t)time(NULL)))) {
      fprintf(stderr, "sample %zu: event json failed\n", i);
    } else if (nostr_nip5f_client_sign_event(cli, evjson, NULL, &signed_json) != 0 || !signed_json) {
      fprintf(stderr, "sample %zu: sign_event failed\n", i);
    } else if (!(ev = nostr_event_new()) || nostr_event_deserialize(ev, signed_json) != 0 ||
               !nostr_event_check_signature(ev) ||
               strcmp(nostr_event_get_content(ev), pt) != 0) {
      fprintf(stderr, "sample %zu: signed event content/signature mismatch\n", i);
    } else {
      ok = 1;
    }
    if (ev) nostr_event_free(ev);
    free(cipher); free(dec); free(local); free(evjson); free(signed_json);
    if (!ok) goto out;
  }

  /* A server error must still be reported as an error, not a result. */
  char *bad = NULL;
  if (nostr_nip5f_client_nip44_decrypt(cli, pub, "not-a-ciphertext", &bad) == 0 || bad) {
    fprintf(stderr, "decrypt of an invalid payload succeeded\n");
    free(bad);
    goto out;
  }
  rc = 0;

out:
  if (cli) nostr_nip5f_client_close(cli);
  if (srv) nostr_nip5f_server_stop(srv);
  unlink(sock_path);
  free(sock_path);
  free(pub);
  free(sk);
  unsetenv("NOSTR_SIGNER_SECKEY_HEX");
  return rc;
}

static int test_with_authorized_client(void) {
  /* Using the proper authorization token */
  
  char *sk = nostr_key_generate_private();
  if (!sk) { fprintf(stderr, "failed to gen sk\n"); return 1; }
  setenv("NOSTR_SIGNER_SECKEY_HEX", sk, 1);
  
  /* Server requires this token; client presents a MATCHING token so it is authorized.
   * The client token is supplied via a CLIENT-side env var so this genuinely exercises
   * the same token-matching path used to reject an unauthorized client in Test 3. */
  const char *test_token = "test_auth_token_12345";
  setenv("NOSTR_SIGNER_AUTH_TOKEN", test_token, 1);
  setenv("NOSTR_SIGNER_CLIENT_AUTH_TOKEN", test_token, 1);
  
  char *expected_pub = nostr_key_get_public(sk);
  if (!expected_pub) { fprintf(stderr, "failed to derive pk\n"); free(sk); return 1; }

  void *srv = NULL;
  char *sock_path = unique_sock_path();
  if (nostr_nip5f_server_start(sock_path, &srv) != 0) {
    fprintf(stderr, "server start failed\n");
    free(sk); free(expected_pub); free(sock_path);
    return 1;
  }

  /* Connect client (auth token is passed via env var NOSTR_SIGNER_AUTH_TOKEN) */
  void *cli = NULL;
  if (nostr_nip5f_client_connect(sock_path, &cli) != 0) {
    fprintf(stderr, "client connect failed\n");
    nostr_nip5f_server_stop(srv);
    free(sk); free(expected_pub); free(sock_path);
    unsetenv("NOSTR_SIGNER_AUTH_TOKEN");
    unsetenv("NOSTR_SIGNER_CLIENT_AUTH_TOKEN");
    return 1;
  }

  /* Test basic operation */
  char *pub = NULL;
  if (nostr_nip5f_client_get_public_key(cli, &pub) != 0) {
    fprintf(stderr, "get_public_key failed\n");
    goto auth_fail;
  }
  if (strcmp(pub, expected_pub) != 0) {
    fprintf(stderr, "pubkey mismatch\n");
    free(pub);
    goto auth_fail;
  }
  free(pub);

  nostr_nip5f_client_close(cli);
  nostr_nip5f_server_stop(srv);
  unlink(sock_path);
  free(sock_path);
  free(expected_pub);
  free(sk);
  unsetenv("NOSTR_SIGNER_SECKEY_HEX");
  unsetenv("NOSTR_SIGNER_AUTH_TOKEN");
  unsetenv("NOSTR_SIGNER_CLIENT_AUTH_TOKEN");
  return 0;

auth_fail:
  if (cli) nostr_nip5f_client_close(cli);
  if (srv) nostr_nip5f_server_stop(srv);
  if (sock_path) { unlink(sock_path); free(sock_path); }
  free(expected_pub);
  free(sk);
  unsetenv("NOSTR_SIGNER_SECKEY_HEX");
  unsetenv("NOSTR_SIGNER_AUTH_TOKEN");
  unsetenv("NOSTR_SIGNER_CLIENT_AUTH_TOKEN");
  return 1;
}

static int test_with_unauthorized_client(void) {
  /* Test that a client without the server's token is rejected */
  
  char *sk = nostr_key_generate_private();
  if (!sk) return 1;
  setenv("NOSTR_SIGNER_SECKEY_HEX", sk, 1);
  
  /* Server requires "correct_token". The client presents a DIFFERENT token via the
   * client-side env var, so enforcement must reject it. (Both endpoints share one
   * process env, so the client token MUST come from a distinct variable to model an
   * unauthorized client.) */
  setenv("NOSTR_SIGNER_AUTH_TOKEN", "correct_token", 1);
  setenv("NOSTR_SIGNER_CLIENT_AUTH_TOKEN", "wrong_token", 1);

  void *srv = NULL;
  char *sock_path = unique_sock_path();
  if (nostr_nip5f_server_start(sock_path, &srv) != 0) {
    free(sk); free(sock_path);
    unsetenv("NOSTR_SIGNER_AUTH_TOKEN");
    unsetenv("NOSTR_SIGNER_CLIENT_AUTH_TOKEN");
    return 1;
  }

  /* Try to connect WITHOUT providing auth token (or with wrong token) */
  void *cli = NULL;
  int connect_rc = nostr_nip5f_client_connect(sock_path, &cli);
  
  if (connect_rc == 0 && cli != NULL) {
    /* Connection succeeded - now try an operation, which should fail */
    char *pub = NULL;
    int op_rc = nostr_nip5f_client_get_public_key(cli, &pub);
    
    if (op_rc == 0) {
      /* Operation succeeded - this is WRONG, auth should have blocked it */
      fprintf(stderr, "ERROR: Unauthorized client was allowed (auth not enforced)\n");
      fprintf(stderr, "NOTE: Server may not have ACL implemented yet\n");
      free(pub);
      nostr_nip5f_client_close(cli);
      nostr_nip5f_server_stop(srv);
      unlink(sock_path);
      free(sock_path);
      free(sk);
      unsetenv("NOSTR_SIGNER_SECKEY_HEX");
      unsetenv("NOSTR_SIGNER_AUTH_TOKEN");
      unsetenv("NOSTR_SIGNER_CLIENT_AUTH_TOKEN");
      return 1;  /* Test failed - auth should have rejected */
    } else {
      /* Operation failed - this is CORRECT behavior */
      fprintf(stderr, "Good: Unauthorized operation was rejected\n");
      free(pub);
      nostr_nip5f_client_close(cli);
    }
  } else {
    /* Connection failed - this is also acceptable (auth at connect time) */
    fprintf(stderr, "Good: Unauthorized connection was rejected\n");
  }

  nostr_nip5f_server_stop(srv);
  unlink(sock_path);
  free(sock_path);
  free(sk);
  unsetenv("NOSTR_SIGNER_SECKEY_HEX");
  unsetenv("NOSTR_SIGNER_AUTH_TOKEN");
  unsetenv("NOSTR_SIGNER_CLIENT_AUTH_TOKEN");
  return 0;  /* Test passed - unauthorized access was blocked */
}
