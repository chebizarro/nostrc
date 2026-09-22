/* Shared strict verify block for the NIP-46 provider variants.
 * See provider_nip46_verify.h for the contract. Extracted from the pre-paired
 * bunker provider (provider_nip46.c) so the QR / nostrconnect variant can
 * reuse the exact same code path (design §5.1). Pure refactor: behaviour is
 * byte-for-byte the same as the previous inline block. */
#include "provider_nip46_verify.h"

#include "nostr-event.h"

#include <string.h>

int nh_nip46_verify_signed_challenge(const char *signed_event_json,
                                     const char *expected_event_id_hex,
                                     const char *expected_account_pubkey_hex) {
  if (!signed_event_json || !expected_event_id_hex ||
      !expected_account_pubkey_hex ||
      strlen(expected_event_id_hex) != 64 ||
      strlen(expected_account_pubkey_hex) != 64)
    return 0;
  NostrEvent *event = nostr_event_new();
  if (!event) return 0;
  int good = 0;
  char id[65];
  if (nostr_event_deserialize_signed(event, signed_event_json, NULL) ==
          NOSTR_EVENT_VALIDATION_OK &&
      nostr_event_compute_id(event, id) == NOSTR_EVENT_VALIDATION_OK &&
      strcmp(id, expected_event_id_hex) == 0 && event->pubkey &&
      strcmp(event->pubkey, expected_account_pubkey_hex) == 0 &&
      nostr_event_check_signature(event)) {
    good = 1;
  }
  nostr_event_free(event);
  return good;
}
