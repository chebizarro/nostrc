#ifndef NIPS_NIP75_NOSTR_NIP75_NIP75_H
#define NIPS_NIP75_NOSTR_NIP75_NIP75_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include "nostr-tag.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * NIP-75: Zap Goals
 *
 * Kind 9041 events for crowdfunding/fundraising targets.
 * Content is the goal description.
 *
 * Required tags:
 *   - "amount"    — target in millisatoshis
 *   - "relays"    — relay URLs for zap receipt tallying
 *
 * Optional tags:
 *   - "closed_at" — deadline timestamp
 *   - "image"     — goal image URL
 *   - "summary"   — brief summary
 *   - "r"         — external reference URL
 *   - "a"         — linked addressable event
 */

#define NOSTR_NIP75_KIND_ZAP_GOAL 9041

/**
 * Parsed zap goal with borrowed pointers.
 * Valid while the source event is alive and unmodified.
 */
typedef struct {
    int64_t amount;       /**< Target amount in millisatoshis */
    int64_t closed_at;    /**< Deadline timestamp, 0 = no deadline */
    const char *image;    /**< "image" tag URL (borrowed, nullable) */
    const char *summary;  /**< "summary" tag value (borrowed, nullable) */
    const char *url;      /**< "r" tag external URL (borrowed, nullable) */
    const char *a_tag;    /**< "a" tag linked event (borrowed, nullable) */
} NostrNip75Goal;

/**
 * nostr_nip75_parse:
 * @ev: (in): event to inspect
 * @out: (out): parsed goal metadata
 *
 * Parses zap goal from event tags.
 * Goal description is in event content (use nostr_event_get_content()).
 *
 * Returns: 0 on success, -EINVAL on bad input, -ENOENT if no "amount" tag
 */
int nostr_nip75_parse(const NostrEvent *ev, NostrNip75Goal *out);

/**
 * nostr_nip75_validate:
 * @ev: (in): event to validate
 *
 * Checks kind 9041 and required tags (amount, relays).
 *
 * Returns: true if valid zap goal event
 */
bool nostr_nip75_validate(const NostrEvent *ev);

/**
 * nostr_nip75_is_zap_goal:
 * @ev: (in): event to check
 *
 * Returns: true if event kind is 9041
 */
bool nostr_nip75_is_zap_goal(const NostrEvent *ev);

/**
 * nostr_nip75_get_relays:
 * @ev: (in): event to inspect
 * @relays: (out caller-allocates): array of borrowed relay URL pointers
 * @max: maximum relays to store
 * @out_count: (out): number stored
 *
 * Extracts relay URLs from the "relays" tag (elements 1..N).
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip75_get_relays(const NostrEvent *ev, const char **relays,
                            size_t max, size_t *out_count);

/**
 * nostr_nip75_is_expired:
 * @goal: (in): parsed goal
 * @now: current unix timestamp
 *
 * Returns: true if goal has a deadline and it has passed
 */
bool nostr_nip75_is_expired(const NostrNip75Goal *goal, int64_t now);

/**
 * nostr_nip75_is_complete:
 * @current_msats: current amount received
 * @target_msats: target amount
 *
 * Returns: true if current >= target
 */
bool nostr_nip75_is_complete(int64_t current_msats, int64_t target_msats);

/**
 * nostr_nip75_progress_percent:
 * @current_msats: current amount received
 * @target_msats: target amount
 *
 * Returns: progress as 0.0–100.0+ percentage, 0.0 if target is 0
 */
double nostr_nip75_progress_percent(int64_t current_msats,
                                     int64_t target_msats);

/**
 * nostr_nip75_create_goal:
 * @ev: (inout): event to populate
 * @goal: (in): goal metadata
 * @content: (in): goal description (event content)
 *
 * Sets kind to 9041, adds tags, sets content.
 * Relays should be added separately with nostr_nip75_add_relays().
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip75_create_goal(NostrEvent *ev, const NostrNip75Goal *goal,
                             const char *content);

/**
 * nostr_nip75_add_relays:
 * @ev: (inout): event to modify
 * @relays: (in): array of relay URL strings
 * @n_relays: number of relays
 *
 * Adds a "relays" tag with all relay URLs.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip75_add_relays(NostrEvent *ev, const char **relays,
                            size_t n_relays);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP75_NOSTR_NIP75_NIP75_H */
