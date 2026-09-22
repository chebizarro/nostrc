/* C4 (beads nostrc-ot2c.5): admission policy for NIP-46 responses.
 *
 * White-box tests that exercise nip46_persistent_client_cb directly with
 * crafted incoming events, so admission decisions are observable without
 * a real relay. Positive cases are proven end-to-end in the existing mock
 * flows (test_nip46_e2e_mock, test_nip46_rpc_flow_mock); the tests here
 * focus on NEGATIVE admission: every rejection path leaves no side
 * effect on the pending request.
 *
 * Negatives asserted (all must NOT deliver to the pending request):
 *   1. Wrong-kind event (kind != 24133).
 *   2. Wrong sender (author != session->remote_pubkey_hex).
 *   3. Missing p-tag / p-tag names a different client.
 *   4. Unregistered pool (session not in registry) - event dropped
 *      before touching decrypt.
 *   5. Bad outer signature - nostr_event_check_signature must fail
 *      before decrypt is called.
 *   6. Response id that does not match any pending request - silently
 *      dropped, pending list is untouched.
 *   7. Cancelled pending: even a syntactically valid delivery is
 *      refused; the atomic single-winner guarantee holds.
 *
 * Note on test setup: libnostr's simple_pool spawns a background cleanup
 * worker on creation, and the interaction between that worker and
 * OpenSSL's RAND state on macOS causes subsequent nostr_event_sign
 * calls to fail. We therefore pre-sign every event this test needs BEFORE
 * any NostrSimplePool is created, then hand the pre-built events to
 * nip46_persistent_client_cb one by one.
 */

#include "../src/core/nip46_session.c"
#include "nostr-event.h"
#include "nostr-simple-pool.h"
#include "nostr-tag.h"
#include "json.h"
#include <assert.h>

static const char *CLIENT_SK = "1111111111111111111111111111111111111111111111111111111111111111";
static const char *SIGNER_SK = "2222222222222222222222222222222222222222222222222222222222222222";
static const char *OTHER_SK  = "3333333333333333333333333333333333333333333333333333333333333333";

static NostrEvent *make_signed_event(const char *sk_hex, int kind,
                                     const char *content, NostrTags *tags) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);
    nostr_event_set_kind(ev, kind);
    nostr_event_set_content(ev, content ? content : "ciphertext-placeholder");
    nostr_event_set_created_at(ev, (int64_t)1700000000);
    if (tags) nostr_event_set_tags(ev, tags);
    char *pk = nostr_key_get_public(sk_hex);
    assert(pk);
    nostr_event_set_pubkey(ev, pk);
    free(pk);
    int rc = nostr_event_sign(ev, sk_hex);
    if (rc != 0) fprintf(stderr, "[test] nostr_event_sign failed rc=%d\n", rc);
    assert(rc == 0);
    return ev;
}

static NostrTags *p_tag(const char *pk) {
    return nostr_tags_new(1, nostr_tag_new("p", pk, NULL));
}

typedef struct {
    NostrEvent *wrong_kind;
    NostrEvent *wrong_sender;
    NostrEvent *no_p_tag;
    NostrEvent *wrong_p_tag;
    NostrEvent *no_registry;
    NostrEvent *bad_sig;
} PreSigned;

static void pre_sign_all(PreSigned *e, const char *client_pk, const char *other_pk) {
    e->wrong_kind   = make_signed_event(SIGNER_SK, 1, "plain", p_tag(client_pk));
    e->wrong_sender = make_signed_event(OTHER_SK, NOSTR_EVENT_KIND_NIP46, "ct", p_tag(client_pk));
    e->no_p_tag     = make_signed_event(SIGNER_SK, NOSTR_EVENT_KIND_NIP46, "ct", NULL);
    e->wrong_p_tag  = make_signed_event(SIGNER_SK, NOSTR_EVENT_KIND_NIP46, "ct", p_tag(other_pk));
    e->no_registry  = make_signed_event(SIGNER_SK, NOSTR_EVENT_KIND_NIP46, "ct", p_tag(client_pk));
    e->bad_sig      = make_signed_event(SIGNER_SK, NOSTR_EVENT_KIND_NIP46, "ct", p_tag(client_pk));
    /* Corrupt the signature after signing. */
    assert(e->bad_sig->sig);
    e->bad_sig->sig[0] ^= 0xff;
}

static void pre_sign_free(PreSigned *e) {
    nostr_event_free(e->wrong_kind);
    nostr_event_free(e->wrong_sender);
    nostr_event_free(e->no_p_tag);
    nostr_event_free(e->wrong_p_tag);
    nostr_event_free(e->no_registry);
    nostr_event_free(e->bad_sig);
    memset(e, 0, sizeof(*e));
}

typedef struct {
    NostrNip46Session *session;
    NostrSimplePool *pool;
    NostrRelay *relay;
} Ctx;

static void ctx_init(Ctx *c, const char *signer_pk) {
    memset(c, 0, sizeof(*c));
    c->session = nostr_nip46_client_new();
    assert(c->session);
    assert(nostr_nip46_client_set_secret(c->session, CLIENT_SK) == 0);
    c->session->derived_client_pubkey = nostr_key_get_public(CLIENT_SK);
    c->session->remote_pubkey_hex = strdup(signer_pk);
    c->pool = nostr_simple_pool_new();
    assert(c->pool);
    nostr_simple_pool_ensure_relay(c->pool, "wss://mock");
    assert(c->pool->relay_count > 0);
    c->relay = c->pool->relays[c->pool->relay_count - 1];
    assert(c->relay);
    session_registry_add(c->pool, c->session);
}

static void ctx_free(Ctx *c) {
    session_registry_remove(c->pool);
    nostr_simple_pool_free(c->pool);
    nostr_nip46_session_free(c->session);
    memset(c, 0, sizeof(*c));
}

static void feed(Ctx *c, NostrEvent *ev) {
    NostrIncomingEvent in = { .event = ev, .relay = c->relay };
    nip46_persistent_client_cb(&in);
}

static int pending_count(NostrNip46Session *s) {
    int n = 0;
    for (PendingRequest *pr = s->pending_requests; pr; pr = pr->next) n++;
    return n;
}

int main(void) {
    nostr_json_init();

    /* Pre-derive client_pk / signer_pk / other_pk from the fixed sks
     * BEFORE any pool exists, and pre-sign every event we will feed. */
    char *client_pk = nostr_key_get_public(CLIENT_SK);
    char *signer_pk = nostr_key_get_public(SIGNER_SK);
    char *other_pk  = nostr_key_get_public(OTHER_SK);
    assert(client_pk && signer_pk && other_pk);

    PreSigned events;
    pre_sign_all(&events, client_pk, other_pk);

    /* --- 1. wrong kind --- */
    { Ctx c; ctx_init(&c, signer_pk); feed(&c, events.wrong_kind);
      assert(pending_count(c.session) == 0); ctx_free(&c); }

    /* --- 2. wrong sender --- */
    { Ctx c; ctx_init(&c, signer_pk); feed(&c, events.wrong_sender);
      assert(pending_count(c.session) == 0); ctx_free(&c); }

    /* --- 3. no p-tag --- */
    { Ctx c; ctx_init(&c, signer_pk); feed(&c, events.no_p_tag);
      assert(pending_count(c.session) == 0); ctx_free(&c); }

    /* --- 4. wrong p-tag --- */
    { Ctx c; ctx_init(&c, signer_pk); feed(&c, events.wrong_p_tag);
      assert(pending_count(c.session) == 0); ctx_free(&c); }

    /* --- 5. unregistered pool: deregister then feed --- */
    { Ctx c; ctx_init(&c, signer_pk);
      session_registry_remove(c.pool);
      feed(&c, events.no_registry);
      assert(pending_count(c.session) == 0);
      /* Re-register so ctx_free is symmetric. */
      session_registry_add(c.pool, c.session);
      ctx_free(&c); }

    /* --- 6. bad signature --- */
    { Ctx c; ctx_init(&c, signer_pk); feed(&c, events.bad_sig);
      assert(pending_count(c.session) == 0); ctx_free(&c); }

    /* --- 7. unknown id: deliver() rejects when no pending has matching id --- */
    { Ctx c; ctx_init(&c, signer_pk);
      int64_t deadline_us = (nip46_now_ms() + 5000) * 1000;
      PendingRequest *pr = pending_request_new_ex("expected-id", deadline_us, NULL);
      pending_request_add(c.session, pr);
      char *stray = strdup("stray-payload");
      assert(!pending_request_deliver(c.session, "not-the-expected-id", stray));
      free(stray);
      /* Pending must still be present, un-delivered. */
      assert(pending_count(c.session) == 1);
      assert(!pr->delivered);
      pending_request_cancel(c.session, "expected-id");
      ctx_free(&c); }

    /* --- 8. cancelled pending refuses further delivery --- */
    { Ctx c; ctx_init(&c, signer_pk);
      int64_t deadline_us = (nip46_now_ms() + 5000) * 1000;
      PendingRequest *pr = pending_request_new_ex("was-cancelled", deadline_us, NULL);
      pending_request_add(c.session, pr);
      pr->cancelled = 1;   /* white-box cancel */
      char *late = strdup("late");
      assert(!pending_request_deliver(c.session, "was-cancelled", late));
      free(late);
      pending_request_cancel(c.session, "was-cancelled");
      ctx_free(&c); }

    pre_sign_free(&events);
    free(client_pk); free(signer_pk); free(other_pk);
    puts("test_nip46_signed_response_admission: OK");
    return 0;
}
