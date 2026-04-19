#ifndef NIPS_NIP39_NOSTR_NIP39_NIP39_H
#define NIPS_NIP39_NOSTR_NIP39_NIP39_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include "nostr-tag.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * NIP-39: External Identities in Profiles
 *
 * Allows users to link external identities (GitHub, Twitter, etc.)
 * to their nostr profile via "i" tags on kind 0 metadata events.
 *
 * Tag format: ["i", "platform:identity", "proof_url"]
 *
 * Example: ["i", "github:jb55", "https://gist.github.com/jb55/abc123/raw"]
 */

/**
 * Known identity platforms.
 */
typedef enum {
    NOSTR_NIP39_PLATFORM_UNKNOWN = 0,
    NOSTR_NIP39_PLATFORM_GITHUB,
    NOSTR_NIP39_PLATFORM_TWITTER,
    NOSTR_NIP39_PLATFORM_MASTODON,
    NOSTR_NIP39_PLATFORM_TELEGRAM,
    NOSTR_NIP39_PLATFORM_KEYBASE,
    NOSTR_NIP39_PLATFORM_DNS,
    NOSTR_NIP39_PLATFORM_REDDIT,
    NOSTR_NIP39_PLATFORM_WEBSITE,
} NostrNip39Platform;

/**
 * A parsed external identity claim.
 *
 * Pointers are borrowed from the event's tag data and valid only
 * while the source event is alive and unmodified.
 */
typedef struct {
    NostrNip39Platform platform; /**< Detected platform enum */
    const char *value;           /**< Full "platform:identity" string (borrowed) */
    const char *identity;        /**< Points past "platform:" in value (borrowed) */
    const char *proof_url;       /**< Third tag element or NULL (borrowed) */
} NostrNip39Identity;

/**
 * nostr_nip39_parse:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 * @entries: (out caller-allocates): array to fill
 * @max_entries: maximum entries to store
 * @out_count: (out): number of entries parsed
 *
 * Parses all "i" tags from the event into identity entries.
 *
 * Returns: 0 on success, negative errno-style value on error
 */
int nostr_nip39_parse(const NostrEvent *ev, NostrNip39Identity *entries,
                       size_t max_entries, size_t *out_count);

/**
 * nostr_nip39_count:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 *
 * Counts the number of "i" tags in the event.
 *
 * Returns: number of identity entries
 */
size_t nostr_nip39_count(const NostrEvent *ev);

/**
 * nostr_nip39_platform_from_string:
 * @str: (in) (nullable): platform name string (e.g. "github")
 *
 * Maps a platform name to the enum. Case-sensitive.
 *
 * Returns: platform enum value, or UNKNOWN if unrecognized
 */
NostrNip39Platform nostr_nip39_platform_from_string(const char *str);

/**
 * nostr_nip39_platform_to_string:
 * @platform: platform enum value
 *
 * Returns: (transfer none): static string for the platform, or "unknown"
 */
const char *nostr_nip39_platform_to_string(NostrNip39Platform platform);

/**
 * nostr_nip39_detect_platform:
 * @value: (in) (not nullable): full "platform:identity" string
 *
 * Detects the platform from a tag value by inspecting the prefix
 * before the first colon.
 *
 * Returns: detected platform enum
 */
NostrNip39Platform nostr_nip39_detect_platform(const char *value);

/**
 * nostr_nip39_build_tag:
 * @platform: platform name string (e.g. "github")
 * @identity: identity on the platform (e.g. "username")
 * @proof_url: (nullable): URL to proof, or NULL
 *
 * Builds an "i" tag: ["i", "platform:identity", "proof_url"].
 *
 * Returns: (transfer full) (nullable): new NostrTag, caller frees
 */
NostrTag *nostr_nip39_build_tag(const char *platform, const char *identity,
                                 const char *proof_url);

/**
 * nostr_nip39_add:
 * @ev: (inout) (transfer none) (not nullable): event to modify
 * @platform: platform name string
 * @identity: identity on the platform
 * @proof_url: (nullable): URL to proof, or NULL
 *
 * Adds an "i" identity tag to the event.
 *
 * Returns: 0 on success, negative errno-style value on error
 */
int nostr_nip39_add(NostrEvent *ev, const char *platform,
                     const char *identity, const char *proof_url);

/**
 * nostr_nip39_find:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 * @platform: (in) (not nullable): platform name to search for
 * @out: (out): found identity entry
 *
 * Finds the first identity entry matching the given platform.
 *
 * Returns: true if found, false otherwise
 */
bool nostr_nip39_find(const NostrEvent *ev, const char *platform,
                       NostrNip39Identity *out);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP39_NOSTR_NIP39_NIP39_H */
