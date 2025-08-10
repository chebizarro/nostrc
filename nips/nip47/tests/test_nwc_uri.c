#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "nostr/nip47/nwc.h"

static void expect_ok_parse(const char *uri,
                            const char *want_pk,
                            const char *want_secret,
                            const char *want_lud16,
                            const char **want_relays,
                            size_t want_relay_count) {
  NostrNwcConnection c;
  int rc = nostr_nwc_uri_parse(uri, &c);
  assert(rc == 0);
  assert(c.wallet_pubkey_hex && strcmp(c.wallet_pubkey_hex, want_pk) == 0);
  assert(c.secret_hex && strcmp(c.secret_hex, want_secret) == 0);
  if (want_lud16) {
    assert(c.lud16 && strcmp(c.lud16, want_lud16) == 0);
  } else {
    assert(c.lud16 == NULL);
  }
  if (want_relay_count == 0) {
    assert(c.relays == NULL);
  } else {
    for (size_t i = 0; i < want_relay_count; i++) {
      assert(c.relays[i] != NULL);
      assert(strcmp(c.relays[i], want_relays[i]) == 0);
    }
    assert(c.relays[want_relay_count] == NULL);
  }
  nostr_nwc_connection_clear(&c);
}

static void test_basic_single_relay(void) {
  const char *pk = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"; // 64 hex
  const char *sk = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"; // 64 hex
  const char *relay = "wss://r.example.com";
  const char *uri = "nostr+walletconnect://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa?secret=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb&relay=wss%3A%2F%2Fr.example.com";
  const char *rels[] = { relay };
  expect_ok_parse(uri, pk, sk, NULL, rels, 1);
}

static void test_multi_relay_and_lud16(void) {
  const char *pk = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  const char *sk = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
  const char *relays[] = {
    "wss://alpha.example/path one",  // contains space
    "wss://β.example/π"              // unicode utf-8
  };
  const char *lud16 = "user@getalby.com";

  // Build a connection and URI using builder, then parse it back
  NostrNwcConnection c = {0};
  c.wallet_pubkey_hex = strdup(pk);
  c.secret_hex = strdup(sk);
  c.lud16 = strdup(lud16);
  c.relays = (char**)calloc(3, sizeof(char*));
  c.relays[0] = strdup(relays[0]);
  c.relays[1] = strdup(relays[1]);
  c.relays[2] = NULL;

  char *uri = NULL;
  int rc = nostr_nwc_uri_build(&c, &uri);
  assert(rc == 0);
  assert(uri && strncmp(uri, "nostr+walletconnect://", 23) == 0);

  // Parse the built URI and assert fields match
  NostrNwcConnection d;
  rc = nostr_nwc_uri_parse(uri, &d);
  assert(rc == 0);
  assert(strcmp(d.wallet_pubkey_hex, pk) == 0);
  assert(strcmp(d.secret_hex, sk) == 0);
  assert(d.relays && d.relays[0] && d.relays[1] && d.relays[2] == NULL);
  assert(strcmp(d.relays[0], relays[0]) == 0);
  assert(strcmp(d.relays[1], relays[1]) == 0);
  assert(d.lud16 && strcmp(d.lud16, lud16) == 0);

  // cleanup
  free(uri);
  nostr_nwc_connection_clear(&c);
  nostr_nwc_connection_clear(&d);
}

static void test_failure_missing_secret(void) {
  const char *bad = "nostr+walletconnect://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  NostrNwcConnection c;
  int rc = nostr_nwc_uri_parse(bad, &c);
  assert(rc != 0);
}

static void test_failure_wrong_scheme(void) {
  const char *bad = "nostr+wc://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa?secret=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  NostrNwcConnection c;
  int rc = nostr_nwc_uri_parse(bad, &c);
  assert(rc != 0);
}

static void test_failure_non_hex(void) {
  const char *bad = "nostr+walletconnect://zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz?secret=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  NostrNwcConnection c;
  int rc = nostr_nwc_uri_parse(bad, &c);
  assert(rc != 0);
}

int main(void) {
  test_basic_single_relay();
  test_multi_relay_and_lud16();
  test_failure_missing_secret();
  test_failure_wrong_scheme();
  test_failure_non_hex();
  printf("test_nwc_uri: OK\n");
  return 0;
}
