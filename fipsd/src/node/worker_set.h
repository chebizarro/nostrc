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
    fipsd_crypto_operation operation;
    fips_cipher_state key;
    uint64_t counter, token; /* token lets dispatcher reject stale completions */
    uint8_t *ad, *input, *output;
    size_t ad_size, input_size, capacity, output_size, worker;
    fips_noise_status result;
} fipsd_crypto_job;
typedef struct fipsd_worker_set fipsd_worker_set;
fipsd_worker_set *fipsd_worker_set_create(size_t workers, size_t capacity);
/* Returns number successfully spawned (short pools degrade, no restart).
 * Results channel is bounded and owned by set; the dispatcher must continuously
 * drain it through shutdown, until it closes. Partial pools are usable. */
size_t fipsd_worker_set_start(fipsd_worker_set *, fipsd_runtime *);
GoChannel *fipsd_worker_set_results(fipsd_worker_set *);
/* Dispatcher-only distribution counter. Nonblocking bounded dispatch; refusal
 * leaves ownership with caller (inline fallback is caller policy). */
int fipsd_worker_set_dispatch(fipsd_worker_set *, fipsd_crypto_job *);
size_t fipsd_worker_set_live(const fipsd_worker_set *);
/* Close input queues. Accepted jobs drain and emit results, despite root
 * cancellation. Close is safe against sends; caller stops producers first. */
void fipsd_worker_set_stop(fipsd_worker_set *);
/* After runtime join AND results drained. Frees leftover jobs on failed start. */
void fipsd_worker_set_destroy(fipsd_worker_set *);
void fipsd_crypto_job_destroy(fipsd_crypto_job *);
#endif
