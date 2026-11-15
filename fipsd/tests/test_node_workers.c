#include "node/worker_set.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    fipsd_runtime_config cfg; fipsd_runtime_config_default(&cfg); cfg.workers = 2; cfg.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&cfg); assert(rt && fipsd_runtime_start(rt) == 0);
    fipsd_worker_set *s = fipsd_worker_set_create(2, 64); assert(s);
    assert(fipsd_worker_set_start(s, rt) == 2);
    uint8_t bytes[32] = {1}; fips_aead_key key;
    assert(fips_aead_key_init(&key, bytes) == FIPS_AEAD_OK);
    fips_cipher_state owner; fips_cipher_state_init(&owner, &key);
    for (size_t i = 0; i < 128; ++i) {
        fipsd_crypto_job *j = calloc(1, sizeof(*j)); assert(j);
        j->operation = FIPSD_CRYPTO_SEAL; j->token = i;
        assert(fips_cipher_state_take_counter(&owner, &j->counter) == FIPS_NOISE_OK);
        fips_cipher_state_init(&j->key, &key); /* independent per-job copy */
        if (i & 1) {
            j->operation = FIPSD_CRYPTO_OPEN;
            j->input = malloc(24); j->output = malloc(8); assert(j->input && j->output);
            j->input_size = 24; j->capacity = 8;
            uint8_t plain[8]; memset(plain, (int)i, sizeof(plain)); size_t sealed;
            assert(fips_aead_seal(&key, j->counter, NULL, 0, plain, sizeof(plain),
                                  j->input, 24, &sealed) == FIPS_AEAD_OK);
            if ((i & 3) == 3) j->input[0] ^= 1; /* authentication refusal */
        } else {
            j->input = malloc(8); j->output = malloc(24); assert(j->input && j->output);
            j->input_size = 8; j->capacity = 24; memset(j->input, (int)i, 8);
        }
        assert(fipsd_worker_set_dispatch(s, j) == 0);
    }
    assert(fips_cipher_state_counter(&owner) == 128);
    /* Cancellation does not discard accepted work. Closed queues drain in
     * parallel while the dispatcher (here main) keeps receiving results. */
    fipsd_runtime_shutdown(rt); fipsd_worker_set_stop(s);
    bool seen[128] = {false}; size_t workers[2] = {0}, received = 0;
    void *value;
    while (go_channel_receive(fipsd_worker_set_results(s), &value) == 0) {
        fipsd_crypto_job *j = value;
        assert(j->token < 128 && !seen[j->token] && j->worker < 2);
        seen[j->token] = true; ++received; ++workers[j->worker];
        if (j->operation == FIPSD_CRYPTO_SEAL) {
            assert(j->result == FIPS_NOISE_OK && j->output_size == 24);
            uint8_t plain[8]; size_t size;
            assert(fips_aead_open(&key, j->counter, NULL, 0, j->output, j->output_size,
                                  plain, sizeof(plain), &size) == FIPS_AEAD_OK);
            assert(size == 8 && plain[0] == j->token);
        } else if ((j->token & 3) == 3) {
            assert(j->result != FIPS_NOISE_OK && j->output_size == 0);
            for (size_t i = 0; i < 8; ++i) assert(j->output[i] == 0);
        } else assert(j->result == FIPS_NOISE_OK && j->output_size == 8 && j->output[0] == j->token);
        fipsd_crypto_job_destroy(j);
    }
    assert(received == 128 && workers[0] == 64 && workers[1] == 64);
    assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK && fipsd_worker_set_live(s) == 0);
    fipsd_crypto_job refused = {0}; assert(fipsd_worker_set_dispatch(s, &refused) == -1);
    fips_cipher_state_destroy(&owner); fips_aead_key_destroy(&key);
    fipsd_worker_set_destroy(s); fipsd_runtime_destroy(rt);
    puts("node workers: 128 explicit-counter jobs, two workers, shutdown drained"); return 0;
}
