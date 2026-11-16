#define _POSIX_C_SOURCE 200809L
#include "nostr/runtime.h"
#include "nostr/signal_gate.h"
#include "nostr/signal_route.h"
#include "host_os.h"
#include "nostr_relay_fixture.h"
#include <select.h>
#include <ticker.h>
#include <sodium.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
typedef enum { DIAL, BOUNDARY, PUBLISHED, FETCHED, OFFERED, ANSWERED, ADMISSION, CACHED } notice_kind;
typedef struct { notice_kind kind; bool ok; int value; char text[129]; } notice;
typedef struct { GoChannel *notices; atomic_uint dials, offers, answers; notice pending[256]; size_t count; } observer;
static void notify(observer *o, notice_kind kind, bool ok, int value, const char *text)
{
    notice *n = calloc(1, sizeof(*n)); CHECK(n); n->kind = kind; n->ok = ok; n->value = value;
    if (text) snprintf(n->text, sizeof(n->text), "%s", text);
    CHECK(go_channel_try_send(o->notices, n) == 0);
}
static bool dial(void *user, const char *key, const fipsd_nostr_advert *a)
{
    observer *o = user; CHECK(a->endpoint_count == 1); atomic_fetch_add(&o->dials, 1);
    notify(o, DIAL, true, 0, key); return true;
}
static bool local(void *user, fipsd_nostr_advert *a)
{
    (void)user; a->endpoint_count = 1; a->endpoints[0] = (fipsd_nostr_endpoint){FIPSD_NOSTR_TCP, "fips.example.com:2121"}; return true;
}
static bool offer(void *user, const fipsd_nostr_traversal *p)
{
    observer *o = user; atomic_fetch_add(&o->offers, 1); notify(o, OFFERED, true, 0, p->session_id); return true;
}
static bool answer(void *user, const fipsd_nostr_traversal *p)
{
    observer *o = user; atomic_fetch_add(&o->answers, 1); notify(o, ANSWERED, true, 0, p->session_id); return true;
}
static void cached(void *user, const char *key, bool live) { notify(user, CACHED, live, 0, key); }
static void admission(void *user, const char *id, bool ok) { notify(user, ADMISSION, ok, 0, id); }
static void boundary(void *user, const char *url, bool live, const char *reason)
{ notify(user, BOUNDARY, live, 0, reason ? reason : url); }
static void published(void *user, const char *url, int kind, bool ok) { notify(user, PUBLISHED, ok, kind, url); }
static void fetched(void *user, const char *peer, fipsd_nostr_refetch_outcome outcome) { notify(user, FETCHED, true, outcome, peer); }
static notice wait_notice(observer *o, notice_kind kind, int value)
{
    for (size_t i = 0; i < o->count; ++i) {
        notice n = o->pending[i];
        if (n.kind == kind && (value < 0 || n.value == value)) {
            memmove(&o->pending[i], &o->pending[i + 1], (--o->count - i) * sizeof(n)); return n;
        }
    }
    for (;;) {
        void *p = NULL;
        GoSelectCase c = {.op = GO_SELECT_RECEIVE, .chan = o->notices, .recv_buf = &p};
        GoSelectResult result = go_select_timeout(&c, 1, 15000);
        CHECK(result.selected_case == 0 && p);
        notice n = *(notice *)p; free(p);
        if (n.kind == kind && (value < 0 || n.value == value)) return n;
        CHECK(o->count < 256); o->pending[o->count++] = n;
    }
}
static uint64_t now_ms(void)
{ struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static char *advert(fips_identity *id, uint64_t at, const char *app)
{
    fipsd_nostr_advert a = {.endpoint_count = 1, .endpoints = {{FIPSD_NOSTR_TCP, "peer.example.com:2121"}}};
    NostrEvent *e = NULL; CHECK(fipsd_nostr_advert_build(id, &a, app, at, 3600, &e) == FIPSD_NOSTR_OK);
    char *json = fipsd_nostr_event_json(e, true); CHECK(json); nostr_event_free(e); return json;
}
static void pure(void)
{
    fipsd_signal_gate g; fipsd_signal_gate_init(&g, 100);
    for (int i = 0; i < 20; ++i) CHECK(fipsd_signal_gate_admit(&g, false, 100) == FIPSD_SIGNAL_ADMIT);
    CHECK(fipsd_signal_gate_admit(&g, false, 100) == FIPSD_SIGNAL_SHED_SHARED);
    for (int i = 0; i < 20; ++i) CHECK(fipsd_signal_gate_admit(&g, true, 100) == FIPSD_SIGNAL_ADMIT);
    CHECK(fipsd_signal_gate_admit(&g, true, 100) == FIPSD_SIGNAL_SHED_RESERVE);
    CHECK(fipsd_signal_gate_admit(&g, false, 300) == FIPSD_SIGNAL_ADMIT);
    CHECK(fipsd_signal_gate_admit(&g, false, 100) == FIPSD_SIGNAL_SHED_SHARED);
    fipsd_nostr_failure_state state; CHECK(fipsd_nostr_failure_init(&state, 2, 2, 1000, 100));
    const char *a = "1111111111111111111111111111111111111111111111111111111111111111";
    const char *b = "2222222222222222222222222222222222222222222222222222222222222222";
    const char *c = "3333333333333333333333333333333333333333333333333333333333333333";
    fipsd_nostr_failure_decision d = fipsd_nostr_failure_record(&state, a, 0); CHECK(d.warn && !d.crossed_threshold && d.streak == 1);
    d = fipsd_nostr_failure_record(&state, a, 1); CHECK(!d.warn && d.crossed_threshold && d.cooldown_ms == 1001);
    d = fipsd_nostr_failure_record(&state, a, 101); CHECK(d.warn && !d.crossed_threshold);
    CHECK(fipsd_nostr_failure_mismatch(&state, a, 102, 10000));
    CHECK(!fipsd_nostr_failure_mismatch(&state, a, 103, 10000));
    CHECK(fipsd_nostr_failure_get(&state, a)->cooldown_ms == 10102);
    fipsd_nostr_failure_skew(&state, a, -42, 104); fipsd_nostr_failure_success(&state, a, 105);
    CHECK(!fipsd_nostr_failure_get(&state, a)->streak && fipsd_nostr_failure_get(&state, a)->skew_ms == -42);
    fipsd_nostr_failure_record(&state, b, 106); fipsd_nostr_failure_record(&state, c, 107);
    CHECK(!fipsd_nostr_failure_get(&state, a)); fipsd_nostr_failure_refresh(&state, c);
    CHECK(!fipsd_nostr_failure_get(&state, c)->streak); fipsd_nostr_failure_clear(&state);
    struct { bool complete, any, author, valid, room, cached; uint64_t old, next; fipsd_nostr_refetch_outcome want; } cases[] = {
        {false, false, false, false, true, true, 10, 0, FIPSD_REFETCH_SKIPPED},
        {true, false, false, false, true, true, 10, 0, FIPSD_REFETCH_EVICTED},
        {true, true, false, true, true, true, 10, 11, FIPSD_REFETCH_SKIPPED},
        {true, true, true, false, true, true, 10, 10, FIPSD_REFETCH_SAME_ADVERT},
        {true, true, true, true, true, true, 10, 9, FIPSD_REFETCH_SAME_ADVERT},
        {true, true, true, false, true, true, 10, 11, FIPSD_REFETCH_SKIPPED},
        {true, true, true, true, false, false, 0, 11, FIPSD_REFETCH_SKIPPED},
        {true, true, true, true, true, true, 10, 11, FIPSD_REFETCH_REFRESHED},
        {true, true, true, true, true, false, 0, 11, FIPSD_REFETCH_REFRESHED}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        CHECK(fipsd_nostr_refetch_decide(cases[i].complete, cases[i].any, cases[i].author, cases[i].valid,
            cases[i].room, cases[i].cached, cases[i].old, cases[i].next) == cases[i].want);
    fipsd_nostr_freshness fresh;
    CHECK(fipsd_nostr_traversal_freshness(100000, 220000, 39999, 120000, &fresh) == FIPSD_NOSTR_FUTURE);
    CHECK(fipsd_nostr_traversal_freshness(100000, 220000, 40000, 120000, &fresh) == FIPSD_NOSTR_OK && fresh == FIPSD_NOSTR_SKEW);
    CHECK(fipsd_nostr_traversal_freshness(100000, 220000, 280001, 120000, &fresh) == FIPSD_NOSTR_STALE);
    RelayInformationDocument *info = nostr_nip11_parse_info("{\"supported_nips\":[1,40,42]}");
    CHECK(fipsd_nostr_relay_capable(info, true)); nostr_nip11_free_info(info);
    info = nostr_nip11_parse_info("{\"supported_nips\":[1],\"limitation\":{\"payment_required\":true}}");
    CHECK(!fipsd_nostr_relay_capable(info, false)); nostr_nip11_free_info(info);
    /* A one-hour publish ticker must not delay cancellation for an hour. */
    Ticker *ticker = create_ticker(3600000); CHECK(ticker); stop_ticker(ticker);
}
int main(void)
{
    CHECK(sodium_init() >= 0); pure();
    fixture *f = fixture_start();
    fixture_call(f, HOLD_EOSE, -1, NULL);
    fips_secp *secp; CHECK(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
    fips_identity *me, *them;
    CHECK(fips_identity_generate(secp, fipsd_host_os_entropy(), &me) == FIPS_IDENTITY_OK);
    CHECK(fips_identity_generate(secp, fipsd_host_os_entropy(), &them) == FIPS_IDENTITY_OK);
    fips_xonly_pubkey their_key = fips_identity_pubkey(them), my_key = fips_identity_pubkey(me);
    char key[65], mine[65]; sodium_bin2hex(key, sizeof(key), their_key.bytes, 32); sodium_bin2hex(mine, sizeof(mine), my_key.bytes, 32);
    uint64_t at = now_ms() / 1000;
    char *old = advert(them, at - 10, FIPSD_NOSTR_IDENTIFIER); fixture_call(f, SEED, -1, old); free(old);
    char yaml[2048]; snprintf(yaml, sizeof(yaml), "node:\n  rendezvous:\n    nostr:\n      enabled: true\n      policy: open\n      seen_sessions_max_entries: 2\n      advert_relays: ['%s', '%s']\n      dm_relays: ['%s', '%s']\n", fixture_url(f, 0), fixture_url(f, 1), fixture_url(f, 0), fixture_url(f, 1));
    fipsd_config_log log = {0}; fipsd_config *config = fipsd_config_parse(yaml, strlen(yaml), &log); CHECK(config); fipsd_config_log_clear(&log);
    observer o = {.notices = go_channel_create(256)};
    fipsd_rendezvous_driver driver = {.user = &o, .dial_candidate = dial, .local_advert = local,
        .traversal_offer = offer, .traversal_answer = answer, .signal_admission = admission,
        .advert_cached = cached, .boundary = boundary, .publish_result = published, .refetch_result = fetched};
    fipsd_nostr_runtime *r = fipsd_nostr_runtime_create(config, me, NULL, driver); CHECK(r); fipsd_config_free(config);
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.handle_signals = false; rc.workers = 2;
    fipsd_runtime *host = fipsd_runtime_create(&rc); CHECK(host);
    CHECK(fipsd_runtime_register(host, fipsd_nostr_runtime_subsystem(r)) == 0 && fipsd_runtime_start(host) == 0);
    CHECK(!wait_notice(&o, CACHED, -1).ok); CHECK(atomic_load(&o.dials) == 0);
    fixture_call(f, RELEASE_EOSE, -1, NULL);
    CHECK(wait_notice(&o, DIAL, -1).ok);
    CHECK(wait_notice(&o, BOUNDARY, -1).ok);
    CHECK(wait_notice(&o, PUBLISHED, NOSTR_KIND_FIPS_OVERLAY_ADVERT).ok);
    CHECK(wait_notice(&o, BOUNDARY, -1).ok);
    CHECK(atomic_load(&o.dials) == 1); /* identical history at two relays */
    char *newer = advert(them, at - 5, FIPSD_NOSTR_IDENTIFIER); fixture_call(f, SEED, -1, newer); free(newer);
    CHECK(fipsd_nostr_runtime_refetch(r, key)); CHECK(wait_notice(&o, FETCHED, -1).value == FIPSD_REFETCH_REFRESHED);
    CHECK(fipsd_nostr_runtime_refetch(r, key)); CHECK(wait_notice(&o, FETCHED, -1).value == FIPSD_REFETCH_SAME_ADVERT);
    char *live = advert(them, at, FIPSD_NOSTR_IDENTIFIER); fixture_call(f, INJECT, -1, live); free(live);
    CHECK(wait_notice(&o, DIAL, -1).ok); CHECK(fipsd_nostr_runtime_refetch(r, key));
    CHECK(wait_notice(&o, FETCHED, -1).value == FIPSD_REFETCH_SAME_ADVERT); CHECK(atomic_load(&o.dials) == 2);
    fixture_call(f, AUTH_CLOSED, 0, NULL); CHECK(wait_notice(&o, PUBLISHED, NOSTR_KIND_CLIENT_AUTHENTICATION).ok);
    CHECK(!wait_notice(&o, BOUNDARY, -1).ok);
    notice resumed;
    do { resumed = wait_notice(&o, BOUNDARY, -1); } while (!resumed.ok || strcmp(resumed.text, fixture_url(f, 0)));
    CHECK(atomic_load(&o.dials) == 2); /* re-REQ backfill is idempotent */
    fixture_call(f, DISCONNECT, 0, NULL); CHECK(!wait_notice(&o, BOUNDARY, -1).ok);
    do { resumed = wait_notice(&o, BOUNDARY, -1); } while (!resumed.ok || strcmp(resumed.text, fixture_url(f, 0)));
    CHECK(atomic_load(&o.dials) == 2);
    fixture_call(f, CLEAR, -1, NULL); CHECK(fipsd_nostr_runtime_refetch(r, key)); CHECK(wait_notice(&o, FETCHED, -1).value == FIPSD_REFETCH_EVICTED);
    char *wrong = advert(them, at + 1, "other-overlay"); fixture_call(f, SEED, -1, wrong); free(wrong);
    CHECK(fipsd_nostr_runtime_refetch(r, key)); CHECK(wait_notice(&o, FETCHED, -1).value == FIPSD_REFETCH_SKIPPED);
    for (int i = 0; i < 3; ++i) {
        fipsd_nostr_traversal p = {.issued_at = now_ms(), .expires_at = now_ms() + 120000};
        CHECK(fips_identity_npub(them, p.sender_npub, sizeof(p.sender_npub)) == FIPS_IDENTITY_OK);
        CHECK(fips_identity_npub(me, p.recipient_npub, sizeof(p.recipient_npub)) == FIPS_IDENTITY_OK);
        snprintf(p.session_id, sizeof(p.session_id), "session-%d", i); snprintf(p.nonce, sizeof(p.nonce), "nonce-%d", i);
        NostrEvent *e; CHECK(fipsd_nostr_traversal_build(them, mine, &p, now_ms() / 1000, 120, &e) == FIPSD_NOSTR_OK);
        char *json = fipsd_nostr_event_json(e, true); CHECK(json); fixture_call(f, INJECT, -1, json); free(json); nostr_event_free(e);
        notice n = wait_notice(&o, ADMISSION, -1); CHECK(n.ok == (i < 2));
        if (i < 2) CHECK(wait_notice(&o, OFFERED, -1).ok);
    }
    CHECK(atomic_load(&o.offers) == 2);
    /* Full responder replay memory must not prevent our own pending answer. */
    fipsd_nostr_traversal sent = {.issued_at = now_ms(), .expires_at = now_ms() + 120000};
    CHECK(fips_identity_npub(me, sent.sender_npub, sizeof(sent.sender_npub)) == FIPS_IDENTITY_OK);
    CHECK(fips_identity_npub(them, sent.recipient_npub, sizeof(sent.recipient_npub)) == FIPS_IDENTITY_OK);
    strcpy(sent.session_id, "outbound"); strcpy(sent.nonce, "outbound-nonce");
    CHECK(fipsd_nostr_runtime_send_signal(r, key, &sent));
    CHECK(wait_notice(&o, PUBLISHED, NOSTR_KIND_FIPS_TRAVERSAL).ok);
    CHECK(wait_notice(&o, PUBLISHED, NOSTR_KIND_FIPS_TRAVERSAL).ok);
    fipsd_nostr_traversal reply = {.answer = true, .has_reason = true, .has_offer_received_at = true, .offer_received_at = now_ms(),
        .issued_at = now_ms(), .expires_at = now_ms() + 120000};
    strcpy(reply.reason, "busy"); strcpy(reply.session_id, sent.session_id); strcpy(reply.in_reply_to, sent.nonce);
    strcpy(reply.sender_npub, sent.recipient_npub); strcpy(reply.recipient_npub, sent.sender_npub);
    NostrEvent *answer_event; CHECK(fipsd_nostr_traversal_build(them, mine, &reply, now_ms() / 1000, 120, &answer_event) == FIPSD_NOSTR_OK);
    char *answer_json = fipsd_nostr_event_json(answer_event, true); CHECK(answer_json);
    fixture_call(f, INJECT, -1, answer_json); free(answer_json); nostr_event_free(answer_event);
    CHECK(wait_notice(&o, ANSWERED, -1).ok); CHECK(atomic_load(&o.answers) == 1);
    /* Every publish checks negative OK too, not just successful sends. */
    fixture_call(f, REJECT_PUBLISH, 1, NULL);
    fipsd_nostr_traversal outgoing = {.issued_at = now_ms(), .expires_at = now_ms() + 120000, .answer = true};
    CHECK(fips_identity_npub(me, outgoing.sender_npub, sizeof(outgoing.sender_npub)) == FIPS_IDENTITY_OK);
    CHECK(fips_identity_npub(them, outgoing.recipient_npub, sizeof(outgoing.recipient_npub)) == FIPS_IDENTITY_OK);
    strcpy(outgoing.session_id, "refusal"); strcpy(outgoing.in_reply_to, "offer-nonce");
    CHECK(fipsd_nostr_runtime_send_signal(r, key, &outgoing));
    CHECK(wait_notice(&o, PUBLISHED, NOSTR_KIND_FIPS_TRAVERSAL).ok);
    CHECK(!wait_notice(&o, PUBLISHED, NOSTR_KIND_FIPS_TRAVERSAL).ok);
    fipsd_runtime_shutdown(host); CHECK(fipsd_runtime_run(host) == FIPSD_RUN_OK);
    fipsd_runtime_destroy(host); fipsd_nostr_runtime_destroy(r);
    fixture_destroy(f);
    void *n; while (go_channel_try_receive(o.notices, &n) == 0) free(n); go_channel_unref(o.notices);
    fips_identity_destroy(them); fips_identity_destroy(me); fips_identity_destroy_secp(secp);
    puts("Nostr runtime: local WebSocket relay, OK/EOSE/CLOSED/AUTH, dedupe, refetch, sessions, freshness, failures passed");
    return 0;
}
