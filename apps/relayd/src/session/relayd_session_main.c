/*
 * nostr-session-relayd — per-user session-scoped local relay (§3.2 Piece A).
 *
 * The system relayd (`nostrc-relayd`) binds a TCP host:port. This companion
 * binary listens on a Unix-domain socket in `$XDG_RUNTIME_DIR/nostr/` and is
 * intended to run under systemd `--user` as socket-activated infrastructure
 * for the desktop session (GNostr, nostr-dav, nostr-notify-daemon). The
 * socket itself IS the auth boundary: 0600 mode + `$XDG_RUNTIME_DIR` dir
 * perms + a defensive SO_PEERCRED uid==getuid() check at accept.
 *
 * Startup precedence:
 *
 *   1. `sd_listen_fds()` — if systemd handed us a socket, use it. The
 *      `.socket` unit owns creation, mode, and unlink; we MUST NOT unlink
 *      a systemd-owned socket on shutdown.
 *   2. Otherwise, fallback: bind our own socket at `$XDG_RUNTIME_DIR/nostr/
 *      relay.sock` and manage cleanup ourselves.
 *
 * See docs/plans/gnome-integration-and-samba-server-2026-09-25.md §3.2 D3-D5
 * for the design and §3.4 for edge cases.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#if __has_include(<systemd/sd-daemon.h>)
#  include <systemd/sd-daemon.h>
#  define NSR_HAVE_SD_DAEMON 1
#else
#  define NSR_HAVE_SD_DAEMON 0
#  define SD_LISTEN_FDS_START 3
static int sd_listen_fds(int u) { (void)u; return 0; }
static int sd_notify(int u, const char *s) { (void)u; (void)s; return 0; }
static int sd_is_socket_unix(int fd, int t, int lst, const char *path, size_t plen) {
  (void)fd; (void)t; (void)lst; (void)path; (void)plen; return 0;
}
#endif

#include "nostr-json.h"
#include "nostr-relay-server.h"
#include "nostr-storage.h"
#include "relayd_config.h"
#include "session_dbus.h"
#ifdef NSR_HAVE_FEDERATION
#include "session_federation.h"
#include "session_tee_storage.h"
#endif
#ifdef NOSTRC_HAVE_NOSTRDB_STORAGE
#include "nostrdb_storage.h"
#endif

#ifndef NSR_VERSION
#define NSR_VERSION "" /* unknown: the build did not pass one */
#endif

/* Shutdown flag flipped by SIGTERM/SIGINT. The library also installs its own
 * signal handlers once run() is called; before then we must catch the signal
 * ourselves so we can drain and unlink the fallback socket cleanly. */
static volatile sig_atomic_t s_stop = 0;
/* Library observes this via NostrRelayServerConfig.stop_flag. */
static volatile int s_stop_flag = 0;

static void on_stop(int sig) {
  (void)sig;
  s_stop = 1;
  s_stop_flag = 1;
}

/*
 * $XDG_RUNTIME_DIR resolution for the fallback bind path. Docs say a normal
 * user session on any modern distro sets this; we do not want to guess.
 */
static const char *xdg_runtime_dir(void) {
  const char *r = getenv("XDG_RUNTIME_DIR");
  return (r && *r == '/') ? r : NULL;
}

/*
 * Session storage directory: `~/.local/share/nostr/session-relay/` per
 * §3.2 D3. Ensure the intermediate dirs exist with 0700 permissions.
 * Returns 0 on success, -1 on failure (message on stderr).
 */
static int resolve_storage_dir(char *out, size_t out_sz) {
  const char *home = getenv("HOME");
  if (!home || !*home) {
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir) home = pw->pw_dir;
  }
  if (!home || !*home) {
    fprintf(stderr, "nostr-session-relayd: HOME unset\n");
    return -1;
  }
  const char *xdg_data = getenv("XDG_DATA_HOME");
  int n;
  if (xdg_data && xdg_data[0] == '/') {
    n = snprintf(out, out_sz, "%s/nostr/session-relay", xdg_data);
  } else {
    n = snprintf(out, out_sz, "%s/.local/share/nostr/session-relay", home);
  }
  if (n < 0 || (size_t)n >= out_sz) {
    fprintf(stderr, "nostr-session-relayd: storage path too long\n");
    return -1;
  }
  /* Create parents lazily; ignore EEXIST. */
  char tmp[512];
  size_t len = strlen(out);
  if (len >= sizeof tmp) return -1;
  memcpy(tmp, out, len + 1);
  for (size_t i = 1; i <= len; i++) {
    if (tmp[i] == '/' || tmp[i] == '\0') {
      char c = tmp[i];
      tmp[i] = '\0';
      if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "nostr-session-relayd: mkdir(%s): %s\n", tmp,
                strerror(errno));
        return -1;
      }
      tmp[i] = c;
    }
  }
  return 0;
}

/*
 * Determine whether the address bound to a Unix listen fd is owned by systemd
 * (matches `%t/nostr/relay.sock`, the socket-unit-declared path). Used to
 * decide whether we should unlink on exit.
 *
 * Returns 0 if the fd is not a Unix listen socket (odd — treat as
 * fallback-owned), 1 if we should keep the socket (systemd-owned), 0 if
 * we should treat as caller-owned.
 */
static int fd_is_systemd_owned(int fd) {
#if NSR_HAVE_SD_DAEMON
  /* If sd_listen_fds handed us this fd, systemd owns the path. Callers set
   * this flag from the sd_listen_fds() return path. */
  return sd_is_socket_unix(fd, SOCK_STREAM, /*listening=*/1, NULL, 0) > 0;
#else
  (void)fd;
  return 0;
#endif
}

/*
 * AF_UNIX stream socket, close-on-exec and optionally non-blocking. Linux
 * sets both atomically through socket(2) flags; macOS/BSDs without
 * SOCK_CLOEXEC / SOCK_NONBLOCK get them through fcntl() right after
 * creation (this process spawns no children in between).
 */
static int unix_stream_socket(int nonblock) {
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
  return socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0), 0);
#else
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0) return -1;
  int fdflags = fcntl(s, F_GETFD, 0);
  int flflags = fcntl(s, F_GETFL, 0);
  if (fdflags < 0 || fcntl(s, F_SETFD, fdflags | FD_CLOEXEC) != 0 || flflags < 0 ||
      (nonblock && fcntl(s, F_SETFL, flflags | O_NONBLOCK) != 0)) {
    int e = errno;
    close(s);
    errno = e;
    return -1;
  }
  return s;
#endif
}

/*
 * Fallback bind path: create $XDG_RUNTIME_DIR/nostr/relay.sock with 0600.
 * `sock_path_out` returns the absolute path so we can unlink at exit.
 * `owned_out` is set to 1 iff we created the socket (fallback bind) so the
 * caller knows to unlink; 0 for systemd-owned.
 */
static int bind_fallback_socket(char *sock_path_out, size_t out_sz,
                                int *owned_out) {
  const char *xrd = xdg_runtime_dir();
  if (!xrd) {
    fprintf(stderr,
            "nostr-session-relayd: XDG_RUNTIME_DIR unset; cannot fallback-"
            "bind (run under a session or use socket activation)\n");
    return -1;
  }
  char dir[256];
  int n = snprintf(dir, sizeof dir, "%s/nostr", xrd);
  if (n < 0 || (size_t)n >= sizeof dir) return -1;
  if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
    fprintf(stderr, "nostr-session-relayd: mkdir(%s): %s\n", dir,
            strerror(errno));
    return -1;
  }

  char path[256];
  n = snprintf(path, sizeof path, "%s/relay.sock", dir);
  if (n < 0 || (size_t)n >= sizeof path) return -1;

  /* Stale-socket recovery, per §3.4: probe for a live listener; if the
   * probe succeeds, another instance already owns it and we abort. If the
   * probe fails (ECONNREFUSED or ENOENT), unlink and retry. */
  int probe = unix_stream_socket(0);
  if (probe >= 0) {
    struct sockaddr_un pa;
    memset(&pa, 0, sizeof pa);
    pa.sun_family = AF_UNIX;
    strncpy(pa.sun_path, path, sizeof(pa.sun_path) - 1);
    if (connect(probe, (struct sockaddr *)&pa, sizeof pa) == 0) {
      close(probe);
      fprintf(stderr,
              "nostr-session-relayd: %s already has a live listener; "
              "refusing to steal\n",
              path);
      return -1;
    }
    close(probe);
    /* connect() failed → either no server or connection refused; unlink
     * defensively (ignoring ENOENT). */
    if (unlink(path) != 0 && errno != ENOENT) {
      fprintf(stderr, "nostr-session-relayd: unlink(%s): %s\n", path,
              strerror(errno));
      return -1;
    }
  }

  int s = unix_stream_socket(1);
  if (s < 0) {
    fprintf(stderr, "nostr-session-relayd: socket(): %s\n", strerror(errno));
    return -1;
  }
  struct sockaddr_un a;
  memset(&a, 0, sizeof a);
  a.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(a.sun_path)) {
    fprintf(stderr, "nostr-session-relayd: socket path too long: %s\n", path);
    close(s);
    return -1;
  }
  strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);

  /* Bind with a defensive umask so no race between bind() and chmod() can
   * expose the socket at wider-than-0600 permissions. `fchmod()` on a
   * Unix-domain socket is silently a no-op on Linux (the kernel does not
   * expose the socket's inode mode to fchmod), so we ALWAYS follow up
   * with a path-based chmod which does update the inode. */
  mode_t saved_umask = umask(0177);
  int bind_rc = bind(s, (struct sockaddr *)&a, sizeof a);
  umask(saved_umask);
  if (bind_rc != 0) {
    fprintf(stderr, "nostr-session-relayd: bind(%s): %s\n", path,
            strerror(errno));
    close(s);
    return -1;
  }
  /* Belt-and-braces: fchmod() first (mostly a no-op on Linux but honored on
   * some BSDs), then chmod() on the path so the inode is definitively 0600
   * regardless of the umask that was in effect during bind(). */
  (void)fchmod(s, 0600);
  if (chmod(path, 0600) != 0) {
    fprintf(stderr,
            "nostr-session-relayd: warning: could not set 0600 on %s: %s\n",
            path, strerror(errno));
  }
  if (listen(s, 16) != 0) {
    fprintf(stderr, "nostr-session-relayd: listen(): %s\n", strerror(errno));
    close(s);
    unlink(path);
    return -1;
  }

  if (sock_path_out && out_sz > 0) {
    snprintf(sock_path_out, out_sz, "%s", path);
  }
  if (owned_out) *owned_out = 1;
  return s;
}

/*
 * Acquire the listening socket: either from systemd socket activation (sd_
 * listen_fds returning 1) or by fallback-binding ourselves.
 *
 * `owned_out` reflects whether we own the file (must unlink) or not.
 */
static int acquire_listen_fd(char *sock_path_out, size_t out_sz,
                             int *owned_out) {
  int n = sd_listen_fds(1); /* unset_environment=1: only read on first call */
  if (n >= 1) {
    int fd = SD_LISTEN_FDS_START;
    if (n > 1) {
      fprintf(stderr,
              "nostr-session-relayd: warning: %d fds passed, using fd %d\n",
              n, fd);
    }
    if (!fd_is_systemd_owned(fd)) {
      fprintf(stderr,
              "nostr-session-relayd: LISTEN_FDS supplied fd %d but it is not "
              "a listening Unix socket; ignoring\n",
              fd);
    } else {
      fprintf(stderr,
              "nostr-session-relayd: adopted systemd-activated listen fd %d\n",
              fd);
      if (owned_out) *owned_out = 0; /* systemd unlinks */
      if (sock_path_out && out_sz > 0) sock_path_out[0] = '\0';
      return fd;
    }
  }
  return bind_fallback_socket(sock_path_out, out_sz, owned_out);
}

/*
 * Load the session config. Uses the same limit/policy loader as the system
 * relayd for the sake of one code path. `cfg.listen` is left at its default
 * ("127.0.0.1:4848") and ignored by the Unix-fd listener path — the
 * validator requires *some* host:port there.
 */
static void session_config_path(char *path, size_t n) {
  const char *xcfg = getenv("XDG_CONFIG_HOME");
  const char *home = getenv("HOME");
  path[0] = '\0';
  if (xcfg && xcfg[0] == '/')
    snprintf(path, n, "%s/nostr/session-relay.conf", xcfg);
  else if (home && home[0] == '/')
    snprintf(path, n, "%s/.config/nostr/session-relay.conf", home);
}

static int load_session_config(RelaydConfig *out) {
  char path[512];
  session_config_path(path, sizeof path);
  /* relayd_config_load() apply_defaults()-first, then reads the file if it
   * exists. Missing file is not an error — we run entirely on defaults. */
  return relayd_config_load(path[0] ? path : NULL, out);
}

#ifdef NSR_HAVE_FEDERATION
static int fed_offer(void *ud, NostrEvent *ev) { return nsr_federation_offer(ud, ev); }
#endif

static void usage(FILE *out) {
  fprintf(out,
          "usage: nostr-session-relayd [--stats | --upstream [EVENT_ID]]\n"
          "\n"
          "Per-user session relay on $XDG_RUNTIME_DIR/nostr/relay.sock (normally\n"
          "socket-activated by nostr-session-relay.socket). Events local apps\n"
          "write are forwarded to the user's own upstream relays (see the\n"
          "federation_* keys of session-relay.conf).\n"
          "\n"
          "  --stats              print the running daemon's org.nostr.SessionRelay1 stats\n"
          "  --upstream           print per-relay upstream delivery status\n"
          "  --upstream EVENT_ID  print one event's upstream delivery status\n"
          "  --help               show this help\n");
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--stats") == 0) return nsr_dbus_print_stats();
    if (strcmp(argv[i], "--upstream") == 0)
      return nsr_dbus_print_upstream(i + 1 < argc ? argv[i + 1] : NULL);
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(stdout);
      return 0;
    }
    fprintf(stderr, "nostr-session-relayd: unknown argument '%s'\n", argv[i]);
    usage(stderr);
    return 2;
  }

  /* Own SIGINT/SIGTERM until the library takes over its own handlers in
   * `nostr_relay_server_run()`. We keep the flag so the library can also
   * observe it via stop_flag. */
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_stop;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  /* SIGPIPE would kill us on a peer disconnect during write. */
  signal(SIGPIPE, SIG_IGN);

  nostr_json_init();

  RelaydConfig cfg;
  if (load_session_config(&cfg) != 0) {
    fprintf(stderr, "nostr-session-relayd: invalid session-relay.conf\n");
    return 1;
  }

  /* Session storage under ~/.local/share/nostr/session-relay/. */
  char storage_dir[512];
  if (resolve_storage_dir(storage_dir, sizeof storage_dir) != 0) return 1;

#ifdef NOSTRC_HAVE_NOSTRDB_STORAGE
  /* Explicit, so the linker keeps the driver (see apps/relayd/CMakeLists). */
  nostr_storage_register("nostrdb", nostrdb_storage_new);
#endif
  const char *driver =
      cfg.storage_driver[0] ? cfg.storage_driver : "nostrdb";
  NostrStorage *st = NULL;
  if (strcmp(driver, "none") == 0) {
    fprintf(stderr,
            "nostr-session-relayd: storage_driver = \"none\": running "
            "cache-less by configuration\n");
  } else if (!(st = nostr_storage_create(driver))) {
    fprintf(stderr,
            "nostr-session-relayd: storage driver '%s' unavailable; the "
            "session relay will run cache-less (REQ answers EOSE at once, "
            "EVENT is refused).\n",
            driver);
  } else if (st->vt && st->vt->open) {
    int rc_open = st->vt->open(st, storage_dir, NULL);
    if (rc_open != 0) {
      fprintf(stderr,
              "nostr-session-relayd: storage open('%s') failed rc=%d; "
              "running cache-less\n",
              storage_dir, rc_open);
      if (st->vt->close) st->vt->close(st);
      free(st);
      st = NULL;
    }
  }

  /* Upstream federation (bead nostrc-7d96): an outbox next to the store
   * and a storage tee that queues what local apps write once nostrdb has
   * stored it. Needs a store: a cache-less relay refuses EVENT anyway. */
  NostrStorage *served = st; /* what the relay core sees */
  void *fed = NULL;
  const char *fed_state = "unavailable";
  char fed_detail[640];
  snprintf(fed_detail, sizeof fed_detail,
           "built without libnostr-publish: no upstream federation");
#ifdef NSR_HAVE_FEDERATION
  NsrOutbox *outbox = NULL;
  NostrStorage *tee = NULL;
  {
    char conf[512];
    session_config_path(conf, sizeof conf);
    NsrFedConfig fcfg;
    char ferr[640];
    if (nsr_fed_config_load(conf[0] ? conf : NULL, &fcfg, ferr, sizeof ferr) != 0) {
      fprintf(stderr, "nostr-session-relayd: invalid session-relay.conf: %s\n", ferr);
      if (st) {
        if (st->vt && st->vt->close) st->vt->close(st);
        free(st);
      }
      return 1;
    }
    if (!fcfg.enabled) {
      fed_state = "disabled";
      snprintf(fed_detail, sizeof fed_detail, "federation = 0 in session-relay.conf");
    } else if (!st) {
      snprintf(fed_detail, sizeof fed_detail,
               "the relay is cache-less: nothing is stored, so nothing is forwarded");
    } else {
      char path[600];
      snprintf(path, sizeof path, "%s/outbox.sqlite3", storage_dir);
      GError *err = NULL;
      outbox = nsr_outbox_open(path, &err);
      if (!outbox) {
        snprintf(fed_detail, sizeof fed_detail, "outbox: %s", err ? err->message : "?");
        g_clear_error(&err);
      } else {
        NsrFederationInit fi = {
            .cfg = &fcfg, .outbox = outbox, .storage = st, .dbus_signer = TRUE,
        };
        NsrFederation *f = nsr_federation_new(&fi);
        tee = f ? nsr_tee_storage_new(st, fed_offer, f) : NULL;
        if (!tee) {
          nsr_federation_free(f);
          nsr_outbox_close(outbox);
          outbox = NULL;
          snprintf(fed_detail, sizeof fed_detail, "out of memory");
        } else {
          nsr_federation_set_observer(f, nsr_dbus_emit_upstream, NULL);
          fed = f;
          served = tee;
          fed_state = NULL; /* reported live by the engine */
          fprintf(stderr, "nostr-session-relayd: upstream federation on (outbox %s)\n", path);
        }
      }
    }
    if (fed_state)
      fprintf(stderr, "nostr-session-relayd: upstream federation %s: %s\n", fed_state,
              fed_detail);
  }
#endif

  /* Acquire the listen fd. Prefer sd_listen_fds; fall back to a manual
   * bind under XDG_RUNTIME_DIR. `sock_path` and `owned_socket` inform the
   * shutdown unlink decision. */
  char sock_path[256]; sock_path[0] = '\0';
  int owned_socket = 0;
  int listen_fd = acquire_listen_fd(sock_path, sizeof sock_path, &owned_socket);
  if (listen_fd < 0) {
    fprintf(stderr, "nostr-session-relayd: could not obtain a listen fd\n");
#ifdef NSR_HAVE_FEDERATION
    nsr_federation_free(fed);
    nsr_tee_storage_free(tee);
    nsr_outbox_close(outbox);
#endif
    if (st) {
      if (st->vt && st->vt->close) st->vt->close(st);
      free(st);
    }
    return 1;
  }

  /* Read-only stats on the session bus (org.nostr.SessionRelay1). Never
   * fatal: without a session bus the relay still serves relay.sock. */
  NsrDbusInfo dbus_info = {
      .cfg = &cfg,
      .storage = st,
      .storage_backend = st ? driver : "none",
      .requested_backend = driver,
      .storage_dir = storage_dir,
      .version = NSR_VERSION,
      .federation = fed,
      .federation_state = fed_state,
      .federation_detail = fed_detail,
  };
  nsr_dbus_start(&dbus_info);
#ifdef NSR_HAVE_FEDERATION
  if (fed) {
    GError *err = NULL;
    if (!nsr_federation_start(fed, &err)) {
      fprintf(stderr, "nostr-session-relayd: federation thread: %s (events stay queued)\n",
              err ? err->message : "?");
      g_clear_error(&err);
    }
  }
#endif

  /* Notify systemd we're up. Harmless no-op when not under `Type=notify`. */
  (void)sd_notify(0, "READY=1\nSTATUS=session relay accepting connections");

  NostrRelayServerConfig server_cfg;
  memset(&server_cfg, 0, sizeof server_cfg);
  server_cfg.cfg = &cfg;
  server_cfg.storage = served;
  server_cfg.stop_flag = &s_stop_flag;
  server_cfg.listener.kind = NOSTR_RELAY_LISTENER_UNIX_FD;
  server_cfg.listener.u.unix_fd.fd = listen_fd;

  int rc = nostr_relay_server_run(&server_cfg);

  (void)sd_notify(0, "STOPPING=1");
#ifdef NSR_HAVE_FEDERATION
  /* Engine first (it emits D-Bus signals), then D-Bus (it reads engine
   * status), then free; storage last (both may read through it). */
  nsr_federation_stop(fed);
#endif
  /* Before closing storage: the D-Bus thread may be counting through it. */
  nsr_dbus_stop();
#ifdef NSR_HAVE_FEDERATION
  nsr_federation_free(fed);
  nsr_tee_storage_free(tee);
  nsr_outbox_close(outbox);
#endif

  /* Only unlink when we own the socket (fallback bind). Never unlink a
   * systemd-owned socket — that fights the .socket unit's contract. */
  if (owned_socket && sock_path[0]) {
    if (unlink(sock_path) != 0 && errno != ENOENT) {
      fprintf(stderr, "nostr-session-relayd: unlink(%s): %s\n", sock_path,
              strerror(errno));
    }
    /* Only close the fd we own; systemd-supplied fds are left open so
     * they can be re-adopted on the next activation. */
    close(listen_fd);
  }

  if (st) {
    if (st->vt && st->vt->close) st->vt->close(st);
    free(st);
  }

  return rc != 0 ? 1 : 0;
}
