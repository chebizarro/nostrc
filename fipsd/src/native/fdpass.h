#ifndef FIPSD_NATIVE_FDPASS_H
#define FIPSD_NATIVE_FDPASS_H
/* Replies and arrival messages carrying a descriptor (upstream
 * src/native/fdpass.rs): the descriptor travels in the SCM_RIGHTS ancillary
 * data of the same sendmsg that carries the line, so a client reads one
 * message and gets both. The receiving half is here too, for the daemon's
 * own tests and tools; the shipped C client carries its own plain-POSIX copy
 * because it links no fiber runtime. */
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* One sendmsg with `fd` in the ancillary data when fd >= 0. timeout_ms as
 * fipsd_native_send_fiber; 0 is the non-parking try the listener's hand-off
 * needs. Returns bytes sent or -1 with errno. */
ssize_t fipsd_native_sendmsg_fd(int sock, const void *data, size_t len, int fd, int timeout_ms);
typedef struct fipsd_native_chunk {
    size_t len;      /* bytes in the caller's buffer; zero is end of file */
    int fd;          /* the descriptor the message carried, or -1 */
    bool truncated;  /* MSG_CTRUNC: a descriptor was lost */
} fipsd_native_chunk;
/* One recvmsg collecting every descriptor the control buffer carried (the
 * first is kept close-on-exec, extras are closed), reporting ECONNRESET as
 * end of file. Fiber-parked; returns 0 or -1 with errno. */
int fipsd_native_recvmsg_fd(int sock, void *buf, size_t cap, fipsd_native_chunk *out, int timeout_ms);
#endif
