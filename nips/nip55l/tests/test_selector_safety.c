/* Direct C callers must treat identity selectors as identities, not key material. */
#include "nostr/nip55l/signer_ops.h"
#include "nostr/nip55l/error.h"

#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
} } while (0)

int main(void) {
  const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";
  const char *other_sk = "0000000000000000000000000000000000000000000000000000000000000002";
  const char *event = "{\"kind\":1,\"created_at\":1,\"tags\":[],\"content\":\"selector\"}";
  char *pk = nostr_key_get_public(sk);
  char *other_pk = nostr_key_get_public(other_sk);
  CHECK(pk && other_pk);
  uint8_t bytes[32];
  char *npub = NULL, *nsec = NULL;
  CHECK(nostr_hex2bin(bytes, pk, sizeof bytes));
  CHECK(nostr_nip19_encode_npub(bytes, &npub) == 0 && npub);
  CHECK(nostr_hex2bin(bytes, sk, sizeof bytes));
  CHECK(nostr_nip19_encode_nsec(bytes, &nsec) == 0 && nsec);
  CHECK(setenv("NOSTR_SIGNER_SECKEY_HEX", sk, 1) == 0);

  char *signed_json = NULL;
  CHECK(nostr_nip55l_sign_event_json(event, pk, "test", &signed_json) == 0);
  CHECK(signed_json);
  NostrEvent *ev = nostr_event_new();
  CHECK(ev);
  CHECK(nostr_event_deserialize_signed(ev, signed_json, NULL) == NOSTR_EVENT_VALIDATION_OK);
  CHECK(nostr_event_validate(ev, NULL) == NOSTR_EVENT_VALIDATION_OK);
  if (strcmp(nostr_event_get_pubkey(ev), pk) != 0)
    fprintf(stderr, "wrong identity: selected pubkey %s, signed as %s\n", pk, nostr_event_get_pubkey(ev));
  CHECK(strcmp(nostr_event_get_pubkey(ev), pk) == 0);
  nostr_event_free(ev);
  free(signed_json);

  char *resolved = NULL;
  CHECK(nostr_nip55l_resolve_npub(pk, &resolved) == 0);
  CHECK(resolved && strcmp(resolved, npub) == 0);
  free(resolved);
  resolved = NULL;
  CHECK(nostr_nip55l_resolve_npub(npub, &resolved) == 0);
  CHECK(resolved && strcmp(resolved, npub) == 0);
  free(resolved);

  resolved = NULL;
  CHECK(nostr_nip55l_resolve_npub(nsec, &resolved) == NOSTR_SIGNER_ERROR_INVALID_ARG);
  CHECK(resolved == NULL);
  signed_json = NULL;
  CHECK(nostr_nip55l_sign_event_json(event, nsec, "test", &signed_json) == NOSTR_SIGNER_ERROR_INVALID_ARG);
  CHECK(signed_json == NULL);

  /* Neither an unknown pubkey nor a secret written as hex can provision a
   * key through the selector. On this disconnected bus the store is unreadable,
   * so BACKEND is possible instead of NOT_FOUND. */
  const char *unknown_selectors[] = { other_pk, sk };
  for (size_t i = 0; i < 2; i++) {
    resolved = NULL;
    int rc = nostr_nip55l_resolve_npub(unknown_selectors[i], &resolved);
    CHECK(rc == NOSTR_SIGNER_ERROR_NOT_FOUND || rc == NOSTR_SIGNER_ERROR_BACKEND);
    CHECK(resolved == NULL);
    signed_json = NULL;
    rc = nostr_nip55l_sign_event_json(event, unknown_selectors[i], "test", &signed_json);
    CHECK(rc == NOSTR_SIGNER_ERROR_NOT_FOUND || rc == NOSTR_SIGNER_ERROR_BACKEND);
    CHECK(signed_json == NULL);
  }

  unsetenv("NOSTR_SIGNER_SECKEY_HEX");
  free(pk); free(other_pk); free(npub); free(nsec);
  puts("test_nip55l_selector_safety: PASS");
  return 0;
}
