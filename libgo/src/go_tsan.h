/* TSan handoffs for an uninstrumented system nsync (nostrc-u6v55.14.4).
 * Instrumented nsync already annotates its locks and CV unlock/relock.
 * Do not wrap that with __tsan_mutex_pre/post_*: their ignore-sync scopes
 * hide nsync's own transitions, and a CV wait cannot be modelled as one
 * uninterrupted mutex operation. This produced races even with the same
 * mutex in both reported locksets. Acquire/release handoffs add the missing
 * happens-before edges for system nsync without nesting lock annotations
 * or hiding any memory accesses/synchronization from instrumented nsync.
 * All channel/select callers use the same helpers.
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
extern void __tsan_acquire(void *addr);
extern void __tsan_release(void *addr);
static inline void tsan_mu_lock(nsync_mu *m){ nsync_mu_lock(m); __tsan_acquire(m); }
static inline void tsan_mu_unlock(nsync_mu *m){ __tsan_release(m); nsync_mu_unlock(m); }
static inline void tsan_cv_wait(nsync_cv *cv, nsync_mu *m){ __tsan_release(m); nsync_cv_wait(cv, m); __tsan_acquire(m); }
static inline int tsan_cv_wait_with_deadline(nsync_cv *cv, nsync_mu *m, nsync_time deadline, nsync_note note){
    int rc;
    __tsan_release(m);
    rc = nsync_cv_wait_with_deadline(cv, m, deadline, note);
    __tsan_acquire(m);
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
