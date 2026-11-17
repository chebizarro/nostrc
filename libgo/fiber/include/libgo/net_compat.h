#ifndef LIBGO_FIBER_NET_COMPAT_H
#define LIBGO_FIBER_NET_COMPAT_H
/*
 * POSIX-shaped socket message types for Windows (included by libgo/fiber.h).
 *
 * Winsock's native shapes are WSAMSG/WSABUF/WSACMSGHDR. Callers of
 * gof_recvmsg()/gof_sendmsg() build the POSIX struct msghdr/struct iovec
 * declared here, with the POSIX field names, and libgo translates them to
 * WSAMSG/WSABUF. Ancillary data keeps Winsock's own layout: struct cmsghdr is
 * WSACMSGHDR on Windows (mswsock.h under mingw-w64, ws2def.h in the Windows
 * SDK), so the kernel writes control messages
 * straight into msg_control and the CMSG_* macros below walk a struct msghdr
 * with the WSA_CMSG_* alignment rules (header and data aligned to SIZE_T).
 * Winsock's own CMSG_* aliases take a WSAMSG and are replaced here.
 *
 * Nothing in this header is defined on POSIX.
 */
#if defined(_WIN32) || defined(__MINGW32__)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h> /* struct cmsghdr (WSACMSGHDR) on mingw-w64 */
#include <stddef.h>

struct iovec {
  void  *iov_base;
  size_t iov_len;   /* each element must fit a WSABUF (ULONG) */
};

struct msghdr {
  void         *msg_name;       /* optional peer address (struct sockaddr_*) */
  socklen_t     msg_namelen;    /* in: capacity; out: length (receive) */
  struct iovec *msg_iov;
  size_t        msg_iovlen;
  void         *msg_control;    /* WSACMSGHDR-formatted ancillary data */
  size_t        msg_controllen; /* in: capacity; out: length (receive) */
  int           msg_flags;      /* out: MSG_TRUNC, MSG_CTRUNC, MSG_BCAST, MSG_MCAST */
};

/* WSA_CMSGHDR_ALIGN and WSA_CMSGDATA_ALIGN both align to SIZE_T (8 bytes on
 * Win64, 4 on Win32); spelled with sizeof so CMSG_SPACE stays an integer
 * constant expression usable in array bounds. */
#define GOF_CMSG_ALIGN(n) (((size_t)(n) + sizeof(SIZE_T) - 1) & ~(sizeof(SIZE_T) - 1))

#undef CMSG_ALIGN
#undef CMSG_FIRSTHDR
#undef CMSG_NXTHDR
#undef CMSG_DATA
#undef CMSG_SPACE
#undef CMSG_LEN
#define CMSG_ALIGN(n)  GOF_CMSG_ALIGN(n)
#define CMSG_LEN(n)    (GOF_CMSG_ALIGN(sizeof(struct cmsghdr)) + (size_t)(n))
#define CMSG_SPACE(n)  (GOF_CMSG_ALIGN(sizeof(struct cmsghdr)) + GOF_CMSG_ALIGN(n))
#define CMSG_DATA(c)   ((unsigned char *)(c) + GOF_CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_FIRSTHDR(m) \
  ((m)->msg_controllen >= sizeof(struct cmsghdr) ? (struct cmsghdr *)(m)->msg_control \
                                                 : (struct cmsghdr *)NULL)
#define CMSG_NXTHDR(m, c) gof_cmsg_nxthdr((m), (c))

static inline struct cmsghdr *gof_cmsg_nxthdr(const struct msghdr *m, const struct cmsghdr *c) {
  if (!c) return CMSG_FIRSTHDR(m);
  unsigned char *next = (unsigned char *)c + GOF_CMSG_ALIGN(c->cmsg_len);
  unsigned char *end = (unsigned char *)m->msg_control + m->msg_controllen;
  if (next + sizeof(struct cmsghdr) > end) return NULL;
  return (struct cmsghdr *)next;
}

#endif /* _WIN32 */
#endif /* LIBGO_FIBER_NET_COMPAT_H */
