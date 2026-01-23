#ifndef NOSTR_NIP59_H
#define NOSTR_NIP59_H

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
int nostr_nip59_create_ephemeral_key(char **sk_hex_out, char **pk_hex_out);

/**
 * nostr_nip59_randomize_timestamp:
 * @base_time: base timestamp (use 0 for current time)
 * @window_seconds: randomization window in seconds (use 0 for default 2 days)
 *
 * Creates an obfuscated timestamp for metadata protection.
 * Returns a random timestamp within [base_time - window_seconds, base_time].
 *
 * Returns: randomized timestamp
 */
int64_t nostr_nip59_randomize_timestamp(int64_t base_time, uint32_t window_seconds);

/**
 * nostr_nip59_wrap:
 * @inner_event: (transfer none): event to wrap (any kind, can be signed or unsigned)
 * @recipient_pubkey_hex: recipient's public key (64 hex chars)
 * @ephemeral_sk_hex: (nullable): ephemeral secret key to use, or NULL to generate
 *
 * Wraps any event in a kind-1059 gift wrap for private transmission.
 * The wrapped event is NIP-44 encrypted to the recipient using an ephemeral key.
 * The timestamp is randomized for metadata protection.
 *
 * Steps:
 * 1. Serialize inner_event to JSON
 * 2. Encrypt with NIP-44 using ephemeral key -> recipient pubkey
 * 3. Create kind 1059 event with encrypted content
 * 4. Add p-tag with recipient pubkey
 * 5. Set randomized timestamp
 * 6. Sign with ephemeral key
 *
 * Returns: (transfer full) (nullable): gift wrap event ready to publish, or NULL on error
 */
NostrEvent *nostr_nip59_wrap(NostrEvent *inner_event,
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
NostrEvent *nostr_nip59_wrap_with_key(NostrEvent *inner_event,
                                       const char *recipient_pubkey_hex,
                                       const uint8_t ephemeral_sk_bin[32]);

/**
 * nostr_nip59_unwrap:
 * @gift_wrap: (transfer none): gift wrap event (kind 1059)
 * @recipient_sk_hex: recipient's secret key (64 hex chars)
 *
 * Decrypts a gift wrap and extracts the wrapped event.
 * Does NOT validate the inner event's signature (it may be unsigned like rumors).
 *
 * Steps:
 * 1. Verify gift_wrap is kind 1059
 * 2. Extract sender pubkey (ephemeral) from gift_wrap
 * 3. Decrypt content with NIP-44 using recipient sk -> sender pk
 * 4. Parse decrypted JSON to event
 *
 * Returns: (transfer full) (nullable): inner event, or NULL on error
 */
NostrEvent *nostr_nip59_unwrap(NostrEvent *gift_wrap,
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
NostrEvent *nostr_nip59_unwrap_with_key(NostrEvent *gift_wrap,
                                         const uint8_t recipient_sk_bin[32]);

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
bool nostr_nip59_validate_gift_wrap(NostrEvent *gift_wrap);

/**
 * nostr_nip59_get_recipient:
 * @gift_wrap: (transfer none): gift wrap event
 *
 * Extracts the recipient pubkey from the p-tag.
 *
 * Returns: (transfer full) (nullable): recipient pubkey hex, or NULL if not found
 */
char *nostr_nip59_get_recipient(NostrEvent *gift_wrap);

/**
 * nostr_nip59_is_gift_wrap:
 * @event: (transfer none) (nullable): event to check
 *
 * Quick check if an event is a gift wrap (kind 1059).
 *
 * Returns: true if event is kind 1059
 */
bool nostr_nip59_is_gift_wrap(NostrEvent *event);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP59_H */
