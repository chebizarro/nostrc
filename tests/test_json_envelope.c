#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "json.h"
#include "envelope.h"
#include "filter.h"
#include "nostr_jansson.h"

static void test_ok(void) {
    nostr_set_json_interface(jansson_impl);
    OKEnvelope ok = { .base = { .type = ENVELOPE_OK }, .event_id = NULL, .ok = false, .reason = NULL };
    const char *s = "[\"OK\",\"eid\",true,\"all good\"]";
    int rc = nostr_envelope_deserialize((Envelope*)&ok, s);
    assert(rc == 0);
    assert(ok.event_id && strcmp(ok.event_id, "eid") == 0);
    assert(ok.ok == true);
    assert(ok.reason && strcmp(ok.reason, "all good") == 0);
}

static void test_event_with_and_without_subid(void) {
    nostr_set_json_interface(jansson_impl);
    // Without sub id
    EventEnvelope e1 = { .base = { .type = ENVELOPE_EVENT }, .subscription_id = NULL, .event = NULL };
    const char *s1 = "[\"EVENT\",{\"kind\":1,\"created_at\":123,\"content\":\"c\"}]";
    assert(nostr_envelope_deserialize((Envelope*)&e1, s1) == 0);
    assert(e1.subscription_id == NULL);
    assert(e1.event && e1.event->kind == 1);
    // Free owned members only; e1 itself is on the stack
    free(e1.subscription_id);
    free_event(e1.event);

    // With sub id
    EventEnvelope e2 = { .base = { .type = ENVELOPE_EVENT }, .subscription_id = NULL, .event = NULL };
    const char *s2 = "[\"EVENT\",\"subx\",{\"kind\":2,\"created_at\":456,\"content\":\"d\"}]";
    assert(nostr_envelope_deserialize((Envelope*)&e2, s2) == 0);
    assert(e2.subscription_id && strcmp(e2.subscription_id, "subx") == 0);
    assert(e2.event && e2.event->kind == 2);
    free(e2.subscription_id);
    free_event(e2.event);
}

static void test_count_without_count_defaults_zero(void) {
    nostr_set_json_interface(jansson_impl);
    CountEnvelope ct = { .base = { .type = ENVELOPE_COUNT }, .subscription_id = NULL, .filters = NULL, .count = 999 };
    const char *cnt = "[\"COUNT\",\"sub1\",{}, {\"authors\":[\"a\"]}]";
    assert(nostr_envelope_deserialize((Envelope*)&ct, cnt) == 0);
    assert(ct.subscription_id && strcmp(ct.subscription_id, "sub1") == 0);
    assert(ct.count == 0);
}

static void test_auth_with_event_object(void) {
    nostr_set_json_interface(jansson_impl);
    AuthEnvelope au = { .base = { .type = ENVELOPE_AUTH }, .challenge = NULL, .event = NULL };
    const char *s = "[\"AUTH\",{\"kind\":22242,\"created_at\":1700000000,\"content\":\"auth\"}]";
    int rc = nostr_envelope_deserialize((Envelope*)&au, s);
    assert(rc == 0);
    assert(au.event && au.event->kind == 22242);
}

static void test_closed(void) {
    nostr_set_json_interface(jansson_impl);
    ClosedEnvelope cl = { .base = { .type = ENVELOPE_CLOSED }, .subscription_id = NULL, .reason = NULL };
    const char *s = "[\"CLOSED\",\"sub\",\"bye\"]";
    int rc = nostr_envelope_deserialize((Envelope*)&cl, s);
    assert(rc == 0);
    assert(cl.subscription_id && strcmp(cl.subscription_id, "sub") == 0);
    assert(cl.reason && strcmp(cl.reason, "bye") == 0);
}

static void test_auth(void) {
    nostr_set_json_interface(jansson_impl);
    AuthEnvelope au = { .base = { .type = ENVELOPE_AUTH }, .challenge = NULL, .event = NULL };
    const char *s = "[\"AUTH\",\"challenge-token\"]";
    int rc = nostr_envelope_deserialize((Envelope*)&au, s);
    assert(rc == 0);
    assert(au.challenge && strcmp(au.challenge, "challenge-token") == 0);
}

static void test_notice_eose(void) {
    nostr_set_json_interface(jansson_impl);
    NoticeEnvelope ne = { .base = { .type = ENVELOPE_NOTICE }, .message = NULL };
    EOSEEnvelope ee = { .base = { .type = ENVELOPE_EOSE }, .message = NULL };
    assert(nostr_envelope_deserialize((Envelope*)&ne, "[\"NOTICE\",\"n\"]") == 0);
    assert(nostr_envelope_deserialize((Envelope*)&ee, "[\"EOSE\",\"done\"]") == 0);
    assert(ne.message && strcmp(ne.message, "n") == 0);
    assert(ee.message && strcmp(ee.message, "done") == 0);
}

// Minimal REQ with one filter
static void test_req_count(void) {
    nostr_set_json_interface(jansson_impl);
    ReqEnvelope rq = { .base = { .type = ENVELOPE_REQ }, .subscription_id = NULL, .filters = NULL };
    CountEnvelope ct = { .base = { .type = ENVELOPE_COUNT }, .subscription_id = NULL, .filters = NULL, .count = 0 };
    const char *req = "[\"REQ\",\"sub1\",{\"authors\":[\"a\"],\"kinds\":[1],\"limit\":2}]";
    const char *cnt = "[\"COUNT\",\"sub1\",{\"count\":5},{\"ids\":[\"x\"]}]";
    assert(nostr_envelope_deserialize((Envelope*)&rq, req) == 0);
    assert(rq.subscription_id && strcmp(rq.subscription_id, "sub1") == 0);
    assert(rq.filters && rq.filters->count >= 1);
    assert(nostr_envelope_deserialize((Envelope*)&ct, cnt) == 0);
    assert(ct.subscription_id && strcmp(ct.subscription_id, "sub1") == 0);
    assert(ct.count == 5);
    assert(ct.filters && ct.filters->count >= 1);
}

int main(void) {
    nostr_json_init();
    test_ok();
    test_closed();
    test_auth();
    test_notice_eose();
    test_req_count();
    test_event_with_and_without_subid();
    test_count_without_count_defaults_zero();
    test_auth_with_event_object();
    nostr_json_cleanup();
    printf("test_json_envelope OK\n");
    return 0;
}
