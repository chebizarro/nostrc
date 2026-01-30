/**
 * nostr-brown-list.h - Relay brown list for persistently failing relays (nostrc-py1)
 *
 * A "brown list" is a soft ban for relays that consistently fail to connect.
 * Unlike a blacklist, brown-listed relays will automatically recover after
 * a timeout period, allowing them to be retried.
 *
 * Key features:
 * - Track consecutive connection failures per relay
 * - Brown-list after N failures (configurable, default: 3)
 * - Auto-expire after timeout (configurable, default: 30 minutes)
 * - Only brown-list when network is confirmed up (other relays work)
 * - Optional persistence across app restarts
 */

#ifndef __NOSTR_BROWN_LIST_H__
#define __NOSTR_BROWN_LIST_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * NostrBrownListEntry:
 *
 * Internal structure tracking a single relay's failure state.
 * Not intended for direct use - access via API functions.
 */
typedef struct NostrBrownListEntry {
    char *url;                      /* Relay URL */
    int failure_count;              /* Consecutive failures */
    time_t last_failure_time;       /* When last failure occurred */
    time_t browned_at;              /* When relay was brown-listed (0 if not) */
    time_t expires_at;              /* When brown-list status expires (0 if not browned) */
    struct NostrBrownListEntry *next;
} NostrBrownListEntry;

/**
 * NostrBrownList:
 *
 * Main brown list structure. Create with nostr_brown_list_new().
 */
typedef struct NostrBrownList {
    NostrBrownListEntry *entries;   /* Linked list of entries */
    size_t entry_count;             /* Number of entries */

    /* Configuration */
    int threshold;                  /* Failures before brown-listing (default: 3) */
    int timeout_seconds;            /* How long to exclude relay (default: 1800 = 30 min) */

    /* Network health tracking */
    int connected_count;            /* Number of currently connected relays */
    time_t last_any_success;        /* Last time any relay connected successfully */

    /* Thread safety */
    void *mutex;                    /* Internal mutex (pthread_mutex_t*) */

    /* Persistence */
    char *storage_path;             /* Path to persistence file (NULL = no persistence) */
} NostrBrownList;

/**
 * NostrBrownListStats:
 *
 * Statistics about the brown list state.
 */
typedef struct NostrBrownListStats {
    size_t total_entries;           /* Total tracked relays */
    size_t browned_count;           /* Currently brown-listed relays */
    size_t healthy_count;           /* Relays with no failures */
    size_t failing_count;           /* Relays with failures but not yet browned */
} NostrBrownListStats;

/**
 * NostrBrownListIterator:
 *
 * Iterator for walking through brown-listed relays.
 */
typedef struct NostrBrownListIterator {
    NostrBrownList *list;
    NostrBrownListEntry *current;
    bool only_browned;              /* If true, only iterate brown-listed entries */
} NostrBrownListIterator;

/* ========================================================================
 * Lifecycle
 * ======================================================================== */

/**
 * nostr_brown_list_new:
 *
 * Create a new brown list with default settings.
 * Default threshold: 3 failures
 * Default timeout: 30 minutes (1800 seconds)
 *
 * Returns: (transfer full): Newly allocated brown list, or NULL on error
 */
NostrBrownList *nostr_brown_list_new(void);

/**
 * nostr_brown_list_new_with_config:
 * @threshold: Number of consecutive failures before brown-listing
 * @timeout_seconds: How long to exclude a brown-listed relay
 *
 * Create a new brown list with custom configuration.
 *
 * Returns: (transfer full): Newly allocated brown list, or NULL on error
 */
NostrBrownList *nostr_brown_list_new_with_config(int threshold, int timeout_seconds);

/**
 * nostr_brown_list_free:
 * @list: (transfer full): Brown list to free
 *
 * Free a brown list and all associated resources.
 */
void nostr_brown_list_free(NostrBrownList *list);

/* ========================================================================
 * Configuration
 * ======================================================================== */

/**
 * nostr_brown_list_set_threshold:
 * @list: Brown list
 * @threshold: Number of failures before brown-listing (minimum: 1)
 *
 * Set the failure threshold. Takes effect for future failures.
 */
void nostr_brown_list_set_threshold(NostrBrownList *list, int threshold);

/**
 * nostr_brown_list_get_threshold:
 * @list: Brown list
 *
 * Returns: Current failure threshold
 */
int nostr_brown_list_get_threshold(NostrBrownList *list);

/**
 * nostr_brown_list_set_timeout:
 * @list: Brown list
 * @timeout_seconds: Seconds to exclude brown-listed relays (minimum: 60)
 *
 * Set the brown-list timeout. Takes effect for future brown-listings.
 */
void nostr_brown_list_set_timeout(NostrBrownList *list, int timeout_seconds);

/**
 * nostr_brown_list_get_timeout:
 * @list: Brown list
 *
 * Returns: Current timeout in seconds
 */
int nostr_brown_list_get_timeout(NostrBrownList *list);

/* ========================================================================
 * Recording failures and successes
 * ======================================================================== */

/**
 * nostr_brown_list_record_failure:
 * @list: Brown list
 * @url: Relay URL that failed to connect
 *
 * Record a connection failure for a relay. If this pushes the relay
 * over the threshold (and network is otherwise healthy), it will be
 * brown-listed.
 *
 * Returns: true if relay is now brown-listed, false otherwise
 */
bool nostr_brown_list_record_failure(NostrBrownList *list, const char *url);

/**
 * nostr_brown_list_record_success:
 * @list: Brown list
 * @url: Relay URL that connected successfully
 *
 * Record a successful connection. Resets failure count and removes
 * from brown list if present.
 */
void nostr_brown_list_record_success(NostrBrownList *list, const char *url);

/**
 * nostr_brown_list_update_connected_count:
 * @list: Brown list
 * @connected: Number of currently connected relays
 *
 * Update the count of connected relays. Used to determine if network
 * is healthy (at least one relay connected) before brown-listing.
 */
void nostr_brown_list_update_connected_count(NostrBrownList *list, int connected);

/* ========================================================================
 * Querying brown list status
 * ======================================================================== */

/**
 * nostr_brown_list_is_browned:
 * @list: Brown list
 * @url: Relay URL to check
 *
 * Check if a relay is currently brown-listed.
 * Automatically handles expiry - returns false if timeout has passed.
 *
 * Returns: true if relay is brown-listed and not expired
 */
bool nostr_brown_list_is_browned(NostrBrownList *list, const char *url);

/**
 * nostr_brown_list_should_skip:
 * @list: Brown list
 * @url: Relay URL to check
 *
 * Check if a relay should be skipped in connection attempts.
 * This is the main query function to use before connecting.
 *
 * Returns: true if relay should be skipped
 */
bool nostr_brown_list_should_skip(NostrBrownList *list, const char *url);

/**
 * nostr_brown_list_get_failure_count:
 * @list: Brown list
 * @url: Relay URL
 *
 * Get the current failure count for a relay.
 *
 * Returns: Number of consecutive failures, or 0 if not tracked
 */
int nostr_brown_list_get_failure_count(NostrBrownList *list, const char *url);

/**
 * nostr_brown_list_get_time_remaining:
 * @list: Brown list
 * @url: Relay URL
 *
 * Get seconds remaining until a brown-listed relay can be retried.
 *
 * Returns: Seconds remaining, or 0 if not brown-listed or expired
 */
int nostr_brown_list_get_time_remaining(NostrBrownList *list, const char *url);

/**
 * nostr_brown_list_get_stats:
 * @list: Brown list
 * @stats: (out): Statistics output structure
 *
 * Get statistics about the brown list.
 */
void nostr_brown_list_get_stats(NostrBrownList *list, NostrBrownListStats *stats);

/* ========================================================================
 * Manual management
 * ======================================================================== */

/**
 * nostr_brown_list_clear_relay:
 * @list: Brown list
 * @url: Relay URL to clear
 *
 * Manually clear a relay from the brown list, allowing immediate retry.
 * Resets failure count to 0.
 *
 * Returns: true if relay was found and cleared
 */
bool nostr_brown_list_clear_relay(NostrBrownList *list, const char *url);

/**
 * nostr_brown_list_clear_all:
 * @list: Brown list
 *
 * Clear all entries from the brown list.
 */
void nostr_brown_list_clear_all(NostrBrownList *list);

/**
 * nostr_brown_list_expire_stale:
 * @list: Brown list
 *
 * Manually expire all brown-listed relays whose timeout has passed.
 * Normally happens automatically during queries, but can be called
 * explicitly for cleanup.
 *
 * Returns: Number of entries expired
 */
int nostr_brown_list_expire_stale(NostrBrownList *list);

/* ========================================================================
 * Iteration
 * ======================================================================== */

/**
 * nostr_brown_list_iterator_new:
 * @list: Brown list
 * @only_browned: If true, only iterate currently brown-listed relays
 *
 * Create an iterator for the brown list.
 *
 * Returns: (transfer full): New iterator, or NULL on error
 */
NostrBrownListIterator *nostr_brown_list_iterator_new(NostrBrownList *list, bool only_browned);

/**
 * nostr_brown_list_iterator_next:
 * @iter: Iterator
 * @url: (out) (transfer none): Pointer to receive URL (do not free)
 * @failure_count: (out) (optional): Pointer to receive failure count
 * @time_remaining: (out) (optional): Pointer to receive seconds until expiry
 *
 * Advance to the next entry.
 *
 * Returns: true if an entry was returned, false if iteration complete
 */
bool nostr_brown_list_iterator_next(NostrBrownListIterator *iter,
                                     const char **url,
                                     int *failure_count,
                                     int *time_remaining);

/**
 * nostr_brown_list_iterator_free:
 * @iter: (transfer full): Iterator to free
 */
void nostr_brown_list_iterator_free(NostrBrownListIterator *iter);

/* ========================================================================
 * Persistence (optional)
 * ======================================================================== */

/**
 * nostr_brown_list_set_storage_path:
 * @list: Brown list
 * @path: (nullable): File path for persistence, or NULL to disable
 *
 * Set the storage path for persistence. The brown list will be saved
 * after each modification and loaded on creation.
 *
 * Returns: true if path was set successfully
 */
bool nostr_brown_list_set_storage_path(NostrBrownList *list, const char *path);

/**
 * nostr_brown_list_save:
 * @list: Brown list
 *
 * Manually save the brown list to the configured storage path.
 *
 * Returns: true on success, false on error
 */
bool nostr_brown_list_save(NostrBrownList *list);

/**
 * nostr_brown_list_load:
 * @list: Brown list
 *
 * Manually load the brown list from the configured storage path.
 *
 * Returns: true on success, false on error
 */
bool nostr_brown_list_load(NostrBrownList *list);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_BROWN_LIST_H__ */
