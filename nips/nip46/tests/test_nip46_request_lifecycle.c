/* C2 (beads nostrc-ot2c.3): per-request cancellation + deadline ownership
 * for NIP-46 client RPC.
 *
 * White-box tests that exercise the internal pending_request / cancel-handle
 * machinery directly. No relay is contacted; a synthetic pending request is
 * inserted into a client session and delivered / cancelled from the same
 * process. This keeps the coverage deterministic and CI-safe while still
 * exercising the atomic-single-winner delivery path.
 *
 * Invariants asserted:
 *  1. A cancel handle attached to a pending request marks it cancelled and
 *     the deliver() path becomes idempotent (later deliveries return failure
 *     without leaking the caller's payload).
 *  2. An expired absolute deadline blocks delivery even before any timer
 *     fires (no wall-clock sleep required).
 *  3. Cancelling twice / cancelling a handle with no attached request is a
 *     no-op and safe.
 *  4. Cancel handle refcount / unref lifecycle survives being detached from
 *     a freed pending request (the pending's ref is dropped when the
 *     pending is freed).
 *  5. NULL callback (fire-and-forget) does not leak the result when the
 *     job carries a cancel handle that was already cancelled at submit
 *     time.
 *  6. The GLib async wrapper still honours cancellation before/after the
 *     RPC on a session with no relays (fast fail path).
 *
 * Complementary coverage lives in test_pending_lifetime.c (race between
 * delivery and release) and test_glib_lifetime.c (GLib task teardown
 * during in-flight callback).
 */

#include "../src/core/nip46_session.c"
#include <assert.h>
#include <pthread.h>
#include <unistd.h>

static void test_cancel_handle_marks_pending_cancelled(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    NostrNip46CancelHandle *h = nostr_nip46_cancel_handle_new();
    assert(s && h);

    /* Attach the handle to a pending request BEFORE cancelling. */
    int64_t deadline_us = (nip46_now_ms() + 5000) * 1000;
    PendingRequest *pr = pending_request_new_ex("req-cancel", deadline_us, h);
    assert(pr && !pr->cancelled);
    pending_request_add(s, pr);

    /* Cancel the handle; the wait loop would notice at the next poll,
     * and any concurrent deliver() must now be rejected. */
    nostr_nip46_cancel_handle_cancel(h);
    assert(nostr_nip46_cancel_handle_is_cancelled(h));

    /* deliver() checks the handle and refuses to hand off. */
    char *late = strdup("late-response");
    int accepted = pending_request_deliver(s, "req-cancel", late);
    assert(!accepted);
    free(late);

    pending_request_cancel(s, "req-cancel");
    assert(!s->pending_requests);

    nostr_nip46_cancel_handle_unref(h);
    nostr_nip46_session_free(s);
}

static void test_absolute_deadline_blocks_delivery(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    assert(s);
    /* Deadline in the past (subtracted below to avoid clock skew). */
    int64_t deadline_us = (nip46_now_ms() - 1000) * 1000;
    PendingRequest *pr = pending_request_new_ex("req-expired", deadline_us, NULL);
    assert(pr);
    assert(pr->deadline_us == deadline_us);
    pending_request_add(s, pr);

    char *late = strdup("payload");
    int accepted = pending_request_deliver(s, "req-expired", late);
    assert(!accepted);
    free(late);

    pending_request_cancel(s, "req-expired");
    nostr_nip46_session_free(s);
}

static void test_cancel_handle_reference_counting(void) {
    /* handle stays valid across ref/unref pairs; final unref frees. */
    NostrNip46CancelHandle *h = nostr_nip46_cancel_handle_new();
    assert(h);
    for (int i = 0; i < 8; i++) nostr_nip46_cancel_handle_ref(h);
    for (int i = 0; i < 8; i++) nostr_nip46_cancel_handle_unref(h);
    /* Not cancelled, unref last time -> freed; use ASan to detect misuse. */
    nostr_nip46_cancel_handle_unref(h);

    /* Independent cancelled state after new(). */
    NostrNip46CancelHandle *h2 = nostr_nip46_cancel_handle_new();
    assert(h2 && !nostr_nip46_cancel_handle_is_cancelled(h2));
    nostr_nip46_cancel_handle_cancel(h2);
    assert(nostr_nip46_cancel_handle_is_cancelled(h2));
    /* Double cancel is a no-op. */
    nostr_nip46_cancel_handle_cancel(h2);
    assert(nostr_nip46_cancel_handle_is_cancelled(h2));
    nostr_nip46_cancel_handle_unref(h2);
    /* NULL is safe. */
    nostr_nip46_cancel_handle_unref(NULL);
    nostr_nip46_cancel_handle_cancel(NULL);
    assert(!nostr_nip46_cancel_handle_is_cancelled(NULL));
}

static void test_pending_ref_survives_free(void) {
    /* pending_request owns a ref to the handle; freeing the pending must
     * drop just its own ref, not the caller's ref. */
    NostrNip46CancelHandle *h = nostr_nip46_cancel_handle_new();
    assert(h);
    /* Caller keeps one ref by not calling unref yet. */
    PendingRequest *pr = pending_request_new_ex("req-ref", 0, h);
    assert(pr && pr->cancel_handle == h);
    pending_request_free(pr);
    /* Handle is still valid; cancel + is_cancelled still work. */
    nostr_nip46_cancel_handle_cancel(h);
    assert(nostr_nip46_cancel_handle_is_cancelled(h));
    nostr_nip46_cancel_handle_unref(h);
}

/* Fire-and-forget async: caller passes NULL callback, pre-cancelled
 * handle. The job must not enqueue a worker or leak. */
static void discard_cb(NostrNip46Session *s, const char *r, const char *e, void *ud) {
    (void)s; (void)ud;
    /* Result/error paths must both be free()able. */
    if (r) free((void *)r);
    if (e) free((void *)e);
}

static void test_async_opts_pre_cancelled_fails_fast(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    assert(s);
    NostrNip46CancelHandle *h = nostr_nip46_cancel_handle_new();
    assert(h);
    nostr_nip46_cancel_handle_cancel(h);

    NostrNip46RequestOptions opts = { .deadline_ms = 0, .cancel_handle = h };
    /* NULL callback fire-and-forget. */
    nostr_nip46_client_sign_event_async_opts(s, "{\"kind\":1}", &opts, NULL, NULL);
    /* Also try with a real callback to exercise the delivery path. */
    nostr_nip46_client_sign_event_async_opts(s, "{\"kind\":1}", &opts, discard_cb, NULL);

    nostr_nip46_cancel_handle_unref(h);
    nostr_nip46_session_free(s);
}

/* Race: one thread delivers, another cancels via handle. Under mutex the
 * outcome is deterministic — at most one wins, no double-free, no leak. */
typedef struct {
    NostrNip46Session *session;
    NostrNip46CancelHandle *handle;
    const char *req_id;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned ready;
} CancelRace;

static void rendezvous(CancelRace *r) {
    pthread_mutex_lock(&r->mutex);
    r->ready++;
    pthread_cond_broadcast(&r->cond);
    while (r->ready < 2) pthread_cond_wait(&r->cond, &r->mutex);
    pthread_mutex_unlock(&r->mutex);
}

static void *race_deliver(void *arg) {
    CancelRace *r = arg;
    char *msg = strdup("winner");
    rendezvous(r);
    if (!pending_request_deliver(r->session, r->req_id, msg)) free(msg);
    return NULL;
}

static void *race_cancel(void *arg) {
    CancelRace *r = arg;
    rendezvous(r);
    nostr_nip46_cancel_handle_cancel(r->handle);
    return NULL;
}

static void test_cancel_delivery_race(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    assert(s);
    for (int i = 0; i < 200; i++) {
        NostrNip46CancelHandle *h = nostr_nip46_cancel_handle_new();
        assert(h);
        int64_t deadline_us = (nip46_now_ms() + 5000) * 1000;
        PendingRequest *pr = pending_request_new_ex("race", deadline_us, h);
        assert(pr);
        pending_request_add(s, pr);

        CancelRace r = { s, h, "race", PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0 };
        pthread_t t_deliver, t_cancel;
        pthread_create(&t_deliver, NULL, race_deliver, &r);
        pthread_create(&t_cancel, NULL, race_cancel, &r);
        pthread_join(t_deliver, NULL);
        pthread_join(t_cancel, NULL);

        /* The waiter path pings back — for the test we manually drain and
         * release. If deliver won, the response is on the channel; else the
         * caller frees it. Either way, pending_request_cancel cleans up. */
        void *drained = NULL;
        if (pr->response_chan &&
            go_channel_try_receive(pr->response_chan, &drained) == 0)
            free(drained);
        pending_request_cancel(s, "race");
        assert(!s->pending_requests);

        pthread_cond_destroy(&r.cond);
        pthread_mutex_destroy(&r.mutex);
        nostr_nip46_cancel_handle_unref(h);
    }
    nostr_nip46_session_free(s);
}

int main(void) {
    test_cancel_handle_marks_pending_cancelled();
    test_absolute_deadline_blocks_delivery();
    test_cancel_handle_reference_counting();
    test_pending_ref_survives_free();
    test_async_opts_pre_cancelled_fails_fast();
    test_cancel_delivery_race();
    puts("test_nip46_request_lifecycle: OK");
    return 0;
}
