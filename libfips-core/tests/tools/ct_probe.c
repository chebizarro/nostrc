#define _POSIX_C_SOURCE 200809L /* clock_gettime under strict C11 on glibc */
/* Timing probe for fips_aead_open failures (nostrc-pwo3h.8).
 *
 * Question: does the time to REJECT a frame depend on WHICH byte of the
 * ciphertext, tag, AD or counter was tampered? It must not: Poly1305 runs over
 * the whole input, the tag check is constant time (libsodium crypto_verify_16
 * or OpenSSL CRYPTO_memcmp, per FIPS_AEAD_BACKEND) and the failure path zeroes a
 * length-determined range, so only the length may show.
 *
 * Method (dudect-style, Reparaz/Balasch/Verbauwhede 2016): two input classes
 * per experiment, chosen pseudo-randomly per measurement so drift affects both
 * equally; each measurement times BATCH opens of one prepared input to lift the
 * sample above timer quantisation; the slowest 1% of each class is cropped
 * (scheduler noise); Welch's t-test on the rest. |t| >= 4.5 is dudect's
 * decision threshold for "distinguishable". A positive control (reject versus
 * accept of the same length, which differs by one ChaCha20 pass) must be
 * detected, showing the probe has power on this machine.
 *
 * Not a ctest: wall-clock timing is machine dependent. Build target
 * fips_core_ct_probe, or compile against the AEAD sources alone with -O2 for a
 * run without sanitizer or coverage overhead (or -DFIPS_AEAD_BACKEND_SODIUM):
 *   clang -O2 -I include -DFIPS_AEAD_BACKEND_OPENSSL tests/tools/ct_probe.c \
 *       src/noise/aead.c src/noise/aead_primitive.c \
 *       $(pkg-config --cflags --libs libsodium libcrypto) -o ct_probe && ./ct_probe
 * Exit status 0 means every tamper class is indistinguishable and the control
 * is distinguishable. */
#include <fips/noise/aead.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PLAIN_LEN 1024u
#define CLEN (PLAIN_LEN + FIPS_NOISE_TAG_SIZE)
#define AD_LEN 16u
#define BATCH 16u
#define THRESHOLD 4.5
#define WARMUP 2000u

static uint64_t rng_state = UINT64_C(0x9e3779b97f4a7c15);
static uint64_t rng(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * UINT64_C(2685821657736338717);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

typedef struct stats { double mean, var; size_t n; } stats;

static stats cropped(uint64_t *v, size_t n)
{
    stats s = {0, 0, 0};
    if (n < 4) return s;
    qsort(v, n, sizeof(*v), cmp_u64);
    size_t keep = n - n / 100;
    double sum = 0;
    for (size_t i = 0; i < keep; ++i) sum += (double)v[i];
    s.mean = sum / (double)keep;
    for (size_t i = 0; i < keep; ++i) {
        double d = (double)v[i] - s.mean;
        s.var += d * d;
    }
    s.var /= (double)(keep - 1);
    s.n = keep;
    return s;
}

static double welch_t(stats a, stats b)
{
    double denominator = sqrt(a.var / (double)a.n + b.var / (double)b.n);
    return denominator > 0 ? (a.mean - b.mean) / denominator : 0;
}

/* Genuine frame shared by every experiment. */
static fips_aead_key key;
static uint8_t sealed[CLEN], genuine_ad[AD_LEN];
static const uint64_t genuine_counter = 7;

typedef struct input {
    uint8_t wire[CLEN], ad[AD_LEN];
    uint64_t counter;
} input;

static uint8_t nonzero_byte(void)
{
    uint8_t b;
    do b = (uint8_t)rng(); while (b == 0);
    return b;
}

static void genuine(input *in)
{
    memcpy(in->wire, sealed, CLEN);
    memcpy(in->ad, genuine_ad, AD_LEN);
    in->counter = genuine_counter;
}
/* Class preparations: class 0 is the FIXED tamper, class 1 the RANDOM one. */
static void ct_fixed_vs_random(unsigned cls, input *in)
{
    genuine(in);
    if (cls == 0) in->wire[0] ^= 1;
    else in->wire[rng() % PLAIN_LEN] ^= nonzero_byte();
}
static void tag_fixed_vs_random(unsigned cls, input *in)
{
    genuine(in);
    if (cls == 0) in->wire[PLAIN_LEN] ^= 1;
    else in->wire[PLAIN_LEN + rng() % FIPS_NOISE_TAG_SIZE] ^= nonzero_byte();
}
static void first_ct_vs_last_tag(unsigned cls, input *in)
{
    genuine(in);
    if (cls == 0) in->wire[0] ^= 1;
    else in->wire[CLEN - 1] ^= 1;
}
static void ad_fixed_vs_random(unsigned cls, input *in)
{
    genuine(in);
    if (cls == 0) in->ad[0] ^= 1;
    else in->ad[rng() % AD_LEN] ^= nonzero_byte();
}
static void counter_vs_tag(unsigned cls, input *in)
{
    genuine(in);
    if (cls == 0) in->counter ^= 1;
    else in->wire[CLEN - 1] ^= 1;
}
static void ct_vs_random_everything(unsigned cls, input *in)
{
    genuine(in);
    if (cls == 0) in->wire[0] ^= 1;
    else in->wire[rng() % CLEN] ^= nonzero_byte();
}
static void accept_vs_reject(unsigned cls, input *in)
{
    genuine(in);
    if (cls == 1) in->wire[CLEN - 1] ^= 1;
}

typedef struct experiment {
    const char *name;
    void (*prepare)(unsigned cls, input *in);
    bool control; /* Must be distinguishable. */
} experiment;

static const experiment experiments[] = {
    {"ciphertext byte 0 vs random ciphertext byte", ct_fixed_vs_random, false},
    {"tag byte 0 vs random tag byte", tag_fixed_vs_random, false},
    {"first ciphertext byte vs last tag byte", first_ct_vs_last_tag, false},
    {"AD byte 0 vs random AD byte", ad_fixed_vs_random, false},
    {"wrong counter vs last tag byte", counter_vs_tag, false},
    {"ciphertext byte 0 vs random byte anywhere", ct_vs_random_everything, false},
    {"CONTROL: accept vs reject (same length)", accept_vs_reject, true},
};

static uint64_t measure(const input *in)
{
    uint8_t out[PLAIN_LEN];
    size_t got;
    uint64_t start = now_ns();
    for (unsigned k = 0; k < BATCH; ++k)
        (void)fips_aead_open(&key, in->counter, in->ad, AD_LEN, in->wire, CLEN, out, sizeof(out), &got);
    return now_ns() - start;
}

static int run(const experiment *e, size_t measurements, uint64_t *samples[2])
{
    size_t n[2] = {0, 0};
    input in;
    for (size_t i = 0; i < WARMUP; ++i) {
        e->prepare((unsigned)(rng() & 1), &in);
        (void)measure(&in);
    }
    for (size_t i = 0; i < measurements; ++i) {
        unsigned cls = (unsigned)(rng() & 1);
        e->prepare(cls, &in);
        samples[cls][n[cls]++] = measure(&in);
    }
    stats s0 = cropped(samples[0], n[0]), s1 = cropped(samples[1], n[1]);
    double t = welch_t(s0, s1);
    bool distinguishable = fabs(t) >= THRESHOLD;
    bool ok = distinguishable == e->control;
    printf("%-46s n=%zu/%zu mean=%.1f/%.1f ns per %u opens  t=%+8.2f  %s\n", e->name, s0.n, s1.n,
           s0.mean, s1.mean, BATCH, t,
           e->control ? (ok ? "DETECTED (control OK)" : "NOT DETECTED (no power)")
                      : (ok ? "indistinguishable" : "LEAK?"));
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    size_t measurements = argc > 1 ? (size_t)strtoull(argv[1], NULL, 10) : 100000;
    if (measurements < 1000) measurements = 1000;
    uint8_t key_bytes[FIPS_AEAD_KEY_SIZE], plain[PLAIN_LEN];
    for (size_t i = 0; i < sizeof(key_bytes); ++i) key_bytes[i] = (uint8_t)(0x40 + i);
    for (size_t i = 0; i < sizeof(plain); ++i) plain[i] = (uint8_t)(i * 7);
    for (size_t i = 0; i < AD_LEN; ++i) genuine_ad[i] = (uint8_t)(0xa0 + i);
    if (fips_aead_key_init(&key, key_bytes) != FIPS_AEAD_OK) return 2;
    size_t clen;
    if (fips_aead_seal(&key, genuine_counter, genuine_ad, AD_LEN, plain, PLAIN_LEN, sealed, CLEN, &clen)
            != FIPS_AEAD_OK || clen != CLEN)
        return 2;
    uint64_t *samples[2] = {calloc(measurements, sizeof(uint64_t)), calloc(measurements, sizeof(uint64_t))};
    if (!samples[0] || !samples[1]) return 2;
    int failures = 0;
    printf("fips_aead_open failure timing, %u-byte plaintext, %zu measurements of %u opens each, "
           "cropped 1%%, |t| threshold %.1f\n", PLAIN_LEN, measurements, BATCH, THRESHOLD);
    for (size_t i = 0; i < sizeof(experiments) / sizeof(experiments[0]); ++i)
        failures += run(&experiments[i], measurements, samples);
    free(samples[0]);
    free(samples[1]);
    fips_aead_key_destroy(&key);
    puts(failures ? "ct_probe: FAIL" : "ct_probe: PASS");
    return failures ? 1 : 0;
}
