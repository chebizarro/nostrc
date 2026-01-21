/**
 * @file test_harness.h
 * @brief Test harness utilities for nostrc testing
 *
 * Provides event factories, deterministic keypairs, and assertion macros
 * for reproducible Nostr protocol tests.
 */

#ifndef NOSTR_TEST_HARNESS_H
#define NOSTR_TEST_HARNESS_H

#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-tag.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Test Keypairs
 * ============================================================================
 * Deterministic keypairs for reproducible tests. Keys are derived from
 * fixed seeds using SHA-256 hashing to ensure consistent values across runs.
 */

/**
 * NostrTestKeypair:
 * Contains both hex-encoded and raw binary forms of a keypair.
 */
typedef struct NostrTestKeypair {
    char privkey_hex[65];    /* 64 hex chars + null */
    char pubkey_hex[65];     /* 64 hex chars + null (x-only) */
    unsigned char privkey[32];
    unsigned char pubkey[32];
} NostrTestKeypair;

/**
 * nostr_test_generate_keypair:
 * Generate a random keypair for testing.
 *
 * @kp: Output keypair structure
 */
void nostr_test_generate_keypair(NostrTestKeypair *kp);

/**
 * nostr_test_keypair_from_seed:
 * Generate deterministic keypair from seed (for reproducible tests).
 * Uses SHA-256(seed_bytes) as the private key.
 *
 * @kp: Output keypair structure
 * @seed: Seed value for deterministic derivation
 */
void nostr_test_keypair_from_seed(NostrTestKeypair *kp, uint32_t seed);

/**
 * nostr_test_keypair_get:
 * Get a well-known test keypair by index.
 *
 * @index: 0=ALICE, 1=BOB, 2=CAROL
 * Returns: Pointer to static keypair, or NULL if index out of range
 */
const NostrTestKeypair *nostr_test_keypair_get(int index);

/* Well-known test keypair indices */
#define NOSTR_TEST_KEYPAIR_ALICE  0
#define NOSTR_TEST_KEYPAIR_BOB    1
#define NOSTR_TEST_KEYPAIR_CAROL  2

/* Well-known test keypairs (deterministic, initialized on first use) */
extern const NostrTestKeypair *NOSTR_TEST_ALICE;
extern const NostrTestKeypair *NOSTR_TEST_BOB;
extern const NostrTestKeypair *NOSTR_TEST_CAROL;

/* ============================================================================
 * Event Factories
 * ============================================================================
 * Functions to create test events with minimal boilerplate. All returned
 * events must be freed by the caller using nostr_event_free().
 */

/**
 * nostr_test_make_text_note:
 * Create a kind-1 text note with the specified content.
 * Uses a random keypair if not signed afterward.
 *
 * @content: Note content (required)
 * @created_at: Timestamp (0 = use current time)
 * Returns: New unsigned event (caller must free)
 */
NostrEvent *nostr_test_make_text_note(const char *content, int64_t created_at);

/**
 * nostr_test_make_metadata:
 * Create a kind-0 metadata event with the given profile JSON.
 *
 * @name: Display name (can be NULL)
 * @about: About text (can be NULL)
 * @picture: Picture URL (can be NULL)
 * @created_at: Timestamp (0 = use current time)
 * Returns: New unsigned event (caller must free)
 */
NostrEvent *nostr_test_make_metadata(const char *name, const char *about,
                                      const char *picture, int64_t created_at);

/**
 * nostr_test_make_dm:
 * Create a direct message event (kind 4 NIP-04 or kind 14 NIP-17).
 *
 * @content: Message content (for kind 4) or encrypted payload (for kind 14)
 * @recipient_pubkey: Recipient's public key hex
 * @kind: 4 for NIP-04 encrypted DM, 14 for NIP-17 sealed DM
 * @created_at: Timestamp (0 = use current time)
 * Returns: New unsigned event (caller must free)
 *
 * Note: For kind 4, content should be the plaintext. The caller should encrypt
 * it using NIP-04 before publishing. For kind 14, content is the inner event.
 */
NostrEvent *nostr_test_make_dm(const char *content, const char *recipient_pubkey,
                                int kind, int64_t created_at);

/**
 * nostr_test_make_signed_event:
 * Create and sign an event with the given private key.
 *
 * @kind: Event kind
 * @content: Event content
 * @privkey_hex: 64-char hex private key
 * @tags: Optional tags (ownership transferred to event)
 * Returns: Signed event (caller must free), NULL on error
 */
NostrEvent *nostr_test_make_signed_event(int kind, const char *content,
                                          const char *privkey_hex, NostrTags *tags);

/**
 * nostr_test_make_signed_event_with_pubkey:
 * Create and sign an event, specifying the pubkey explicitly.
 * Useful when the pubkey is pre-computed.
 *
 * @kind: Event kind
 * @content: Event content
 * @privkey_hex: 64-char hex private key
 * @pubkey_hex: 64-char hex public key (x-only)
 * @tags: Optional tags (ownership transferred to event)
 * @created_at: Timestamp (0 = use current time)
 * Returns: Signed event (caller must free), NULL on error
 */
NostrEvent *nostr_test_make_signed_event_with_pubkey(int kind, const char *content,
                                                      const char *privkey_hex,
                                                      const char *pubkey_hex,
                                                      NostrTags *tags,
                                                      int64_t created_at);

/* ============================================================================
 * Batch Event Generation
 * ============================================================================
 */

/**
 * nostr_test_generate_events:
 * Generate N events matching a pattern.
 *
 * @count: Number of events to generate
 * @kind: Event kind (-1 = random kinds 0-10)
 * @pubkey_hex: Fixed pubkey (NULL = random per event)
 * @time_start: Start timestamp
 * @time_step: Seconds between events
 * Returns: Array of events (caller must free array and each event)
 */
NostrEvent **nostr_test_generate_events(size_t count, int kind,
                                         const char *pubkey_hex,
                                         int64_t time_start, int64_t time_step);

/**
 * nostr_test_generate_signed_events:
 * Generate N signed events from a single keypair.
 *
 * @count: Number of events to generate
 * @kind: Event kind (-1 = random kinds 0-10)
 * @kp: Keypair to sign with
 * @time_start: Start timestamp
 * @time_step: Seconds between events
 * Returns: Array of signed events (caller must free array and each event)
 */
NostrEvent **nostr_test_generate_signed_events(size_t count, int kind,
                                                const NostrTestKeypair *kp,
                                                int64_t time_start, int64_t time_step);

/**
 * nostr_test_free_events:
 * Free an array of events returned by batch generation functions.
 *
 * @events: Array of event pointers
 * @count: Number of events in array
 */
void nostr_test_free_events(NostrEvent **events, size_t count);

/* ============================================================================
 * Assertion Functions
 * ============================================================================
 * These functions check conditions and print detailed failure messages.
 * Use the macros below for automatic file/line information.
 */

/**
 * nostr_test_assert_event_matches:
 * Assert that an event matches a filter.
 */
void nostr_test_assert_event_matches(const NostrEvent *event, const NostrFilter *filter,
                                      const char *file, int line);

/**
 * nostr_test_assert_event_not_matches:
 * Assert that an event does NOT match a filter.
 */
void nostr_test_assert_event_not_matches(const NostrEvent *event, const NostrFilter *filter,
                                          const char *file, int line);

/**
 * nostr_test_assert_event_equals:
 * Assert two events are equivalent (same id, pubkey, kind, content, created_at, tags).
 */
void nostr_test_assert_event_equals(const NostrEvent *a, const NostrEvent *b,
                                     const char *file, int line);

/**
 * nostr_test_assert_signature_valid:
 * Assert event has valid signature.
 */
void nostr_test_assert_signature_valid(const NostrEvent *event,
                                        const char *file, int line);

/**
 * nostr_test_assert_tag_exists:
 * Assert that the event has a tag with the given key and optional value.
 *
 * @event: Event to check
 * @key: Tag key (e.g., "p", "e", "t")
 * @value: Expected value (NULL to match any value)
 * @file: Source file (use __FILE__)
 * @line: Source line (use __LINE__)
 */
void nostr_test_assert_tag_exists(const NostrEvent *event, const char *key,
                                   const char *value, const char *file, int line);

/**
 * nostr_test_assert_tag_not_exists:
 * Assert that the event does NOT have a tag with the given key and value.
 */
void nostr_test_assert_tag_not_exists(const NostrEvent *event, const char *key,
                                       const char *value, const char *file, int line);

/* ============================================================================
 * Assertion Macros
 * ============================================================================
 * Convenience macros that automatically pass file and line information.
 */

#define NOSTR_ASSERT_EVENT_MATCHES(ev, f) \
    nostr_test_assert_event_matches((ev), (f), __FILE__, __LINE__)

#define NOSTR_ASSERT_EVENT_NOT_MATCHES(ev, f) \
    nostr_test_assert_event_not_matches((ev), (f), __FILE__, __LINE__)

#define NOSTR_ASSERT_EVENT_EQUALS(a, b) \
    nostr_test_assert_event_equals((a), (b), __FILE__, __LINE__)

#define NOSTR_ASSERT_SIG_VALID(ev) \
    nostr_test_assert_signature_valid((ev), __FILE__, __LINE__)

#define NOSTR_ASSERT_TAG_EXISTS(ev, key, value) \
    nostr_test_assert_tag_exists((ev), (key), (value), __FILE__, __LINE__)

#define NOSTR_ASSERT_TAG_NOT_EXISTS(ev, key, value) \
    nostr_test_assert_tag_not_exists((ev), (key), (value), __FILE__, __LINE__)

/* Generic assertion with message */
#define NOSTR_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "ASSERTION FAILED at %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
        exit(1); \
    } \
} while(0)

#define NOSTR_ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "ASSERTION FAILED at %s:%d: %s (expected %ld, got %ld)\n", \
                __FILE__, __LINE__, (msg), (long)(b), (long)(a)); \
        exit(1); \
    } \
} while(0)

#define NOSTR_ASSERT_STR_EQ(a, b, msg) do { \
    const char *_a = (a); const char *_b = (b); \
    if ((_a == NULL && _b != NULL) || (_a != NULL && _b == NULL) || \
        (_a != NULL && _b != NULL && strcmp(_a, _b) != 0)) { \
        fprintf(stderr, "ASSERTION FAILED at %s:%d: %s\n  expected: \"%s\"\n  got:      \"%s\"\n", \
                __FILE__, __LINE__, (msg), _b ? _b : "(null)", _a ? _a : "(null)"); \
        exit(1); \
    } \
} while(0)

#define NOSTR_ASSERT_NOT_NULL(ptr, msg) do { \
    if ((ptr) == NULL) { \
        fprintf(stderr, "ASSERTION FAILED at %s:%d: %s (got NULL)\n", __FILE__, __LINE__, (msg)); \
        exit(1); \
    } \
} while(0)

/* ============================================================================
 * Timing Utilities
 * ============================================================================
 */

/**
 * NostrTestCondition:
 * Function type for condition checks.
 */
typedef bool (*NostrTestCondition)(void *ctx);

/**
 * nostr_test_wait_condition:
 * Wait for a condition to become true.
 *
 * @check: Function returning true when condition is met
 * @ctx: User context for check function
 * @timeout_ms: Maximum wait time in milliseconds
 * Returns: true if condition met, false on timeout
 */
bool nostr_test_wait_condition(NostrTestCondition check, void *ctx, int timeout_ms);

/**
 * nostr_test_get_timestamp:
 * Get current Unix timestamp in seconds.
 */
int64_t nostr_test_get_timestamp(void);

/* ============================================================================
 * Fixture Loading
 * ============================================================================
 */

/**
 * nostr_test_load_events_jsonl:
 * Load events from a JSONL file (one JSON event per line).
 *
 * @path: Path to JSONL file
 * @count: Output parameter for number of events loaded
 * Returns: Array of events (caller must free array and events), NULL on error
 */
NostrEvent **nostr_test_load_events_jsonl(const char *path, size_t *count);

/**
 * nostr_test_fixture_path:
 * Get the full path to a fixture file.
 * Uses NOSTR_TEST_FIXTURES_DIR environment variable or compile-time default.
 *
 * @filename: Fixture filename (e.g., "events.jsonl")
 * Returns: Newly allocated path string (caller must free), NULL on error
 */
char *nostr_test_fixture_path(const char *filename);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_TEST_HARNESS_H */
