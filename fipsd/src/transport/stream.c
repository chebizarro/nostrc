#include "transport/stream.h"
#include <libgo/fiber.h>
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
void fipsd_stream_init(fipsd_stream *s, uint8_t *buf, size_t cap, uint16_t ceiling)
{ *s = (fipsd_stream){.buffer = buf, .capacity = cap, .ceiling = ceiling}; }
void fipsd_stream_reset(fipsd_stream *s) { s->used = s->expected = 0; s->failed = false; }
int fipsd_stream_feed(fipsd_stream *s, const uint8_t *p, size_t n, size_t *consumed)
{
    *consumed = 0;
    if (s->failed) return -1;
    while (*consumed < n || s->used == 4) {
        if (!s->expected && s->used == 4) {
            if (fipsd_frame_size(s->buffer, s->ceiling, &s->expected) != FIPS_OK ||
                s->expected > s->capacity) { s->failed = true; return -1; }
        }
        if (s->expected && s->used == s->expected) return 1;
        size_t target = s->expected ? s->expected : 4;
        size_t take = target - s->used;
        if (take > n - *consumed) take = n - *consumed;
        if (s->used + take > s->capacity || !s->buffer) { s->failed = true; return -1; }
        if (!take) break;
        memcpy(s->buffer + s->used, p + *consumed, take);
        s->used += take; *consumed += take;
    }
    return s->expected && s->used == s->expected ? 1 : 0;
}
bool fipsd_stream_eof(const fipsd_stream *s) { return !s->failed && s->used == 0; }
int fipsd_stream_read(int fd, uint8_t *buf, size_t cap, size_t *size)
{
    fipsd_stream s; fipsd_stream_init(&s, buf, cap, UINT16_MAX);
    if (cap < 4) { errno = EMSGSIZE; return -1; }
    for (;;) {
        size_t want = (s.expected ? s.expected : 4) - s.used;
        ssize_t n = gof_read(fd, buf + s.used, want);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (!n) { if (!s.used) return 0; errno = EPROTO; return -1; }
        s.used += (size_t)n;
        if (!s.expected && s.used == 4) {
            if (fipsd_frame_size(buf, UINT16_MAX, &s.expected) != FIPS_OK || s.expected > cap) {
                errno = EPROTO; return -1;
            }
        }
        if (s.expected && s.used == s.expected) { *size = s.used; return 1; }
    }
}
int fipsd_stream_write(int fd, const uint8_t *p, size_t len)
{
    if (fipsd_datagram_frame(p, len, UINT16_MAX) != FIPS_OK) { errno = EPROTO; return -1; }
    size_t sent = 0;
    while (sent < len) {
        struct iovec v = {.iov_base = (void *)(p + sent), .iov_len = len - sent};
        struct msghdr msg = {.msg_iov = &v, .msg_iovlen = 1};
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        /* Apple fd owners set SO_NOSIGPIPE at socket creation. */
        ssize_t n = gof_sendmsg(fd, &msg, flags, -1);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (!n) { errno = EPIPE; return -1; }
        sent += (size_t)n;
    }
    return 0;
}
uint64_t fipsd_stream_next_id(void)
{
    static _Atomic uint64_t next = 1;
    return atomic_fetch_add_explicit(&next, 1, memory_order_relaxed);
}
