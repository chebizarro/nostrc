#ifndef FIPSD_NATIVE_DGRAM_PROBE_H
#define FIPSD_NATIVE_DGRAM_PROBE_H
/* What a connected AF_UNIX SOCK_DGRAM pair does at end of file, and what the
 * kernel does to a socket in flight (upstream src/native/dgram_probe.rs).
 * Measurement helpers for the test suite: the assertions over them record,
 * per kernel, the facts seqpacket.c and the hand-off's descriptor pin rest
 * on. Nothing in the daemon calls these. */
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
int fipsd_native_probe_dgram_pair(int fds[2]);
/* One non-blocking receive: bytes, or -1 with errno. */
ssize_t fipsd_native_probe_recv(int fd, void *buf, size_t cap);
ssize_t fipsd_native_probe_send(int fd, const void *buf, size_t len);
bool fipsd_native_probe_hung_up(int fd);
/* Give the kernel's descriptor collector a reason and time to run: closing a
 * fresh unix pair queues xnu's collector, which runs asynchronously, so this
 * is the one bounded wait the measurement needs rather than a synchronization
 * sleep. */
void fipsd_native_probe_provoke_collection(void);
/* Hand a flow's client half across a listener pair the way the daemon does,
 * close the sender's copy, provoke a collection and read what reaches the
 * receiver: the held bytes if the socket survived, zero if it was flushed.
 * -1 with errno when the plumbing itself failed. */
ssize_t fipsd_native_probe_read_after_collection_in_flight(void);
#endif
