/* Hold a real relay write queue full while fire/refire enqueues a REQ. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <nsync_mu_wait.h>
#include <nsync_time.h>
#include "nostr-subscription.h"
#include "subscription-private.h"
#include "relay-private.h"

struct Fire { NostrSubscription *sub; bool refire; bool result; Error *error; };
static void *fire(void *data) {
    struct Fire *f = data;
    f->result = f->refire ? nostr_subscription_refire_since(f->sub, 1700000000, &f->error)
                          : nostr_subscription_fire(f->sub, &f->error);
    return NULL;
}
static int sender_entered(const void *data) {
    const GoChannel *q = data;
    return atomic_load(&q->active_waiters) > 0;
}
static char *drain(NostrRelay *relay) {
    NostrRelayWriteRequest *req = NULL;
    assert(go_channel_receive(relay->priv->write_queue, (void **)&req) == 0 && req);
    char *msg = req->msg;
    /* The caller may already have abandoned its answer, as the production
     * writer permits. The writer still owns and releases its reference. */
    (void)go_channel_try_send(req->answer, NULL);
    go_channel_close(req->answer);
    go_channel_unref(req->answer);
    free(req);
    return msg;
}
static void check(bool refire, int retirement) {
    Error *error = NULL;
    GoContext *parent = go_context_background();
    NostrRelay *relay = nostr_relay_new(parent, "ws://enqueue.invalid", &error);
    assert(relay && !error); go_context_unref(parent);
    /* The offline connection has no writer thread: only this test can drain
     * the real write queue. No test hook substitutes for the blocking send. */
    relay->connection = nostr_connection_new("ws://enqueue.invalid");
    assert(relay->connection);
    NostrFilters *filters = nostr_filters_new();
    NostrFilter *f = nostr_filter_new(); f->limit = 10;
    assert(nostr_filters_add(filters, f)); nostr_filter_free(f);
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    assert(sub);
    char *id = strdup(nostr_subscription_get_id_const(sub)); assert(id);
    GoChannel *q = relay->priv->write_queue;
    for (size_t i = 0; i < q->capacity; i++) {
        GoChannel *answer = nostr_relay_write(relay, "[\"CLOSE\",\"preload\"]");
        assert(answer); nostr_relay_write_answer_release(answer);
    }
    assert(go_channel_get_depth(q) == q->capacity);
    struct Fire args = {.sub = sub, .refire = refire}; pthread_t thread;
    nsync_mu_lock(&q->mutex);
    assert(pthread_create(&thread, NULL, fire, &args) == 0);
    int rc = nsync_mu_wait_with_deadline(&q->mutex, sender_entered, q, NULL,
                                        nsync_time_add(nsync_time_now(), nsync_time_ms(5000)), NULL);
    nsync_mu_unlock(&q->mutex);
    assert(rc == 0 && go_channel_get_depth(q) == q->capacity);
    /* Before the repair this fails deterministically: the real full-queue
     * sender still owns sub_mutex, so dispatch/retirement cannot proceed. */
    assert(nsync_mu_trylock(&sub->priv->sub_mutex));
    nsync_mu_unlock(&sub->priv->sub_mutex);
    NostrEvent *event = nostr_event_new();
    event->id = strdup("1111111111111111111111111111111111111111111111111111111111111111");
    nostr_event_set_created_at(event, 1700000001);
    nostr_subscription_dispatch_event(sub, event);
    void *delivered = NULL;
    assert(go_channel_try_receive(sub->events, &delivered) == 0 && delivered == event);
    nostr_event_free(delivered);
    AsyncCleanupHandle *cleanup = NULL;
    if (retirement == 0) nostr_subscription_unsubscribe(sub);
    else if (retirement == 1) nostr_subscription_free(sub);
    else {
        cleanup = nostr_subscription_free_async(sub, 5000); assert(cleanup);
        assert(nostr_subscription_cleanup_wait(cleanup, 5000));
        nostr_subscription_cleanup_abandon(cleanup);
    }
    nostr_filters_free(filters); /* enqueue must not touch these ever again */
    assert(go_channel_get_depth(q) == q->capacity); /* still no writer progress */
    for (size_t i = 0; i < q->capacity; i++) free(drain(relay));
    char *req = drain(relay); assert(strstr(req, "REQ") && strstr(req, id)); free(req);
    char *close = drain(relay); assert(strstr(close, "CLOSE") && strstr(close, id)); free(close);
    assert(pthread_join(thread, NULL) == 0);
    assert(!args.result && args.error); free_error(args.error);
    if (retirement == 0) nostr_subscription_free(sub);
    free(id); nostr_relay_unref(relay);
}
int main(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);
    for (int refire = 0; refire < 2; refire++)
        for (int retirement = 0; retirement < 3; retirement++) check(refire, retirement);
    return 0;
}
