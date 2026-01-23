/* main_native_host.c - Native messaging host for NIP-07 browser extension
 *
 * This is the entry point for the native messaging host binary that communicates
 * with browser extensions (Chrome/Firefox) via native messaging protocol.
 *
 * Usage:
 *   gnostr-signer-native [OPTIONS]
 *
 * The host is launched by the browser when an extension requests access to
 * the window.nostr API. It communicates via stdin/stdout using the native
 * messaging protocol (4-byte length prefix + JSON).
 *
 * Security:
 * - Core dumps are disabled to protect secret keys
 * - The host runs as the current user with their keychain access
 * - Origin information from extensions is passed for policy decisions
 */
#include "native_messaging.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif

#define VERSION "0.1.0"
#define PROGRAM_NAME "gnostr-signer-native"

/* Global context for signal handling */
static NativeMessagingContext *g_ctx = NULL;

/* Signal handler for graceful shutdown */
static void handle_signal(int sig) {
  (void)sig;
  /* The main loop will exit when stdin is closed */
  exit(0);
}

/* Disable core dumps for security */
static void disable_core_dumps(void) {
#ifdef _WIN32
  /* Windows: Prevent creating minidumps */
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
  struct rlimit rl;
  rl.rlim_cur = 0;
  rl.rlim_max = 0;
  setrlimit(RLIMIT_CORE, &rl);
#endif

#ifdef __linux__
  /* Linux: Also prevent ptrace attachment */
  prctl(PR_SET_DUMPABLE, 0);
#endif
}

/* Print usage information */
static void print_usage(void) {
  g_print("Usage: %s [OPTIONS]\n", PROGRAM_NAME);
  g_print("\n");
  g_print("NIP-07 Native Messaging Host for browser extensions\n");
  g_print("\n");
  g_print("This program is normally launched by a browser when an extension\n");
  g_print("requests access to the window.nostr API. It should not typically\n");
  g_print("be run directly.\n");
  g_print("\n");
  g_print("Options:\n");
  g_print("  -h, --help       Show this help message\n");
  g_print("  -v, --version    Show version information\n");
  g_print("  --identity NPUB  Use specific identity for signing\n");
  g_print("  --auto-approve   Auto-approve all requests (dangerous)\n");
  g_print("\n");
  g_print("Environment Variables:\n");
  g_print("  GNOSTR_SIGNER_IDENTITY   Default identity to use\n");
  g_print("  GNOSTR_SIGNER_DEBUG      Enable debug logging to stderr\n");
  g_print("\n");
}

/* Print version information */
static void print_version(void) {
  g_print("%s version %s\n", PROGRAM_NAME, VERSION);
}

/* Authorization callback - for now auto-approve
 * In a real implementation, this would communicate with the main
 * gnostr-signer UI to get user approval */
static gboolean auth_callback(const NativeMessagingRequest *req,
                              const gchar *preview,
                              gpointer user_data) {
  gboolean auto_approve = GPOINTER_TO_INT(user_data);

  if (auto_approve) {
    return TRUE;
  }

  /* TODO: Implement proper UI approval via D-Bus to gnostr-signer */
  /* For now, log the request and approve */
  g_printerr("[%s] Request: %s - %s\n", PROGRAM_NAME,
             req->method_str ? req->method_str : "unknown",
             preview ? preview : "");

  /* Auto-approve for testing - production should prompt user */
  return TRUE;
}

int main(int argc, char **argv) {
  gboolean show_help = FALSE;
  gboolean show_version = FALSE;
  gboolean auto_approve = FALSE;
  gchar *identity = NULL;

  /* Parse command line arguments */
  for (int i = 1; i < argc; i++) {
    if (g_strcmp0(argv[i], "-h") == 0 || g_strcmp0(argv[i], "--help") == 0) {
      show_help = TRUE;
    } else if (g_strcmp0(argv[i], "-v") == 0 || g_strcmp0(argv[i], "--version") == 0) {
      show_version = TRUE;
    } else if (g_strcmp0(argv[i], "--auto-approve") == 0) {
      auto_approve = TRUE;
    } else if (g_strcmp0(argv[i], "--identity") == 0 && i + 1 < argc) {
      identity = argv[++i];
    }
  }

  if (show_help) {
    print_usage();
    return 0;
  }

  if (show_version) {
    print_version();
    return 0;
  }

  /* Security: Disable core dumps */
  disable_core_dumps();

  /* Set up signal handlers */
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);
#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
#endif

  /* Check for identity from environment */
  if (!identity) {
    identity = (gchar *)g_getenv("GNOSTR_SIGNER_IDENTITY");
  }

  /* Enable debug logging if requested */
  if (g_getenv("GNOSTR_SIGNER_DEBUG")) {
    g_printerr("[%s] Starting native messaging host v%s\n", PROGRAM_NAME, VERSION);
    if (identity) {
      g_printerr("[%s] Using identity: %s\n", PROGRAM_NAME, identity);
    }
  }

  /* Create context */
  g_ctx = native_messaging_context_new(identity);

  /* Set authorization callback */
  native_messaging_set_authorize_cb(g_ctx, auth_callback, GINT_TO_POINTER(auto_approve));

  /* Run the message loop */
  NativeMessagingError rc = native_messaging_run(g_ctx);

  /* Cleanup */
  native_messaging_context_free(g_ctx);
  g_ctx = NULL;

  if (g_getenv("GNOSTR_SIGNER_DEBUG")) {
    g_printerr("[%s] Shutting down (rc=%d)\n", PROGRAM_NAME, rc);
  }

  return (rc == NM_OK) ? 0 : 1;
}
