/* nip55l 0.2.0 SignEvent JSON contract test (in-process helper).
 *
 * The org.nostr.Signer.SignEvent method now returns the complete signed
 * event JSON (id, pubkey, created_at, kind, tags, content, sig). This
 * exercises the underlying helper the D-Bus dispatch calls
 * (nostr_nip55l_sign_event_json) with an explicit hex-selector identity, so
 * the test never reaches libsecret, the Keychain, or a session bus.
 *
 * Asserts:
 *   - The reply is a JSON object (not a bare 128-hex signature).
 *   - It parses as a signed event and validates cryptographically
 *     (canonical id + Schnorr signature).
 *   - pubkey is the signing key's, regardless of what the template said.
 *   - A zero created_at is filled with the current time (not the epoch).
 *   - Malformed input is refused with NOSTR_SIGNER_ERROR_INVALID_JSON.
 */
#include "nostr/nip55l/signer_ops.h"
#include "nostr/nip55l/error.h"

#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-utils.h>

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char *sign_or_die(const char *tmpl, const char *sk_selector) {
  char *out = NULL;
  int rc = nostr_nip55l_sign_event_json(tmpl, sk_selector, "test", &out);
  if (rc != 0 || !out) {
    fprintf(stderr, "sign_event_json failed: rc=%d out=%s\n", rc, out ? out : "(null)");
    exit(1);
  }
  return out;
}

static void assert_verified_signed_event(const char *json, const char *want_pk_hex, int want_kind, int64_t not_before) {
  assert(json && json[0] == '{');
  NostrEvent *ev = nostr_event_new();
  assert(ev);
  assert(nostr_event_deserialize_signed(ev, json, NULL) == NOSTR_EVENT_VALIDATION_OK);
  assert(nostr_event_validate(ev, NULL) == NOSTR_EVENT_VALIDATION_OK);
  const char *pk = nostr_event_get_pubkey(ev);
  assert(pk && strcmp(pk, want_pk_hex) == 0);
  assert(nostr_event_get_kind(ev) == want_kind);
  assert(nostr_event_get_created_at(ev) >= not_before);
  nostr_event_free(ev);
}

int main(void) {
  char *sk_hex = nostr_key_generate_private();
  assert(sk_hex);
  char *pk_hex = nostr_key_get_public(sk_hex);
  assert(pk_hex);

  /* Template with the caller's own pubkey and a real timestamp. */
  int64_t now = (int64_t)time(NULL);
  char tmpl_a[512];
  snprintf(tmpl_a, sizeof tmpl_a,
    "{\"kind\":1,\"created_at\":%lld,\"pubkey\":\"%s\","
    "\"tags\":[[\"e\",\"aa\"]],\"content\":\"hello \\\"world\\\"\"}",
    (long long)now, pk_hex);
  char *out_a = sign_or_die(tmpl_a, sk_hex);
  assert_verified_signed_event(out_a, pk_hex, 1, now);
  free(out_a);

  /* Template with a caller-supplied pubkey that does NOT match the signing
   * key: the signer must overwrite it with its own, so the returned event
   * still verifies against the returned pubkey. */
  char tmpl_b[512];
  snprintf(tmpl_b, sizeof tmpl_b,
    "{\"kind\":22242,\"created_at\":0,"
    "\"pubkey\":\"%s\","
    "\"tags\":[[\"challenge\",\"abc\"]],\"content\":\"\"}",
    /* Deliberately wrong pubkey (all zeros). */
    "0000000000000000000000000000000000000000000000000000000000000000");
  int64_t before = (int64_t)time(NULL);
  char *out_b = sign_or_die(tmpl_b, sk_hex);
  /* pubkey overwritten to the signing key's; created_at filled with now. */
  assert_verified_signed_event(out_b, pk_hex, 22242, before);
  free(out_b);

  /* Malformed JSON is refused with the invalid-JSON code, not silently
   * signed as an empty event. */
  char *out_c = NULL;
  int rc = nostr_nip55l_sign_event_json("{not json", sk_hex, "test", &out_c);
  assert(rc == NOSTR_SIGNER_ERROR_INVALID_JSON);
  assert(out_c == NULL);

  free(sk_hex); free(pk_hex);
  printf("test_nip55l_sign_event_json: PASS\n");
  return 0;
}
