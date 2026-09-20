/* White-box lifecycle test of the actual production dispatch/cleanup helpers.
 * Including the implementation keeps private request ownership out of the ABI. */
#include "../src/core/nip46_session.c"
#include <assert.h>

static void test_terminal_delivery(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    PendingRequest *pr = pending_request_new("request", 1000);
    assert(s && pr);
    pending_request_add(s, pr);
    char *first = strdup("first");
    assert(pending_request_deliver(s, "request", first));
    char *second = strdup("second");
    assert(!pending_request_deliver(s, "request", second));
    free(second);
    void *received = NULL;
    assert(go_channel_try_receive(pr->response_chan, &received) == 0);
    assert(received == first);
    free(received);
    pending_request_cancel(s, "request");
    assert(!s->pending_requests);

    pr = pending_request_new("expired", 1);
    assert(pr);
    pr->submit_time_us -= 10000; /* Expired without a wall-clock sleep. */
    pending_request_add(s, pr);
    char *late = strdup("late");
    assert(!pending_request_deliver(s, "expired", late));
    free(late);
    pending_request_cancel(s, "expired");

    pr = pending_request_new("cancelled", 1000);
    assert(pr);
    pending_request_add(s, pr);
    nostr_nip46_client_cancel_all(s);
    char *cancelled = strdup("cancelled");
    assert(!pending_request_deliver(s, "cancelled", cancelled));
    free(cancelled);
    pending_request_cancel(s, "cancelled");
    nostr_nip46_session_free(s);
}

typedef struct {
    NostrNip46Session *session;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned ready;
} Race;

static void rendezvous(Race *race) {
    pthread_mutex_lock(&race->mutex);
    race->ready++;
    pthread_cond_broadcast(&race->cond);
    while (race->ready < 2) pthread_cond_wait(&race->cond, &race->mutex);
    pthread_mutex_unlock(&race->mutex);
}

static void *deliver(void *arg) {
    Race *race = arg;
    char *response = strdup("response");
    rendezvous(race);
    if (!pending_request_deliver(race->session, "race", response)) free(response);
    return NULL;
}

static void *release(void *arg) {
    Race *race = arg;
    rendezvous(race);
    pending_request_cancel(race->session, "race");
    return NULL;
}

static void test_delivery_release_race(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    assert(s);
    for (int i = 0; i < 500; i++) {
        PendingRequest *pr = pending_request_new("race", 1000);
        assert(pr);
        pending_request_add(s, pr);
        Race race = { s, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0 };
        pthread_t sender, receiver;
        assert(pthread_create(&sender, NULL, deliver, &race) == 0);
        assert(pthread_create(&receiver, NULL, release, &race) == 0);
        pthread_join(sender, NULL);
        pthread_join(receiver, NULL);
        assert(!s->pending_requests);
        pthread_cond_destroy(&race.cond);
        pthread_mutex_destroy(&race.mutex);
    }
    nostr_nip46_session_free(s);
}

int main(void) {
    test_terminal_delivery();
    test_delivery_release_race();
    puts("pending lifetime: PASS");
    return 0;
}
