// gn-crashtrace.c - Self-reporting backtrace on GLib fatal assertions
// Catches refcount underflows, g_error, g_critical without debugger
#define _GNU_SOURCE
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <dlfcn.h>
#include <glib.h>

static void gn_print_backtrace(const char *tag) {
  void *frames[128];
  int n = backtrace(frames, 128);

  dprintf(STDERR_FILENO, "===== BACKTRACE =====\n");
  if (tag) dprintf(STDERR_FILENO, "%s\n", tag);

  for (int i = 0; i < n; i++) {
    Dl_info info = {0};
    if (dladdr(frames[i], &info) && info.dli_fname) {
      const char *sym = info.dli_sname ? info.dli_sname : "??";
      uintptr_t off = info.dli_saddr ? (uintptr_t)frames[i] - (uintptr_t)info.dli_saddr : 0;
      dprintf(STDERR_FILENO, "#%02d %p %s!%s+0x%lx\n",
              i, frames[i], info.dli_fname, sym, (unsigned long)off);
    } else {
      dprintf(STDERR_FILENO, "#%02d %p ??\n", i, frames[i]);
    }
  }
  dprintf(STDERR_FILENO, "=====================\n");
}

static void gn_glib_log_handler(const gchar *domain, GLogLevelFlags level,
                                const gchar *message, gpointer user_data) {
  // Print the log line first
  g_log_default_handler(domain, level, message, user_data);

  // If it's critical/warning/error and GLib is going to abort, dump stack now.
  if (level & (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR)) {
    gn_print_backtrace("GLib fatal log");
    abort();
  }
}

static void gn_signal_handler(int sig) {
  gn_print_backtrace("Signal");
  _exit(128 + sig);
}

void gn_install_crashtrace(void) {
  // Ensure we do NOT lose the opportunity to dump
  g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);
  g_log_set_default_handler(gn_glib_log_handler, NULL);

  // Also handle SIGABRT/SIGTRAP because GLib often ends there
  struct sigaction sa = {0};
  sa.sa_handler = gn_signal_handler;
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGTRAP, &sa, NULL);
}
