/*
 * nostrc-relayd — the system-daemon front for libnostr-relay-server.
 *
 * The reusable server core lives in `apps/relayd/src/relay_server.c`
 * (public entry point in `include/nostr-relay-server.h`). This file is
 * intentionally thin: read `relay.toml` from the cwd, open the storage
 * backend, hand a TCP listener to the library, then tear the state back
 * down when the loop returns. The future per-user session relay (#13) is
 * the second consumer of the library; it will replace this file with a
 * daemon that receives a pre-bound Unix-domain socket via sd_listen_fds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr-json.h"
#include "nostr-relay-server.h"
#include "nostr-storage.h"
#include "relayd_config.h"
#ifdef NOSTRC_HAVE_NOSTRDB_STORAGE
#include "nostrdb_storage.h"
#endif

/* Where the store lives: systemd's StateDirectory= (the unit sets
 * StateDirectory=nostr-relayd, i.e. /var/lib/nostr-relayd), else a
 * directory under the cwd for hand-run instances. */
static void resolve_storage_dir(char *out, size_t out_sz) {
  const char *state = getenv("STATE_DIRECTORY");
  if (state && state[0] == '/') {
    /* Several StateDirectory= entries arrive colon-separated; use the first. */
    size_t n = strcspn(state, ":");
    snprintf(out, out_sz, "%.*s", (int)n, state);
  } else {
    snprintf(out, out_sz, "%s", "nostrc-relayd-data");
  }
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;

  nostr_json_init();

  RelaydConfig cfg;
  if (relayd_config_load("relay.toml", &cfg) != 0) {
    fprintf(stderr, "nostrc-relayd: invalid relay.toml security limits\n");
    return 1;
  }

  /* Storage lifecycle stays in the daemon: the session relay in #13 will
   * open its own per-user path via nostr_storage_create_at_path(). Keeping
   * this here means libnostr-relay-server has no opinion on where the store
   * lives. */
#ifdef NOSTRC_HAVE_NOSTRDB_STORAGE
  nostr_storage_register("nostrdb", nostrdb_storage_new);
#endif
  const char *driver = cfg.storage_driver[0] ? cfg.storage_driver : "nostrdb";
  NostrStorage *st = NULL;
  if (strcmp(driver, "none") != 0) {
    st = nostr_storage_create(driver);
    if (!st) {
      fprintf(stderr,
              "nostrc-relayd: storage '%s' not available; running without "
              "storage (REQ answers EOSE, EVENT is refused)\n",
              driver);
    }
  }
  if (st && st->vt && st->vt->open) {
    /* An unopened driver would fail every query and write; run without
     * storage instead. */
    char storage_dir[512];
    resolve_storage_dir(storage_dir, sizeof storage_dir);
    int rc_open = st->vt->open(st, storage_dir, NULL);
    if (rc_open != 0) {
      fprintf(stderr,
              "nostrc-relayd: storage open('%s') failed rc=%d; running "
              "without storage\n",
              storage_dir, rc_open);
      if (st->vt->close) st->vt->close(st);
      free(st);
      st = NULL;
    } else {
      fprintf(stderr, "nostrc-relayd: storage '%s' at %s\n", driver,
              storage_dir);
    }
  }

  /* Parse the TCP listener out of cfg.listen. relayd_config_load()
   * already validated the format; treat a failure here as an assertion. */
  NostrRelayServerConfig server_cfg;
  server_cfg.cfg = &cfg;
  server_cfg.storage = st;
  server_cfg.stop_flag = NULL;
  server_cfg.listener.kind = NOSTR_RELAY_LISTENER_TCP;
  server_cfg.listener.u.tcp.port = 0;
  if (relayd_config_parse_listen(cfg.listen,
                                 server_cfg.listener.u.tcp.host,
                                 sizeof(server_cfg.listener.u.tcp.host),
                                 &server_cfg.listener.u.tcp.port) != 0) {
    fprintf(stderr, "nostrc-relayd: invalid listen '%s'\n", cfg.listen);
    if (st && st->vt && st->vt->close) st->vt->close(st);
    free(st);
    return 1;
  }

  int rc = nostr_relay_server_run(&server_cfg);

  if (st && st->vt && st->vt->close) st->vt->close(st);
  free(st);
  return rc != 0 ? 1 : 0;
}
