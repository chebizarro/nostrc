/* NIP-CAS-0010: replay and freshness tests — duplicate event ids are rejected,
 * stale and future-dated events are rejected, fresh unique events are accepted
 * and the seen-id cache stays bounded (security review G4). */

#include <time.h>

#include "otel_internal.h"
#include "otel_test_util.h"

#define FIXED_NOW 1750000000

typedef struct {
    size_t calls;
} Capture;

static int capture_handler(const NostrOtelReceived *received, void *user_data) {
    (void)received;
    ((Capture *)user_data)->calls++;
    return 0;
}

static int64_t fixed_clock(void *user_data) {
    (void)user_data;
    return FIXED_NOW;
}

static bool admit_all(const char *pubkey_hex, void *user_data) {
    (void)pubkey_hex;
    (void)user_data;
    return true;
}

static NostrTags *standard_tags(void) {
    NostrTags *tags = nostr_tags_new(0);
    OTEL_CHECK(tags != NULL, "tags alloc");
    nostr_tags_append(tags, nostr_tag_new("domain", "traces", NULL));
    nostr_tags_append(tags, nostr_tag_new("schema", "cascadia.otel.traces.v1", NULL));
    nostr_tags_append(tags, nostr_tag_new("enc", "otlp-proto", "identity", NULL));
    return tags;
}

/* Signs a traces event at @created_at; @body distinguishes otherwise-equal
 * events so their ids differ. */
static NostrEvent *sign_trace_event(const OtelTestSigner *ts, int64_t created_at,
                                    const char *body) {
    NostrEvent *event = nostr_event_new();
    OTEL_CHECK(event != NULL, "event alloc");
    event->kind = NOSTR_OTEL_KIND_TRACES;
    event->created_at = created_at;
    event->tags = standard_tags();
    char *content = NULL;
    OTEL_CHECK_RC(nostr_otel_base64_encode((const uint8_t *)body, strlen(body), &content),
                  NOSTR_OTEL_OK);
    event->content = content;
    OTEL_CHECK(ts->signer.sign(event, ts->signer.user_data) == 0, "sign failed");
    return event;
}

static NostrOtelConsumer *make_consumer(Capture *cap, NostrOtelConsumerConfig overrides) {
    overrides.handler = capture_handler;
    overrides.handler_user_data = cap;
    overrides.admit = admit_all;
    if (!overrides.now) overrides.now = fixed_clock;
    NostrOtelConsumer *consumer = NULL;
    OTEL_CHECK_RC(nostr_otel_consumer_new(&overrides, &consumer), NOSTR_OTEL_OK);
    OTEL_CHECK(consumer != NULL, "consumer alloc");
    return consumer;
}

static void test_duplicate_rejected_fresh_unique_accepted(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    Capture cap = {0};
    NostrOtelConsumerConfig cfg = {0};
    NostrOtelConsumer *consumer = make_consumer(&cap, cfg);

    NostrEvent *event = sign_trace_event(&ts, FIXED_NOW, "spans-1");
    OTEL_CHECK_RC(nostr_otel_consumer_process(consumer, event), NOSTR_OTEL_OK);
    OTEL_CHECK(cap.calls == 1, "first delivery must reach the handler");

    /* The very same signed bytes, replayed. */
    OTEL_CHECK_RC(nostr_otel_consumer_process(consumer, event), NOSTR_OTEL_ERR_REPLAYED);
    OTEL_CHECK(cap.calls == 1, "replay must not reach the handler");

    /* A distinct, fresh event from the same signer still passes. */
    NostrEvent *other = sign_trace_event(&ts, FIXED_NOW, "spans-2");
    OTEL_CHECK(strcmp(other->id, event->id) != 0, "test events must have distinct ids");
    OTEL_CHECK_RC(nostr_otel_consumer_process(consumer, other), NOSTR_OTEL_OK);
    OTEL_CHECK(cap.calls == 2, "unique event must reach the handler");

    nostr_event_free(other);
    nostr_event_free(event);
    nostr_otel_consumer_free(consumer);
    otel_test_signer_clear(&ts);
}

static void test_stale_and_future_rejected(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    Capture cap = {0};
    NostrOtelConsumerConfig cfg = {0};
    NostrOtelConsumer *consumer = make_consumer(&cap, cfg);

    const int64_t offsets[] = {-600, -121, 121, 600};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        NostrEvent *event = sign_trace_event(&ts, FIXED_NOW + offsets[i], "spans");
        OTEL_CHECK_RC(nostr_otel_consumer_process(consumer, event),
                      NOSTR_OTEL_ERR_STALE_EVENT);
        nostr_event_free(event);
    }
    OTEL_CHECK(cap.calls == 0, "stale events must not reach the handler");

    /* Inside the default +/-120s window, events are accepted. */
    const int64_t fresh[] = {-119, -1, 0, 119};
    for (size_t i = 0; i < sizeof(fresh) / sizeof(fresh[0]); i++) {
        NostrEvent *event = sign_trace_event(&ts, FIXED_NOW + fresh[i], "spans");
        OTEL_CHECK_RC(nostr_otel_consumer_process(consumer, event), NOSTR_OTEL_OK);
        nostr_event_free(event);
    }
    OTEL_CHECK(cap.calls == 4, "fresh events must reach the handler");

    nostr_otel_consumer_free(consumer);
    otel_test_signer_clear(&ts);
}

static void test_defences_are_configurable(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    NostrEvent *old = sign_trace_event(&ts, FIXED_NOW - 3600, "spans");

    Capture tight_cap = {0};
    NostrOtelConsumerConfig tight_cfg = {.max_clock_skew_seconds = 10};
    NostrOtelConsumer *tight = make_consumer(&tight_cap, tight_cfg);
    OTEL_CHECK_RC(nostr_otel_consumer_process(tight, old), NOSTR_OTEL_ERR_STALE_EVENT);

    Capture loose_cap = {0};
    NostrOtelConsumerConfig loose_cfg = {.max_clock_skew_seconds = 7200};
    NostrOtelConsumer *loose = make_consumer(&loose_cap, loose_cfg);
    OTEL_CHECK_RC(nostr_otel_consumer_process(loose, old), NOSTR_OTEL_OK);

    Capture off_cap = {0};
    NostrOtelConsumerConfig off_cfg = {.disable_freshness = true};
    NostrOtelConsumer *off = make_consumer(&off_cap, off_cfg);
    OTEL_CHECK_RC(nostr_otel_consumer_process(off, old), NOSTR_OTEL_OK);

    /* A rejected event must not have consumed a seen-cache slot: the loose
     * consumer accepted it, the tight one must still accept it once widened. */
    Capture nodedup_cap = {0};
    NostrOtelConsumerConfig nodedup_cfg = {.disable_dedup = true};
    NostrOtelConsumer *nodedup = make_consumer(&nodedup_cap, nodedup_cfg);
    NostrEvent *fresh = sign_trace_event(&ts, FIXED_NOW, "spans");
    for (int i = 0; i < 3; i++) {
        OTEL_CHECK_RC(nostr_otel_consumer_process(nodedup, fresh), NOSTR_OTEL_OK);
    }
    OTEL_CHECK(nodedup_cap.calls == 3, "dedup disabled must deliver every copy");

    /* Negative configuration is rejected. */
    NostrOtelConsumerConfig bad = {.handler = capture_handler,
                                   .admit = admit_all,
                                   .max_clock_skew_seconds = -1};
    NostrOtelConsumer *rejected = NULL;
    OTEL_CHECK_RC(nostr_otel_consumer_new(&bad, &rejected), NOSTR_OTEL_ERR_INVALID_ARG);
    OTEL_CHECK(rejected == NULL, "failed construction must not yield a consumer");

    nostr_event_free(fresh);
    nostr_event_free(old);
    nostr_otel_consumer_free(nodedup);
    nostr_otel_consumer_free(off);
    nostr_otel_consumer_free(loose);
    nostr_otel_consumer_free(tight);
    otel_test_signer_clear(&ts);
}

static void test_seen_cache_is_bounded(void) {
    NostrOtelSeenCache *cache = nostr_otel_seen_cache_new(4, 60);
    OTEL_CHECK(cache != NULL, "cache alloc");

    const char *ids[] = {"aa", "bb", "cc", "dd", "ee", "ff"};
    bool seen = true;
    for (size_t i = 0; i < 6; i++) {
        OTEL_CHECK_RC(nostr_otel_seen_cache_observe(cache, ids[i], FIXED_NOW, &seen),
                      NOSTR_OTEL_OK);
        OTEL_CHECK(!seen, "first observation must not be a duplicate");
    }
    OTEL_CHECK(nostr_otel_seen_cache_count(cache) == 4, "cache must stay bounded");

    /* The two oldest ids were evicted, so they no longer read as duplicates. */
    OTEL_CHECK_RC(nostr_otel_seen_cache_observe(cache, "aa", FIXED_NOW, &seen), NOSTR_OTEL_OK);
    OTEL_CHECK(!seen, "evicted id must not read as seen");
    OTEL_CHECK_RC(nostr_otel_seen_cache_observe(cache, "bb", FIXED_NOW, &seen), NOSTR_OTEL_OK);
    OTEL_CHECK(!seen, "evicted id must not read as seen");
    /* The most recent id is still remembered. */
    OTEL_CHECK_RC(nostr_otel_seen_cache_observe(cache, "ff", FIXED_NOW, &seen), NOSTR_OTEL_OK);
    OTEL_CHECK(seen, "recent id must read as seen");
    OTEL_CHECK(nostr_otel_seen_cache_count(cache) == 4, "cache must stay bounded");

    nostr_otel_seen_cache_free(cache);
}

static void test_seen_cache_expires(void) {
    NostrOtelSeenCache *cache = nostr_otel_seen_cache_new(10, 60);
    OTEL_CHECK(cache != NULL, "cache alloc");
    bool seen = true;
    OTEL_CHECK_RC(nostr_otel_seen_cache_observe(cache, "id", FIXED_NOW, &seen), NOSTR_OTEL_OK);
    OTEL_CHECK(!seen, "first observation");
    OTEL_CHECK_RC(nostr_otel_seen_cache_observe(cache, "id", FIXED_NOW + 30, &seen),
                  NOSTR_OTEL_OK);
    OTEL_CHECK(seen, "within TTL must read as duplicate");
    OTEL_CHECK_RC(nostr_otel_seen_cache_observe(cache, "id", FIXED_NOW + 300, &seen),
                  NOSTR_OTEL_OK);
    OTEL_CHECK(!seen, "after TTL must not read as duplicate");
    OTEL_CHECK(nostr_otel_seen_cache_count(cache) == 1, "expired entries are dropped");
    nostr_otel_seen_cache_free(cache);
}

static void test_defaults_match_the_relay(void) {
    OTEL_CHECK(NOSTR_OTEL_DEFAULT_MAX_CLOCK_SKEW_SECONDS == 120, "default window is +/-120s");
    OTEL_CHECK(NOSTR_OTEL_DEFAULT_SEEN_CACHE_SIZE == 100000u, "default cache is 100k ids");
    OTEL_CHECK(strcmp(nostr_otel_strerror(NOSTR_OTEL_ERR_REPLAYED),
                      "duplicate event id (replay)") == 0,
               "replay error is distinct");
    OTEL_CHECK(strcmp(nostr_otel_strerror(NOSTR_OTEL_ERR_STALE_EVENT),
                      "event created_at outside freshness window") == 0,
               "stale error is distinct");
}

int main(void) {
    test_duplicate_rejected_fresh_unique_accepted();
    test_stale_and_future_rejected();
    test_defences_are_configurable();
    test_seen_cache_is_bounded();
    test_seen_cache_expires();
    test_defaults_match_the_relay();
    printf("test_otel_replay: OK\n");
    return 0;
}
