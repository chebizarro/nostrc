#include "node/worker_set.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fipsd_crypto_job *seal_job(const fips_aead_key *key, uint64_t counter, uint64_t shard)
{
    fipsd_crypto_job *j = calloc(1, sizeof(*j)); assert(j);
    j->operation = FIPSD_CRYPTO_SEAL; j->counter = j->token = counter; j->shard = shard;
    fips_cipher_state_init(&j->key, key);
    j->input = calloc(1, 8); j->output = malloc(24); assert(j->input && j->output);
    j->input_size = 8; j->capacity = 24;
    return j;
}
/* nostrc-u19cz.3: an inline run is refused while its shard's worker holds an
 * uncompleted job, so it can never overtake that worker's completions; a
 * shard whose worker is idle runs on the caller and completes in order. */
static void inline_order(void)
{
    fipsd_runtime_config cfg; fipsd_runtime_config_default(&cfg); cfg.workers = 2; cfg.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&cfg); assert(rt && fipsd_runtime_start(rt) == 0);
    fipsd_worker_set *s = fipsd_worker_set_create(2, 1); assert(s);
    fipsd_crypto_job unstarted = {.operation = FIPSD_CRYPTO_SEAL};
    assert(fipsd_worker_set_run_inline(s, &unstarted) == -1); /* no live worker yet */
    assert(fipsd_worker_set_start(s, rt) == 2);
    uint8_t bytes[32] = {3}; fips_aead_key key; assert(fips_aead_key_init(&key, bytes) == FIPS_AEAD_OK);
    uint64_t busy = FIPSD_JOB_SHARD(0), idle = FIPSD_JOB_SHARD(1);
    /* A sentinel fills the one-slot results channel: worker 0's job stays in
     * flight (blocked delivering) until the sentinel is received. */
    static int sentinel;
    assert(go_channel_try_send(fipsd_worker_set_results(s), &sentinel) == 0);
    assert(fipsd_worker_set_dispatch(s, seal_job(&key, 0, busy)) == 0);
    fipsd_crypto_job *refused = seal_job(&key, 1, busy);
    assert(fipsd_worker_set_run_inline(s, refused) == -1 && refused->output_size == 0);
    void *value; fipsd_crypto_job *j;
    assert(go_channel_receive(fipsd_worker_set_results(s), &value) == 0 && value == &sentinel);
    assert(go_channel_receive(fipsd_worker_set_results(s), &value) == 0); j = value;
    assert(j->token == 0 && j->worker == 0 && j->result == FIPS_NOISE_OK); fipsd_crypto_job_destroy(j);
    fipsd_crypto_job_destroy(refused);
    /* Worker 1 never had a job: the inline run is complete on return. */
    fipsd_crypto_job *mine = seal_job(&key, 3, idle);
    assert(fipsd_worker_set_run_inline(s, mine) == 0);
    assert(go_channel_try_receive(fipsd_worker_set_results(s), &value) == 0 && value == mine);
    assert(mine->worker == 1 && mine->result == FIPS_NOISE_OK && mine->output_size == 24);
    fipsd_crypto_job_destroy(mine);
    fipsd_runtime_shutdown(rt); fipsd_worker_set_stop(s);
    assert(go_channel_receive(fipsd_worker_set_results(s), &value) != 0); /* EOF */
    assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    fips_aead_key_destroy(&key); fipsd_worker_set_destroy(s); fipsd_runtime_destroy(rt);
}
int main(void)
{
    inline_order();
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
