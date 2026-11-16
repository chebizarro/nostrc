/* The first program from upstream docs/how-to/use-the-native-datagram-api.md,
 * in C: open a flow to a peer named by npub and port, send one datagram, and
 * wait a bounded time for one back.
 *
 *     native-send /run/fips/api.sock npub1...:4600 "hello"
 *
 * `connect` contacts no peer: it is a local registration at the daemon, and
 * nothing about it proves the peer exists, is reachable or is listening. So
 * the wait for the answer is bounded by a read deadline of the program's own;
 * without one the recv would block for ever against a peer that is offline. */
#define _POSIX_C_SOURCE 200809L
#include <fips-native-client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 4) { fputs("usage: native-send <socket-path> <npub:port> <text>\n", stderr); return 1; }
    fips_native_addr peer;
    int rc = fips_native_addr_parse(argv[2], &peer);
    if (rc != 0) { fprintf(stderr, "native-send: %s is not an npub:port address: %s\n", argv[2], fips_native_strerror(rc)); return 1; }
    fips_native_stream *flow = NULL;
    rc = fips_native_connect(argv[1], 0, &peer, &flow);
    if (rc != 0) { fprintf(stderr, "native-send: connect: %s\n", fips_native_strerror(rc)); return 1; }
    rc = fips_native_send(flow, argv[3], strlen(argv[3]));
    if (rc != 0) { fprintf(stderr, "native-send: send: %s\n", fips_native_strerror(rc)); fips_native_stream_close(flow); return 1; }
    /* Nothing tells you a peer is never going to answer, so bound the wait. */
    (void)fips_native_stream_set_read_timeout(flow, 5000);
    size_t max = fips_native_stream_max_payload(flow);
    unsigned char *buf = malloc(max ? max : 1);
    if (!buf) { fips_native_stream_close(flow); return 1; }
    ssize_t len = fips_native_recv(flow, buf, max);
    if (len < 0) { fprintf(stderr, "native-send: recv: %s\n", fips_native_strerror((int)len)); free(buf); fips_native_stream_close(flow); return 1; }
    char from[FIPS_NATIVE_ADDR_CAPACITY];
    fips_native_addr addr = fips_native_stream_peer_addr(flow);
    (void)fips_native_addr_format(&addr, from, sizeof from);
    printf("%zd bytes back from %s: %.*s\n", len, from, (int)len, (const char *)buf);
    free(buf);
    fips_native_stream_close(flow);
    return 0;
}
