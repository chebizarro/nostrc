/* Regression test for libnostr-idle-writable-busy-poll-20260817.
 *
 * The libnostr shared lws service loop used to call
 * lws_callback_on_writable_all_protocol() unconditionally after every
 * lws_service() iteration (a workaround for the nostrc-7k6 stranded-write
 * bug).  Established TCP sockets are normally writable, so poll() returned
 * POLLOUT immediately instead of sleeping for the 50 ms service timeout and
 * an idle process busy-polled a full CPU core (~80k poll calls in 4 s
 * observed in signetd on edge-01).
 *
 * This test verifies, against a local in-process WebSocket server:
 *   1. An idle connected client with an empty send queue consumes near-zero
 *      CPU (the service loop sleeps instead of busy-polling POLLOUT).
 *   2. Enqueueing a frame still wakes the service loop and drains promptly
 *      (the nostrc-7k6 reliability guarantee is preserved).
 *   3. A short-lived-client-style larger publish frame is delivered intact
 *      before close (no stranded frame).
 *   4. After the queue drains, the connection returns to near-idle CPU
 *      (the bounded fallback sweep self-terminates).
 */

#include <libwebsockets.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#include "go.h"
#include "nostr-connection.h"
#include "error.h"

/* assert() is compiled out in Release (NDEBUG) builds; use an unconditional
 * check so the regression test can never pass vacuously. */
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s (%s) at %s:%d\n", msg, #cond, \
                    __FILE__, __LINE__);                                     \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define TEST_PORT 48731
#define IDLE_WINDOW_SECONDS 2.0
/* An idle busy-poll burns ~1.0 CPU-second per wall second.  A healthy loop
 * (50 ms poll timeout on client + server contexts) burns a few ms.  0.5 s
 * over a 2 s window separates the two regimes with a wide margin even on
 * loaded CI hosts. */
#define IDLE_CPU_BUDGET_SECONDS 0.5

static atomic_ulong g_srv_rx_bytes;
static atomic_ulong g_srv_rx_msgs;
static atomic_int g_srv_stop;

static int server_cb(struct lws *wsi, enum lws_callback_reasons reason,
                     void *user, void *in, size_t len) {
    (void)wsi; (void)user; (void)in;
    switch (reason) {
    case LWS_CALLBACK_RECEIVE:
        atomic_fetch_add(&g_srv_rx_bytes, (unsigned long)len);
        if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
            atomic_fetch_add(&g_srv_rx_msgs, 1UL);
        }
        break;
    default:
        break;
    }
    return 0;
}

/* The libnostr client requests subprotocol "wss"; the server must offer it. */
static struct lws_protocols g_srv_protocols[] = {
    { "wss", server_cb, 0, 256 * 1024, 0, NULL, 0 },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

static struct lws_context *g_srv_ctx;

static void *server_thread(void *arg) {
    (void)arg;
    while (!atomic_load(&g_srv_stop)) {
        lws_service(g_srv_ctx, 50);
    }
    return NULL;
}

static double cpu_seconds_self(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6 +
           (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
}

static double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* Wait until the server has received at least `msgs` complete messages. */
static int wait_srv_msgs(unsigned long msgs, double timeout_s) {
    double deadline = now_seconds() + timeout_s;
    while (now_seconds() < deadline) {
        if (atomic_load(&g_srv_rx_msgs) >= msgs) return 1;
        usleep(10000);
    }
    return 0;
}

int main(void) {
    /* Real network path: test mode must be off. */
    unsetenv("NOSTR_TEST_MODE");
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

    /* --- Start local WebSocket server --- */
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = TEST_PORT;
    info.iface = "127.0.0.1";
    info.protocols = g_srv_protocols;
    info.gid = (gid_t)-1;
    info.uid = (uid_t)-1;
    g_srv_ctx = lws_create_context(&info);
    CHECK(g_srv_ctx, "failed to create local ws server context");
    pthread_t srv;
    CHECK(pthread_create(&srv, NULL, server_thread, NULL) == 0, "check");

    /* --- Connect libnostr client --- */
    char url[64];
    snprintf(url, sizeof url, "ws://127.0.0.1:%d", TEST_PORT);
    NostrConnection *conn = nostr_connection_new(url);
    CHECK(conn, "nostr_connection_new failed (is port free?)");

    Error *err = NULL;

    /* Prove the connection is established and the TX path drains: a small
     * write must reach the server promptly (enqueue -> lws_cancel_service ->
     * EVENT_WAIT_CANCELLED / bounded sweep -> CLIENT_WRITEABLE -> lws_write). */
    char hello[] = "[\"PING\",\"hello\"]";
    /* Connection setup is asynchronous (the service thread processes the
     * connect request); retry the first write until the wsi is established. */
    double t_send = now_seconds();
    double connect_deadline = t_send + 10.0;
    for (;;) {
        err = NULL;
        nostr_connection_write_message(conn, NULL, hello, &err);
        if (err == NULL) break;
        fprintf(stderr, "write err: %s\n", err->message ? err->message : "?");
        free_error(err);
        err = NULL;
        CHECK(now_seconds() < connect_deadline, "connection never established");
        usleep(20000);
    }
    t_send = now_seconds();
    CHECK(wait_srv_msgs(1, 5.0), "first frame never reached local server");
    double drain_latency = now_seconds() - t_send;
    printf("first-frame drain latency: %.3fs\n", drain_latency);
    CHECK(drain_latency < 2.0, "queued frame took too long to drain");

    /* --- Idle CPU check: empty send queue must let the service loop sleep --- */
    double cpu0 = cpu_seconds_self();
    double wall0 = now_seconds();
    while (now_seconds() - wall0 < IDLE_WINDOW_SECONDS) {
        usleep(50000);
    }
    double idle_cpu = cpu_seconds_self() - cpu0;
    printf("idle CPU over %.1fs window: %.3fs\n", IDLE_WINDOW_SECONDS, idle_cpu);
    CHECK(idle_cpu < IDLE_CPU_BUDGET_SECONDS,
           "idle connection busy-polls: writable sweep is arming POLLOUT with an empty queue");

    /* --- Larger short-lived-client-style publish frame drains intact --- */
    size_t big_len = 64 * 1024;
    char *big = malloc(big_len + 1);
    CHECK(big, "check");
    memset(big, 'A', big_len);
    big[big_len] = '\0';
    unsigned long bytes_before = atomic_load(&g_srv_rx_bytes);
    nostr_connection_write_message(conn, NULL, big, &err);
    CHECK(err == NULL, "check");
    CHECK(wait_srv_msgs(2, 5.0), "large frame never reached local server");
    /* Allow fragmentation on the wire; total byte count must match exactly
     * (no stranded, truncated, or duplicated frame). */
    double bytes_deadline = now_seconds() + 2.0;
    while (now_seconds() < bytes_deadline &&
           atomic_load(&g_srv_rx_bytes) - bytes_before < big_len) {
        usleep(10000);
    }
    unsigned long got = atomic_load(&g_srv_rx_bytes) - bytes_before;
    printf("large frame bytes received: %lu (expected %zu)\n", got, big_len);
    CHECK(got == big_len, "large frame stranded, truncated, or duplicated");
    free(big);

    /* --- Post-drain idle check: the bounded sweep must self-terminate --- */
    cpu0 = cpu_seconds_self();
    wall0 = now_seconds();
    while (now_seconds() - wall0 < IDLE_WINDOW_SECONDS) {
        usleep(50000);
    }
    idle_cpu = cpu_seconds_self() - cpu0;
    printf("post-drain idle CPU over %.1fs window: %.3fs\n", IDLE_WINDOW_SECONDS, idle_cpu);
    CHECK(idle_cpu < IDLE_CPU_BUDGET_SECONDS,
           "writable_pending leaked: fallback sweep kept arming POLLOUT after drain");

    /* --- Teardown --- */
    nostr_connection_close(conn);
    atomic_store(&g_srv_stop, 1);
    lws_cancel_service(g_srv_ctx);
    pthread_join(srv, NULL);
    lws_context_destroy(g_srv_ctx);

    printf("test_connection_idle_writable: OK\n");
    return 0;
}
