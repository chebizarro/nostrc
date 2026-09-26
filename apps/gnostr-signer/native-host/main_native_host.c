/* main_native_host.c - nostr-signer-webext-host (nostrc-jjyp)
 *
 * Native-messaging host for the NIP-07 browser extension
 * (browser-extension/nip07/). The browser launches it through the
 * org.nostr.signer_bridge host manifest and talks to it over stdin/stdout
 * with 4-byte length-prefixed JSON frames. Every window.nostr call is
 * forwarded to the desktop signer daemon org.nostr.Signer on the session
 * bus with the page origin as app_id, so web clients use the
 * desktop-managed identity and the signer's own approval UI.
 *
 * Threading: a reader thread blocks on stdin and hands whole frames to the
 * main context; all routing, D-Bus I/O and stdout writes happen on the
 * main thread, so replies never interleave.
 *
 * stdout hygiene: the framed channel is moved to a private descriptor and
 * fd 1 is pointed at stderr, so a stray printf or a GLib INFO/DEBUG log
 * line (which GLib writes to stdout) can never corrupt the protocol.
 *
 * Browsers pass extra arguments (Firefox: manifest path + extension id;
 * Chromium: the caller origin, and --parent-window on Windows); they are
 * accepted and ignored - the host manifest's allowed_extensions /
 * allowed_origins is what restricts who may launch this binary.
 */
#include "native_messaging.h"
#include "nm_router.h"

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifndef NM_HOST_VERSION
#define NM_HOST_VERSION "0.2.0"
#endif
#define PROGRAM_NAME "nostr-signer-webext-host"

typedef struct {
  GMainLoop *loop;
  NmRouter *router;
  int out_fd;
  int exit_code;
} Host;

typedef struct {
  Host *host;
  NmFrameStatus status;
  gchar *msg;
  gsize len;
} FrameEvent;

static void disable_core_dumps(void) {
  struct rlimit rl = { 0, 0 };
  (void)setrlimit(RLIMIT_CORE, &rl);
#ifdef __linux__
  (void)prctl(PR_SET_DUMPABLE, 0);
#endif
}

static void on_reply(const gchar *json, gsize len, gpointer user_data) {
  Host *h = user_data;
  NmFrameStatus st = nm_frame_write(h->out_fd, json, len);
  if (st == NM_FRAME_IO) {
    /* Browser went away; nothing left to talk to. */
    g_main_loop_quit(h->loop);
  }
}

static gboolean on_frame(gpointer data) {
  FrameEvent *ev = data;
  Host *h = ev->host;
  switch (ev->status) {
    case NM_FRAME_OK:
      nm_router_handle(h->router, ev->msg, ev->len);
      break;
    case NM_FRAME_TOO_LARGE:
      nm_router_reply_frame_error(h->router, NM_ERR_TOO_LARGE);
      break;
    case NM_FRAME_EMPTY:
      nm_router_reply_frame_error(h->router, NM_ERR_INVALID_REQUEST);
      break;
    case NM_FRAME_EOF:
      g_main_loop_quit(h->loop);
      break;
    case NM_FRAME_IO:
    default:
      h->exit_code = 1;
      g_main_loop_quit(h->loop);
      break;
  }
  return G_SOURCE_REMOVE;
}

static void frame_event_free(gpointer data) {
  FrameEvent *ev = data;
  g_free(ev->msg);
  g_free(ev);
}

static gpointer reader_thread(gpointer data) {
  Host *h = data;
  for (;;) {
    FrameEvent *ev = g_new0(FrameEvent, 1);
    ev->host = h;
    ev->msg = nm_frame_read(STDIN_FILENO, &ev->len, &ev->status);
    NmFrameStatus st = ev->status;
    g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, on_frame, ev, frame_event_free);
    if (st == NM_FRAME_EOF || st == NM_FRAME_IO) break;
  }
  return NULL;
}

static gboolean on_signal(gpointer data) {
  g_main_loop_quit(((Host *)data)->loop);
  return G_SOURCE_REMOVE;
}

static gint env_int(const gchar *name, gint fallback) {
  const gchar *v = g_getenv(name);
  if (!v || !*v) return fallback;
  gint64 n = g_ascii_strtoll(v, NULL, 10);
  return (n > 0 && n < G_MAXINT) ? (gint)n : fallback;
}

static void usage(void) {
  g_printerr(
    "Usage: %s [--identity NPUB]\n"
    "\n"
    "Native-messaging host for the NIP-07 (window.nostr) browser extension.\n"
    "Launched by the browser via the org.nostr.signer_bridge host manifest;\n"
    "forwards every request to org.nostr.Signer on the session bus.\n"
    "\n"
    "  --identity NPUB   signer identity selector (default: active identity)\n"
    "  -h, --help        this help\n"
    "  -v, --version     print the version\n"
    "\n"
    "Environment:\n"
    "  NOSTR_SIGNER_BRIDGE_IDENTITY        same as --identity\n"
    "  NOSTR_SIGNER_BRIDGE_CALL_TIMEOUT_MS  non-interactive call timeout (30000)\n"
    "  NOSTR_SIGNER_BRIDGE_APPROVAL_TIMEOUT_MS  approval call timeout (120000)\n"
    "  NOSTR_SIGNER_BRIDGE_DEBUG=1         debug logging to stderr\n",
    PROGRAM_NAME);
}

int main(int argc, char **argv) {
  const gchar *identity = g_getenv("NOSTR_SIGNER_BRIDGE_IDENTITY");

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      usage();
      return 0;
    } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
      g_printerr("%s %s\n", PROGRAM_NAME, NM_HOST_VERSION);
      return 0;
    } else if (!strcmp(argv[i], "--identity") && i + 1 < argc) {
      identity = argv[++i];
    }
    /* Anything else is a browser-supplied argument; ignore. */
  }

  disable_core_dumps();
  signal(SIGPIPE, SIG_IGN);

  /* Move the protocol channel off fd 1 before anything can log. */
  int out_fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
  if (out_fd < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
    g_printerr("%s: cannot isolate stdout\n", PROGRAM_NAME);
    return 1;
  }

  if (g_getenv("NOSTR_SIGNER_BRIDGE_DEBUG"))
    g_log_set_debug_enabled(TRUE);

  Host h = { 0 };
  h.loop = g_main_loop_new(NULL, FALSE);
  h.out_fd = out_fd;

  NmRouterConfig cfg = {
    .identity = identity ? identity : "",
    .call_timeout_ms = env_int("NOSTR_SIGNER_BRIDGE_CALL_TIMEOUT_MS", 30000),
    .approval_timeout_ms = env_int("NOSTR_SIGNER_BRIDGE_APPROVAL_TIMEOUT_MS", 120000),
    .signer_bus_name = "org.nostr.Signer",
  };
  h.router = nm_router_new(NULL, &cfg, on_reply, &h);

  g_unix_signal_add(SIGINT, on_signal, &h);
  g_unix_signal_add(SIGTERM, on_signal, &h);

  GThread *reader = g_thread_new("nm-reader", reader_thread, &h);
  g_thread_unref(reader); /* detached: it may be blocked in read() at exit */

  g_debug("%s %s started", PROGRAM_NAME, NM_HOST_VERSION);
  g_main_loop_run(h.loop);
  g_debug("%s exiting (%u request(s) in flight)", PROGRAM_NAME, nm_router_in_flight(h.router));

  nm_router_free(h.router);
  /* _exit: the reader thread may still own stdin; skip atexit teardown. */
  _exit(h.exit_code);
}
