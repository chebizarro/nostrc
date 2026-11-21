/* Test-only traffic generator. Both endpoints use the real libfipsnative ABI;
 * packets traverse two daemon processes and loopback UDP, never an in-process
 * transport. One clock measures receipt at each endpoint (not RTT / 2).
 * Every datagram moves through fips_native_send/fips_native_recv, the
 * descriptor only through poll: on macOS and FreeBSD the flow is a framed
 * stream (nostrc-6ljk8), whose raw bytes are not datagrams. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "fips-native-client.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define PAYLOAD 1200u
#define WINDOW 0u /* Unpaced offered load; report delivered goodput and loss. */
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #x, errno); exit(1); } } while (0)
static uint64_t now(void)
{
    struct timespec ts; CHECK(!clock_gettime(CLOCK_MONOTONIC, &ts));
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
/* Opt-in handshake keeps direct callers and stdout unchanged. The harness
 * samples both daemon CPU clocks before acknowledging each boundary. */
static void cpu_marker(const char *name)
{
    if (!getenv("FIPSD_PERF_CPU_MARKERS")) return;
    CHECK(fprintf(stderr, "FIPSD_PERF_TRAFFIC_%s\n", name) > 0);
    CHECK(!fflush(stderr));
    CHECK(getchar() == '\n');
}
static void readable(int fd)
{
    struct pollfd p = {.fd=fd, .events=POLLIN}; int rc;
    do { rc = poll(&p, 1, 5000); } while (rc < 0 && errno == EINTR);
    CHECK(rc == 1 && (p.revents & POLLIN));
}
static uint64_t receive_seq(fips_native_stream *stream)
{
    uint8_t bytes[PAYLOAD + 1];
    readable(fips_native_stream_fd(stream));
    CHECK(fips_native_recv(stream, bytes, sizeof(bytes)) == PAYLOAD);
    uint64_t got; memcpy(&got, bytes, sizeof(got));
    for (size_t i = sizeof(got); i < PAYLOAD; ++i) CHECK(bytes[i] == 0xa5);
    return got;
}
static void receive(fips_native_stream *stream, uint64_t want)
{ CHECK(receive_seq(stream) == want); }
static int try_transmit(fips_native_stream *stream, uint64_t seq)
{
    uint8_t bytes[PAYLOAD]; memset(bytes, 0xa5, sizeof(bytes)); memcpy(bytes, &seq, sizeof(seq));
    return fips_native_send(stream, bytes, sizeof(bytes));
}
static void transmit(fips_native_stream *stream, uint64_t seq)
{ CHECK(!try_transmit(stream, seq)); }
static int cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static void latency(const char *key, uint64_t *values, size_t n)
{
    qsort(values, n, sizeof(*values), cmp);
    printf("\"%s\":{\"p50_us\":%.3f,\"p99_us\":%.3f}", key,
        (double)values[n / 2] / 1000.0, (double)values[(99 * n + 99) / 100 - 1] / 1000.0);
}
typedef struct receiver {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool ready, started, paced;
    uint64_t *samples;
    size_t sample_count, sample_capacity;
    fips_native_stream *stream;
    int fd;
    uint64_t deadline, received, highest;
} receiver;
static void *drain(void *arg)
{
    receiver *r = arg;
    CHECK(!pthread_mutex_lock(&r->mu)); r->ready = true;
    CHECK(!pthread_cond_signal(&r->cv));
    while (!r->started) CHECK(!pthread_cond_wait(&r->cv, &r->mu));
    CHECK(!pthread_mutex_unlock(&r->mu));
    size_t cap = 65536; uint8_t *seen = calloc(cap, 1); CHECK(seen);
    uint8_t expected[PAYLOAD]; memset(expected, 0xa5, sizeof expected);
    for (;;) {
        uint64_t current = now();
        if (current >= r->deadline) break;
        uint8_t bytes[PAYLOAD + 1];
        /* Drain first: poll only after EAGAIN, not once per queued datagram.
         * The non-blocking public API retains partial frames across EAGAIN. */
        ssize_t len = fips_native_recv(r->stream, bytes, sizeof(bytes));
        if (len == -EAGAIN) {
            uint64_t remaining = r->deadline - current;
            struct pollfd fd = {.fd=r->fd, .events=POLLIN};
            int ms = (int)((remaining + 999999) / 1000000);
            int rc = poll(&fd, 1, ms);
            if (rc < 0 && errno == EINTR) continue;
            CHECK(rc >= 0);
            if (!rc) break;
            continue;
        }
        if (len == -EINTR) continue;
        CHECK(len == PAYLOAD);
        uint64_t seq; memcpy(&seq, bytes, sizeof(seq));
        CHECK(seq < UINT64_C(1000000000));
        size_t slot = (size_t)(seq / 8);
        if (slot >= cap) {
            size_t next = cap;
            while (slot >= next) next *= 2;
            uint8_t *grown = realloc(seen, next); CHECK(grown);
            memset(grown + cap, 0, next - cap); seen = grown; cap = next;
        }
        uint8_t bit = (uint8_t)(1u << (seq % 8)); CHECK(!(seen[slot] & bit)); seen[slot] |= bit;
        if (r->paced && seq % 256 == 0 && r->sample_count < r->sample_capacity) {
            uint64_t timestamp; memcpy(&timestamp, bytes + sizeof(seq), sizeof(timestamp));
            r->samples[r->sample_count++] = now() - timestamp;
        }
        size_t checked = sizeof(seq) + (r->paced ? sizeof(uint64_t) : 0);
        CHECK(memcmp(bytes + checked, expected + checked, PAYLOAD - checked) == 0);
        ++r->received;
        if (seq > r->highest) r->highest = seq;
    }
    free(seen); return NULL;
}
int main(int argc, char **argv)
{
    if (argc < 6 || argc > 8) { fprintf(stderr, "usage: perf_native LEFT_API RIGHT_API RIGHT_NPUB SECONDS LATENCY_SAMPLES [OFFERED_MBPS [PORT]]\n"); return 2; }
    char *end;
    double seconds = strtod(argv[4], &end); CHECK(end != argv[4] && !*end && seconds >= .1 && seconds <= 3600);
    unsigned long count = strtoul(argv[5], &end, 10); CHECK(end != argv[5] && !*end && count >= 100 && count <= 100000);
    double mbps = 0;
    if (argc >= 7) {
        mbps = strtod(argv[6], &end);
        CHECK(end != argv[6] && !*end && isfinite(mbps) && mbps > 0 && mbps <= 100000);
    }
    unsigned long port = 4242;
    if (argc == 8) {
        port = strtoul(argv[7], &end, 10);
        CHECK(end != argv[7] && !*end && port >= 1 && port <= 65535);
    }
    double pps = mbps * 1e6 / (PAYLOAD * 8);
    size_t n = (size_t)count;
    fips_native_listener *listener;
    CHECK(!fips_native_listen(argv[2], (uint16_t)port, &listener));
    fips_native_addr peer;
    CHECK(!fips_native_addr_from_npub(argv[3], (uint16_t)port, &peer));
    fips_native_stream *left, *right;
    CHECK(!fips_native_connect(argv[1], 0, &peer, &left));
    CHECK(!fips_native_stream_set_write_timeout(left, 5000));
    transmit(left, 0);
    readable(fips_native_listener_fd(listener));
    CHECK(!fips_native_accept(listener, &right, NULL));
    CHECK(!fips_native_stream_set_write_timeout(right, 5000));
    receive(right, 0);
    /* Warm both directions, outside both measurements. */
    for (uint64_t i = 1; i <= 100; ++i) {
        transmit(left, i); receive(right, i); transmit(right, i); receive(left, i);
    }
    uint64_t *one = calloc(n, sizeof(*one)), *back = calloc(n, sizeof(*back)), *rtt = calloc(n, sizeof(*rtt));
    CHECK(one && back && rtt);
    for (size_t i = 0; i < n; ++i) {
        uint64_t start = now(); transmit(left, i); receive(right, i);
        uint64_t arrived = now(); transmit(right, i); receive(left, i);
        uint64_t done = now(); one[i] = arrived - start; back[i] = done - arrived; rtt[i] = done - start;
    }
    CHECK(!fips_native_stream_set_nonblocking(right, true));
    CHECK(!fips_native_stream_set_nonblocking(left, true));
    uint64_t *loaded = calloc(n, sizeof(*loaded)); CHECK(loaded);
    receiver r = {.stream=right, .fd=fips_native_stream_fd(right), .paced=mbps > 0,
                  .samples=loaded, .sample_capacity=n};
    CHECK(!pthread_mutex_init(&r.mu, NULL)); CHECK(!pthread_cond_init(&r.cv, NULL));
    pthread_t thread; CHECK(!pthread_create(&thread, NULL, drain, &r));
    CHECK(!pthread_mutex_lock(&r.mu));
    while (!r.ready) CHECK(!pthread_cond_wait(&r.cv, &r.mu));
    cpu_marker("START");
    uint64_t sent = 0, offered = 0, backpressure = 0, start = now();
    uint64_t elapsed = (uint64_t)(seconds * 1e9);
    uint64_t send_deadline = start + elapsed;
    r.deadline = send_deadline + (mbps ? UINT64_C(200000000) : 0); r.started = true;
    CHECK(!pthread_cond_signal(&r.cv)); CHECK(!pthread_mutex_unlock(&r.mu));
    uint8_t bytes[PAYLOAD]; memset(bytes, 0xa5, sizeof(bytes));
    int fd = fips_native_stream_fd(left);
    while (now() < send_deadline) {
        unsigned batch = 1;
        if (mbps) {
            /* Absolute deadlines, in <=50 us batches. Spin rather than ask
             * macOS timers to wake once per packet. Catch-up after scheduler
             * delays is bounded to 64 offers between clock checks. */
            uint64_t deadline = start + (uint64_t)((double)offered * 1e9 / pps);
            while (now() < deadline && now() < send_deadline) { }
            if (now() >= send_deadline) break;
            batch = (unsigned)(pps * .00005);
            if (!batch) batch = 1;
            if (batch > 64) batch = 64;
            uint64_t due = (uint64_t)(seconds * pps);
            if (offered + batch > due) batch = (unsigned)(due - offered);
            if (!batch) break;
        }
        for (unsigned i = 0; i < batch; ++i) {
            uint64_t seq = mbps ? offered : sent;
            memcpy(bytes, &seq, sizeof(seq));
            if (mbps && seq % 256 == 0) {
                uint64_t timestamp = now();
                memcpy(bytes + sizeof(seq), &timestamp, sizeof(timestamp));
            }
            int rc = fips_native_send(left, bytes, sizeof(bytes));
            if (mbps) ++offered;
            if (rc == -ENOBUFS || rc == -EAGAIN || rc == -EINTR) {
                ++backpressure;
                if (!mbps) {
                    struct pollfd writable = {.fd=fd, .events=POLLOUT};
                    int status = poll(&writable, 1, 1); CHECK(status >= 0 || errno == EINTR);
                }
                continue; /* Paced offers are never retried or hidden. */
            }
            CHECK(rc == 0); ++sent;
        }
    }
    /* A final partial batch can exhaust the offer count just before the
     * deadline. Keep the end boundary at the deadline, not that batch. */
    while (now() < send_deadline) { }
    cpu_marker("END");
    CHECK(!pthread_join(thread, NULL));
    CHECK(!pthread_cond_destroy(&r.cv)); CHECK(!pthread_mutex_destroy(&r.mu));
    uint64_t received = r.received;
    CHECK(received <= sent && (!received || r.highest < (mbps ? offered : sent)));
    if (!mbps) CHECK(received);
    uint64_t accepted = sent;
    if (mbps) sent = offered;
    printf("{\"payload_bytes\":%u,\"window\":%u,\"received\":%" PRIu64 ",\"sent\":%" PRIu64 ",\"loss\":%" PRIu64 ","
        "\"elapsed_s\":%.9f,\"throughput_mbps\":%.6f,\"latency_samples\":%zu,",
        PAYLOAD, WINDOW, received, sent, sent - received, (double)elapsed / 1e9,
        (double)received * PAYLOAD * 8 * 1000.0 / (double)elapsed, n);
    printf("\"local_backpressure\":%" PRIu64 ",", backpressure);
    if (mbps) printf("\"target_mbps\":%.6f,\"offered_mbps\":%.6f,\"accepted\":%" PRIu64
        ",\"loss_fraction\":%.9f,\"drain_s\":0.2,",
        mbps, (double)offered * PAYLOAD * 8 * 1000.0 / (double)elapsed, accepted,
        sent ? (double)(sent - received) / (double)sent : 1.0);
    if (mbps) {
        printf("\"loaded_latency_samples\":%zu,", r.sample_count);
        if (r.sample_count) { latency("loaded_one_way", loaded, r.sample_count); putchar(','); }
        else printf("\"loaded_one_way\":null,");
    }
    latency("one_way", one, n); putchar(','); latency("reverse_one_way", back, n); putchar(','); latency("rtt", rtt, n);
    puts("}");
    free(loaded); free(one); free(back); free(rtt);
    fips_native_stream_close(left); fips_native_stream_close(right); fips_native_listener_close(listener);
    return 0;
}
