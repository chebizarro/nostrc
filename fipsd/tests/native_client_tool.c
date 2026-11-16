/* A driver for the C<->Rust interop harness (udp_interop.py) over
 * libfipsnative. `serve` holds a port and, per arriving flow, reads one
 * datagram, prints it as hex, answers with the given bytes and closes the
 * flow; `send` opens a flow, sends one datagram and optionally waits a
 * bounded time for one back. Every line printed is flushed, because the
 * harness waits on them. */
#define _POSIX_C_SOURCE 200809L
#include <fips-native-client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nibble(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static size_t unhex(const char *text, unsigned char *out, size_t cap)
{
    size_t n = strlen(text) / 2;
    if (n > cap || strlen(text) % 2) return (size_t)-1;
    for (size_t i = 0; i < n; ++i) {
        int hi = nibble(text[2 * i]), lo = nibble(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return (size_t)-1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return n;
}
static void print_hex(const char *label, const unsigned char *buf, size_t n)
{
    printf("%s ", label);
    for (size_t i = 0; i < n; ++i) printf("%02x", buf[i]);
    printf("\n"); fflush(stdout);
}
static int serve(const char *sock, const char *port_text, const char *reply_hex)
{
    unsigned char reply[2048]; size_t reply_len = unhex(reply_hex, reply, sizeof reply);
    if (reply_len == (size_t)-1) { fputs("bad reply hex\n", stderr); return 2; }
    fips_native_listener *listener = NULL;
    int rc = fips_native_listen(sock, (uint16_t)atoi(port_text), &listener);
    if (rc != 0) { fprintf(stderr, "listen: %s\n", fips_native_strerror(rc)); return 1; }
    printf("holding port %u\n", (unsigned)fips_native_listener_local_addr(listener).port); fflush(stdout);
    for (;;) {
        fips_native_stream *flow = NULL; fips_native_addr peer;
        rc = fips_native_accept(listener, &flow, &peer);
        if (rc != 0) { fprintf(stderr, "accept: %s\n", fips_native_strerror(rc)); return 1; }
        unsigned char buf[65535];
        ssize_t n = fips_native_recv(flow, buf, sizeof buf);
        if (n < 0) { fprintf(stderr, "recv: %s\n", fips_native_strerror((int)n)); fips_native_stream_close(flow); continue; }
        char from[FIPS_NATIVE_ADDR_CAPACITY]; (void)fips_native_addr_format(&peer, from, sizeof from);
        printf("from %s\n", from);
        print_hex("received", buf, (size_t)n);
        rc = fips_native_send(flow, reply, reply_len);
        if (rc != 0) fprintf(stderr, "send: %s\n", fips_native_strerror(rc));
        else { printf("replied %zu bytes\n", reply_len); fflush(stdout); }
        fips_native_stream_close(flow);
        printf("closed\n"); fflush(stdout);
    }
}
static int send_one(const char *sock, const char *npub, const char *port_text, const char *hex, const char *timeout_text)
{
    unsigned char payload[2048]; size_t len = unhex(hex, payload, sizeof payload);
    if (len == (size_t)-1) { fputs("bad payload hex\n", stderr); return 2; }
    fips_native_addr peer;
    int rc = fips_native_addr_from_npub(npub, (uint16_t)atoi(port_text), &peer);
    if (rc != 0) { fprintf(stderr, "address: %s\n", fips_native_strerror(rc)); return 2; }
    fips_native_stream *flow = NULL;
    rc = fips_native_connect(sock, 0, &peer, &flow);
    if (rc != 0) { fprintf(stderr, "connect: %s\n", fips_native_strerror(rc)); return 1; }
    printf("local port %u max_payload %zu\n", (unsigned)fips_native_stream_local_addr(flow).port, fips_native_stream_max_payload(flow)); fflush(stdout);
    rc = fips_native_send(flow, payload, len);
    if (rc != 0) { fprintf(stderr, "send: %s\n", fips_native_strerror(rc)); fips_native_stream_close(flow); return 1; }
    printf("sent %zu bytes\n", len); fflush(stdout);
    if (timeout_text) {
        (void)fips_native_stream_set_read_timeout(flow, atoll(timeout_text));
        unsigned char buf[65535];
        ssize_t n = fips_native_recv(flow, buf, sizeof buf);
        if (n < 0) { fprintf(stderr, "recv: %s\n", fips_native_strerror((int)n)); fips_native_stream_close(flow); return 1; }
        print_hex("received", buf, (size_t)n);
    }
    fips_native_stream_close(flow);
    return 0;
}
int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "serve")) return serve(argv[2], argv[3], argv[4]);
    if ((argc == 6 || argc == 7) && !strcmp(argv[1], "send")) return send_one(argv[2], argv[3], argv[4], argv[5], argc == 7 ? argv[6] : NULL);
    fputs("usage: native_client_tool serve <api-socket> <port> <reply-hex>\n"
          "       native_client_tool send <api-socket> <npub> <remote-port> <hex> [recv-timeout-ms]\n", stderr);
    return 2;
}
