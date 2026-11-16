#ifndef FIPSD_CRYPTO_WORKER_H
#define FIPSD_CRYPTO_WORKER_H
#include "node/worker_set.h"
/* Pure worker-side operations: no session, peer, counter allocator or window. */
void fipsd_encrypt_worker_execute(fipsd_crypto_job *);
void fipsd_decrypt_worker_execute(fipsd_crypto_job *);
#endif
