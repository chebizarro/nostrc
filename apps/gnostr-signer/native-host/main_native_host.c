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
#include <gio/gio.h>
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

/* D-Bus approval request context */
typedef struct {
  GMainLoop *loop;
  gchar *request_id;
  gboolean approved;
  gboolean got_response;
} ApprovalContext;

/* D-Bus constants */
#define SIGNER_DBUS_NAME "org.nostr.Signer"
#define SIGNER_DBUS_PATH "/org/nostr/signer"
#define SIGNER_DBUS_INTERFACE "org.nostr.Signer"

/* Handle ApprovalCompleted signal */
static void on_approval_completed(GDBusConnection *connection,
                                  const gchar *sender_name,
                                  const gchar *object_path,
                                  const gchar *interface_name,
                                  const gchar *signal_name,
                                  GVariant *parameters,
                                  gpointer user_data) {
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;

  ApprovalContext *ctx = user_data;
  const gchar *request_id = NULL;
  gboolean decision = FALSE;

  g_variant_get(parameters, "(&sb)", &request_id, &decision);

  if (g_strcmp0(request_id, ctx->request_id) == 0) {
    ctx->approved = decision;
    ctx->got_response = TRUE;
    g_main_loop_quit(ctx->loop);
  }
}

/* Timeout callback */
static gboolean approval_timeout(gpointer user_data) {
  ApprovalContext *ctx = user_data;
  g_main_loop_quit(ctx->loop);
  return G_SOURCE_REMOVE;
}

/* Request approval via D-Bus */
static gboolean request_dbus_approval(const gchar *app_id,
                                       const gchar *identity,
                                       const gchar *kind,
                                       const gchar *preview) {
  GError *error = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);

  if (!bus) {
    g_printerr("[%s] Failed to connect to session bus: %s\n",
               PROGRAM_NAME, error ? error->message : "unknown");
    g_clear_error(&error);
    return FALSE;
  }

  /* Generate unique request ID */
  gchar *request_id = g_uuid_string_random();

  /* Set up approval context */
  ApprovalContext ctx = {
    .loop = g_main_loop_new(NULL, FALSE),
    .request_id = request_id,
    .approved = FALSE,
    .got_response = FALSE
  };

  /* Subscribe to ApprovalCompleted signal */
  guint sub_id = g_dbus_connection_signal_subscribe(
    bus,
    SIGNER_DBUS_NAME,
    SIGNER_DBUS_INTERFACE,
    "ApprovalCompleted",
    SIGNER_DBUS_PATH,
    NULL,
    G_DBUS_SIGNAL_FLAGS_NONE,
    on_approval_completed,
    &ctx,
    NULL
  );

  /* Emit ApprovalRequested signal */
  g_dbus_connection_emit_signal(
    bus,
    NULL, /* destination - broadcast */
    SIGNER_DBUS_PATH,
    SIGNER_DBUS_INTERFACE,
    "ApprovalRequested",
    g_variant_new("(sssss)",
                  app_id ? app_id : "unknown",
                  identity ? identity : "",
                  kind ? kind : "event",
                  preview ? preview : "",
                  request_id),
    &error
  );

  if (error) {
    g_printerr("[%s] Failed to emit ApprovalRequested: %s\n",
               PROGRAM_NAME, error->message);
    g_clear_error(&error);
    g_dbus_connection_signal_unsubscribe(bus, sub_id);
    g_main_loop_unref(ctx.loop);
    g_free(request_id);
    g_object_unref(bus);
    return FALSE;
  }

  /* Set 60 second timeout */
  guint timeout_id = g_timeout_add_seconds(60, approval_timeout, &ctx);

  /* Wait for response */
  g_main_loop_run(ctx.loop);

  /* Cleanup */
  if (g_source_remove(timeout_id)) {
    /* Timeout was still pending, source removed successfully */
  }
  g_dbus_connection_signal_unsubscribe(bus, sub_id);
  g_main_loop_unref(ctx.loop);
  g_free(request_id);
  g_object_unref(bus);

  if (!ctx.got_response) {
    g_printerr("[%s] Approval request timed out\n", PROGRAM_NAME);
    return FALSE;
  }

  return ctx.approved;
}

/* Authorization callback - requests approval via D-Bus to gnostr-signer UI */
static gboolean auth_callback(const NativeMessagingRequest *req,
                              const gchar *preview,
                              gpointer user_data) {
  gboolean auto_approve = GPOINTER_TO_INT(user_data);

  if (auto_approve) {
    return TRUE;
  }

  /* Log the request */
  g_printerr("[%s] Request: %s - %s\n", PROGRAM_NAME,
             req->method_str ? req->method_str : "unknown",
             preview ? preview : "");

  /* Request approval via D-Bus to gnostr-signer UI */
  gboolean approved = request_dbus_approval(
    req->origin,      /* app_id - browser extension origin */
    NULL,             /* identity - use default */
    req->method_str,  /* kind - the request type */
    preview           /* preview - human-readable content */
  );

  if (!approved) {
    g_printerr("[%s] Request denied by user\n", PROGRAM_NAME);
  }

  return approved;
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
