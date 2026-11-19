#ifndef FIPSD_NATIVE_FRAME_H
#define FIPSD_NATIVE_FRAME_H
/* Length framing for a stream flow (nostrc-6ljk8), shared by the daemon and
 * libfipsnative; pure, so both sides and the tests carry one definition.
 *
 * On macOS and FreeBSD a flow libfipsnative opens with `"framing":"stream"`
 * is an AF_UNIX SOCK_STREAM pair instead of the platform's SOCK_DGRAM one,
 * because a datagram pair there refuses a full receiver with ENOBUFS and
 * raises no writability event, so a slow client could only be dropped; a
 * stream's writer is woken when its reader drains. Each datagram is one frame:
 * a 4-byte big-endian length, then that many payload bytes (0 is an empty
 * datagram). A length above FIPSD_NATIVE_FRAME_MAX is a framing violation
 * that ends the flow. Either end may see a frame arrive in pieces. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIPSD_NATIVE_FRAME_HEADER 4u
#define FIPSD_NATIVE_FRAME_MAX 65535u /* the largest datagram the API carries */

static inline void fipsd_native_frame_header(uint8_t out[FIPSD_NATIVE_FRAME_HEADER], uint32_t len)
{
    out[0] = (uint8_t)(len >> 24); out[1] = (uint8_t)(len >> 16); out[2] = (uint8_t)(len >> 8); out[3] = (uint8_t)len;
}
static inline uint32_t fipsd_native_frame_length(const uint8_t in[FIPSD_NATIVE_FRAME_HEADER])
{
    return (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | (uint32_t)in[3];
}
typedef enum fipsd_native_frame_status {
    FIPSD_NATIVE_FRAME_PARTIAL, /* more bytes are needed */
    FIPSD_NATIVE_FRAME_WHOLE,   /* one frame: payload at buf + FIPSD_NATIVE_FRAME_HEADER */
    FIPSD_NATIVE_FRAME_INVALID  /* a length above FIPSD_NATIVE_FRAME_MAX */
} fipsd_native_frame_status;
/* Whether `buf[0..have)` starts with a whole frame; `*len` is its payload
 * length once the header is complete (also for PARTIAL and INVALID). */
static inline fipsd_native_frame_status fipsd_native_frame_take(const uint8_t *buf, size_t have, uint32_t *len)
{
    if (have < FIPSD_NATIVE_FRAME_HEADER) return FIPSD_NATIVE_FRAME_PARTIAL;
    *len = fipsd_native_frame_length(buf);
    if (*len > FIPSD_NATIVE_FRAME_MAX) return FIPSD_NATIVE_FRAME_INVALID;
    return have - FIPSD_NATIVE_FRAME_HEADER >= *len ? FIPSD_NATIVE_FRAME_WHOLE : FIPSD_NATIVE_FRAME_PARTIAL;
}
#endif
