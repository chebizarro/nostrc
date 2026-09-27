/*
 * libnostr-relay-server — the reusable relay-server core factored out of
 * nostrc-relayd. Both the system daemon (`nostrc-relayd`, which binds a TCP
 * host:port from relay.toml) and the future per-user session relay (which
 * receives a pre-bound Unix-domain socket via sd_listen_fds) link against
 * this library and differ only in:
 *
 *   (a) how the listening socket is created (see NostrRelayListener); and
 *   (b) which RelaydConfig / NostrStorage they hand in.
 *
 * Everything else — the protocol handlers (NIP-01/11/42/45/50/77), the
 * verification budget, replay policy, rate limits, retention loop, and the
 * libwebsockets event loop — lives inside `nostr_relay_server_run()`.
 */
#ifndef NOSTR_RELAY_SERVER_H
#define NOSTR_RELAY_SERVER_H

#include <stddef.h>

#include "nostr-storage.h"
#include "relayd_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Interface strings passed to libwebsockets are bounded so we can hold the
 * host inside the listener struct without heap allocation. Matches the size
 * of `RelaydConfig::listen`, which is the widest input we accept. */
#define NOSTR_RELAY_MAX_LISTEN_HOST RELAYD_MAX_LISTEN_LEN

typedef enum {
  /*
   * Bind a fresh TCP socket at host:port. This is what `nostrc-relayd` uses
   * today, driven by the `listen = "host:port"` line in relay.toml.
   * `host` may be any string libwebsockets accepts as `info.iface`, most
   * commonly `127.0.0.1`, `0.0.0.0`, `::1`, `::`, or a hostname.
   */
  NOSTR_RELAY_LISTENER_TCP = 0,

  /*
   * Adopt a pre-bound *listening* Unix-domain socket. This is what the
   * per-user session relay (nostr-session-relayd) uses: systemd hands the
   * daemon a socket via sd_listen_fds, or the daemon binds its own under
   * $XDG_RUNTIME_DIR, and passes that fd here. Peers whose UID differs from
   * the server's effective UID are rejected at accept.
   */
  NOSTR_RELAY_LISTENER_UNIX_FD = 1,
} NostrRelayListenerKind;

typedef struct {
  NostrRelayListenerKind kind;
  union {
    struct {
      /* Interface hint (host string). Must be non-empty; validated by
       * relayd_config_parse_listen() before it lands here. */
      char host[NOSTR_RELAY_MAX_LISTEN_HOST];
      int port; /* 1..65535 */
    } tcp;
    struct {
      /* Already-bound listening socket. The server services a dup of it
       * for the duration of run(); the caller's fd is never closed, so
       * systemd can hand the same fd back on the next activation. */
      int fd;
    } unix_fd;
  } u;
} NostrRelayListener;

typedef struct {
  /* Borrowed. NIP-11 identity, replay window, rate limits, verification
   * budgets — the full policy surface. Must outlive the run() call. */
  const RelaydConfig *cfg;

  /* Borrowed. Backend chosen by the caller: relayd today picks nostrdb via
   * `nostr_storage_create(cfg->storage_driver)`; the session daemon will
   * open its own per-user path. May be NULL, in which case the server logs
   * a warning and continues (queries return empty; useful for smoke tests). */
  NostrStorage *storage;

  /* How to obtain the listening socket. */
  NostrRelayListener listener;

  /* Optional caller-owned stop signal. If non-NULL, `*stop_flag != 0`
   * causes the event loop to exit alongside SIGINT/SIGTERM. */
  volatile int *stop_flag;
} NostrRelayServerConfig;

/*
 * Run the relay event loop. Blocks until SIGINT/SIGTERM, until `*stop_flag`
 * (if provided) becomes non-zero, or until a fatal startup error.
 *
 * Returns 0 on graceful shutdown, or a positive value on fatal startup
 * failure (see fprintf(stderr) for the human-readable cause).
 *
 * The caller retains ownership of `cfg` and `storage` and is responsible for
 * destroying them after this function returns.
 */
int nostr_relay_server_run(const NostrRelayServerConfig *server_cfg);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_RELAY_SERVER_H */
