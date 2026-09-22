/* Shared strict verify block for the two NIP-46 provider variants
 * (pre-paired bunker, client-initiated QR / nostrconnect).
 *
 * The signer hands the client a complete signed nostr event as a JSON string
 * (spec: sign_event returns the full event). Before we trust it as a login
 * proof we recompute its id from the canonical serialisation and require:
 *   - id == expected_id (the immutable challenge id the broker built), and
 *   - event.pubkey == account.pubkey_hex, and
 *   - signature verifies against event.id / event.pubkey.
 *
 * Failing any of these means the signer swapped identities, mangled the
 * challenge, or lied — in every case the broker's own
 * nh_auth_challenge_verify() would reject it, but we fail fast here so the
 * provider event contract emits a clean INVALID_PROOF outcome without
 * running challenge_verify against an obvious forgery.
 *
 * Returns 1 iff the event passes every check, 0 otherwise. Never crashes on
 * malformed JSON. */
#ifndef NH_AUTH_PROVIDER_NIP46_VERIFY_H
#define NH_AUTH_PROVIDER_NIP46_VERIFY_H

#ifdef __cplusplus
extern "C" {
#endif

int nh_nip46_verify_signed_challenge(const char *signed_event_json,
                                     const char *expected_event_id_hex,
                                     const char *expected_account_pubkey_hex);

#ifdef __cplusplus
}
#endif

#endif /* NH_AUTH_PROVIDER_NIP46_VERIFY_H */
