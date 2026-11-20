#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* pthread_setname_np */
#endif
#include "node/worker_set.h"
#include "node/crypto_worker.h"
#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>

/* Crypto workers are OS threads (nostrc-u19cz.3). As scheduler fibers they
 * shared the dispatcher's thread: libgo wakes a fiber on its last worker and
 * steals only from deep queues, so packet crypto never left the dispatcher's
 * core. Each worker owns a bounded FIFO ring under a mutex/condvar (pthreads,
 * which are winpthreads on Windows, as libgo's blocking executor uses). A
 * producer signals only when it makes a waiting worker's empty queue
 * non-empty, not per job (upstream encrypt_worker.rs MacWorkerSender: a
 * per-packet semaphore signal dominated its dispatch path). An idle worker
 * blocks on the condvar: no timer, no spinning. Completions go to the bounded
 * results channel with a blocking send, which is safe from a plain thread
 * (OS-thread waits) and wakes the dispatcher's select (gof_hook_make_runnable,
 * as the blocking executor does). */
#define FIPSD_WORKER_BATCH 32u
typedef struct worker {
    struct fipsd_worker_set *set;
    pthread_mutex_t mu;
    pthread_cond_t ready;
    fipsd_crypto_job **ring; /* mu */
    size_t head, count, index; /* head/count: mu */
    bool waiting, closed, started; /* waiting/closed: mu; started: owner */
    bool sync; /* mu/ready initialized */
    pthread_t thread;
    _Atomic bool live;
    /* Accepted and not yet completed: queued, held in a batch or running. A
     * completion is on the results channel before this drops. */
    _Atomic size_t flight;
} worker;
struct fipsd_worker_set {
    worker *workers;
    size_t count, capacity, next;
    GoChannel *results;
    _Atomic size_t remaining;
    bool started;
};
void fipsd_crypto_job_destroy(fipsd_crypto_job *j)
{
    if (!j) return;
    fips_cipher_state_destroy(&j->key);
    if (j->recycle) { j->recycle(j); return; }
    if (j->ad) { sodium_memzero(j->ad, j->ad_size); free(j->ad); }
    if (j->input) { sodium_memzero(j->input, j->input_size); free(j->input); }
    if (j->output) { sodium_memzero(j->output, j->capacity); free(j->output); }
    sodium_memzero(j, sizeof(*j)); free(j);
}
static void name_thread(size_t index)
{
    char name[16];
    (void)snprintf(name, sizeof name, "fipsd-crypto-%zu", index);
#if defined(__APPLE__)
    (void)pthread_setname_np(name);
#elif defined(__linux__)
    (void)pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}
static void run_job(worker *w, fipsd_crypto_job *j)
{
    FIPSD_PERF_JOB_WAIT(j);
    j->worker = w->index;
    if (j->operation == FIPSD_CRYPTO_SEAL) fipsd_encrypt_worker_execute(j);
    else fipsd_decrypt_worker_execute(j);
    if (j->result == FIPS_NOISE_OK && j->worker_counts) atomic_fetch_add(&j->worker_counts[w->index], 1);
    fips_cipher_state_destroy(&j->key);
}
static void execute(worker *w, fipsd_crypto_job *j)
{
    run_job(w, j);
    if (go_channel_send(w->set->results, j) != 0) fipsd_crypto_job_destroy(j);
    atomic_fetch_sub_explicit(&w->flight, 1, memory_order_release);
}
static void *run(void *arg)
{
    worker *w = arg;
    name_thread(w->index);
    fipsd_crypto_job *batch[FIPSD_WORKER_BATCH];
    for (;;) {
        size_t n = 0;
        pthread_mutex_lock(&w->mu);
        while (!w->count && !w->closed) {
            w->waiting = true;
            pthread_cond_wait(&w->ready, &w->mu);
            w->waiting = false;
        }
        /* Closed queues still drain: accepted jobs always produce results. */
        while (w->count && n < FIPSD_WORKER_BATCH) {
            batch[n++] = w->ring[w->head];
            w->head = (w->head + 1) % w->set->capacity; --w->count;
        }
        pthread_mutex_unlock(&w->mu);
        if (!n) break;
        for (size_t i = 0; i < n; ++i) execute(w, batch[i]);
    }
    atomic_store(&w->live, false);
    if (atomic_fetch_sub(&w->set->remaining, 1) == 1) go_channel_close(w->set->results);
    return NULL;
}
/* Dispatcher side. Refusal (closed or full) leaves ownership with the caller. */
static int push(worker *w, fipsd_crypto_job *j)
{
    pthread_mutex_lock(&w->mu);
    if (w->closed || w->count == w->set->capacity) { pthread_mutex_unlock(&w->mu); return -1; }
    w->ring[(w->head + w->count) % w->set->capacity] = j;
    atomic_fetch_add_explicit(&w->flight, 1, memory_order_relaxed);
    /* A waiting worker found its queue empty; later pushes ride that wake. */
    bool wake = w->waiting && w->count == 0;
    ++w->count;
    pthread_mutex_unlock(&w->mu);
    if (wake) pthread_cond_signal(&w->ready);
    return 0;
}
static void close_queue(worker *w)
{
    if (!w->sync) return;
    pthread_mutex_lock(&w->mu);
    w->closed = true;
    pthread_mutex_unlock(&w->mu);
    pthread_cond_broadcast(&w->ready);
}
fipsd_worker_set *fipsd_worker_set_create(size_t n, size_t capacity)
{
    if (!n || n > 64 || !capacity || capacity > 65536) return NULL;
    fipsd_worker_set *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->count = n; s->capacity = capacity; s->workers = calloc(n, sizeof(*s->workers));
    s->results = go_channel_create(capacity);
    if (!s->workers || !s->results) { fipsd_worker_set_destroy(s); return NULL; }
    for (size_t i = 0; i < n; ++i) {
        worker *w = &s->workers[i];
        *w = (worker){.set = s, .index = i, .ring = calloc(capacity, sizeof(*w->ring))};
        if (!w->ring) { fipsd_worker_set_destroy(s); return NULL; }
        if (pthread_mutex_init(&w->mu, NULL)) { fipsd_worker_set_destroy(s); return NULL; }
        if (pthread_cond_init(&w->ready, NULL)) {
            pthread_mutex_destroy(&w->mu); fipsd_worker_set_destroy(s); return NULL;
        }
        w->sync = true;
    }
    return s;
}
size_t fipsd_worker_set_start(fipsd_worker_set *s, fipsd_runtime *rt)
{
    (void)rt;
    if (s->started) return 0;
    s->started = true;
    /* Sentinel prevents an early exit from closing results during spawning. */
    atomic_store(&s->remaining, 1);
    size_t live = 0;
    for (size_t i = 0; i < s->count; ++i) {
        worker *w = &s->workers[i];
        atomic_fetch_add(&s->remaining, 1); atomic_store(&w->live, true);
        if (pthread_create(&w->thread, NULL, run, w) == 0) { w->started = true; ++live; }
        else {
            atomic_store(&w->live, false); close_queue(w);
            atomic_fetch_sub(&s->remaining, 1);
        }
    }
    if (atomic_fetch_sub(&s->remaining, 1) == 1) go_channel_close(s->results);
    return live;
}
GoChannel *fipsd_worker_set_results(fipsd_worker_set *s) { return s->results; }
size_t fipsd_worker_set_live(const fipsd_worker_set *s)
{
    size_t n = 0;
    for (size_t i = 0; i < s->count; ++i) n += atomic_load(&s->workers[i].live);
    return n;
}
int fipsd_worker_set_dispatch(fipsd_worker_set *s, fipsd_crypto_job *j)
{
    if (!j || (j->operation != FIPSD_CRYPTO_SEAL && j->operation != FIPSD_CRYPTO_OPEN)) return -1;
    FIPSD_PERF_JOB_QUEUE(j);
    /* A sharded flow keeps its worker; the others are tried only when it has
     * exited or (beyond the packet pool, which bounds in-flight jobs) is full. */
    size_t first = j->shard ? (size_t)((uint32_t)j->shard % s->count) : s->next++ % s->count;
    for (size_t n = 0; n < s->count; ++n) {
        worker *w = &s->workers[(first + n) % s->count];
        if (atomic_load(&w->live) && push(w, j) == 0) return 0;
    }
    return -1;
}
int fipsd_worker_set_run_inline(fipsd_worker_set *s, fipsd_crypto_job *j)
{
    if (!j || (j->operation != FIPSD_CRYPTO_SEAL && j->operation != FIPSD_CRYPTO_OPEN)) return -1;
    worker *w = &s->workers[j->shard ? (uint32_t)j->shard % s->count : s->next % s->count];
    /* Zero in flight (acquire): each of this worker's completions is already
     * on the channel, so this one queues behind them. */
    if (!atomic_load(&w->live) || atomic_load_explicit(&w->flight, memory_order_acquire)) return -1;
    FIPSD_PERF_JOB_QUEUE(j);
    run_job(w, j);
    if (go_channel_try_send(s->results, j) != 0) fipsd_crypto_job_destroy(j);
    return 0;
}
void fipsd_worker_set_stop(fipsd_worker_set *s)
{
    if (s && s->workers)
        for (size_t i = 0; i < s->count; ++i) close_queue(&s->workers[i]);
}
void fipsd_worker_set_destroy(fipsd_worker_set *s)
{
    if (!s) return;
    fipsd_worker_set_stop(s);
    /* Workers finish the jobs they hold; a send into a full results queue
     * that nobody drains any more is released by closing it. */
    if (s->results) go_channel_close(s->results);
    if (s->workers) {
        for (size_t i = 0; i < s->count; ++i) {
            worker *w = &s->workers[i];
            if (w->started) (void)pthread_join(w->thread, NULL);
            for (; w->count; --w->count, w->head = (w->head + 1) % s->capacity)
                fipsd_crypto_job_destroy(w->ring[w->head]);
            if (w->sync) { pthread_cond_destroy(&w->ready); pthread_mutex_destroy(&w->mu); }
            free(w->ring);
        }
    }
    if (s->results) {
        void *j;
        while (go_channel_try_receive(s->results, &j) == 0) fipsd_crypto_job_destroy(j);
        go_channel_unref(s->results);
    }
    free(s->workers); free(s);
}
