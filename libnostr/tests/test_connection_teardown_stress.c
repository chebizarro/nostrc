/* Stress: many relays dialling, failing and being torn down at once, all on
 * libnostr's single libwebsockets service thread (nostrc-flp7).
 *
 * Hosted run 36647380306 (groundhog-sanitizers, Linux x86_64 ASan) died in
 * lws_dll2_add_sorted <- __lws_sul_insert <- lws_service on that thread: lws's
 * sorted timer (sul) list held a node inside a freed wsi. How it got there:
 * a dial whose TCP connect succeeds but whose peer hangs up before the
 * upgrade gets only CLOSED_CLIENT_HTTP and WSI_DESTROY from lws 4.3, which
 * websocket_callback ignored, so priv->wsi kept naming the freed wsi. The
 * owner's nostr_connection_close() then had the service thread call
 * lws_wsi_close() on it, and lws_set_timeout() unlinked and relinked the
 * freed wsi's timeout timer. (Also fixed: the close handler called
 * lws_set_timer_usecs(wsi, 0) meaning "cancel"; 0 arms a timer due now.)
 *
 * Worker threads dial a listener that accepts and hangs up (every other
 * connection with a TCP reset, which reliably gives the callbacks above),
 * wait for the failed handshake, then free the relay; a quarter of the dials
 * go to a refused port and are freed while still in progress. libwebsockets
 * is not instrumented, so under ASan freed memory is filled (see
 * __asan_default_options) and a freed wsi is touched as wild pointers: before
 * the fix this crashes in lws_dll2_remove <- lws_set_timeout <-
 * lws_service_loop (10 of 10 runs on the Linux gate image); without the fill
 * the stale node survives until its memory is reused, which is the hosted
 * __lws_sul_insert crash. Bounded: every wait has a deadline. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "error.h"
#include "nostr-relay.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    abort(); \
} } while (0)

#if defined(__SANITIZE_ADDRESS__)
#define TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_ASAN 1
#endif
#endif
#ifdef TEST_ASAN
/* Fill freed memory (0xbe keeps bit 0 clear) so a pointer read from freed
 * memory by uninstrumented code is wild. Environment ASAN_OPTIONS apply on
 * top of these defaults. */
const char *__asan_default_options(void);
const char *__asan_default_options(void) {
    return "max_free_fill_size=4096:free_fill_byte=190";
}
#endif

#define WORKERS 16
#define ROUNDS 60
#define HANDSHAKE_WAIT_MS 1000

static int refused_port;
static int hangup_port;
static atomic_bool stop_listener;
static atomic_int dials;
static atomic_uint accepted;

static int bind_loopback(int *port_out) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s >= 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(s, (struct sockaddr *)&a, sizeof a) == 0);
    socklen_t len = sizeof a;
    CHECK(getsockname(s, (struct sockaddr *)&a, &len) == 0);
    *port_out = ntohs(a.sin_port);
    return s;
}

/* Accepts and hangs up at once: the TCP connect succeeds and the socket
 * closes before any upgrade response. lws 4.3 often reports that as
 * CLOSED_CLIENT_HTTP + WSI_DESTROY only (sometimes as a plain connection
 * error, depending on timing), the path that left priv->wsi dangling. */
static void *hangup_listener(void *arg) {
    int s = *(int *)arg;
    while (!atomic_load(&stop_listener)) {
        struct pollfd p = { .fd = s, .events = POLLIN };
        if (poll(&p, 1, 50) <= 0) continue;
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        if (atomic_fetch_add(&accepted, 1) & 1) {
            /* Every other one with a reset instead of a FIN. */
            struct linger l = { .l_onoff = 1, .l_linger = 0 };
            setsockopt(c, SOL_SOCKET, SO_LINGER, &l, sizeof l);
        }
        close(c);
    }
    return NULL;
}

static void *worker(void *arg) {
    unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u;
    char url[64];
    for (int round = 0; round < ROUNDS; round++) {
        /* Mostly the hang-up path; some refused dials for variety. */
        int port = (rand_r(&seed) % 4 == 0) ? refused_port : hangup_port;
        snprintf(url, sizeof url, "ws://127.0.0.1:%d/", port);
        Error *err = NULL;
        NostrRelay *relay = nostr_relay_new(NULL, url, &err);
        CHECK(relay && !err);
        nostr_relay_set_auto_reconnect(relay, false);
        if (nostr_relay_connect(relay, &err)) {
            atomic_fetch_add(&dials, 1);
            /* Hang-up dials wait for the (failed) handshake, so the owner's
             * close comes after lws has finished with the wsi: that close is
             * what used to reach the freed wsi. Refused dials are torn down
             * while still in progress, the owner-close-while-connecting
             * path. No dial ever succeeds. */
            if (port == hangup_port)
                CHECK(!nostr_relay_wait_established(relay, HANDSHAKE_WAIT_MS, &err));
        }
        if (err) free_error(err);
        nostr_relay_free(relay);
    }
    return NULL;
}

int main(void) {
    int refused = bind_loopback(&refused_port);
    close(refused); /* nothing listens there any more */
    int hangup = bind_loopback(&hangup_port);
    CHECK(listen(hangup, 128) == 0);
    pthread_t listener;
    CHECK(pthread_create(&listener, NULL, hangup_listener, &hangup) == 0);

    pthread_t workers[WORKERS];
    for (uintptr_t i = 0; i < WORKERS; i++)
        CHECK(pthread_create(&workers[i], NULL, worker, (void *)(i + 1)) == 0);
    for (int i = 0; i < WORKERS; i++)
        CHECK(pthread_join(workers[i], NULL) == 0);

    atomic_store(&stop_listener, true);
    CHECK(pthread_join(listener, NULL) == 0);
    close(hangup);
    fprintf(stderr, "ok - %d dials over %d workers, service thread survived\n",
            atomic_load(&dials), WORKERS);
    CHECK(atomic_load(&dials) > 0);
    return 0;
}
