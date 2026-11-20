#ifndef FIPSD_UDP_LOCK_H
#define FIPSD_UDP_LOCK_H
#include <nsync.h>
/* The instance and DNS-cache locks are also taken on crypto worker threads,
 * which send sealed data-lane frames themselves (nostrc-u19cz.3). The system
 * nsync is not TSan-instrumented, so publish each lock's acquire/release to
 * TSan as libgo's NLOCK does; otherwise these are plain nsync calls. */
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define FIPSD_UDP_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) && !defined(FIPSD_UDP_TSAN)
#define FIPSD_UDP_TSAN 1
#endif
#ifdef FIPSD_UDP_TSAN
void __tsan_acquire(void *addr);
void __tsan_release(void *addr);
static inline void udp_mu_lock(nsync_mu *m) { nsync_mu_lock(m); __tsan_acquire(m); }
static inline void udp_mu_unlock(nsync_mu *m) { __tsan_release(m); nsync_mu_unlock(m); }
#else
static inline void udp_mu_lock(nsync_mu *m) { nsync_mu_lock(m); }
static inline void udp_mu_unlock(nsync_mu *m) { nsync_mu_unlock(m); }
#endif
#endif
