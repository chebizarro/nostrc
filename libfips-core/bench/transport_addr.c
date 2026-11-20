/* Same inputs/shapes as transport_addr_clone.rs. C's shipped representation
 * is a bounded POD, not Rust's Arc: /shared names identify the corresponding
 * current-representation workload, not a fabricated C reference count. */
#include "bench.h"
#include <fips/transport/types.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>

typedef struct value { uint8_t *vec; fips_transport_addr pod; } value;
typedef struct channel {
    pthread_mutex_t mu;
    pthread_cond_t readable, writable;
    value slots[256];
    size_t head, count;
    bool ready, done, former;
} channel;
static void drop(value *v, bool former)
{
    bench_observe(former ? (void *)v->vec : (void *)&v->pod);
    if (former) free(v->vec);
}
static value clone(const fips_transport_addr *addr, bool former)
{
    value v = {0};
    if (former) {
        v.vec = malloc(addr->len); REQUIRE(v.vec);
        memcpy(v.vec, addr->bytes, addr->len);
    } else v.pod = *addr;
    return v;
}
static void *consume(void *arg)
{
    channel *q = arg;
    REQUIRE(!pthread_mutex_lock(&q->mu));
    q->ready = true; REQUIRE(!pthread_cond_signal(&q->writable));
    for (;;) {
        while (!q->count && !q->done) REQUIRE(!pthread_cond_wait(&q->readable, &q->mu));
        if (!q->count && q->done) break;
        value v = q->slots[q->head];
        q->head = (q->head + 1) % 256; --q->count;
        REQUIRE(!pthread_cond_signal(&q->writable));
        REQUIRE(!pthread_mutex_unlock(&q->mu));
        drop(&v, q->former);
        REQUIRE(!pthread_mutex_lock(&q->mu));
    }
    REQUIRE(!pthread_mutex_unlock(&q->mu));
    return NULL;
}
static double cross_thread(const fips_transport_addr *addr, bool former, size_t n)
{
    channel *q = calloc(1, sizeof(*q)); REQUIRE(q); q->former = former;
    REQUIRE(!pthread_mutex_init(&q->mu, NULL));
    REQUIRE(!pthread_cond_init(&q->readable, NULL)); REQUIRE(!pthread_cond_init(&q->writable, NULL));
    pthread_t thread; REQUIRE(!pthread_create(&thread, NULL, consume, q));
    REQUIRE(!pthread_mutex_lock(&q->mu));
    while (!q->ready) REQUIRE(!pthread_cond_wait(&q->writable, &q->mu));
    REQUIRE(!pthread_mutex_unlock(&q->mu));
    uint64_t start = bench_now();
    for (size_t i = 0; i < n; ++i) {
        value v = clone(addr, former);
        REQUIRE(!pthread_mutex_lock(&q->mu));
        while (q->count == 256) REQUIRE(!pthread_cond_wait(&q->writable, &q->mu));
        q->slots[(q->head + q->count) % 256] = v; ++q->count;
        REQUIRE(!pthread_cond_signal(&q->readable)); REQUIRE(!pthread_mutex_unlock(&q->mu));
    }
    REQUIRE(!pthread_mutex_lock(&q->mu)); q->done = true;
    REQUIRE(!pthread_cond_signal(&q->readable)); REQUIRE(!pthread_mutex_unlock(&q->mu));
    REQUIRE(!pthread_join(thread, NULL));
    double elapsed = (double)(bench_now() - start) / (double)n;
    REQUIRE(!pthread_cond_destroy(&q->readable)); REQUIRE(!pthread_cond_destroy(&q->writable));
    REQUIRE(!pthread_mutex_destroy(&q->mu)); free(q);
    return elapsed;
}
static void socket_clone(unsigned i, bool former)
{
    static const char *ips[] = {"192.168.1.1", "2001:db8::1", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"};
    static const char *names[] = {"ipv4", "ipv6", "scoped_ipv6"};
    static const char *expected[] = {"192.168.1.1:2121", "[2001:db8::1]:2121",
        "[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295]:65535"};
    uint8_t ip[16]; REQUIRE(inet_pton(i ? AF_INET6 : AF_INET, ips[i], ip) == 1);
    unsigned port = i == 2 ? 65535 : 2121;
    char name[100]; snprintf(name, sizeof(name), "transport_addr_socket_and_clone/%s/%s", former ? "former_vec" : "shared", names[i]);
    if (former) {
        BENCH(name, {
            /* Rust's former Vec constructor reserves 56 bytes then formats. */
            char text[INET6_ADDRSTRLEN]; REQUIRE(inet_ntop(i ? AF_INET6 : AF_INET, ip, text, sizeof(text)));
            uint8_t *v = malloc(56); REQUIRE(v);
            char formatted[80];
            int len = i == 2 ? snprintf(formatted, sizeof(formatted), "[%s%%4294967295]:%u", text, port) :
                (i ? snprintf(formatted, sizeof(formatted), "[%s]:%u", text, port) : snprintf(formatted, sizeof(formatted), "%s:%u", text, port));
            REQUIRE(len > 0 && (size_t)len < sizeof(formatted));
            if (len > 56) { uint8_t *grown = realloc(v, 112); REQUIRE(grown); v = grown; }
            memcpy(v, formatted, (size_t)len);
            uint8_t *copy = malloc((size_t)len); REQUIRE(copy); memcpy(copy, v, (size_t)len);
            bench_observe(copy); bench_observe(v); free(copy); free(v);
        });
    } else {
        fips_transport_addr addr;
        REQUIRE(fips_transport_addr_from_socket_addr(ip, i ? 16 : 4, (uint16_t)port, i == 2 ? UINT32_MAX : 0, &addr) == FIPS_OK);
        REQUIRE(addr.len == strlen(expected[i]) && !memcmp(addr.bytes, expected[i], addr.len));
        BENCH(name, {
            REQUIRE(fips_transport_addr_from_socket_addr(ip, i ? 16 : 4, (uint16_t)port, i == 2 ? UINT32_MAX : 0, &addr) == FIPS_OK);
            fips_transport_addr copy = addr;
            bench_observe(&copy); bench_observe(&addr);
        });
    }
}
int main(void)
{
    const size_t sizes[] = {6, 18, 58}; uint8_t bytes[58]; memset(bytes, 0x5a, sizeof(bytes));
    for (size_t i = 0; i < 3; ++i) {
        fips_transport_addr addr; REQUIRE(fips_transport_addr_from_bytes(bytes, sizes[i], &addr) == FIPS_OK);
        for (unsigned mode = 0; mode < 2; ++mode) {
            bool former = mode == 0; char name[100];
            snprintf(name, sizeof(name), "transport_addr_clone/%s/%zu", former ? "former_vec" : "shared", sizes[i]);
            BENCH(name, { value v = clone(&addr, former); drop(&v, former); });
            snprintf(name, sizeof(name), "transport_addr_cross_thread/%s/%zu", former ? "former_vec" : "shared", sizes[i]);
            double samples[BENCH_SAMPLES];
            for (unsigned s = 0; s < BENCH_SAMPLES + 10; ++s) {
                double ns = cross_thread(&addr, former, bench_iterations());
                if (s >= 10) samples[s - 10] = ns;
            }
            bench_report(name, samples);
        }
    }
    for (unsigned i = 0; i < 3; ++i) { socket_clone(i, true); socket_clone(i, false); }
    return 0;
}
