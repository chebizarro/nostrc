#ifndef NOSTR_NIP_CAS0010_TEST_UTIL_H
#define NOSTR_NIP_CAS0010_TEST_UTIL_H

/* Shared helpers for the NIP-CAS-0010 tests. Every key used here is generated
 * fresh at run time — no private key is embedded anywhere in this module. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr-keys.h"
#include "nostr/nip_cas0010/otel.h"

#define OTEL_CHECK(cond, msg)                                                          \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));            \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

#define OTEL_CHECK_RC(rc, expected)                                                    \
    do {                                                                               \
        int otel_rc_ = (rc);                                                           \
        if (otel_rc_ != (expected)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: rc=%d (%s), expected %d\n", __FILE__,         \
                    __LINE__, otel_rc_, nostr_otel_strerror(otel_rc_), (expected));    \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

/* Ephemeral test signer: generates a keypair for this process only. */
typedef struct {
    char *privkey;
    NostrOtelLocalSigner *local;
    NostrOtelSigner signer;
} OtelTestSigner;

static inline void otel_test_signer_init(OtelTestSigner *ts) {
    ts->privkey = nostr_key_generate_private();
    OTEL_CHECK(ts->privkey != NULL, "key generation failed");
    ts->local = nostr_otel_local_signer_new(ts->privkey);
    OTEL_CHECK(ts->local != NULL, "local signer creation failed");
    OTEL_CHECK_RC(nostr_otel_local_signer_bind(ts->local, &ts->signer), NOSTR_OTEL_OK);
}

static inline void otel_test_signer_clear(OtelTestSigner *ts) {
    nostr_otel_local_signer_free(ts->local);
    if (ts->privkey) {
        memset(ts->privkey, 0, strlen(ts->privkey));
        free(ts->privkey);
    }
    ts->local = NULL;
    ts->privkey = NULL;
}

static inline const char *otel_test_signer_pubkey(const OtelTestSigner *ts) {
    return nostr_otel_local_signer_pubkey(ts->local);
}

/* A publish sink that keeps deep copies of the events it is handed. */
typedef struct {
    NostrEvent **events;
    size_t count;
    size_t capacity;
    int fail_next;
} OtelEventSink;

/* Deep-copies a signed event.
 *
 * This deliberately does not use nostr_event_copy(): its tag_clone() helper
 * seeds each cloned tag with the key and then re-appends every element, so the
 * copy carries a duplicated first element and no longer hashes to the signed
 * id. Tests need a copy that still verifies. */
static inline NostrEvent *otel_test_event_copy(const NostrEvent *src) {
    NostrEvent *copy = nostr_event_new();
    OTEL_CHECK(copy != NULL, "event alloc");
    copy->kind = src->kind;
    copy->created_at = src->created_at;
    copy->id = src->id ? strdup(src->id) : NULL;
    copy->pubkey = src->pubkey ? strdup(src->pubkey) : NULL;
    copy->sig = src->sig ? strdup(src->sig) : NULL;
    copy->content = src->content ? strdup(src->content) : NULL;
    copy->tags = nostr_tags_new(0);
    OTEL_CHECK(copy->tags != NULL, "tags alloc");
    size_t n = nostr_tags_size(src->tags);
    for (size_t i = 0; i < n; i++) {
        const NostrTag *tag = nostr_tags_get(src->tags, i);
        size_t m = nostr_tag_size(tag);
        NostrTag *dst = nostr_tag_new(nostr_tag_get(tag, 0), NULL);
        OTEL_CHECK(dst != NULL, "tag alloc");
        for (size_t k = 1; k < m; k++) {
            nostr_tag_append(dst, nostr_tag_get(tag, k));
        }
        nostr_tags_append(copy->tags, dst);
    }
    return copy;
}

static inline int otel_sink_publish(NostrEvent *event, void *user_data) {
    OtelEventSink *sink = (OtelEventSink *)user_data;
    if (sink->fail_next) {
        sink->fail_next = 0;
        return -1;
    }
    if (sink->count == sink->capacity) {
        size_t cap = sink->capacity ? sink->capacity * 2 : 8;
        NostrEvent **grown = realloc(sink->events, cap * sizeof(NostrEvent *));
        OTEL_CHECK(grown != NULL, "sink realloc failed");
        sink->events = grown;
        sink->capacity = cap;
    }
    NostrEvent *copy = otel_test_event_copy(event);
    sink->events[sink->count++] = copy;
    return 0;
}

static inline void otel_sink_clear(OtelEventSink *sink) {
    for (size_t i = 0; i < sink->count; i++) {
        nostr_event_free(sink->events[i]);
    }
    free(sink->events);
    sink->events = NULL;
    sink->count = 0;
    sink->capacity = 0;
}

/* Finds a tag by name; returns NULL when absent. */
static inline const NostrTag *otel_find_tag(const NostrTags *tags, const char *name) {
    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; i++) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 1) continue;
        const char *key = nostr_tag_get(tag, 0);
        if (key && strcmp(key, name) == 0) return tag;
    }
    return NULL;
}

static inline size_t otel_count_tags(const NostrTags *tags, const char *name) {
    size_t n = nostr_tags_size(tags);
    size_t found = 0;
    for (size_t i = 0; i < n; i++) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 1) continue;
        const char *key = nostr_tag_get(tag, 0);
        if (key && strcmp(key, name) == 0) found++;
    }
    return found;
}

#endif /* NOSTR_NIP_CAS0010_TEST_UTIL_H */
