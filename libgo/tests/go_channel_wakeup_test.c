#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
/* nostrc-v624: force stale MPMC snapshots against real blocking peers. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <nsync.h>
#include "channel.h"

static void snapshot_hook(GoChannel *, bool);
static void observed_wait(nsync_cv *, nsync_mu *);
#undef NOSTR_REFINED_SIGNALING
#define NOSTR_REFINED_SIGNALING 0
#define NOSTR_CHANNEL_MPMC_SLOTS 1
#define NOSTR_MPMC_SNAPSHOT_HOOK snapshot_hook
#define NOSTR_METRICS_API
#define NOSTR_ENABLE_METRICS 0
#define nsync_cv_wait observed_wait
#include "../src/channel.c"
#undef nsync_cv_wait

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool snapshot, release_snapshot, waiting, completed;
static _Thread_local int role;
static bool send_first;
static GoChannel *channel;

static void snapshot_hook(GoChannel *c, bool sending) {
    if (role != 1 || c != channel || sending != send_first) return;
    pthread_mutex_lock(&mutex);
    snapshot = true;
    pthread_cond_broadcast(&changed);
    while (!release_snapshot) pthread_cond_wait(&changed, &mutex);
    pthread_mutex_unlock(&mutex);
}

static void observed_wait(nsync_cv *cv, nsync_mu *mu) {
    if (role == 2) {
        pthread_mutex_lock(&mutex);
        waiting = true;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&mutex);
    }
    nsync_cv_wait(cv, mu);
}

static bool wait_for(bool *predicate) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5; /* failure watchdog only */
    pthread_mutex_lock(&mutex);
    int rc = 0;
    while (!*predicate && rc == 0)
        rc = pthread_cond_timedwait(&changed, &mutex, &deadline);
    bool result = *predicate;
    pthread_mutex_unlock(&mutex);
    return result;
}

static void *transfer(void *unused) {
    (void)unused;
    role = 1;
    void *value = NULL;
    int rc = send_first ? go_channel_try_send(channel, (void *)(uintptr_t)2)
                        : go_channel_try_receive(channel, &value);
    assert(rc == 0);
    return NULL;
}

static void *blocking_peer(void *unused) {
    (void)unused;
    role = 2;
    void *value = NULL;
    int rc = send_first ? go_channel_receive(channel, &value)
                        : go_channel_send(channel, (void *)(uintptr_t)3);
    pthread_mutex_lock(&mutex);
    completed = rc == 0;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
    return NULL;
}

static void run(bool sending) {
    send_first = sending;
    snapshot = release_snapshot = waiting = completed = false;
    channel = go_channel_create(2);
    assert(channel && channel->slot_seq);
    assert(go_channel_try_send(channel, (void *)(uintptr_t)1) == 0);
    pthread_t first, peer;
    assert(pthread_create(&first, NULL, transfer, NULL) == 0);
    assert(wait_for(&snapshot));
    /* Invalidate the saved occupancy without changing the first thread's ticket. */
    void *value = NULL;
    if (sending) assert(go_channel_try_receive(channel, &value) == 0);
    else assert(go_channel_try_send(channel, (void *)(uintptr_t)2) == 0);
    assert(pthread_create(&peer, NULL, blocking_peer, NULL) == 0);
    assert(wait_for(&waiting));
    /* The peer has released this mutex inside its actual condition wait. */
    nsync_mu_lock(&channel->mutex);
    nsync_mu_unlock(&channel->mutex);
    pthread_mutex_lock(&mutex);
    release_snapshot = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
    pthread_join(first, NULL);
    bool woke = wait_for(&completed);
    go_channel_close(channel); /* also permits deterministic cleanup on failure */
    pthread_join(peer, NULL);
    go_channel_unref(channel);
    assert(woke && "MPMC transfer skipped the blocking peer's wakeup");
}

int main(int argc, char **argv) {
    if (argc == 1 || strcmp(argv[1], "send") == 0) run(true);
    if (argc == 1 || strcmp(argv[1], "receive") == 0) run(false);
    return 0;
}
