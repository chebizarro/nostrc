/* Regression test for nostrc-pnc7: an explicit NIP-01 "limit":0 ("no stored
 * events, send EOSE now") must be distinguishable from an absent limit.
 * The compact filter parser (used by nostr_filter_deserialize() and by
 * nostr_envelope_parse() for REQ frames, i.e. what relayd sees) never set
 * NostrFilter.limit_zero, so relayd ran an unbounded backfill instead.
 * The jansson backend path is covered in tests/test_json_filter.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "nostr-envelope.h"
#include "nostr-filter.h"

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #expr);                                                \
            abort();                                                       \
        }                                                                  \
    } while (0)

static NostrFilter *parse(const char *json) {
    NostrFilter *f = nostr_filter_new();
    CHECK(f != NULL);
    CHECK(nostr_filter_deserialize(f, json) == 0);
    return f;
}

static void test_parse(void) {
    NostrFilter *zero = parse("{\"kinds\":[1],\"limit\":0}");
    CHECK(nostr_filter_get_limit(zero) == 0);
    CHECK(nostr_filter_get_limit_zero(zero));
    nostr_filter_free(zero);

    NostrFilter *absent = parse("{\"kinds\":[1]}");
    CHECK(nostr_filter_get_limit(absent) == 0);
    CHECK(!nostr_filter_get_limit_zero(absent));
    nostr_filter_free(absent);

    NostrFilter *five = parse("{\"limit\":5,\"kinds\":[1]}");
    CHECK(nostr_filter_get_limit(five) == 5);
    CHECK(!nostr_filter_get_limit_zero(five));
    nostr_filter_free(five);
    printf("  [ok] limit:0 sets limit_zero; absent and positive limits do not\n");
}

static void test_roundtrip(void) {
    NostrFilter *zero = parse("{\"kinds\":[1],\"limit\":0}");
    char *s = nostr_filter_serialize(zero);
    CHECK(s != NULL);
    CHECK(strstr(s, "\"limit\":0") != NULL);
    NostrFilter *back = parse(s);
    CHECK(nostr_filter_get_limit_zero(back));
    free(s);
    nostr_filter_free(back);
    nostr_filter_free(zero);
    printf("  [ok] limit:0 survives serialize -> parse\n");
}

static void test_req_envelope(void) {
    NostrEnvelope *env = nostr_envelope_parse(
        "[\"REQ\",\"s1\",{\"kinds\":[1],\"limit\":0},{\"kinds\":[7]}]");
    CHECK(env != NULL);
    CHECK(env->type == NOSTR_ENVELOPE_REQ);
    NostrFilters *fs = nostr_req_envelope_get_filters((NostrReqEnvelope *)env);
    CHECK(fs != NULL && fs->count == 2);
    CHECK(nostr_filter_get_limit_zero(&fs->filters[0]));
    CHECK(!nostr_filter_get_limit_zero(&fs->filters[1]));
    nostr_envelope_free(env);
    printf("  [ok] REQ envelope: per-filter limit_zero\n");
}

int main(void) {
    printf("test_filter_limit_zero (nostrc-pnc7)\n");
    test_parse();
    test_roundtrip();
    test_req_envelope();
    printf("test_filter_limit_zero: OK\n");
    return 0;
}
