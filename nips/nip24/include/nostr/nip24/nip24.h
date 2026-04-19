#ifndef NIPS_NIP24_NOSTR_NIP24_NIP24_H
#define NIPS_NIP24_NOSTR_NIP24_NIP24_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * NIP-24: Extra Metadata Fields and Conventions
 *
 * Defines additional profile metadata fields for kind 0 events.
 * Profile data is stored as JSON in the event content.
 *
 * Standard fields: name, display_name, about, picture, banner,
 *   website, nip05, lud06, lud16, bot
 */

/**
 * Parsed profile metadata.
 *
 * All string pointers are heap-allocated. The caller must free the
 * entire struct with nostr_nip24_profile_free().
 */
typedef struct {
    char *name;          /**< Username / handle */
    char *display_name;  /**< Display name */
    char *about;         /**< Bio / description */
    char *picture;       /**< Avatar URL */
    char *banner;        /**< Banner image URL */
    char *website;       /**< Website URL */
    char *nip05;         /**< NIP-05 identifier */
    char *lud06;         /**< LNURL pay */
    char *lud16;         /**< Lightning address */
    bool bot;            /**< Whether this is a bot account */
} NostrNip24Profile;

/**
 * nostr_nip24_parse_profile:
 * @json: (in): JSON content string from a kind 0 event
 * @out: (out): profile to populate (heap-allocated strings)
 *
 * Parses profile metadata fields from JSON content.
 * Uses simple string scanning; handles standard JSON string values.
 * Caller must free with nostr_nip24_profile_free().
 *
 * Returns: 0 on success, -EINVAL on bad input
 */
int nostr_nip24_parse_profile(const char *json, NostrNip24Profile *out);

/**
 * nostr_nip24_profile_free:
 * @profile: (in): profile to free
 *
 * Frees all allocated strings in the profile. Does NOT free the
 * struct itself (it may be stack-allocated).
 */
void nostr_nip24_profile_free(NostrNip24Profile *profile);

/**
 * nostr_nip24_get_display_name:
 * @json: (in): JSON content string
 *
 * Returns display_name if set, falling back to name.
 * Caller must free() the result.
 *
 * Returns: (transfer full) (nullable): display name or NULL
 */
char *nostr_nip24_get_display_name(const char *json);

/**
 * nostr_nip24_is_bot:
 * @json: (in): JSON content string
 *
 * Returns: true if the "bot" field is true
 */
bool nostr_nip24_is_bot(const char *json);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP24_NOSTR_NIP24_NIP24_H */
