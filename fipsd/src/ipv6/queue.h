#ifndef FIPSD_IPV6_QUEUE_H
#define FIPSD_IPV6_QUEUE_H
/* The queue of packets toward the host (upstream queue.rs). Everything the
 * shim hands the host (a delivered packet, an ICMPv6 answer, a hairpinned
 * packet) goes through one bounded queue read by the TUN writer fiber. A send
 * never waits for room: a writer that has stopped keeping up loses packets, as
 * a router's full output queue does, rather than holding the node back or
 * growing without limit. */
#include "channel.h"
#include <stddef.h>
#include <stdint.h>
#define FIPSD_IPV6_HOST_QUEUE_DEFAULT 4096u
typedef struct fipsd_ipv6_packet { size_t len; uint8_t bytes[]; } fipsd_ipv6_packet;
typedef struct fipsd_ipv6_queue fipsd_ipv6_queue;
fipsd_ipv6_queue *fipsd_ipv6_queue_create(size_t capacity);
/* After the writer stopped: frees whatever is still queued. */
void fipsd_ipv6_queue_destroy(fipsd_ipv6_queue *queue);
/* 0 when queued or dropped on a full queue (counted, logged at the first drop
 * and each doubling); -1 only when the reader has gone. */
int fipsd_ipv6_queue_send(fipsd_ipv6_queue *queue, const uint8_t *bytes, size_t len);
/* The reader has gone: later sends are refused. Queued packets stay for the
 * writer's final drain. */
void fipsd_ipv6_queue_close(fipsd_ipv6_queue *queue);
/* A new reader is about to start: what the last drain left is freed and
 * sends are taken again. Only after every producer and the reader joined. */
void fipsd_ipv6_queue_open(fipsd_ipv6_queue *queue);
GoChannel *fipsd_ipv6_queue_channel(fipsd_ipv6_queue *queue);
/* NULL when empty. The caller owns (frees) the packet. */
fipsd_ipv6_packet *fipsd_ipv6_queue_try_receive(fipsd_ipv6_queue *queue);
uint64_t fipsd_ipv6_queue_dropped(const fipsd_ipv6_queue *queue);
/* The shared drop counter the TUN status reads. */
const _Atomic uint64_t *fipsd_ipv6_queue_counter(const fipsd_ipv6_queue *queue);
#endif
