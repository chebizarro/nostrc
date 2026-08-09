/* TSAN-aware wrappers for nsync mutex/cv operations.
 *
 * nsync's mutexes are not pthread mutexes, so ThreadSanitizer cannot see
 * the synchronization unless it is told via __tsan_mutex_* annotations.
 * channel.c historically defined these wrappers privately; select.c (and
 * any future file) must use the SAME annotations when touching the same
 * nsync_mu, otherwise TSAN reports false data races between an annotated
 * lock (channel.c) and an invisible raw lock (select.c) on the same mutex.
 */
#ifndef GO_TSAN_H
#define GO_TSAN_H

#include <nsync.h>

#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define GO_TSAN_ENABLED 1
#  endif
#endif
#if defined(__SANITIZE_THREAD__)
#  define GO_TSAN_ENABLED 1
#endif
#ifdef GO_ENABLE_TSAN
#  if GO_ENABLE_TSAN
#    ifndef GO_TSAN_ENABLED
#      define GO_TSAN_ENABLED 1
#    endif
#  endif
#endif

#ifdef GO_TSAN_ENABLED
extern void __tsan_mutex_pre_lock(void *addr, unsigned flags);
extern void __tsan_mutex_post_lock(void *addr, unsigned flags, int recursion);
extern void __tsan_mutex_pre_unlock(void *addr, unsigned flags);
extern void __tsan_mutex_post_unlock(void *addr, unsigned flags);
static inline void tsan_mu_lock(nsync_mu *m){ __tsan_mutex_pre_lock(m, 0); nsync_mu_lock(m); __tsan_mutex_post_lock(m, 0, 0); }
static inline void tsan_mu_unlock(nsync_mu *m){ __tsan_mutex_pre_unlock(m, 0); nsync_mu_unlock(m); __tsan_mutex_post_unlock(m, 0); }
static inline void tsan_cv_wait(nsync_cv *cv, nsync_mu *m){ __tsan_mutex_pre_unlock(m, 0); nsync_cv_wait(cv, m); __tsan_mutex_post_lock(m, 0, 0); }
static inline int tsan_cv_wait_with_deadline(nsync_cv *cv, nsync_mu *m, nsync_time deadline, nsync_note note){
    int rc;
    __tsan_mutex_pre_unlock(m, 0);
    rc = nsync_cv_wait_with_deadline(cv, m, deadline, note);
    __tsan_mutex_post_lock(m, 0, 0);
    return rc;
}
#  define NLOCK(mu_ptr)   tsan_mu_lock((mu_ptr))
#  define NUNLOCK(mu_ptr) tsan_mu_unlock((mu_ptr))
#  define CV_WAIT_OS(cv_ptr, mu_ptr) tsan_cv_wait((cv_ptr), (mu_ptr))
#  define CV_WAIT_DEADLINE_OS(cv_ptr, mu_ptr, deadline, note) \
    tsan_cv_wait_with_deadline((cv_ptr), (mu_ptr), (deadline), (note))
#else
#  define NLOCK(mu_ptr)   nsync_mu_lock((mu_ptr))
#  define NUNLOCK(mu_ptr) nsync_mu_unlock((mu_ptr))
#  define CV_WAIT_OS(cv_ptr, mu_ptr) nsync_cv_wait((cv_ptr), (mu_ptr))
#  define CV_WAIT_DEADLINE_OS(cv_ptr, mu_ptr, deadline, note) \
    nsync_cv_wait_with_deadline((cv_ptr), (mu_ptr), (deadline), (note))
#endif

#endif /* GO_TSAN_H */
