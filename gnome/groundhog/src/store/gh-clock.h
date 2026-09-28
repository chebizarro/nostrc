#ifndef GH_CLOCK_H
#define GH_CLOCK_H

#include <glib.h>

G_BEGIN_DECLS

/* GhClock (privacy charter H6): the single injectable source of wall-clock
 * time, monotonic time, timers and jitter for everything that schedules work
 * (outbox backoff, expiry purge, directory refresh, self-copy delay, WAL
 * checkpoint rate limiting). Production code takes a GhClock instead of
 * calling g_get_real_time(), g_timeout_add() or a PRNG directly, so tests can
 * drive it deterministically with a fake clock and never sleep.
 *
 * The system clock uses GLib time, GSources attached to the calling thread's
 * thread-default main context, and libsodium's CSPRNG for jitter: delays that
 * exist for privacy (timestamp and self-copy jitter) must not be predictable.
 *
 * The fake clock starts at a given wall-clock time and only moves when
 * advanced. gh_clock_fake_advance() dispatches every timeout that becomes due,
 * in deadline order (ties in creation order), with the clock set to that
 * timeout's deadline while its callback runs. Callbacks may add or remove
 * timeouts. Jitter comes from values queued with gh_clock_fake_push_random()
 * and otherwise from a fixed-seed PRNG, so runs are reproducible.
 *
 * Time and random calls on the system clock are thread-safe; its timeouts
 * must be removed from the thread that added them. A fake clock must be used
 * from a single thread. */

typedef struct _GhClock GhClock;

/* Backend for gh_clock_new(). Every member is required. */
typedef struct {
  gint64 (*get_real_time)(gpointer data);      /* microseconds since the Unix epoch */
  gint64 (*get_monotonic_time)(gpointer data); /* microseconds, never decreases */
  guint (*timeout_add)(gpointer data, guint64 interval_ms, GSourceFunc func,
                       gpointer func_data, GDestroyNotify notify);
  gboolean (*source_remove)(gpointer data, guint id);
  guint32 (*random_uniform)(gpointer data, guint32 upper_bound); /* [0, upper_bound) */
} GhClockVTable;

GhClock *gh_clock_new(const GhClockVTable *vtable, gpointer data,
                      GDestroyNotify destroy);
GhClock *gh_clock_new_system(void);
GhClock *gh_clock_ref(GhClock *clock);
void gh_clock_unref(GhClock *clock);

gint64 gh_clock_get_real_time(GhClock *clock);
/* Wall-clock time in whole seconds since the Unix epoch (Nostr timestamps). */
gint64 gh_clock_get_unix(GhClock *clock);
gint64 gh_clock_get_monotonic_time(GhClock *clock);

/* Calls func every interval_ms until it returns G_SOURCE_REMOVE or the source
 * is removed; notify runs once afterwards. Returns a non-zero id. Callers must
 * re-check their deadline when fired: long system-clock intervals are clamped
 * and wall-clock time can jump (suspend/resume). */
guint gh_clock_timeout_add(GhClock *clock, guint64 interval_ms, GSourceFunc func,
                           gpointer data, GDestroyNotify notify);
gboolean gh_clock_source_remove(GhClock *clock, guint id);

/* Uniform in [0, upper_bound); upper_bound must be > 0. */
guint32 gh_clock_random_uniform(GhClock *clock, guint32 upper_bound);
/* Uniform in [min, max] inclusive; max - min must be < G_MAXUINT32. */
gint64 gh_clock_random_range(GhClock *clock, gint64 min, gint64 max);

/* ---- Fake clock (tests) --------------------------------------------------- */

GhClock *gh_clock_new_fake(gint64 start_real_time_us);
gboolean gh_clock_is_fake(GhClock *clock);
/* Moves both clocks forward by delta_us (>= 0), dispatching due timeouts. */
void gh_clock_fake_advance(GhClock *clock, gint64 delta_us);
/* Sets only the wall clock (models suspend/resume or NTP steps); monotonic
 * time does not move and no timeout fires. */
void gh_clock_fake_set_real_time(GhClock *clock, gint64 real_time_us);
/* The next gh_clock_random_uniform(upper) returns value % upper (FIFO). */
void gh_clock_fake_push_random(GhClock *clock, guint32 value);
guint gh_clock_fake_get_n_timeouts(GhClock *clock);
/* Monotonic deadline of the earliest pending timeout, or -1 if none. */
gint64 gh_clock_fake_get_next_deadline(GhClock *clock);
/* Shortest interval among pending timeouts, or G_MAXUINT64 if none. */
guint64 gh_clock_fake_get_shortest_interval_ms(GhClock *clock);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhClock, gh_clock_unref)

G_END_DECLS
#endif
