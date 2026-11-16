#include "node/crypto_worker.h"
void fipsd_encrypt_worker_execute(fipsd_crypto_job *j)
{
    j->result = fips_cipher_state_seal_with_counter(&j->key, j->counter,
        j->ad, j->ad_size, j->input, j->input_size,
        j->output, j->capacity, &j->output_size);
}
