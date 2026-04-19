#ifndef NIPS_NIP58_NOSTR_NIP58_NIP58_H
#define NIPS_NIP58_NOSTR_NIP58_NIP58_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include "nostr-tag.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * NIP-58: Badges
 *
 * Three event kinds for badge system:
 *   - Kind 30009: Badge Definition (created by issuer)
 *   - Kind 8:     Badge Award (issuer awards to users)
 *   - Kind 30008: Profile Badges (user displays earned badges)
 */

#define NOSTR_NIP58_KIND_BADGE_DEFINITION 30009
#define NOSTR_NIP58_KIND_BADGE_AWARD      8
#define NOSTR_NIP58_KIND_PROFILE_BADGES   30008

/* ============== Badge Definition (kind 30009) ============== */

/**
 * Parsed badge definition with borrowed pointers.
 * Valid while the source event is alive and unmodified.
 */
typedef struct {
    const char *identifier;  /**< "d" tag value (borrowed, required) */
    const char *name;        /**< "name" tag value (borrowed, nullable) */
    const char *description; /**< "description" tag value (borrowed, nullable) */
    const char *image_url;   /**< "image" tag URL (borrowed, nullable) */
    const char *image_dims;  /**< "image" tag dimensions (borrowed, nullable) */
    const char *thumb_url;   /**< First "thumb" tag URL (borrowed, nullable) */
    const char *thumb_dims;  /**< First "thumb" tag dimensions (borrowed, nullable) */
} NostrNip58BadgeDef;

/**
 * nostr_nip58_parse_definition:
 * @ev: (in): event to inspect (should be kind 30009)
 * @out: (out): parsed badge definition
 *
 * Parses badge definition metadata from tag data.
 * Does NOT check the event kind — caller can do that if desired.
 *
 * Returns: 0 on success (found "d" tag), -EINVAL on bad input, -ENOENT if no "d" tag
 */
int nostr_nip58_parse_definition(const NostrEvent *ev, NostrNip58BadgeDef *out);

/**
 * nostr_nip58_validate_definition:
 * @ev: (in): event to validate
 *
 * Checks that the event is kind 30009 and has a "d" tag.
 *
 * Returns: true if valid badge definition event
 */
bool nostr_nip58_validate_definition(const NostrEvent *ev);

/**
 * nostr_nip58_create_definition:
 * @ev: (inout): event to populate (should be freshly created)
 * @def: (in): badge definition to encode
 *
 * Sets kind to 30009 and adds all badge definition tags.
 * Only "identifier" (d tag) is required; others are optional.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip58_create_definition(NostrEvent *ev, const NostrNip58BadgeDef *def);

/* ============== Badge Award (kind 8) ============== */

/**
 * Parsed badge award with borrowed pointer.
 */
typedef struct {
    const char *badge_ref; /**< "a" tag value, e.g. "30009:pubkey:id" (borrowed) */
} NostrNip58BadgeAward;

/**
 * nostr_nip58_parse_award:
 * @ev: (in): event to inspect (should be kind 8)
 * @out: (out): parsed badge award
 *
 * Parses the badge reference ("a" tag) from an award event.
 *
 * Returns: 0 on success, -EINVAL on bad input, -ENOENT if no "a" tag
 */
int nostr_nip58_parse_award(const NostrEvent *ev, NostrNip58BadgeAward *out);

/**
 * nostr_nip58_validate_award:
 * @ev: (in): event to validate
 *
 * Checks that the event is kind 8 and has both "a" and "p" tags.
 *
 * Returns: true if valid badge award event
 */
bool nostr_nip58_validate_award(const NostrEvent *ev);

/**
 * nostr_nip58_award_count_awardees:
 * @ev: (in): badge award event
 *
 * Counts the number of "p" tags (awardees) in the event.
 *
 * Returns: number of awardees
 */
size_t nostr_nip58_award_count_awardees(const NostrEvent *ev);

/**
 * nostr_nip58_award_get_awardees:
 * @ev: (in): badge award event
 * @pubkeys: (out caller-allocates): array of borrowed pubkey pointers
 * @max: maximum pubkeys to store
 * @out_count: (out): number of pubkeys stored
 *
 * Extracts awardee pubkeys from "p" tags.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip58_award_get_awardees(const NostrEvent *ev,
                                    const char **pubkeys, size_t max,
                                    size_t *out_count);

/**
 * nostr_nip58_create_award:
 * @ev: (inout): event to populate
 * @badge_ref: (in): badge definition reference ("30009:pubkey:identifier")
 * @pubkeys: (in): array of awardee pubkey strings
 * @n_pubkeys: number of pubkeys
 *
 * Sets kind to 8 and adds "a" and "p" tags.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip58_create_award(NostrEvent *ev, const char *badge_ref,
                              const char **pubkeys, size_t n_pubkeys);

/* ============== Profile Badges (kind 30008) ============== */

/**
 * A single badge entry in a profile badges event.
 * Represents an alternating "a"/"e" tag pair.
 */
typedef struct {
    const char *badge_ref;   /**< "a" tag value — definition reference (borrowed) */
    const char *award_id;    /**< "e" tag value — award event ID (borrowed) */
    const char *award_relay; /**< "e" tag relay hint (borrowed, nullable) */
} NostrNip58ProfileBadge;

/**
 * nostr_nip58_parse_profile_badges:
 * @ev: (in): event to inspect (should be kind 30008)
 * @entries: (out caller-allocates): array to fill
 * @max_entries: maximum entries to store
 * @out_count: (out): number of entries parsed
 *
 * Parses alternating "a"/"e" tag pairs from a profile badges event.
 * Verifies that the "d" tag equals "profile_badges".
 *
 * Returns: 0 on success, -EINVAL on bad input, -ENOENT if invalid d tag
 */
int nostr_nip58_parse_profile_badges(const NostrEvent *ev,
                                      NostrNip58ProfileBadge *entries,
                                      size_t max_entries, size_t *out_count);

/**
 * nostr_nip58_validate_profile_badges:
 * @ev: (in): event to validate
 *
 * Checks that the event is kind 30008 and has d=profile_badges.
 *
 * Returns: true if valid profile badges event
 */
bool nostr_nip58_validate_profile_badges(const NostrEvent *ev);

/**
 * nostr_nip58_create_profile_badges:
 * @ev: (inout): event to populate
 * @badges: (in): array of profile badge entries
 * @n_badges: number of badge entries
 *
 * Sets kind to 30008, adds d=profile_badges tag, then alternating "a"/"e" tags.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip58_create_profile_badges(NostrEvent *ev,
                                       const NostrNip58ProfileBadge *badges,
                                       size_t n_badges);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP58_NOSTR_NIP58_NIP58_H */
