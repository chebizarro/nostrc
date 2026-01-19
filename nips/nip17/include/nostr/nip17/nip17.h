#ifndef NOSTR_NIP17_H
#define NOSTR_NIP17_H

/**
 * NIP-17: Private Direct Messages
 *
 * Implements gift-wrapped direct messages using three-layer encryption:
 * - Rumor (kind 14): Unsigned chat message
 * - Seal (kind 13): Signed wrapper encrypted with NIP-44
 * - Gift Wrap (kind 1059): Final container with ephemeral sender
 */

#include <stdbool.h>
#include <stdint.h>
#include "nostr-event.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * nostr_nip17_create_rumor:
 * @sender_pubkey_hex: sender's public key (64 hex chars)
 * @recipient_pubkey_hex: recipient's public key (64 hex chars)
 * @content: message content (UTF-8)
 * @created_at: timestamp (use 0 for current time)
 *
 * Creates an unsigned kind 14 rumor event containing the DM content.
 * The rumor is NOT signed - it will be wrapped in a seal.
 *
 * Returns: (transfer full) (nullable): new rumor event, or NULL on error
 */
NostrEvent *nostr_nip17_create_rumor(const char *sender_pubkey_hex,
                                      const char *recipient_pubkey_hex,
                                      const char *content,
                                      int64_t created_at);

/**
 * nostr_nip17_create_seal:
 * @rumor: (transfer none): unsigned rumor event
 * @sender_sk_hex: sender's private key (64 hex chars)
 * @recipient_pubkey_hex: recipient's public key (64 hex chars)
 *
 * Creates a kind 13 seal event that wraps the rumor.
 * The seal's content is the NIP-44 encrypted JSON of the rumor.
 * The seal is signed by the sender.
 *
 * Returns: (transfer full) (nullable): new seal event, or NULL on error
 */
NostrEvent *nostr_nip17_create_seal(NostrEvent *rumor,
                                     const char *sender_sk_hex,
                                     const char *recipient_pubkey_hex);

/**
 * nostr_nip17_create_gift_wrap:
 * @seal: (transfer none): signed seal event
 * @recipient_pubkey_hex: recipient's public key (64 hex chars)
 *
 * Creates a kind 1059 gift wrap event containing the encrypted seal.
 * Uses an ephemeral keypair for the gift wrap signature.
 * Randomizes created_at timestamp for metadata protection.
 *
 * Returns: (transfer full) (nullable): new gift wrap event, or NULL on error
 */
NostrEvent *nostr_nip17_create_gift_wrap(NostrEvent *seal,
                                          const char *recipient_pubkey_hex);

/**
 * nostr_nip17_wrap_dm:
 * @sender_sk_hex: sender's private key (64 hex chars)
 * @recipient_pubkey_hex: recipient's public key (64 hex chars)
 * @content: message content (UTF-8)
 *
 * Convenience function to create a complete gift-wrapped DM.
 * Combines create_rumor, create_seal, and create_gift_wrap.
 *
 * Returns: (transfer full) (nullable): gift wrap event ready to publish
 */
NostrEvent *nostr_nip17_wrap_dm(const char *sender_sk_hex,
                                 const char *recipient_pubkey_hex,
                                 const char *content);

/**
 * nostr_nip17_unwrap_gift_wrap:
 * @gift_wrap: (transfer none): gift wrap event (kind 1059)
 * @recipient_sk_hex: recipient's private key (64 hex chars)
 *
 * Decrypts a gift wrap to extract the seal event.
 *
 * Returns: (transfer full) (nullable): seal event, or NULL on error
 */
NostrEvent *nostr_nip17_unwrap_gift_wrap(NostrEvent *gift_wrap,
                                          const char *recipient_sk_hex);

/**
 * nostr_nip17_unwrap_seal:
 * @seal: (transfer none): seal event (kind 13)
 * @recipient_sk_hex: recipient's private key (64 hex chars)
 *
 * Decrypts a seal to extract the rumor event.
 *
 * Returns: (transfer full) (nullable): rumor event, or NULL on error
 */
NostrEvent *nostr_nip17_unwrap_seal(NostrEvent *seal,
                                     const char *recipient_sk_hex);

/**
 * nostr_nip17_decrypt_dm:
 * @gift_wrap: (transfer none): gift wrap event (kind 1059)
 * @recipient_sk_hex: recipient's private key (64 hex chars)
 * @content_out: (out) (transfer full): extracted message content
 * @sender_pubkey_out: (out) (transfer full) (nullable): sender's pubkey
 *
 * Convenience function to fully unwrap a DM and extract content.
 * Validates that the seal pubkey matches the rumor pubkey.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip17_decrypt_dm(NostrEvent *gift_wrap,
                            const char *recipient_sk_hex,
                            char **content_out,
                            char **sender_pubkey_out);

/**
 * nostr_nip17_validate_gift_wrap:
 * @gift_wrap: (transfer none): gift wrap event
 *
 * Validates gift wrap structure (kind, signature, p-tag).
 *
 * Returns: true if valid
 */
bool nostr_nip17_validate_gift_wrap(NostrEvent *gift_wrap);

/**
 * nostr_nip17_validate_seal:
 * @seal: (transfer none): seal event
 * @rumor: (transfer none) (nullable): rumor to validate against
 *
 * Validates seal structure and optionally checks pubkey consistency.
 * If @rumor is provided, verifies seal.pubkey == rumor.pubkey.
 *
 * Returns: true if valid
 */
bool nostr_nip17_validate_seal(NostrEvent *seal, NostrEvent *rumor);

/* ---- DM Relay Preferences (Kind 10050) ---- */

/**
 * NostrDmRelayList:
 *
 * List of relay URLs where a user prefers to receive DMs.
 */
typedef struct {
    char **relays;      /* NULL-terminated array of relay URLs */
    size_t count;       /* Number of relays */
} NostrDmRelayList;

/**
 * nostr_nip17_create_dm_relay_list:
 * @relays: NULL-terminated array of relay URLs
 * @sk_hex: private key to sign the event (64 hex chars)
 *
 * Creates a kind 10050 event advertising preferred DM relays.
 * This is a replaceable event - only the latest one is valid.
 *
 * Returns: (transfer full) (nullable): signed relay list event
 */
NostrEvent *nostr_nip17_create_dm_relay_list(const char **relays,
                                              const char *sk_hex);

/**
 * nostr_nip17_parse_dm_relay_list:
 * @event: (transfer none): kind 10050 event
 *
 * Extracts relay URLs from a DM relay list event.
 *
 * Returns: (transfer full) (nullable): relay list, or NULL on error
 */
NostrDmRelayList *nostr_nip17_parse_dm_relay_list(NostrEvent *event);

/**
 * nostr_nip17_free_dm_relay_list:
 * @list: (transfer full) (nullable): relay list to free
 *
 * Frees a DM relay list and all its contents.
 */
void nostr_nip17_free_dm_relay_list(NostrDmRelayList *list);

/**
 * nostr_nip17_get_dm_relays_for_pubkey:
 * @pubkey_hex: target user's public key
 * @default_relays: fallback relays if no 10050 event found (nullable)
 *
 * Helper to get relay URLs for sending DMs to a user.
 * Caller should query relays for kind 10050 events from the target pubkey.
 * This function parses the event if found, or returns defaults.
 *
 * Note: This is a convenience wrapper - actual relay querying is
 * application-specific and not handled here.
 *
 * Returns: (transfer full) (nullable): relay list
 */
NostrDmRelayList *nostr_nip17_get_dm_relays_from_event(NostrEvent *event,
                                                        const char **default_relays);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP17_H */
