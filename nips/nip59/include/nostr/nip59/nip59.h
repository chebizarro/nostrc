#ifndef NOSTR_NIP59_H
#define NOSTR_NIP59_H

#include <nostr/nip59/export.h>

/**
 * NIP-59: Gift Wrap
 *
 * Provides a standard way to wrap any event for private transmission:
 * - Kind 1059: Gift wrap event
 * - Contains NIP-44 encrypted payload with wrapped event
 * - Random timestamp (within 2 days) for metadata protection
 * - Ephemeral sender key (p-tag indicates recipient)
 *
 * This is a general-purpose protocol that can wrap any event type.
 * NIP-17 uses this for private DMs (rumor -> seal -> gift wrap).
 * Kind-selecting APIs also support kind 21059 for NIP-AC, with a current
 * timestamp and the signed call event directly encrypted (no kind-13 seal).
 */

#include <stdbool.h>
#include <stdint.h>
#include "nostr-event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NIP-59 error codes */
#define NIP59_OK                    0
#define NIP59_ERR_INVALID_ARG      -1
#define NIP59_ERR_MEMORY           -2
#define NIP59_ERR_ENCRYPTION       -3
#define NIP59_ERR_DECRYPTION       -4
#define NIP59_ERR_KEY_GENERATION   -5
#define NIP59_ERR_SERIALIZATION    -6
#define NIP59_ERR_DESERIALIZATION  -7
#define NIP59_ERR_INVALID_KIND     -8
#define NIP59_ERR_SIGNATURE        -9
#define NIP59_ERR_RANDOMNESS      -10

/**
 * nostr_nip59_create_ephemeral_key:
 * @sk_hex_out: (out) (transfer full): newly-allocated hex secret key (64 chars)
 * @pk_hex_out: (out) (transfer full): newly-allocated hex public key (64 chars)
 *
 * Generates a random ephemeral keypair for use in gift wrapping.
 * The ephemeral key should be used once and discarded.
 *
 * Returns: NIP59_OK on success, negative error code on failure
 */
NOSTR_NIP59_API int nostr_nip59_create_ephemeral_key(char **sk_hex_out, char **pk_hex_out);

/**
 * nostr_nip59_randomize_timestamp:
 * @base_time: base timestamp (use 0 for current time)
 * @window_seconds: randomization window in seconds (use 0 for default 2 days)
 * @out_time: (out): the obfuscated timestamp
 *
 * Creates an obfuscated timestamp for metadata protection: uniform over
 * [base_time - window_seconds, base_time - 1], drawn from the system CSPRNG,
 * so always strictly in the past (relays may refuse future events).
 *
 * There is no fallback. If randomness is unavailable this fails, and the
 * caller must not publish: the real time is the metadata being hidden.
 *
 * Returns: NIP59_OK; NIP59_ERR_INVALID_ARG if @out_time is NULL or
 * @base_time is below the window; NIP59_ERR_RANDOMNESS if the CSPRNG fails.
 */
NOSTR_NIP59_API int nostr_nip59_randomize_timestamp(int64_t base_time, uint32_t window_seconds,
                                    int64_t *out_time);

/**
 * nostr_nip59_wrap:
 * @inner_event: (transfer none): event to wrap (any kind, can be signed or unsigned)
 * @recipient_pubkey_hex: recipient's public key (64 hex chars)
 * @ephemeral_sk_hex: (nullable): ephemeral secret key to use, or NULL to generate
 *
 * Wraps any event in a kind-1059 gift wrap for private transmission.
 * The wrapped event is NIP-44 encrypted to the recipient using an ephemeral key.
 * The timestamp is randomized for metadata protection. A missing unsigned
 * template id is computed for the wire without mutating @inner_event. Supplied
 * ids must match. Signed payloads retain their signature for receiver validation;
 * incomplete signed payloads are not converted into unsigned rumors.
 *
 * Steps:
 * 1. Serialize inner_event to JSON
 * 2. Encrypt with NIP-44 using ephemeral key -> recipient pubkey
 * 3. Create kind 1059 event with encrypted content
 * 4. Add p-tag with recipient pubkey
 * 5. Set randomized timestamp (see nostr_nip59_randomize_timestamp())
 * 6. Sign with ephemeral key
 *
 * Returns: (transfer full) (nullable): gift wrap event ready to publish, or
 * NULL on error, including when no randomness is available for the timestamp
 */
NOSTR_NIP59_API NostrEvent *nostr_nip59_wrap(NostrEvent *inner_event,
                              const char *recipient_pubkey_hex,
                              const char *ephemeral_sk_hex);

/**
 * nostr_nip59_wrap_with_key:
 * @inner_event: (transfer none): event to wrap
 * @recipient_pubkey_hex: recipient's public key (64 hex chars)
 * @ephemeral_sk_bin: (array fixed-size=32): 32-byte ephemeral secret key
 *
 * Like nostr_nip59_wrap but accepts binary key for efficiency when
 * generating multiple wraps or integrating with secure key storage.
 *
 * Returns: (transfer full) (nullable): gift wrap event, or NULL on error
 */
NOSTR_NIP59_API NostrEvent *nostr_nip59_wrap_with_key(NostrEvent *inner_event,
                                       const char *recipient_pubkey_hex,
                                       const uint8_t ephemeral_sk_bin[32]);

/**
 * nostr_nip59_wrap_kind:
 * @inner_event: (transfer none): signed event or unsigned rumor to wrap
 * @recipient_pubkey_hex: recipient's public key (64 hex chars)
 * @outer_kind: NOSTR_KIND_GIFT_WRAP (1059) or NOSTR_KIND_EPHEMERAL_GIFT_WRAP (21059)
 * @ephemeral_sk_hex: (nullable): one-use ephemeral key, or NULL to generate
 *
 * Like nostr_nip59_wrap, but selects the outer kind. Kind 1059 retains the
 * randomized timestamp; kind 21059 uses the current time, as in NIP-AC.
 * The inner event is encrypted directly, without adding a seal or changing
 * its timestamp, canonical id or signature. Signature validation of signed
 * inner events occurs on unwrap, as with nostr_nip59_wrap.
 *
 * Returns: (transfer full) (nullable): signed gift wrap, or NULL on error
 * (including an unsupported outer kind)
 */
NOSTR_NIP59_API NostrEvent *nostr_nip59_wrap_kind(NostrEvent *inner_event,
                              const char *recipient_pubkey_hex,
                              int outer_kind,
                              const char *ephemeral_sk_hex);

/**
 * nostr_nip59_wrap_kind_with_key:
 * @inner_event: (transfer none): event to wrap
 * @recipient_pubkey_hex: recipient's public key (64 hex chars)
 * @outer_kind: 1059 or 21059
 * @ephemeral_sk_bin: (array fixed-size=32): one-use ephemeral secret key
 *
 * Like nostr_nip59_wrap_kind but accepts a binary key.
 *
 * Returns: (transfer full) (nullable): signed gift wrap, or NULL on error
 */
NOSTR_NIP59_API NostrEvent *nostr_nip59_wrap_kind_with_key(NostrEvent *inner_event,
                                       const char *recipient_pubkey_hex,
                                       int outer_kind,
                                       const uint8_t ephemeral_sk_bin[32]);

/**
 * nostr_nip59_unwrap:
 * @gift_wrap: (transfer none): gift wrap event (kind 1059)
 * @recipient_sk_hex: recipient's secret key (64 hex chars)
 *
 * Decrypts a gift wrap and extracts the wrapped event.
 * Requires the inner event's declared canonical id. Signed inner events also
 * require a valid signature; unsigned rumors must omit the signature field.
 *
 * Steps:
 * 1. Verify gift_wrap is kind 1059
 * 2. Extract sender pubkey (ephemeral) from gift_wrap
 * 3. Decrypt content with NIP-44 using recipient sk -> sender pk
 * 4. Parse decrypted JSON to event
 *
 * Returns: (transfer full) (nullable): inner event, or NULL on error
 */
NOSTR_NIP59_API NostrEvent *nostr_nip59_unwrap(NostrEvent *gift_wrap,
                                const char *recipient_sk_hex);

/**
 * nostr_nip59_unwrap_with_key:
 * @gift_wrap: (transfer none): gift wrap event (kind 1059)
 * @recipient_sk_bin: (array fixed-size=32): 32-byte recipient secret key
 *
 * Like nostr_nip59_unwrap but accepts binary key for efficiency.
 *
 * Returns: (transfer full) (nullable): inner event, or NULL on error
 */
NOSTR_NIP59_API NostrEvent *nostr_nip59_unwrap_with_key(NostrEvent *gift_wrap,
                                         const uint8_t recipient_sk_bin[32]);

/**
 * nostr_nip59_unwrap_kind:
 * @gift_wrap: (transfer none): gift wrap event
 * @recipient_sk_hex: recipient's secret key (64 hex chars)
 * @expected_kind: 1059 or 21059; must match the outer event exactly
 *
 * Like nostr_nip59_unwrap, including canonical id and signature validation,
 * but requires the selected outer kind. Does not unwrap an additional seal.
 *
 * Returns: (transfer full) (nullable): inner event, or NULL on error
 */
NOSTR_NIP59_API NostrEvent *nostr_nip59_unwrap_kind(NostrEvent *gift_wrap,
                                const char *recipient_sk_hex,
                                int expected_kind);

/**
 * nostr_nip59_unwrap_kind_with_key:
 * @gift_wrap: (transfer none): gift wrap event
 * @recipient_sk_bin: (array fixed-size=32): recipient's secret key
 * @expected_kind: 1059 or 21059; must match the outer event exactly
 *
 * Like nostr_nip59_unwrap_kind but accepts a binary key.
 *
 * Returns: (transfer full) (nullable): inner event, or NULL on error
 */
NOSTR_NIP59_API NostrEvent *nostr_nip59_unwrap_kind_with_key(NostrEvent *gift_wrap,
                                         const uint8_t recipient_sk_bin[32],
                                         int expected_kind);

/**
 * nostr_nip59_validate_gift_wrap:
 * @gift_wrap: (transfer none): gift wrap event to validate
 *
 * Validates gift wrap structure without decrypting:
 * - Kind is 1059
 * - Has valid signature
 * - Has p-tag with recipient pubkey
 * - Has non-empty content
 *
 * Returns: true if structurally valid
 */
NOSTR_NIP59_API bool nostr_nip59_validate_gift_wrap(NostrEvent *gift_wrap);

/**
 * nostr_nip59_validate_gift_wrap_kind:
 * @gift_wrap: (transfer none): gift wrap event to validate
 * @expected_kind: 1059 or 21059; must match the outer event exactly
 *
 * Like nostr_nip59_validate_gift_wrap, but requires the selected outer kind.
 * Existing kind-1059 entry points never accept kind 21059 implicitly.
 *
 * Returns: true if structurally valid, with a canonical id and valid signature
 */
NOSTR_NIP59_API bool nostr_nip59_validate_gift_wrap_kind(NostrEvent *gift_wrap,
                                                       int expected_kind);

/**
 * nostr_nip59_get_recipient:
 * @gift_wrap: (transfer none): gift wrap event
 *
 * Extracts the recipient pubkey from the p-tag.
 *
 * Returns: (transfer full) (nullable): recipient pubkey hex, or NULL if not found
 */
NOSTR_NIP59_API char *nostr_nip59_get_recipient(NostrEvent *gift_wrap);

/**
 * nostr_nip59_is_gift_wrap:
 * @event: (transfer none) (nullable): event to check
 *
 * Quick check if an event is a gift wrap (kind 1059).
 *
 * Returns: true if event is kind 1059
 */
NOSTR_NIP59_API bool nostr_nip59_is_gift_wrap(NostrEvent *event);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP59_H */
