#include "node/perf_profile.h"
#ifndef FIPSD_WORKER_SET_H
#define FIPSD_WORKER_SET_H
#include "runtime.h"
#include <fips/noise/cipher.h>

typedef enum fipsd_crypto_operation { FIPSD_CRYPTO_SEAL, FIPSD_CRYPTO_OPEN } fipsd_crypto_operation;
/* Linear owned job. Dispatcher supplies a per-job key clone and a nonce
 * reserved by the sole send-counter owner, across ALL clones of this key.
 * Workers never auto-seal or access peer/session/replay state. OPEN authenticates
 * only; dispatcher must recheck replay AND epoch/session validity before accept.
 * AD/input/output are independent owned buffers, never borrowed peer memory. */
typedef struct fipsd_crypto_job {
    FIPSD_PERF_JOB_FIELDS
    fipsd_crypto_operation operation;
    fips_cipher_state key;
    uint64_t counter, token; /* token lets dispatcher reject stale completions */
    uint8_t *ad, *input, *output;
    size_t ad_size, input_size, capacity, output_size, worker;
    /* Worker affinity. 0: any worker (round robin). Otherwise FIPSD_JOB_SHARD
     * of a flow key: jobs with one key run, and complete, in submission order
     * on one worker while it is live and has queue room. */
    uint64_t shard;
    fips_noise_status result;
    /* Optional slab return, called after the key is destroyed. The owner wipes
     * whatever its buffers held; without it, destroy wipes and frees them. */
    void (*recycle)(struct fipsd_crypto_job *);
    /* Optional step on the executing thread (a worker, or the dispatcher for
     * an inline run) after the AEAD and key destruction, before the completion
     * is queued: e.g. the owner's transport send. It may use only the job's own
     * buffers and objects safe from any thread; never dispatcher state. */
    void (*after)(struct fipsd_crypto_job *);
    void *owner;
    _Atomic uint64_t *worker_counts; /* optional 64 lane success counters */
} fipsd_crypto_job;
#define FIPSD_JOB_SHARD(key) ((uint64_t)1 << 63 | (uint64_t)(uint32_t)(key))
typedef struct fipsd_worker_set fipsd_worker_set;
fipsd_worker_set *fipsd_worker_set_create(size_t workers, size_t capacity);
/* Starts one OS thread per worker, not a scheduler fiber (nostrc-u19cz.3), so
 * jobs run in parallel with the dispatcher; rt is unused. An idle worker
 * blocks on its queue: no timer, no polling. Returns the number started
 * (short pools degrade, no restart). Results channel is bounded and owned by
 * set; the dispatcher must continuously drain it through shutdown, until it
 * closes. Partial pools are usable. */
size_t fipsd_worker_set_start(fipsd_worker_set *, fipsd_runtime *);
GoChannel *fipsd_worker_set_results(fipsd_worker_set *);
/* Dispatcher-only distribution counter. Nonblocking bounded dispatch; refusal
 * leaves ownership with caller (inline fallback is caller policy). */
int fipsd_worker_set_dispatch(fipsd_worker_set *, fipsd_crypto_job *);
/* Dispatcher-only. Runs j on the calling thread, as the worker dispatch would
 * pick, and queues its completion on the results channel behind everything
 * that worker has produced. Refused (-1: not run, ownership kept) unless that
 * worker is live with nothing in flight, so a shard's completions keep their
 * order, and unless the open results channel has a slot no accepted job can
 * still claim, so the completion is never lost and the call never blocks.
 * Dispatch the refused job instead. For when a handoff buys no parallelism,
 * only a thread wake each way (nostrc-u19cz.3). */
int fipsd_worker_set_run_inline(fipsd_worker_set *, fipsd_crypto_job *);
size_t fipsd_worker_set_live(const fipsd_worker_set *);
/* Close input queues. Accepted jobs drain and emit results, despite root
 * cancellation. Close is safe against sends; caller stops producers first. */
void fipsd_worker_set_stop(fipsd_worker_set *);
/* After runtime join AND results drained: joins the threads, then frees
 * leftover jobs (e.g. after a failed start). */
void fipsd_worker_set_destroy(fipsd_worker_set *);
void fipsd_crypto_job_destroy(fipsd_crypto_job *);
#endif
