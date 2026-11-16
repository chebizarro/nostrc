/* An echo server on the native datagram API (upstream examples/native-echo.rs
 * in C). It holds the port given as the second argument on the daemon whose
 * API socket is the first, and returns every datagram sent to that port to
 * whoever sent it.
 *
 *     native-echo /run/fips/api.sock 4600
 *
 * One exchange, one flow: a served flow takes a single datagram, sends it
 * back and closes. A datagram API carries no far-end close, so a server that
 * kept reading would hold the flow, and its slot against the node's ceiling,
 * until its own process went away. The accept loop takes the next arrival
 * instead; each flow is served on a detached thread, as the Rust example
 * does, and the thread owns everything it touches. */
#define _POSIX_C_SOURCE 200809L
#include <fips-native-client.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *serve(void *arg)
{
    fips_native_stream *flow = arg;
    char peer[FIPS_NATIVE_ADDR_CAPACITY];
    fips_native_addr addr = fips_native_stream_peer_addr(flow);
    (void)fips_native_addr_format(&addr, peer, sizeof peer);
    /* Sized at the flow's own limit, so no datagram the flow can carry is
     * truncated on the way in and echoed short. */
    size_t max = fips_native_stream_max_payload(flow);
    unsigned char *buf = malloc(max ? max : 1);
    if (!buf) { fips_native_stream_close(flow); return NULL; }
    ssize_t len = fips_native_recv(flow, buf, max);
    if (len < 0) fprintf(stderr, "native-echo: receiving from %s: %s\n", peer, fips_native_strerror((int)len));
    else {
        int rc = fips_native_send(flow, buf, (size_t)len);
        if (rc == 0) printf("native-echo: returned %zd bytes to %s\n", len, peer);
        else fprintf(stderr, "native-echo: returning to %s: %s\n", peer, fips_native_strerror(rc));
        fflush(stdout);
    }
    free(buf);
    /* Closing the flow is what tells the daemon it is finished. */
    fips_native_stream_close(flow);
    return NULL;
}
int main(int argc, char **argv)
{
    if (argc != 3) { fputs("usage: native-echo <socket-path> <local-port>\n", stderr); return 1; }
    char *end = NULL;
    long port = strtol(argv[2], &end, 10);
    if (!*argv[2] || *end || port < 0 || port > 65535) { fprintf(stderr, "native-echo: %s is not a port\n", argv[2]); return 1; }
    fips_native_listener *listener = NULL;
    int rc = fips_native_listen(argv[1], (uint16_t)port, &listener);
    if (rc != 0) { fprintf(stderr, "native-echo: cannot hold port %ld on %s: %s\n", port, argv[1], fips_native_strerror(rc)); return 1; }
    /* The port the daemon actually held, which is what a caller asking for
     * an ephemeral one needs; flushed, because a caller waits on this line. */
    printf("native-echo: holding port %u\n", (unsigned)fips_native_listener_local_addr(listener).port);
    fflush(stdout);
    for (;;) {
        fips_native_stream *flow = NULL;
        rc = fips_native_accept(listener, &flow, NULL);
        if (rc != 0) { fprintf(stderr, "native-echo: accepting on port %ld: %s\n", port, fips_native_strerror(rc)); break; }
        pthread_t thread;
        if (pthread_create(&thread, NULL, serve, flow) != 0) { fips_native_stream_close(flow); continue; }
        pthread_detach(thread);
    }
    fips_native_listener_close(listener);
    return 1;
}
