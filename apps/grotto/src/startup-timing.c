/* startup-timing.c - Startup profiling implementation
 *
 * SPDX-License-Identifier: MIT
 */
#include "startup-timing.h"
#include <stdio.h>
#include <string.h>

/* Phase names for reporting */
static const gchar *phase_names[] = {
  [STARTUP_PHASE_INIT]     = "init",
  [STARTUP_PHASE_SETTINGS] = "settings",
  [STARTUP_PHASE_THEME]    = "theme",
  [STARTUP_PHASE_CSS]      = "css",
  [STARTUP_PHASE_TYPES]    = "types",
  [STARTUP_PHASE_WINDOW]   = "window",
  [STARTUP_PHASE_PAGES]    = "pages",
  [STARTUP_PHASE_SECRETS]  = "secrets",
  [STARTUP_PHASE_ACCOUNTS] = "accounts",
  [STARTUP_PHASE_DBUS]     = "dbus",
  [STARTUP_PHASE_READY]    = "ready",
};

/* Timing state */
static gboolean timing_enabled = FALSE;
static gint64 startup_time = 0;
static gint64 phase_start[STARTUP_PHASE_COUNT] = {0};
static gint64 phase_duration[STARTUP_PHASE_COUNT] = {0};
static gboolean phase_completed[STARTUP_PHASE_COUNT] = {0};

/* Custom marks (up to 32) */
#define MAX_MARKS 32
static struct {
  gchar *label;
  gint64 timestamp;
} custom_marks[MAX_MARKS];
static guint num_marks = 0;

void startup_timing_init(void) {
  startup_time = g_get_monotonic_time();

  /* Check if timing is enabled via G_MESSAGES_DEBUG */
  const gchar *debug_env = g_getenv("G_MESSAGES_DEBUG");
  if (debug_env) {
    timing_enabled = (strstr(debug_env, "startup") != NULL ||
                      strcmp(debug_env, "all") == 0 ||
                      strcmp(debug_env, "*") == 0);
  }

  if (timing_enabled) {
    g_message("startup-timing: Profiling enabled, T=0.000ms");
  }
}

gboolean startup_timing_is_enabled(void) {
  return timing_enabled;
}

void startup_timing_begin(StartupPhase phase) {
  if (!timing_enabled) return;
  if (phase >= STARTUP_PHASE_COUNT) return;

  phase_start[phase] = g_get_monotonic_time();

  gdouble elapsed = (phase_start[phase] - startup_time) / 1000.0;
  g_message("startup-timing: [%s] BEGIN @ T+%.3fms",
            phase_names[phase], elapsed);
}

void startup_timing_end(StartupPhase phase) {
  if (!timing_enabled) return;
  if (phase >= STARTUP_PHASE_COUNT) return;

  gint64 now = g_get_monotonic_time();
  phase_duration[phase] = now - phase_start[phase];
  phase_completed[phase] = TRUE;

  gdouble elapsed = (now - startup_time) / 1000.0;
  gdouble duration = phase_duration[phase] / 1000.0;
  g_message("startup-timing: [%s] END @ T+%.3fms (%.3fms)",
            phase_names[phase], elapsed, duration);
}

void startup_timing_mark(const gchar *label) {
  if (!timing_enabled) return;
  if (!label || num_marks >= MAX_MARKS) return;

  gint64 now = g_get_monotonic_time();
  custom_marks[num_marks].label = g_strdup(label);
  custom_marks[num_marks].timestamp = now;
  num_marks++;

  gdouble elapsed = (now - startup_time) / 1000.0;
  g_message("startup-timing: [mark] %s @ T+%.3fms", label, elapsed);
}

void startup_timing_report(void) {
  if (!timing_enabled) return;

  gint64 now = g_get_monotonic_time();
  gdouble total_ms = (now - startup_time) / 1000.0;

  g_message("startup-timing: ========== STARTUP REPORT ==========");
  g_message("startup-timing: Total startup time: %.3fms", total_ms);
  g_message("startup-timing: Phase breakdown:");

  gdouble accounted = 0.0;
  for (guint i = 0; i < STARTUP_PHASE_COUNT; i++) {
    if (phase_completed[i]) {
      gdouble dur_ms = phase_duration[i] / 1000.0;
      gdouble pct = (dur_ms / total_ms) * 100.0;
      g_message("startup-timing:   %-12s: %7.3fms (%5.1f%%)",
                phase_names[i], dur_ms, pct);
      accounted += dur_ms;
    }
  }

  gdouble overhead = total_ms - accounted;
  if (overhead > 0.1) {
    gdouble pct = (overhead / total_ms) * 100.0;
    g_message("startup-timing:   %-12s: %7.3fms (%5.1f%%)",
              "overhead", overhead, pct);
  }

  if (num_marks > 0) {
    g_message("startup-timing: Custom marks:");
    for (guint i = 0; i < num_marks; i++) {
      gdouble t = (custom_marks[i].timestamp - startup_time) / 1000.0;
      g_message("startup-timing:   @ T+%7.3fms: %s", t, custom_marks[i].label);
      g_free(custom_marks[i].label);
    }
    num_marks = 0;
  }

  g_message("startup-timing: ====================================");

  /* Performance warnings */
  for (guint i = 0; i < STARTUP_PHASE_COUNT; i++) {
    if (phase_completed[i]) {
      gdouble dur_ms = phase_duration[i] / 1000.0;
      if (dur_ms > 100.0) {
        g_warning("startup-timing: SLOW PHASE [%s]: %.3fms > 100ms threshold",
                  phase_names[i], dur_ms);
      }
    }
  }
}

gdouble startup_timing_get_elapsed_ms(void) {
  if (startup_time == 0) return 0.0;
  gint64 now = g_get_monotonic_time();
  return (now - startup_time) / 1000.0;
}
