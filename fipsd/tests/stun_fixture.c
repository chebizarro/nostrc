#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "stun_fixture.h"
#include "transport/udp/io.h"
#include <assert.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void *serve(void *arg)
{
    stun_fixture *f = arg; unsigned received = 0;
    for (;;) {
        struct pollfd wait[] = {{f->fd, POLLIN, 0}, {f->stop[0], POLLIN, 0}};
        assert(poll(wait, 2, -1) > 0); if (wait[1].revents) break;
        struct sockaddr_in from; socklen_t len = sizeof from; uint8_t bytes[64];
        ssize_t n = recvfrom(f->fd, bytes, sizeof bytes, 0, (struct sockaddr *)&from, &len);
        assert(n == 20 && bytes[0] == 0 && bytes[1] == 1);
        stun_request *r = calloc(1, sizeof *r); assert(r);
        fipsd_udp_endpoint ep = {.len = len}; memcpy(&ep.address, &from, len);
        assert(fipsd_udp_endpoint_addr(&ep, &r->source)); memcpy(r->request, bytes, 20);
        assert(!go_channel_try_send(f->requests, r));
        if (++received <= f->drop || f->blackhole) continue;
        uint8_t reply[32] = {0}; memcpy(reply, bytes, 20); reply[0] = 1; reply[3] = 12;
        reply[21] = 0x20; reply[23] = 8; reply[25] = 1;
        uint16_t port = ntohs(from.sin_port) ^ 0x2112; reply[26] = (uint8_t)(port >> 8); reply[27] = (uint8_t)port;
        const uint8_t *ip = (const uint8_t *)&from.sin_addr;
        for (size_t i = 0; i < 4; ++i) reply[28 + i] = ip[i] ^ bytes[4 + i];
        assert(sendto(f->fd, reply, sizeof reply, 0, (struct sockaddr *)&from, len) == sizeof reply);
    }
    return NULL;
}
void stun_fixture_start(stun_fixture *f, unsigned drop, bool blackhole)
{
    *f = (stun_fixture){.drop = drop, .blackhole = blackhole};
    f->requests = go_channel_create(64); assert(f->requests);
    f->fd = socket(AF_INET, SOCK_DGRAM, 0); assert(f->fd >= 0 && !pipe(f->stop));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(f->fd, (struct sockaddr *)&a, sizeof a)); socklen_t len = sizeof a;
    assert(!getsockname(f->fd, (struct sockaddr *)&a, &len));
    snprintf(f->url, sizeof f->url, "stun:127.0.0.1:%u", ntohs(a.sin_port));
    assert(fips_transport_addr_from_string(f->url + 5, strlen(f->url + 5), &f->address) == FIPS_OK);
    assert(!pthread_create(&f->thread, NULL, serve, f));
}
void stun_fixture_stop(stun_fixture *f)
{
    assert(write(f->stop[1], "x", 1) == 1); assert(!pthread_join(f->thread, NULL));
    close(f->fd); close(f->stop[0]); close(f->stop[1]);
    void *p; while (!go_channel_try_receive(f->requests, &p)) free(p); go_channel_unref(f->requests);
}
