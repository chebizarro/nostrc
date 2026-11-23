/* Unit barriers wrap select only in this TU; the implementation and select are
 * real. Wire admission/late OK/rejection coverage lives in test_relay_auth. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include "select.h"
#include "nostr-relay.h"
#include "nostr-event.h"
#include "context.h"

static GoSelectResult barrier_select(GoSelectCase *, size_t, uint64_t);
#define go_select_timeout barrier_select
#include "../src/relay.c"
#undef go_select_timeout

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    NostrRelay *relay;
    bool admission, entered, release, accepted;
    int phase;
    GoContext *cancel;
    NostrEvent *event;
    Error *error;
} test;

static GoSelectResult barrier_select(GoSelectCase *cases, size_t count, uint64_t ms) {
    int phase = ++test.phase;
    bool hold = (test.admission && cases[0].op == GO_SELECT_SEND) ||
                (!test.admission && phase == 3);
    if (hold) {
        pthread_mutex_lock(&test.mu);
        test.entered = true;
        pthread_cond_broadcast(&test.cv);
        while (!test.release) pthread_cond_wait(&test.cv, &test.mu);
        pthread_mutex_unlock(&test.mu);
    }
    GoSelectResult result = go_select_timeout(cases, count, ms);
    if (!test.admission && cases[0].op == GO_SELECT_SEND && result.ok) {
        /* Unit transport completion; no network or altered OK semantics. */
        NostrRelayWriteRequest *req = NULL;
        assert(go_channel_try_receive(test.relay->priv->write_queue, (void **)&req) == 0);
        assert(req && go_channel_try_send(req->answer, NULL) == 0);
        go_channel_unref(req->answer); /* writer's lease */
        free(req->msg);
        free(req);
    }
    return result;
}
static void *publish(void *unused) {
    (void)unused;
    test.accepted = nostr_relay_publish_and_wait_with_context(test.relay, test.event,
        10000, test.cancel, &test.error);
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    test.admission = !strcmp(argv[1], "admission");
    bool disconnected = !strcmp(argv[1], "disconnect");
    bool rejected = !strcmp(argv[1], "rejected");
    pthread_mutex_init(&test.mu, NULL);
    pthread_cond_init(&test.cv, NULL);
    Error *err = NULL;
    test.relay = nostr_relay_new(NULL, "ws://127.0.0.1:1", &err);
    assert(test.relay && !err);
    go_channel_unref(test.relay->priv->write_queue);
    test.relay->priv->write_queue = go_channel_create(1);
    NostrRelayWriteRequest occupied = {0};
    if (test.admission) assert(go_channel_try_send(test.relay->priv->write_queue, &occupied) == 0);
    GoContext *root = go_context_background();
    CancelContextResult cc = go_context_with_cancel(root);
    test.cancel = cc.context;
    test.event = nostr_event_new();
    nostr_event_set_kind(test.event, 1);
    nostr_event_set_created_at(test.event, 1);
    nostr_event_set_pubkey(test.event, "0000000000000000000000000000000000000000000000000000000000000001");
    nostr_event_set_content(test.event, "unit barrier");
    char *id = nostr_event_get_id(test.event);
    assert(id);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, publish, NULL));
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 15;
    pthread_mutex_lock(&test.mu);
    while (!test.entered) assert(!pthread_cond_timedwait(&test.cv, &test.mu, &deadline));
    pthread_mutex_unlock(&test.mu);
    if (!test.admission) {
        if (disconnected) relay_fail_pending_ok(test.relay, "unit disconnect");
        else {
            NostrOKEnvelope ok = {.base.type = NOSTR_ENVELOPE_OK,
                .event_id = id, .ok = !rejected, .reason = "blocked: unit"};
            nostr_relay_dispatch_control_envelope(test.relay, &ok.base);
        }
    }
    cc.cancel(cc.context); /* ready alongside OK/OK false/disconnect, or full SEND */
    pthread_mutex_lock(&test.mu);
    test.release = true;
    pthread_cond_broadcast(&test.cv);
    pthread_mutex_unlock(&test.mu);
    pthread_join(thread, NULL);
    assert(!test.accepted && test.error && test.error->code == ECANCELED);
    assert(!go_hash_map_get_string(test.relay->priv->ok_callbacks, id));
    if (test.admission) {
        void *item = NULL;
        assert(go_channel_try_receive(test.relay->priv->write_queue, &item) == 0 && item == &occupied);
    }
    free_error(test.error);
    free(id);
    nostr_event_free(test.event);
    go_context_unref(cc.context);
    go_context_unref(root);
    nostr_relay_free(test.relay);
    pthread_cond_destroy(&test.cv);
    pthread_mutex_destroy(&test.mu);
    return 0;
}
