/* Envelope serializer round-trip tests (nostrc-ptwq).
 *
 * Every envelope type goes through nostr_envelope_serialize_compact(), the
 * output is checked with an independent JSON parser (jansson, strict: one
 * top-level array, nothing after it), parsed back with the strict
 * nostr_envelope_parse(), and serialized again: the second frame must equal
 * the first. REQ and COUNT used to lose their closing ']' here. */
#undef NDEBUG
#include "json.h"
#include "nostr-envelope.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-tag.h"
#include "security_limits_runtime.h"
#include <jansson.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECRET "0000000000000000000000000000000000000000000000000000000000000003"
#define HEX_A  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define HEX_B  "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

/* Characters that need escaping, a control character (\u0001) and UTF-8. */
#define AWKWARD "q\"uote \\back\nnl\ttab\x01 caf\xc3\xa9 /"

static char *repeat(char c, size_t n) {
    char *s = malloc(n + 1);
    assert(s);
    memset(s, c, n);
    s[n] = '\0';
    return s;
}

/* The frame is exactly one JSON array whose first element is `label`. */
static void assert_json_frame(const char *frame, const char *label) {
    json_error_t err;
    json_t *root = json_loads(frame, 0, &err);
    if (!root) {
        fprintf(stderr, "invalid JSON (%s at %d): %.200s\n", err.text, err.position, frame);
        abort();
    }
    assert(json_is_array(root));
    assert(json_array_size(root) >= 2);
    json_t *first = json_array_get(root, 0);
    assert(json_is_string(first));
    assert(strcmp(json_string_value(first), label) == 0);
    json_decref(root);
}

/* Serialize, check, parse, reserialize. Returns the parsed envelope; the
 * caller checks its fields and frees it. */
static NostrEnvelope *roundtrip(const NostrEnvelope *env, const char *label, char **frame_out) {
    char *frame = nostr_envelope_serialize_compact(env);
    assert(frame);
    assert_json_frame(frame, label);
    size_t n = strlen(frame);
    assert(n >= 2 && frame[0] == '[' && frame[n - 1] == ']');

    /* The public entry point takes the same path. */
    char *pub = nostr_envelope_serialize(env);
    assert(pub && strcmp(pub, frame) == 0);
    free(pub);

    NostrEnvelope *parsed = nostr_envelope_parse(frame);
    if (!parsed) {
        fprintf(stderr, "nostr_envelope_parse rejected: %.200s\n", frame);
        abort();
    }
    assert(parsed->type == env->type);

    char *again = nostr_envelope_serialize_compact(parsed);
    assert(again);
    if (strcmp(again, frame) != 0) {
        fprintf(stderr, "not a fixed point:\n  %.200s\n  %.200s\n", frame, again);
        abort();
    }
    free(again);
    if (frame_out) *frame_out = frame; else free(frame);
    return parsed;
}

static NostrEvent *signed_event(int kind, const char *content) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, kind);
    nostr_event_set_created_at(ev, 1700000000);
    nostr_event_set_content(ev, content);
    assert(nostr_event_sign(ev, SECRET) == 0);
    return ev;
}

static void assert_filters_equal(const NostrFilters *a, const NostrFilters *b) {
    assert(a && b);
    assert(a->count == b->count);
    for (size_t i = 0; i < a->count; i++) {
        char *fa = nostr_filter_serialize_compact(&a->filters[i]);
        char *fb = nostr_filter_serialize_compact(&b->filters[i]);
        assert(fa && fb);
        assert(strcmp(fa, fb) == 0);
        free(fa);
        free(fb);
    }
}

static void add_filter(NostrFilters *fs, NostrFilter *f) {
    assert(nostr_filters_add(fs, f));
    nostr_filter_free(f);
}

/* Three filters that between them use most filter fields. */
static NostrFilters *several_filters(void) {
    NostrFilters *fs = nostr_filters_new();

    NostrFilter *f1 = nostr_filter_new();
    nostr_filter_add_kind(f1, 1);
    nostr_filter_add_kind(f1, 6);
    nostr_filter_add_author(f1, HEX_A);
    nostr_filter_set_limit(f1, 50);
    add_filter(fs, f1);

    NostrFilter *f2 = nostr_filter_new();
    nostr_filter_add_id(f2, HEX_B);
    nostr_filter_set_tags(f2, nostr_tags_new(1, nostr_tag_new("e", HEX_A, NULL)));
    nostr_filter_set_since(f2, 1600000000);
    nostr_filter_set_until(f2, 1700000000);
    add_filter(fs, f2);

    NostrFilter *f3 = nostr_filter_new();
    nostr_filter_add_kind(f3, 30023);
    nostr_filter_set_search(f3, AWKWARD);
    add_filter(fs, f3);
    return fs;
}

static NostrFilters *n_filters(size_t n) {
    NostrFilters *fs = nostr_filters_new();
    for (size_t i = 0; i < n; i++) {
        NostrFilter *f = nostr_filter_new();
        nostr_filter_add_kind(f, (int)i);
        add_filter(fs, f);
    }
    return fs;
}

/* ---- EVENT ---- */

static void test_event(void) {
    NostrEvent *ev = signed_event(1, AWKWARD);
    char *id = nostr_event_get_id(ev);
    assert(id);

    NostrEventEnvelope with = { .base = { NOSTR_ENVELOPE_EVENT }, .subscription_id = "sub-1", .event = ev };
    NostrEventEnvelope *p = (NostrEventEnvelope *)roundtrip(&with.base, "EVENT", NULL);
    assert(p->subscription_id && strcmp(p->subscription_id, "sub-1") == 0);
    char *pid = nostr_event_get_id(p->event);
    assert(pid && strcmp(pid, id) == 0);
    assert(strcmp(nostr_event_get_content(p->event), AWKWARD) == 0);
    free(pid);
    nostr_envelope_free(&p->base);

    NostrEventEnvelope without = { .base = { NOSTR_ENVELOPE_EVENT }, .subscription_id = NULL, .event = ev };
    p = (NostrEventEnvelope *)roundtrip(&without.base, "EVENT", NULL);
    assert(p->subscription_id == NULL);
    nostr_envelope_free(&p->base);

    free(id);
    nostr_event_free(ev);
}

/* ---- REQ ---- */

static void test_req_exact_frame(void) {
    NostrFilters *fs = nostr_filters_new();
    NostrFilter *f = nostr_filter_new();
    nostr_filter_add_kind(f, 1);
    add_filter(fs, f);
    NostrReqEnvelope req = { .base = { NOSTR_ENVELOPE_REQ }, .subscription_id = "sub", .filters = fs };
    char *frame = NULL;
    NostrEnvelope *p = roundtrip(&req.base, "REQ", &frame);
    assert(strcmp(frame, "[\"REQ\",\"sub\",{\"kinds\":[1]}]") == 0);
    free(frame);
    nostr_envelope_free(p);
    nostr_filters_free(fs);
}

static void test_req_several_filters(void) {
    NostrFilters *fs = several_filters();
    NostrReqEnvelope req = { .base = { NOSTR_ENVELOPE_REQ }, .subscription_id = AWKWARD, .filters = fs };
    NostrReqEnvelope *p = (NostrReqEnvelope *)roundtrip(&req.base, "REQ", NULL);
    assert(strcmp(p->subscription_id, AWKWARD) == 0);
    assert(p->filters->count == 3);
    assert_filters_equal(fs, p->filters);
    nostr_envelope_free(&p->base);
    nostr_filters_free(fs);
}

static void test_req_empty_filter_list(void) {
    NostrFilters *fs = nostr_filters_new();
    NostrReqEnvelope req = { .base = { NOSTR_ENVELOPE_REQ }, .subscription_id = "empty", .filters = fs };
    char *frame = NULL;
    NostrReqEnvelope *p = (NostrReqEnvelope *)roundtrip(&req.base, "REQ", &frame);
    assert(strcmp(frame, "[\"REQ\",\"empty\"]") == 0);
    assert(p->filters && p->filters->count == 0);
    free(frame);
    nostr_envelope_free(&p->base);
    nostr_filters_free(fs);

    /* No filter set at all is refused rather than guessed at. */
    NostrReqEnvelope none = { .base = { NOSTR_ENVELOPE_REQ }, .subscription_id = "x", .filters = NULL };
    assert(nostr_envelope_serialize_compact(&none.base) == NULL);
}

/* At the per-REQ filter limit every filter is kept; above it the frame is
 * trimmed to the limit and is still well formed. */
static void test_req_filter_limit(void) {
    size_t maxf = (size_t)nostr_limit_max_filters_per_req();
    for (size_t n = maxf - 1; n <= maxf + 1; n++) {
        NostrFilters *fs = n_filters(n);
        NostrReqEnvelope req = { .base = { NOSTR_ENVELOPE_REQ }, .subscription_id = "lim", .filters = fs };
        NostrReqEnvelope *p = (NostrReqEnvelope *)roundtrip(&req.base, "REQ", NULL);
        assert(p->filters->count == (n < maxf ? n : maxf));
        nostr_envelope_free(&p->base);

        NostrCountEnvelope cnt = { .base = { NOSTR_ENVELOPE_COUNT }, .subscription_id = "lim", .filters = fs, .count = 0 };
        NostrCountEnvelope *c = (NostrCountEnvelope *)roundtrip(&cnt.base, "COUNT", NULL);
        assert(c->filters->count == (n < maxf ? n : maxf));
        nostr_envelope_free(&c->base);
        nostr_filters_free(fs);
    }
}

/* ---- COUNT ---- */

static void test_count(void) {
    /* A relay's answer: ["COUNT",<id>,{"count":N}] */
    NostrCountEnvelope answer = { .base = { NOSTR_ENVELOPE_COUNT }, .subscription_id = "c1", .filters = NULL, .count = 42 };
    char *frame = NULL;
    NostrCountEnvelope *p = (NostrCountEnvelope *)roundtrip(&answer.base, "COUNT", &frame);
    assert(strcmp(frame, "[\"COUNT\",\"c1\",{\"count\":42}]") == 0);
    assert(p->count == 42);
    assert(p->filters && p->filters->count == 0);
    free(frame);
    nostr_envelope_free(&p->base);

    /* With filters. */
    NostrFilters *fs = several_filters();
    NostrCountEnvelope withf = { .base = { NOSTR_ENVELOPE_COUNT }, .subscription_id = AWKWARD, .filters = fs, .count = 7 };
    p = (NostrCountEnvelope *)roundtrip(&withf.base, "COUNT", NULL);
    assert(strcmp(p->subscription_id, AWKWARD) == 0);
    assert(p->count == 7);
    assert_filters_equal(fs, p->filters);
    nostr_envelope_free(&p->base);

    /* Empty filter list. */
    NostrFilters *none = nostr_filters_new();
    NostrCountEnvelope empty = { .base = { NOSTR_ENVELOPE_COUNT }, .subscription_id = "c2", .filters = none, .count = 0 };
    p = (NostrCountEnvelope *)roundtrip(&empty.base, "COUNT", NULL);
    assert(p->count == 0 && p->filters->count == 0);
    nostr_envelope_free(&p->base);
    nostr_filters_free(none);
    nostr_filters_free(fs);
}

/* ---- the string-only envelopes ---- */

static void test_close_eose_notice(void) {
    NostrCloseEnvelope cl = { .base = { NOSTR_ENVELOPE_CLOSE }, .message = AWKWARD };
    NostrCloseEnvelope *c = (NostrCloseEnvelope *)roundtrip(&cl.base, "CLOSE", NULL);
    assert(strcmp(c->message, AWKWARD) == 0);
    nostr_envelope_free(&c->base);

    NostrEOSEEnvelope eo = { .base = { NOSTR_ENVELOPE_EOSE }, .message = "sub-eose" };
    NostrEOSEEnvelope *e = (NostrEOSEEnvelope *)roundtrip(&eo.base, "EOSE", NULL);
    assert(strcmp(e->message, "sub-eose") == 0);
    nostr_envelope_free(&e->base);

    NostrNoticeEnvelope no = { .base = { NOSTR_ENVELOPE_NOTICE }, .message = AWKWARD };
    NostrNoticeEnvelope *n = (NostrNoticeEnvelope *)roundtrip(&no.base, "NOTICE", NULL);
    assert(strcmp(n->message, AWKWARD) == 0);
    nostr_envelope_free(&n->base);
}

static void test_ok(void) {
    NostrOKEnvelope yes = { .base = { NOSTR_ENVELOPE_OK }, .event_id = HEX_A, .ok = true, .reason = "" };
    NostrOKEnvelope *p = (NostrOKEnvelope *)roundtrip(&yes.base, "OK", NULL);
    assert(strcmp(p->event_id, HEX_A) == 0 && p->ok && p->reason && p->reason[0] == '\0');
    nostr_envelope_free(&p->base);

    NostrOKEnvelope no = { .base = { NOSTR_ENVELOPE_OK }, .event_id = HEX_B, .ok = false, .reason = "blocked: " AWKWARD };
    p = (NostrOKEnvelope *)roundtrip(&no.base, "OK", NULL);
    assert(!p->ok && strcmp(p->reason, "blocked: " AWKWARD) == 0);
    nostr_envelope_free(&p->base);

    NostrOKEnvelope bare = { .base = { NOSTR_ENVELOPE_OK }, .event_id = HEX_B, .ok = true, .reason = NULL };
    p = (NostrOKEnvelope *)roundtrip(&bare.base, "OK", NULL);
    assert(p->ok && p->reason == NULL);
    nostr_envelope_free(&p->base);
}

static void test_closed(void) {
    NostrClosedEnvelope cl = { .base = { NOSTR_ENVELOPE_CLOSED }, .subscription_id = "sub-x", .reason = "auth-required: " AWKWARD };
    NostrClosedEnvelope *p = (NostrClosedEnvelope *)roundtrip(&cl.base, "CLOSED", NULL);
    assert(strcmp(p->subscription_id, "sub-x") == 0);
    assert(strcmp(p->reason, "auth-required: " AWKWARD) == 0);
    nostr_envelope_free(&p->base);
}

static void test_auth(void) {
    NostrAuthEnvelope ch = { .base = { NOSTR_ENVELOPE_AUTH }, .challenge = "challenge-" AWKWARD, .event = NULL };
    NostrAuthEnvelope *p = (NostrAuthEnvelope *)roundtrip(&ch.base, "AUTH", NULL);
    assert(strcmp(p->challenge, "challenge-" AWKWARD) == 0 && p->event == NULL);
    nostr_envelope_free(&p->base);

    NostrEvent *ev = signed_event(22242, "");
    NostrAuthEnvelope au = { .base = { NOSTR_ENVELOPE_AUTH }, .challenge = NULL, .event = ev };
    p = (NostrAuthEnvelope *)roundtrip(&au.base, "AUTH", NULL);
    assert(p->event && nostr_event_get_kind(p->event) == 22242);
    nostr_envelope_free(&p->base);
    nostr_event_free(ev);
}

/* ---- long ids and buffer-size boundaries ---- */

/* One of every envelope that carries `sid`, round-tripped, checking the id
 * comes back intact. */
static void roundtrip_all_with_sid(const char *sid, NostrFilters *fs, NostrEvent *ev) {
    NostrReqEnvelope req = { .base = { NOSTR_ENVELOPE_REQ }, .subscription_id = (char *)sid, .filters = fs };
    NostrReqEnvelope *r = (NostrReqEnvelope *)roundtrip(&req.base, "REQ", NULL);
    assert(strcmp(r->subscription_id, sid) == 0);
    nostr_envelope_free(&r->base);

    NostrCountEnvelope cnt = { .base = { NOSTR_ENVELOPE_COUNT }, .subscription_id = (char *)sid, .filters = fs, .count = 3 };
    NostrCountEnvelope *c = (NostrCountEnvelope *)roundtrip(&cnt.base, "COUNT", NULL);
    assert(strcmp(c->subscription_id, sid) == 0 && c->count == 3);
    nostr_envelope_free(&c->base);

    NostrEventEnvelope evt = { .base = { NOSTR_ENVELOPE_EVENT }, .subscription_id = (char *)sid, .event = ev };
    NostrEventEnvelope *e = (NostrEventEnvelope *)roundtrip(&evt.base, "EVENT", NULL);
    assert(strcmp(e->subscription_id, sid) == 0);
    nostr_envelope_free(&e->base);

    NostrCloseEnvelope cl = { .base = { NOSTR_ENVELOPE_CLOSE }, .message = (char *)sid };
    NostrCloseEnvelope *x = (NostrCloseEnvelope *)roundtrip(&cl.base, "CLOSE", NULL);
    assert(strcmp(x->message, sid) == 0);
    nostr_envelope_free(&x->base);

    NostrEOSEEnvelope eo = { .base = { NOSTR_ENVELOPE_EOSE }, .message = (char *)sid };
    NostrEOSEEnvelope *o = (NostrEOSEEnvelope *)roundtrip(&eo.base, "EOSE", NULL);
    assert(strcmp(o->message, sid) == 0);
    nostr_envelope_free(&o->base);

    NostrClosedEnvelope cd = { .base = { NOSTR_ENVELOPE_CLOSED }, .subscription_id = (char *)sid, .reason = (char *)sid };
    NostrClosedEnvelope *d = (NostrClosedEnvelope *)roundtrip(&cd.base, "CLOSED", NULL);
    assert(strcmp(d->subscription_id, sid) == 0 && strcmp(d->reason, sid) == 0);
    nostr_envelope_free(&d->base);

    NostrNoticeEnvelope no = { .base = { NOSTR_ENVELOPE_NOTICE }, .message = (char *)sid };
    NostrNoticeEnvelope *n = (NostrNoticeEnvelope *)roundtrip(&no.base, "NOTICE", NULL);
    assert(strcmp(n->message, sid) == 0);
    nostr_envelope_free(&n->base);

    NostrOKEnvelope ok = { .base = { NOSTR_ENVELOPE_OK }, .event_id = (char *)sid, .ok = true, .reason = (char *)sid };
    NostrOKEnvelope *k = (NostrOKEnvelope *)roundtrip(&ok.base, "OK", NULL);
    assert(strcmp(k->event_id, sid) == 0 && strcmp(k->reason, sid) == 0);
    nostr_envelope_free(&k->base);

    NostrAuthEnvelope au = { .base = { NOSTR_ENVELOPE_AUTH }, .challenge = (char *)sid, .event = NULL };
    NostrAuthEnvelope *a = (NostrAuthEnvelope *)roundtrip(&au.base, "AUTH", NULL);
    assert(strcmp(a->challenge, sid) == 0);
    nostr_envelope_free(&a->base);
}

static void test_long_subscription_ids(void) {
    NostrFilters *fs = several_filters();
    NostrEvent *ev = signed_event(1, "long");
    static const size_t lengths[] = { 64, 65, 255, 256, 4096, 65536 };
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        char *sid = repeat('s', lengths[i]);
        roundtrip_all_with_sid(sid, fs, ev);
        free(sid);
        /* Every character escaped: the escaped id is twice as long. */
        sid = repeat('"', lengths[i]);
        roundtrip_all_with_sid(sid, fs, ev);
        free(sid);
    }
    nostr_event_free(ev);
    nostr_filters_free(fs);
}

/* Each serializer sizes its buffer from the escaped lengths of its parts.
 * Walk every id length from 0 to 300 with plain, doubled (\") and six-fold
 * (\u00XX) escapes, with no filters and with filters, so every sizing path
 * lands on every length around its fixed overhead. Under ASan an off-by-one
 * in any of them is an error; without it a truncated frame fails the JSON
 * check. */
static void test_buffer_boundaries(void) {
    NostrFilters *none = nostr_filters_new();
    NostrFilters *one = n_filters(1);
    NostrFilters *many = several_filters();
    NostrEvent *ev = signed_event(1, "b");
    static const char fills[] = { 'a', '"', '\x1f' };
    for (size_t f = 0; f < sizeof(fills); f++) {
        for (size_t len = 0; len <= 300; len++) {
            char *sid = repeat(fills[f], len);
            roundtrip_all_with_sid(sid, len % 3 == 0 ? none : len % 3 == 1 ? one : many, ev);
            free(sid);
        }
    }
    nostr_event_free(ev);
    nostr_filters_free(many);
    nostr_filters_free(one);
    nostr_filters_free(none);
}

/* The legacy EVENT marshaller escapes the subscription id too. */
char *event_envelope_marshal_json(NostrEventEnvelope *envelope);

static void test_event_marshal_json_escapes(void) {
    NostrEvent *ev = signed_event(1, "m");
    NostrEventEnvelope env = { .base = { NOSTR_ENVELOPE_EVENT }, .subscription_id = AWKWARD, .event = ev };
    char *frame = event_envelope_marshal_json(&env);
    assert(frame);
    assert_json_frame(frame, "EVENT");
    NostrEventEnvelope *p = (NostrEventEnvelope *)nostr_envelope_parse(frame);
    assert(p && strcmp(p->subscription_id, AWKWARD) == 0);
    nostr_envelope_free(&p->base);
    free(frame);
    nostr_event_free(ev);
}

int main(void) {
    test_event();
    test_req_exact_frame();
    test_req_several_filters();
    test_req_empty_filter_list();
    test_req_filter_limit();
    test_count();
    test_close_eose_notice();
    test_ok();
    test_closed();
    test_auth();
    test_long_subscription_ids();
    test_buffer_boundaries();
    test_event_marshal_json_escapes();
    printf("test_envelope: all envelope round-trips passed\n");
    return 0;
}
