/* Regression tests for Item A libnostr parsing/serialization audit fixes.
 *
 * Covers:
 *   nostrc-s1d  compact event/envelope parsers reject malformed/truncated input
 *   nostrc-d12  event and relay response serializers JSON-escape string fields
 *   nostrc-0wd  filter compact parser/serializer handles JSON escapes safely
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-envelope.h"
#include "nostr-filter.h"
#include "nostr-relay-core.h"

static const char *full_event_json(void) {
    return "{\"id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
           "\"pubkey\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
           "\"created_at\":1,\"kind\":1,\"tags\":[],\"content\":\"ok\","
           "\"sig\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
                    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"}";
}

static void test_filter_escape_roundtrip(void) {
    const char *expect_search = "line\ncol\t" "\xE2\x98\x83" " " "\xF0\x9F\x98\x80";
    const char *expect_tag = "tag\n" "\xE2\x98\x83";
    const char *json = "{\"search\":\"line\\ncol\\t\\u2603 \\uD83D\\uDE00\","
                       "\"#e\":[\"tag\\n\\u2603\"],\"kinds\":[1,2]}";

    NostrFilter *f = nostr_filter_new();
    assert(f != NULL);
    assert(nostr_filter_deserialize_compact(f, json, NULL) == 1);
    assert(f->search != NULL);
    assert(strcmp(f->search, expect_search) == 0);
    assert(nostr_filter_tags_len(f) == 1);
    assert(strcmp(nostr_filter_tag_get(f, 0, 1), expect_tag) == 0);

    char *out = nostr_filter_serialize_compact(f);
    assert(out != NULL);
    assert(strchr(out, '\n') == NULL);
    assert(strchr(out, '\t') == NULL);
    assert(strstr(out, "\\n") != NULL);
    assert(strstr(out, "\\t") != NULL);

    NostrFilter *round = nostr_filter_new();
    assert(round != NULL);
    assert(nostr_filter_deserialize_compact(round, out, NULL) == 1);
    assert(round->search != NULL);
    assert(strcmp(round->search, expect_search) == 0);
    assert(nostr_filter_tags_len(round) == 1);
    assert(strcmp(nostr_filter_tag_get(round, 0, 1), expect_tag) == 0);

    NostrFilter *bad = nostr_filter_new();
    assert(bad != NULL);
    assert(nostr_filter_deserialize_compact(bad, "{\"kinds\":[2147483648]}", NULL) == 0);
    assert(nostr_filter_deserialize_compact(bad, "{\"kinds\":[-2147483649]}", NULL) == 0);
    assert(nostr_filter_deserialize_compact(bad, "{\"kinds\":[+1]}", NULL) == 0);
    assert(nostr_filter_deserialize_compact(bad, "{\"kinds\":[01]}", NULL) == 0);

    nostr_filter_free(bad);
    nostr_filter_free(round);
    free(out);
    nostr_filter_free(f);
    printf("  [ok] filter escape round-trip and kind bounds\n");
}

static void test_event_parser_rejects_malformed(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev != NULL);
    assert(nostr_event_deserialize_compact(ev, "{}", NULL) == 0);
    assert(nostr_event_deserialize_compact(ev, "{\"kind\":1,\"created_at\":1}", NULL) == 0);
    assert(nostr_event_deserialize_compact(ev,
        "{\"id\":\"a\",\"pubkey\":\"b\",\"created_at\":1,\"kind\":1,\"content\":\"ok\",\"sig\":\"c\"}", NULL) == 0);
    assert(nostr_event_deserialize_compact(ev,
        "{\"id\":\"a\",\"pubkey\":\"b\",\"created_at\":1,\"kind\":1,\"tags\":[],\"sig\":\"c\"}", NULL) == 0);
    assert(nostr_event_deserialize_compact(ev,
        "{\"id\":\"a\",\"pubkey\":\"b\",\"created_at\":1,\"kind\":1,\"tags\":[],\"content\":\"ok\",\"sig\":\"c\",}", NULL) == 0);

    char valid_with_trailing[1024];
    snprintf(valid_with_trailing, sizeof valid_with_trailing, "%s junk", full_event_json());
    assert(nostr_event_deserialize_compact(ev, valid_with_trailing, NULL) == 0);

    char truncated[1024];
    snprintf(truncated, sizeof truncated, "%s", full_event_json());
    truncated[strlen(truncated) - 1] = '\0';
    assert(nostr_event_deserialize_compact(ev, truncated, NULL) == 0);

    assert(nostr_event_deserialize_compact(ev, full_event_json(), NULL) == 1);
    nostr_event_free(ev);
    printf("  [ok] event compact parser rejects missing/trailing/truncated input\n");
}

static NostrEnvelope *alloc_envelope(NostrEnvelopeType type) {
    NostrEnvelope *env = NULL;
    switch (type) {
    case NOSTR_ENVELOPE_EVENT:  env = calloc(1, sizeof(NostrEventEnvelope)); break;
    case NOSTR_ENVELOPE_REQ:    env = calloc(1, sizeof(NostrReqEnvelope)); break;
    case NOSTR_ENVELOPE_COUNT:  env = calloc(1, sizeof(NostrCountEnvelope)); break;
    case NOSTR_ENVELOPE_NOTICE: env = calloc(1, sizeof(NostrNoticeEnvelope)); break;
    case NOSTR_ENVELOPE_EOSE:   env = calloc(1, sizeof(NostrEOSEEnvelope)); break;
    case NOSTR_ENVELOPE_CLOSE:  env = calloc(1, sizeof(NostrCloseEnvelope)); break;
    case NOSTR_ENVELOPE_CLOSED: env = calloc(1, sizeof(NostrClosedEnvelope)); break;
    case NOSTR_ENVELOPE_OK:     env = calloc(1, sizeof(NostrOKEnvelope)); break;
    case NOSTR_ENVELOPE_AUTH:   env = calloc(1, sizeof(NostrAuthEnvelope)); break;
    default: break;
    }
    assert(env != NULL);
    env->type = type;
    return env;
}

static void assert_envelope_rejects(NostrEnvelopeType type, const char *json) {
    NostrEnvelope *env = alloc_envelope(type);
    assert(nostr_envelope_deserialize_compact(env, json, NULL) == 0);
    nostr_envelope_free(env);
}

static void test_envelope_parser_rejects_malformed(void) {
    char event_missing_close[1200];
    snprintf(event_missing_close, sizeof event_missing_close, "[\"EVENT\",\"sub\",%s", full_event_json());

    assert_envelope_rejects(NOSTR_ENVELOPE_EVENT, event_missing_close);
    assert_envelope_rejects(NOSTR_ENVELOPE_REQ, "[\"REQ\",\"sub\",{},BROKEN");
    assert_envelope_rejects(NOSTR_ENVELOPE_REQ, "[\"REQ\",\"sub\",]");
    assert_envelope_rejects(NOSTR_ENVELOPE_COUNT, "[\"COUNT\",\"sub\",{\"count\":1},JUNK");
    assert_envelope_rejects(NOSTR_ENVELOPE_COUNT, "[\"COUNT\",\"sub\",]");
    assert_envelope_rejects(NOSTR_ENVELOPE_OK, "[\"OK\",\"id\"]");
    assert_envelope_rejects(NOSTR_ENVELOPE_OK, "[\"OK\",\"id\",true,\"\",JUNK");
    assert_envelope_rejects(NOSTR_ENVELOPE_NOTICE, "[\"NOTICE\",\"msg\",JUNK");
    assert_envelope_rejects(NOSTR_ENVELOPE_EOSE, "[\"EOSE\",\"sub\"");
    assert_envelope_rejects(NOSTR_ENVELOPE_CLOSE, "[\"CLOSE\",\"sub\",");
    assert_envelope_rejects(NOSTR_ENVELOPE_CLOSED, "[\"CLOSED\",\"sub\",\"reason\",JUNK");
    assert_envelope_rejects(NOSTR_ENVELOPE_AUTH, "[\"AUTH\",\"challenge\",]");

    NostrEnvelope *ok = alloc_envelope(NOSTR_ENVELOPE_OK);
    assert(nostr_envelope_deserialize_compact(ok, "[\"OK\",\"id\",true,\"fine\"]", NULL) == 1);
    nostr_envelope_free(ok);
    printf("  [ok] envelope compact parser rejects malformed frames\n");
}

static void test_serializers_escape_json_strings(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev != NULL);
    ev->id = strdup("id\"x");
    ev->pubkey = strdup("pk\\y");
    ev->sig = strdup("sig\nz");
    ev->created_at = 123;
    ev->kind = 1;
    ev->content = strdup("content");
    assert(ev->id && ev->pubkey && ev->sig && ev->content);

    char *event_json = nostr_event_serialize_compact(ev);
    assert(event_json != NULL);
    assert(strstr(event_json, "id\\\"x") != NULL);
    assert(strstr(event_json, "pk\\\\y") != NULL);
    assert(strstr(event_json, "sig\\nz") != NULL);
    assert(strchr(event_json, '\n') == NULL);

    NostrEvent *parsed = nostr_event_new();
    assert(parsed != NULL);
    assert(nostr_event_deserialize_compact(parsed, event_json, NULL) == 1);
    assert(strcmp(parsed->id, "id\"x") == 0);
    assert(strcmp(parsed->pubkey, "pk\\y") == 0);
    assert(strcmp(parsed->sig, "sig\nz") == 0);

    char *ok_json = nostr_ok_build_json("id\"x", 0, "blocked \"bad\"\nreason");
    assert(ok_json != NULL);
    assert(strstr(ok_json, "id\\\"x") != NULL);
    assert(strstr(ok_json, "blocked \\\"bad\\\"\\nreason") != NULL);
    NostrEnvelope *ok = alloc_envelope(NOSTR_ENVELOPE_OK);
    assert(nostr_envelope_deserialize_compact(ok, ok_json, NULL) == 1);
    assert(strcmp(((NostrOKEnvelope *)ok)->event_id, "id\"x") == 0);
    assert(strcmp(((NostrOKEnvelope *)ok)->reason, "blocked \"bad\"\nreason") == 0);

    char *closed_json = nostr_closed_build_json("sub\"1", "closed\nwhy");
    assert(closed_json != NULL);
    NostrEnvelope *closed = alloc_envelope(NOSTR_ENVELOPE_CLOSED);
    assert(nostr_envelope_deserialize_compact(closed, closed_json, NULL) == 1);
    assert(strcmp(((NostrClosedEnvelope *)closed)->subscription_id, "sub\"1") == 0);
    assert(strcmp(((NostrClosedEnvelope *)closed)->reason, "closed\nwhy") == 0);

    char *eose_json = nostr_eose_build_json("sub\"2");
    assert(eose_json != NULL);
    NostrEnvelope *eose = alloc_envelope(NOSTR_ENVELOPE_EOSE);
    assert(nostr_envelope_deserialize_compact(eose, eose_json, NULL) == 1);
    assert(strcmp(((NostrEOSEEnvelope *)eose)->message, "sub\"2") == 0);

    nostr_envelope_free(eose);
    free(eose_json);
    nostr_envelope_free(closed);
    free(closed_json);
    nostr_envelope_free(ok);
    free(ok_json);
    nostr_event_free(parsed);
    free(event_json);
    nostr_event_free(ev);
    printf("  [ok] event and relay response serializers escape JSON strings\n");
}

int main(void) {
    printf("libnostr parsing audit regression tests:\n");
    test_filter_escape_roundtrip();
    test_event_parser_rejects_malformed();
    test_envelope_parser_rejects_malformed();
    test_serializers_escape_json_strings();
    printf("All libnostr parsing audit tests passed.\n");
    return 0;
}
