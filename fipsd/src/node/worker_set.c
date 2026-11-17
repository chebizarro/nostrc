#include "node/worker_set.h"
#include "node/crypto_worker.h"
#include <libgo/fiber.h>
#include <sodium.h>
#include <stdlib.h>

typedef struct worker {
    struct fipsd_worker_set *set;
    GoChannel *in;
    size_t index;
    _Atomic bool live;
} worker;
struct fipsd_worker_set {
    worker *workers;
    size_t count, next;
    GoChannel *results;
    _Atomic size_t remaining;
    bool started;
};
void fipsd_crypto_job_destroy(fipsd_crypto_job *j)
{
    if (!j) return;
    fips_cipher_state_destroy(&j->key);
    if (j->recycle) {
        if (j->ad) sodium_memzero(j->ad, j->ad_size);
        if (j->input) sodium_memzero(j->input, j->input_size);
        if (j->output) sodium_memzero(j->output, j->capacity);
        j->recycle(j);
        return;
    }
    if (j->ad) { sodium_memzero(j->ad, j->ad_size); free(j->ad); }
    if (j->input) { sodium_memzero(j->input, j->input_size); free(j->input); }
    if (j->output) { sodium_memzero(j->output, j->capacity); free(j->output); }
    sodium_memzero(j, sizeof(*j)); free(j);
}
static void run(void *arg)
{
    worker *w = arg;
    void *value;
    unsigned streak = 0;
    while (go_channel_receive(w->in, &value) == 0) {
        fipsd_crypto_job *j = value;
        FIPSD_PERF_JOB_WAIT(j);
        j->worker = w->index;
        if (j->operation == FIPSD_CRYPTO_SEAL) fipsd_encrypt_worker_execute(j);
        else fipsd_decrypt_worker_execute(j);
        if (j->result == FIPS_NOISE_OK && j->worker_counts) atomic_fetch_add(&j->worker_counts[w->index], 1);
        fips_cipher_state_destroy(&j->key);
        if (go_channel_send(w->set->results, j) != 0) fipsd_crypto_job_destroy(j);
        /* Stay fair on a shared scheduler thread without a reschedule per job. */
        if (++streak == 8) { streak = 0; gof_yield(); }
    }
    atomic_store(&w->live, false);
    if (atomic_fetch_sub(&w->set->remaining, 1) == 1) go_channel_close(w->set->results);
}
fipsd_worker_set *fipsd_worker_set_create(size_t n, size_t capacity)
{
    if (!n || n > 64 || !capacity || capacity > 65536) return NULL;
    fipsd_worker_set *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->count = n; s->workers = calloc(n, sizeof(*s->workers));
    s->results = go_channel_create(capacity);
    if (!s->workers || !s->results) { fipsd_worker_set_destroy(s); return NULL; }
    for (size_t i = 0; i < n; ++i) {
        s->workers[i] = (worker){.set = s, .index = i, .in = go_channel_create(capacity)};
        if (!s->workers[i].in) { fipsd_worker_set_destroy(s); return NULL; }
    }
    return s;
}
size_t fipsd_worker_set_start(fipsd_worker_set *s, fipsd_runtime *rt)
{
    if (s->started) return 0;
    s->started = true;
    /* Sentinel prevents an early exit from closing results during spawning. */
    atomic_store(&s->remaining, 1);
    size_t live = 0;
    for (size_t i = 0; i < s->count; ++i) {
        worker *w = &s->workers[i];
        atomic_fetch_add(&s->remaining, 1); atomic_store(&w->live, true);
        if (fipsd_runtime_spawn(rt, run, w) == 0) ++live;
        else {
            atomic_store(&w->live, false); go_channel_close(w->in);
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
    size_t first = s->next++ % s->count;
    for (size_t n = 0; n < s->count; ++n) {
        size_t i = (first + n) % s->count;
        if (atomic_load(&s->workers[i].live) && go_channel_try_send(s->workers[i].in, j) == 0) return 0;
    }
    return -1;
}
void fipsd_worker_set_stop(fipsd_worker_set *s)
{
    if (s && s->workers)
        for (size_t i = 0; i < s->count; ++i)
            if (s->workers[i].in) go_channel_close(s->workers[i].in);
}
void fipsd_worker_set_destroy(fipsd_worker_set *s)
{
    if (!s) return;
    fipsd_worker_set_stop(s);
    if (s->workers) {
        for (size_t i = 0; i < s->count; ++i) {
            void *j;
            if (!s->workers[i].in) continue;
            while (go_channel_try_receive(s->workers[i].in, &j) == 0) fipsd_crypto_job_destroy(j);
            go_channel_unref(s->workers[i].in);
        }
    }
    if (s->results) {
        void *j;
        while (go_channel_try_receive(s->results, &j) == 0) fipsd_crypto_job_destroy(j);
        go_channel_unref(s->results);
    }
    free(s->workers); free(s);
}
