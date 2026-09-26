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

#include "nostr-json.h"
#include "nostr-relay-server.h"
#include "nostr-storage.h"
#include "relayd_config.h"

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
  const char *driver = cfg.storage_driver[0] ? cfg.storage_driver : "nostrdb";
  NostrStorage *st = nostr_storage_create(driver);
  if (!st) {
    fprintf(stderr,
            "nostrc-relayd: storage '%s' not available; please enable "
            "components/nostrdb or choose another driver.\n",
            driver);
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
