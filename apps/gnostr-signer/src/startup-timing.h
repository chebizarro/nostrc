/* startup-timing.h - Startup profiling infrastructure
 *
 * Provides timing macros and functions to profile application startup.
 * Enabled when G_MESSAGES_DEBUG includes "startup" or "*".
 *
 * Usage:
 *   G_MESSAGES_DEBUG=startup ./gnostr-signer
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APPS_GNOSTR_SIGNER_STARTUP_TIMING_H
#define APPS_GNOSTR_SIGNER_STARTUP_TIMING_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * StartupPhase:
 * @STARTUP_PHASE_INIT: Initial GLib/GTK setup
 * @STARTUP_PHASE_SETTINGS: Settings manager initialization
 * @STARTUP_PHASE_THEME: Theme and CSS loading
 * @STARTUP_PHASE_WINDOW: Main window creation
 * @STARTUP_PHASE_PAGES: Page widget instantiation
 * @STARTUP_PHASE_SECRETS: Secret store enumeration
 * @STARTUP_PHASE_ACCOUNTS: Account store loading
 * @STARTUP_PHASE_DBUS: D-Bus registration and signal setup
 * @STARTUP_PHASE_READY: Application ready for interaction
 *
 * Phases of application startup for profiling purposes.
 */
typedef enum {
  STARTUP_PHASE_INIT = 0,
  STARTUP_PHASE_SETTINGS,
  STARTUP_PHASE_THEME,
  STARTUP_PHASE_CSS,       /* CSS stylesheet loading */
  STARTUP_PHASE_TYPES,     /* GType registration for UI widgets */
  STARTUP_PHASE_WINDOW,
  STARTUP_PHASE_PAGES,
  STARTUP_PHASE_SECRETS,
  STARTUP_PHASE_ACCOUNTS,
  STARTUP_PHASE_DBUS,
  STARTUP_PHASE_READY,
  STARTUP_PHASE_COUNT
} StartupPhase;

/**
 * startup_timing_init:
 *
 * Initialize the startup timing system. Call this as early as possible
 * in main(), before any other initialization.
 */
void startup_timing_init(void);

/**
 * startup_timing_is_enabled:
 *
 * Check if startup timing is enabled (G_MESSAGES_DEBUG includes "startup").
 *
 * Returns: %TRUE if timing is enabled
 */
gboolean startup_timing_is_enabled(void);

/**
 * startup_timing_begin:
 * @phase: the startup phase beginning
 *
 * Mark the beginning of a startup phase.
 */
void startup_timing_begin(StartupPhase phase);

/**
 * startup_timing_end:
 * @phase: the startup phase ending
 *
 * Mark the end of a startup phase and log the elapsed time.
 */
void startup_timing_end(StartupPhase phase);

/**
 * startup_timing_mark:
 * @label: a custom label for the timing mark
 *
 * Record a timing mark with a custom label. Useful for sub-phase timing.
 */
void startup_timing_mark(const gchar *label);

/**
 * startup_timing_report:
 *
 * Print a summary report of all startup phase timings.
 * Call this after STARTUP_PHASE_READY.
 */
void startup_timing_report(void);

/**
 * startup_timing_get_elapsed_ms:
 *
 * Get total elapsed time since startup_timing_init() in milliseconds.
 *
 * Returns: elapsed time in milliseconds
 */
gdouble startup_timing_get_elapsed_ms(void);

/* Convenience macros for scoped timing */
#define STARTUP_TIME_BEGIN(phase) startup_timing_begin(phase)
#define STARTUP_TIME_END(phase) startup_timing_end(phase)
#define STARTUP_TIME_MARK(label) startup_timing_mark(label)

/* Auto-scoped timing block (requires GCC/Clang) */
#ifdef __GNUC__
static inline void _startup_timing_cleanup(StartupPhase *phase) {
  if (phase) startup_timing_end(*phase);
}
#define STARTUP_TIME_SCOPE(phase) \
  __attribute__((cleanup(_startup_timing_cleanup))) StartupPhase _phase_##phase = phase; \
  startup_timing_begin(phase)
#else
#define STARTUP_TIME_SCOPE(phase) startup_timing_begin(phase)
#endif

/**
 * STARTUP_PROFILE_BLOCK:
 * @name: a string label for the block
 *
 * Simple macro to log time elapsed from startup to this point.
 * Useful for quick profiling without defining a full phase.
 */
#define STARTUP_PROFILE_BLOCK(name) \
  do { \
    if (startup_timing_is_enabled()) { \
      g_debug("[STARTUP] %s: %.3fms", name, startup_timing_get_elapsed_ms()); \
    } \
  } while (0)

/**
 * STARTUP_MARK_IF_SLOW:
 * @name: label for the operation
 * @threshold_ms: threshold in milliseconds
 *
 * Only log a mark if the operation took longer than the threshold.
 * Call STARTUP_TIME_MARK first, then this macro with the same label.
 */
#define STARTUP_WARN_IF_SLOW(name, duration_ms, threshold_ms) \
  do { \
    if (startup_timing_is_enabled() && (duration_ms) > (threshold_ms)) { \
      g_warning("[STARTUP] SLOW: %s took %.3fms (threshold: %dms)", \
                name, (double)(duration_ms), threshold_ms); \
    } \
  } while (0)

/**
 * startup_timing_measure_call:
 * @label: description of the operation
 * @threshold_ms: warn if operation exceeds this (0 to disable)
 *
 * Helper to measure a function call duration.
 * Returns the start time for use with startup_timing_measure_end.
 */
static inline gint64 startup_timing_measure_start(void) {
  return g_get_monotonic_time();
}

static inline void startup_timing_measure_end(gint64 start_time,
                                               const gchar *label,
                                               gint threshold_ms) {
  if (!startup_timing_is_enabled()) return;
  gint64 elapsed = (g_get_monotonic_time() - start_time) / 1000;
  if (threshold_ms > 0 && elapsed > threshold_ms) {
    g_warning("[STARTUP] SLOW: %s took %" G_GINT64_FORMAT "ms (threshold: %dms)",
              label, elapsed, threshold_ms);
  } else {
    g_debug("[STARTUP] %s: %" G_GINT64_FORMAT "ms", label, elapsed);
  }
}

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_STARTUP_TIMING_H */
