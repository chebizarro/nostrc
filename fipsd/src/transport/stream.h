#ifndef FIPSD_STREAM_H
#define FIPSD_STREAM_H
#include "transport/framing.h"
/* Caller-owned fixed storage, no allocation from an untrusted prefix. Feed
 * stops at one frame; consumed allows coalesced frames to be fed again.
 * Result: 1 complete, 0 incomplete, -1 malformed/capacity. Error is latched
 * until reset. Frame remains borrowed until reset. EOF is valid only empty. */
typedef struct fipsd_stream {
    uint8_t *buffer;
    size_t capacity, used, expected;
    uint16_t ceiling;
    bool failed;
} fipsd_stream;
void fipsd_stream_init(fipsd_stream *, uint8_t *, size_t, uint16_t);
void fipsd_stream_reset(fipsd_stream *);
int fipsd_stream_feed(fipsd_stream *, const uint8_t *, size_t, size_t *consumed);
bool fipsd_stream_eof(const fipsd_stream *);
/* On a fiber; fd owner must shutdown/wake and join pending I/O before close.
 * gof_read parks indefinitely; no hidden timeout or extra length prefix.
 * Returns 1 frame, 0 clean EOF, -1 error (EPROTO for truncated/malformed). */
int fipsd_stream_read(int fd, uint8_t *, size_t capacity, size_t *size);
int fipsd_stream_write(int fd, const uint8_t *, size_t);
uint64_t fipsd_stream_next_id(void);
#endif
