/**
 * @file test_harness.c
 * @brief Implementation of test harness utilities for nostrc testing
 */

#include "nostr/testing/test_harness.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-tag.h"
#include "keys.h"

#include <openssl/sha.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ============================================================================
 * Internal Helpers
 * ============================================================================
 */

/* Convert bytes to hex string */
static void bytes_to_hex(const unsigned char *bytes, size_t len, char *hex_out) {
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        hex_out[i * 2] = hex_chars[(bytes[i] >> 4) & 0x0F];
        hex_out[i * 2 + 1] = hex_chars[bytes[i] & 0x0F];
    }
    hex_out[len * 2] = '\0';
}

/* Convert hex string to bytes */
static int hex_to_bytes(const char *hex, unsigned char *bytes_out, size_t max_len) {
    size_t hex_len = strlen(hex);
    if (hex_len % 2 != 0 || hex_len / 2 > max_len) {
        return -1;
    }
    for (size_t i = 0; i < hex_len / 2; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
            return -1;
        }
        bytes_out[i] = (unsigned char)byte;
    }
    return (int)(hex_len / 2);
}

/* ============================================================================
 * Test Keypairs
 * ============================================================================
 */

/* Static storage for well-known keypairs */
static NostrTestKeypair s_alice;
static NostrTestKeypair s_bob;
static NostrTestKeypair s_carol;
static int s_keypairs_initialized = 0;

/* Pointers that will be initialized to point to static storage */
const NostrTestKeypair *NOSTR_TEST_ALICE = NULL;
const NostrTestKeypair *NOSTR_TEST_BOB = NULL;
const NostrTestKeypair *NOSTR_TEST_CAROL = NULL;

void nostr_test_keypair_from_seed(NostrTestKeypair *kp, uint32_t seed) {
    if (!kp) return;

    /* Use SHA-256 of seed bytes to derive private key deterministically */
    unsigned char seed_bytes[4];
    seed_bytes[0] = (seed >> 24) & 0xFF;
    seed_bytes[1] = (seed >> 16) & 0xFF;
    seed_bytes[2] = (seed >> 8) & 0xFF;
    seed_bytes[3] = seed & 0xFF;

    SHA256(seed_bytes, 4, kp->privkey);
    bytes_to_hex(kp->privkey, 32, kp->privkey_hex);

    /* Derive public key using libnostr's key utility */
    char *pubkey_hex = nostr_key_get_public(kp->privkey_hex);
    if (pubkey_hex) {
        strncpy(kp->pubkey_hex, pubkey_hex, 64);
        kp->pubkey_hex[64] = '\0';
        hex_to_bytes(pubkey_hex, kp->pubkey, 32);
        free(pubkey_hex);
    } else {
        /* Fallback: zero pubkey on error */
        memset(kp->pubkey, 0, 32);
        memset(kp->pubkey_hex, '0', 64);
        kp->pubkey_hex[64] = '\0';
    }
}

void nostr_test_generate_keypair(NostrTestKeypair *kp) {
    if (!kp) return;

    /* Generate random private key */
    if (RAND_bytes(kp->privkey, 32) != 1) {
        /* Fallback to less secure random if OpenSSL fails */
        for (int i = 0; i < 32; i++) {
            kp->privkey[i] = (unsigned char)(rand() & 0xFF);
        }
    }

    bytes_to_hex(kp->privkey, 32, kp->privkey_hex);

    /* Derive public key */
    char *pubkey_hex = nostr_key_get_public(kp->privkey_hex);
    if (pubkey_hex) {
        strncpy(kp->pubkey_hex, pubkey_hex, 64);
        kp->pubkey_hex[64] = '\0';
        hex_to_bytes(pubkey_hex, kp->pubkey, 32);
        free(pubkey_hex);
    } else {
        memset(kp->pubkey, 0, 32);
        memset(kp->pubkey_hex, '0', 64);
        kp->pubkey_hex[64] = '\0';
    }
}

static void init_well_known_keypairs(void) {
    if (s_keypairs_initialized) return;

    /* Use fixed seeds for deterministic ALICE, BOB, CAROL */
    nostr_test_keypair_from_seed(&s_alice, 0x414C4943); /* "ALIC" */
    nostr_test_keypair_from_seed(&s_bob, 0x424F4220);   /* "BOB " */
    nostr_test_keypair_from_seed(&s_carol, 0x4341524F); /* "CARO" */

    NOSTR_TEST_ALICE = &s_alice;
    NOSTR_TEST_BOB = &s_bob;
    NOSTR_TEST_CAROL = &s_carol;

    s_keypairs_initialized = 1;
}

const NostrTestKeypair *nostr_test_keypair_get(int index) {
    init_well_known_keypairs();

    switch (index) {
        case NOSTR_TEST_KEYPAIR_ALICE: return NOSTR_TEST_ALICE;
        case NOSTR_TEST_KEYPAIR_BOB:   return NOSTR_TEST_BOB;
        case NOSTR_TEST_KEYPAIR_CAROL: return NOSTR_TEST_CAROL;
        default: return NULL;
    }
}

/* ============================================================================
 * Event Factories
 * ============================================================================
 */

int64_t nostr_test_get_timestamp(void) {
    return (int64_t)time(NULL);
}

NostrEvent *nostr_test_make_text_note(const char *content, int64_t created_at) {
    NostrEvent *ev = nostr_event_new();
    if (!ev) return NULL;

    nostr_event_set_kind(ev, 1);
    nostr_event_set_content(ev, content ? content : "");
    nostr_event_set_created_at(ev, created_at > 0 ? created_at : nostr_test_get_timestamp());

    return ev;
}

NostrEvent *nostr_test_make_metadata(const char *name, const char *about,
                                      const char *picture, int64_t created_at) {
    NostrEvent *ev = nostr_event_new();
    if (!ev) return NULL;

    /* Build JSON content for metadata */
    char content[4096];
    int pos = 0;
    pos += snprintf(content + pos, sizeof(content) - pos, "{");

    int need_comma = 0;
    if (name) {
        pos += snprintf(content + pos, sizeof(content) - pos, "\"name\":\"%s\"", name);
        need_comma = 1;
    }
    if (about) {
        if (need_comma) pos += snprintf(content + pos, sizeof(content) - pos, ",");
        pos += snprintf(content + pos, sizeof(content) - pos, "\"about\":\"%s\"", about);
        need_comma = 1;
    }
    if (picture) {
        if (need_comma) pos += snprintf(content + pos, sizeof(content) - pos, ",");
        pos += snprintf(content + pos, sizeof(content) - pos, "\"picture\":\"%s\"", picture);
    }
    snprintf(content + pos, sizeof(content) - pos, "}");

    nostr_event_set_kind(ev, 0);
    nostr_event_set_content(ev, content);
    nostr_event_set_created_at(ev, created_at > 0 ? created_at : nostr_test_get_timestamp());

    return ev;
}

NostrEvent *nostr_test_make_dm(const char *content, const char *recipient_pubkey,
                                int kind, int64_t created_at) {
    NostrEvent *ev = nostr_event_new();
    if (!ev) return NULL;

    nostr_event_set_kind(ev, kind);
    nostr_event_set_content(ev, content ? content : "");
    nostr_event_set_created_at(ev, created_at > 0 ? created_at : nostr_test_get_timestamp());

    /* Add p-tag for recipient */
    if (recipient_pubkey) {
        NostrTags *tags = nostr_tags_new(0);
        NostrTag *p_tag = nostr_tag_new("p", recipient_pubkey, NULL);
        nostr_tags_append(tags, p_tag);
        nostr_event_set_tags(ev, tags);
    }

    return ev;
}

NostrEvent *nostr_test_make_signed_event(int kind, const char *content,
                                          const char *privkey_hex, NostrTags *tags) {
    if (!privkey_hex || strlen(privkey_hex) != 64) {
        return NULL;
    }

    /* Derive pubkey from privkey */
    char *pubkey_hex = nostr_key_get_public(privkey_hex);
    if (!pubkey_hex) {
        return NULL;
    }

    NostrEvent *ev = nostr_test_make_signed_event_with_pubkey(
        kind, content, privkey_hex, pubkey_hex, tags, 0);

    free(pubkey_hex);
    return ev;
}

NostrEvent *nostr_test_make_signed_event_with_pubkey(int kind, const char *content,
                                                      const char *privkey_hex,
                                                      const char *pubkey_hex,
                                                      NostrTags *tags,
                                                      int64_t created_at) {
    if (!privkey_hex || strlen(privkey_hex) != 64) {
        return NULL;
    }
    if (!pubkey_hex || strlen(pubkey_hex) != 64) {
        return NULL;
    }

    NostrEvent *ev = nostr_event_new();
    if (!ev) return NULL;

    nostr_event_set_kind(ev, kind);
    nostr_event_set_content(ev, content ? content : "");
    nostr_event_set_pubkey(ev, pubkey_hex);
    nostr_event_set_created_at(ev, created_at > 0 ? created_at : nostr_test_get_timestamp());

    if (tags) {
        nostr_event_set_tags(ev, tags);
    }

    /* Sign the event */
    int rc = nostr_event_sign(ev, privkey_hex);
    if (rc != 0) {
        nostr_event_free(ev);
        return NULL;
    }

    return ev;
}

/* ============================================================================
 * Batch Event Generation
 * ============================================================================
 */

NostrEvent **nostr_test_generate_events(size_t count, int kind,
                                         const char *pubkey_hex,
                                         int64_t time_start, int64_t time_step) {
    if (count == 0) return NULL;

    NostrEvent **events = calloc(count, sizeof(NostrEvent *));
    if (!events) return NULL;

    for (size_t i = 0; i < count; i++) {
        char content[128];
        snprintf(content, sizeof(content), "Test event %zu", i);

        int ev_kind = (kind >= 0) ? kind : (int)(i % 11); /* kinds 0-10 */
        int64_t ev_time = time_start + (int64_t)(i * time_step);

        events[i] = nostr_event_new();
        if (!events[i]) {
            /* Cleanup on failure */
            for (size_t j = 0; j < i; j++) {
                nostr_event_free(events[j]);
            }
            free(events);
            return NULL;
        }

        nostr_event_set_kind(events[i], ev_kind);
        nostr_event_set_content(events[i], content);
        nostr_event_set_created_at(events[i], ev_time);

        if (pubkey_hex) {
            nostr_event_set_pubkey(events[i], pubkey_hex);
        }
    }

    return events;
}

NostrEvent **nostr_test_generate_signed_events(size_t count, int kind,
                                                const NostrTestKeypair *kp,
                                                int64_t time_start, int64_t time_step) {
    if (count == 0 || !kp) return NULL;

    NostrEvent **events = calloc(count, sizeof(NostrEvent *));
    if (!events) return NULL;

    for (size_t i = 0; i < count; i++) {
        char content[128];
        snprintf(content, sizeof(content), "Signed test event %zu", i);

        int ev_kind = (kind >= 0) ? kind : (int)(i % 11);
        int64_t ev_time = time_start + (int64_t)(i * time_step);

        events[i] = nostr_test_make_signed_event_with_pubkey(
            ev_kind, content, kp->privkey_hex, kp->pubkey_hex, NULL, ev_time);

        if (!events[i]) {
            /* Cleanup on failure */
            for (size_t j = 0; j < i; j++) {
                nostr_event_free(events[j]);
            }
            free(events);
            return NULL;
        }
    }

    return events;
}

void nostr_test_free_events(NostrEvent **events, size_t count) {
    if (!events) return;
    for (size_t i = 0; i < count; i++) {
        if (events[i]) {
            nostr_event_free(events[i]);
        }
    }
    free(events);
}

/* ============================================================================
 * Assertion Functions
 * ============================================================================
 */

void nostr_test_assert_event_matches(const NostrEvent *event, const NostrFilter *filter,
                                      const char *file, int line) {
    if (!event) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Event is NULL\n", file, line);
        exit(1);
    }
    if (!filter) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Filter is NULL\n", file, line);
        exit(1);
    }

    /* Use the filter matching function */
    bool matches = nostr_filter_matches((NostrFilter *)filter, (NostrEvent *)event);
    if (!matches) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Event does not match filter\n", file, line);
        fprintf(stderr, "  Event kind: %d, pubkey: %.16s...\n",
                event->kind, event->pubkey ? event->pubkey : "(null)");
        exit(1);
    }
}

void nostr_test_assert_event_not_matches(const NostrEvent *event, const NostrFilter *filter,
                                          const char *file, int line) {
    if (!event) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Event is NULL\n", file, line);
        exit(1);
    }
    if (!filter) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Filter is NULL\n", file, line);
        exit(1);
    }

    bool matches = nostr_filter_matches((NostrFilter *)filter, (NostrEvent *)event);
    if (matches) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Event unexpectedly matches filter\n", file, line);
        fprintf(stderr, "  Event kind: %d, pubkey: %.16s...\n",
                event->kind, event->pubkey ? event->pubkey : "(null)");
        exit(1);
    }
}

void nostr_test_assert_event_equals(const NostrEvent *a, const NostrEvent *b,
                                     const char *file, int line) {
    if (!a || !b) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: One or both events are NULL (a=%p, b=%p)\n",
                file, line, (void*)a, (void*)b);
        exit(1);
    }

    /* Compare kind */
    if (a->kind != b->kind) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Event kinds differ (a=%d, b=%d)\n",
                file, line, a->kind, b->kind);
        exit(1);
    }

    /* Compare created_at */
    if (a->created_at != b->created_at) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Event timestamps differ (a=%lld, b=%lld)\n",
                file, line, (long long)a->created_at, (long long)b->created_at);
        exit(1);
    }

    /* Compare pubkey */
    if ((a->pubkey == NULL) != (b->pubkey == NULL) ||
        (a->pubkey && b->pubkey && strcmp(a->pubkey, b->pubkey) != 0)) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Event pubkeys differ\n  a: %s\n  b: %s\n",
                file, line, a->pubkey ? a->pubkey : "(null)", b->pubkey ? b->pubkey : "(null)");
        exit(1);
    }

    /* Compare content */
    if ((a->content == NULL) != (b->content == NULL) ||
        (a->content && b->content && strcmp(a->content, b->content) != 0)) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Event contents differ\n  a: %.50s\n  b: %.50s\n",
                file, line, a->content ? a->content : "(null)", b->content ? b->content : "(null)");
        exit(1);
    }

    /* Compare id if both have one */
    if (a->id && b->id && strcmp(a->id, b->id) != 0) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Event IDs differ\n  a: %s\n  b: %s\n",
                file, line, a->id, b->id);
        exit(1);
    }
}

void nostr_test_assert_signature_valid(const NostrEvent *event, const char *file, int line) {
    if (!event) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Event is NULL\n", file, line);
        exit(1);
    }

    bool valid = nostr_event_check_signature((NostrEvent *)event);
    if (!valid) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Event signature is invalid\n", file, line);
        fprintf(stderr, "  Event id: %s\n", event->id ? event->id : "(null)");
        fprintf(stderr, "  Pubkey: %s\n", event->pubkey ? event->pubkey : "(null)");
        fprintf(stderr, "  Sig: %.32s...\n", event->sig ? event->sig : "(null)");
        exit(1);
    }
}

void nostr_test_assert_tag_exists(const NostrEvent *event, const char *key,
                                   const char *value, const char *file, int line) {
    if (!event) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Event is NULL\n", file, line);
        exit(1);
    }
    if (!key) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Tag key is NULL\n", file, line);
        exit(1);
    }

    NostrTags *tags = event->tags;
    if (!tags) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Event has no tags, expected tag [%s, %s]\n",
                file, line, key, value ? value : "*");
        exit(1);
    }

    bool found = false;
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 1) continue;

        const char *tag_key = nostr_tag_get(tag, 0);
        if (!tag_key || strcmp(tag_key, key) != 0) continue;

        if (value == NULL) {
            /* Match any value */
            found = true;
            break;
        }

        if (nostr_tag_size(tag) >= 2) {
            const char *tag_value = nostr_tag_get(tag, 1);
            if (tag_value && strcmp(tag_value, value) == 0) {
                found = true;
                break;
            }
        }
    }

    if (!found) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Tag [%s, %s] not found in event\n",
                file, line, key, value ? value : "*");
        exit(1);
    }
}

void nostr_test_assert_tag_not_exists(const NostrEvent *event, const char *key,
                                       const char *value, const char *file, int line) {
    if (!event) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Event is NULL\n", file, line);
        exit(1);
    }
    if (!key) {
        fprintf(stderr, "ASSERTION FAILED at %s:%d: Tag key is NULL\n", file, line);
        exit(1);
    }

    NostrTags *tags = event->tags;
    if (!tags) {
        /* No tags means tag doesn't exist - success */
        return;
    }

    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 1) continue;

        const char *tag_key = nostr_tag_get(tag, 0);
        if (!tag_key || strcmp(tag_key, key) != 0) continue;

        if (value == NULL) {
            /* Any tag with this key is a failure */
            fprintf(stderr, "ASSERTION FAILED at %s:%d: Tag [%s, *] unexpectedly found\n",
                    file, line, key);
            exit(1);
        }

        if (nostr_tag_size(tag) >= 2) {
            const char *tag_value = nostr_tag_get(tag, 1);
            if (tag_value && strcmp(tag_value, value) == 0) {
                fprintf(stderr, "ASSERTION FAILED at %s:%d: Tag [%s, %s] unexpectedly found\n",
                        file, line, key, value);
                exit(1);
            }
        }
    }
}

/* ============================================================================
 * Timing Utilities
 * ============================================================================
 */

bool nostr_test_wait_condition(NostrTestCondition check, void *ctx, int timeout_ms) {
    if (!check) return false;

    int elapsed = 0;
    const int sleep_interval_ms = 10;

    while (elapsed < timeout_ms) {
        if (check(ctx)) {
            return true;
        }
        usleep(sleep_interval_ms * 1000);
        elapsed += sleep_interval_ms;
    }

    return check(ctx); /* Final check */
}

/* ============================================================================
 * Fixture Loading
 * ============================================================================
 */

char *nostr_test_fixture_path(const char *filename) {
    if (!filename) return NULL;

    const char *fixtures_dir = getenv("NOSTR_TEST_FIXTURES_DIR");
    if (!fixtures_dir) {
        /* Default to compile-time path or relative path */
#ifdef NOSTR_TEST_FIXTURES_DIR_DEFAULT
        fixtures_dir = NOSTR_TEST_FIXTURES_DIR_DEFAULT;
#else
        fixtures_dir = "testing/fixtures";
#endif
    }

    size_t len = strlen(fixtures_dir) + 1 + strlen(filename) + 1;
    char *path = malloc(len);
    if (!path) return NULL;

    snprintf(path, len, "%s/%s", fixtures_dir, filename);
    return path;
}

NostrEvent **nostr_test_load_events_jsonl(const char *path, size_t *count) {
    if (!path || !count) return NULL;
    *count = 0;

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Failed to open fixture file: %s\n", path);
        return NULL;
    }

    /* First pass: count lines */
    size_t line_count = 0;
    char line[65536];
    while (fgets(line, sizeof(line), f)) {
        /* Skip empty lines and comments */
        size_t len = strlen(line);
        if (len == 0) continue;
        if (line[0] == '#' || line[0] == '\n') continue;
        line_count++;
    }

    if (line_count == 0) {
        fclose(f);
        return NULL;
    }

    /* Allocate array */
    NostrEvent **events = calloc(line_count, sizeof(NostrEvent *));
    if (!events) {
        fclose(f);
        return NULL;
    }

    /* Second pass: parse events */
    rewind(f);
    size_t idx = 0;
    while (fgets(line, sizeof(line), f) && idx < line_count) {
        size_t len = strlen(line);
        if (len == 0 || line[0] == '#' || line[0] == '\n') continue;

        /* Remove trailing newline */
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        NostrEvent *ev = nostr_event_new();
        if (!ev) continue;

        /* Parse JSON using compact deserializer */
        if (nostr_event_deserialize_compact(ev, line, NULL) != 1) {
            nostr_event_free(ev);
            continue;
        }

        events[idx++] = ev;
    }

    fclose(f);
    *count = idx;

    if (idx == 0) {
        free(events);
        return NULL;
    }

    return events;
}
