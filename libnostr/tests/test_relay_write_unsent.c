/* nostrc-xbso: a write the relay never sends is answered and releases its
 * answer channel.
 *
 * nostr_relay_write() returns an answer channel holding two references: the
 * caller's and the writer's. Two kinds of write never reach a writer:
 *  - unqueued: the relay is closing, so the request cannot be queued and
 *    write_error() answers instead. It used to keep the writer's reference
 *    forever (one leaked channel per write that lost the race with a close)
 *    and to drop its Error when the caller had already closed the channel;
 *    the out-of-memory branch dropped the writer's reference before
 *    write_error() sent, so a caller's unref could free the channel under it.
 *  - queued at free: the request sat in the write queue (no writer running)
 *    when the relay was freed, and the queue was freed with it: the request,
 *    its frame copy and the writer's reference leaked, and the caller waited
 *    for an answer that never came.
 *
 * Offline and deterministic. Unqueued: a relay that never connected is
 * closed, which closes its write queue. Queued: a relay that never connected
 * has no writer, so writes stay queued until it is freed. The test
 * holds an extra reference of its own on each answer channel and watches the
 * count come back to it: on the old code it stays one higher, forever. Under
 * ASan/LSan the leaked channel or Error is also reported. Every wait is
 * bounded; the bound is a failure, never a pass. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "nostr-relay.h"
#include "../src/relay-private.h"
#include "channel.h"
#include "error.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    abort(); \
} } while (0)

#define WAIT_BOUND_MS 10000

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Waits (bounded) until @chan holds exactly @refs references. */
static bool refs_reach(GoChannel *chan, int refs) {
    for (int waited = 0; waited < WAIT_BOUND_MS; waited += 5) {
        if (atomic_load(&chan->refs) == refs) return true;
        sleep_ms(5);
    }
    fprintf(stderr, "refs stuck at %d, expected %d\n", atomic_load(&chan->refs), refs);
    return false;
}

static NostrRelay *closed_relay(void) {
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(NULL, "ws://127.0.0.1:1", &err);
    CHECK(relay != NULL && err == NULL);
    nostr_relay_close(relay, &err);
    if (err) free_error(err);
    CHECK(go_channel_is_closed(relay->priv->write_queue));
    return relay;
}

/* The caller waits for the answer: it gets an Error, and once write_error()
 * is done only the caller's reference (and the test's) is left. */
static void test_caller_receives_error(NostrRelay *relay) {
    GoChannel *answer = nostr_relay_write(relay, "[\"EVENT\",{}]");
    CHECK(answer != NULL);
    go_channel_ref(answer); /* the test's own: the channel outlives the caller */
    void *got = NULL;
    CHECK(go_channel_receive(answer, &got) == 0);
    CHECK(got != NULL); /* an Error, never a success */
    free_error((Error *)got);
    CHECK(refs_reach(answer, 2)); /* caller + test: the writer's is released */
    CHECK(go_channel_is_closed(answer));
    go_channel_unref(answer); /* the caller's */
    go_channel_unref(answer); /* the test's: frees it */
}

/* The caller gives up at once (Groundhog closes the channel when its write
 * confirmation times out): write_error() cannot deliver, frees its Error,
 * and still releases the writer's reference. */
static void test_caller_gone_first(NostrRelay *relay) {
    for (int i = 0; i < 50; i++) {
        GoChannel *answer = nostr_relay_write(relay, "[\"REQ\",\"s\",{}]");
        CHECK(answer != NULL);
        go_channel_ref(answer);
        go_channel_close(answer);
        go_channel_unref(answer); /* the caller's */
        CHECK(refs_reach(answer, 1)); /* only the test's is left */
        /* write_error() may have sent before the close: drain what it left. */
        void *left = NULL;
        while (go_channel_try_receive(answer, &left) == 0)
            if (left) free_error((Error *)left);
        go_channel_unref(answer);
    }
}

/* Writes queued on a relay with no writer, then the relay freed: each caller
 * gets an Error and the writer's reference is released. */
static void test_queued_at_free(void) {
    enum { N = 3 };
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(NULL, "ws://127.0.0.1:1", &err);
    CHECK(relay != NULL && err == NULL);
    GoChannel *answers[N];
    for (int i = 0; i < N; i++) {
        answers[i] = nostr_relay_write(relay, "[\"EVENT\",{}]");
        CHECK(answers[i] != NULL);
        go_channel_ref(answers[i]); /* the test's own */
    }
    CHECK(go_channel_get_depth(relay->priv->write_queue) == N); /* all queued */
    nostr_relay_free(relay);
    for (int i = 0; i < N; i++) {
        void *got = NULL;
        bool answered = false;
        for (int waited = 0; waited < WAIT_BOUND_MS && !answered; waited += 5) {
            answered = go_channel_try_receive(answers[i], &got) == 0;
            if (!answered) sleep_ms(5);
        }
        CHECK(answered && got != NULL); /* an Error, never silence */
        free_error((Error *)got);
        CHECK(refs_reach(answers[i], 2)); /* caller + test */
        CHECK(go_channel_is_closed(answers[i]));
        go_channel_unref(answers[i]);
        go_channel_unref(answers[i]);
    }
}

int main(void) {
    NostrRelay *relay = closed_relay();
    test_caller_receives_error(relay);
    test_caller_gone_first(relay);
    nostr_relay_free(relay);
    test_queued_at_free();
    printf("relay write unsent: ok\n");
    return 0;
}
