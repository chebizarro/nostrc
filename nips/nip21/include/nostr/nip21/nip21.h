#ifndef NIPS_NIP21_NOSTR_NIP21_NIP21_H
#define NIPS_NIP21_NOSTR_NIP21_NIP21_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr/nip19/nip19.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * NIP-21: nostr: URI Scheme
 *
 * Defines the "nostr:" URI scheme for referencing nostr entities.
 * A nostr URI is simply "nostr:" followed by a NIP-19 bech32 string:
 *
 *   nostr:npub1...     → public key
 *   nostr:note1...     → event ID
 *   nostr:nprofile1... → profile with relay hints
 *   nostr:nevent1...   → event with relay hints
 *   nostr:naddr1...    → addressable event
 */

/** The nostr URI prefix. */
#define NOSTR_NIP21_PREFIX "nostr:"
#define NOSTR_NIP21_PREFIX_LEN 6

/**
 * nostr_nip21_is_uri:
 * @str: (in) (not nullable): string to test
 *
 * Tests whether a string is a valid nostr: URI.
 * Checks for the "nostr:" prefix and validates the bech32 portion.
 *
 * Returns: true if the string is a valid nostr: URI
 */
bool nostr_nip21_is_uri(const char *str);

/**
 * nostr_nip21_parse:
 * @uri: (in) (not nullable): nostr: URI string
 * @out_type: (out): detected NIP-19 entity type
 * @out_bech32: (out) (transfer none): pointer to the bech32 portion
 *     within the URI string (borrowed, valid while @uri is alive)
 *
 * Parses a nostr: URI, extracting the bech32 portion and detecting
 * the entity type. The returned bech32 pointer points into the
 * original URI string (no allocation).
 *
 * Returns: 0 on success, -EINVAL if not a valid nostr: URI
 */
int nostr_nip21_parse(const char *uri, NostrBech32Type *out_type,
                       const char **out_bech32);

/**
 * nostr_nip21_build:
 * @bech32: (in) (not nullable): NIP-19 bech32 string
 *
 * Constructs a nostr: URI by prepending "nostr:" to a bech32 string.
 *
 * Returns: (transfer full) (nullable): heap-allocated URI, caller frees
 */
char *nostr_nip21_build(const char *bech32);

/**
 * nostr_nip21_build_npub:
 * @pubkey: 32-byte public key
 *
 * Convenience: encodes a public key as a nostr:npub1... URI.
 *
 * Returns: (transfer full) (nullable): heap-allocated URI, caller frees
 */
char *nostr_nip21_build_npub(const uint8_t pubkey[32]);

/**
 * nostr_nip21_build_note:
 * @event_id: 32-byte event ID
 *
 * Convenience: encodes an event ID as a nostr:note1... URI.
 *
 * Returns: (transfer full) (nullable): heap-allocated URI, caller frees
 */
char *nostr_nip21_build_note(const uint8_t event_id[32]);

/**
 * nostr_nip21_build_nprofile:
 * @p: (in) (not nullable): profile pointer with pubkey and optional relays
 *
 * Convenience: encodes a profile pointer as a nostr:nprofile1... URI.
 *
 * Returns: (transfer full) (nullable): heap-allocated URI, caller frees
 */
char *nostr_nip21_build_nprofile(const NostrProfilePointer *p);

/**
 * nostr_nip21_build_nevent:
 * @e: (in) (not nullable): event pointer with ID and optional metadata
 *
 * Convenience: encodes an event pointer as a nostr:nevent1... URI.
 *
 * Returns: (transfer full) (nullable): heap-allocated URI, caller frees
 */
char *nostr_nip21_build_nevent(const NostrEventPointer *e);

/**
 * nostr_nip21_build_naddr:
 * @a: (in) (not nullable): entity pointer for an addressable event
 *
 * Convenience: encodes an entity pointer as a nostr:naddr1... URI.
 *
 * Returns: (transfer full) (nullable): heap-allocated URI, caller frees
 */
char *nostr_nip21_build_naddr(const NostrEntityPointer *a);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP21_NOSTR_NIP21_NIP21_H */
