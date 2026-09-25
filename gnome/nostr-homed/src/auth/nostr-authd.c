/* nostr-authd: broker daemon.
 *
 * Listens on auth.sock (SO_PEERCRED-privileged, uid 0 only, for login) and,
 * when configured with an SMB journal path, ALSO listens on user.sock
 * (SO_PEERCRED-tagged, any uid, own-account SMB proof). Each connection is
 * driven serially by the broker.
 *
 * Isolation choice (bucket B0 / D6 nostrc-rb0e.7): the SMB authority is
 * still isolated at the type/data-flow level — a distinct nh_smb_authority
 * with its own SQLite journal and passdb adapter — but it is hosted inside
 * the same nostr-authd process on a separate SEQPACKET socket for this
 * increment. A dedicated nostr-smbd binary can later split it out without
 * changing the broker or the wire protocol; the choice is deliberately
 * reversible because user.sock is a distinct filesystem endpoint bound to
 * a distinct authority object.
 *
 * Usage:
 *   nostr-authd <auth-socket-path> <authority-dir>
 *   nostr-authd <auth-socket-path> <authority-dir> <user-socket-path> <smb-journal-path>
 */
#define _GNU_SOURCE
#include "auth_broker.h"
#include "nostr_identity.h"

#ifdef NH_AUTH_BROKER_ENABLE_SMB
#  include "passdb_tdbsam.h"
#  include "smb_credential.h"
#endif

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static nh_identity_ownership_result probe(void *c, const char *n, uint32_t u,
                                          uint32_t g) {
  (void)c; (void)n; (void)u; (void)g;
  return NH_IDENTITY_OWNERSHIP_FREE;
}

/* Bind a SEQPACKET listener at `path` with the given socket mode.  Any
 * pre-existing entry is unlinked first: nostr-authd is the sole owner of its
 * sockets. Returns the listening fd or -1 on failure. */
static int bind_listener(const char *path, mode_t mode) {
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof addr.sun_path) {
    fprintf(stderr, "nostr-authd: socket path too long: %s\n", path);
    return -1;
  }
  strcpy(addr.sun_path, path);
  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) { perror("socket"); return -1; }
  unlink(path);
  mode_t old = umask(0777 & ~mode);
  int bad = bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0;
  umask(old);
  if (bad) { perror("bind"); close(fd); return -1; }
  /* umask alone is a race with newer glibc/kernel; belt-and-braces chmod. */
  (void)chmod(path, mode);
  if (listen(fd, 16) != 0) { perror("listen"); close(fd); unlink(path); return -1; }
  return fd;
}

int main(int argc, char **argv) {
  if (argc != 3 && argc != 5) {
    fprintf(stderr,
            "usage: %s <auth-socket-path> <authority-dir>\n"
            "       %s <auth-socket-path> <authority-dir> <user-socket-path> <smb-journal-path>\n",
            argv[0], argv[0]);
    return 2;
  }
  const char *auth_socket_path = argv[1];
  const char *dir = argv[2];
  const char *user_socket_path = (argc == 5) ? argv[3] : NULL;
  const char *smb_journal_path = (argc == 5) ? argv[4] : NULL;

  nh_identity_config config;
  nh_identity_config_defaults(&config);
  snprintf(config.authority_path, sizeof config.authority_path, "%s/authority.db", dir);
  snprintf(config.projection_path, sizeof config.projection_path, "%s/nss.db", dir);
  snprintf(config.home_root, sizeof config.home_root, "%s/home", dir);
  nh_identity_store_options options = {0};
  options.config = &config;
  options.ownership_probe = probe;
  options.flags = 0;
  nh_identity_store *store = NULL;
  if (nh_identity_store_open(&options, &store) != NH_IDENTITY_OK) {
    fprintf(stderr, "nostr-authd: cannot open authority at %s\n", config.authority_path);
    return 1;
  }
  nh_auth_broker *broker = nh_auth_broker_new(store);
  if (!broker) { nh_identity_store_close(store); return 1; }

  /* Load /etc/nostr-auth/auth.conf (or NH_AUTH_CONF override) so QR-login
   * defaults can be pinned per site. Non-fatal — the file may not exist yet
   * and every key has a compiled-in default (design D12). */
  static nh_auth_conf conf; /* module-scope: pointers stay valid for broker */
  {
    const char *conf_path = getenv("NH_AUTH_CONF");
    if (!conf_path || !*conf_path) conf_path = "/etc/nostr-auth/auth.conf";
    (void)nh_auth_conf_load(conf_path, &conf);
    if (conf.nip46_qr_relays_count > 0) {
      static const char *relay_ptrs[NH_AUTH_CONF_RELAYS_MAX];
      for (size_t i = 0; i < conf.nip46_qr_relays_count; i++)
        relay_ptrs[i] = conf.nip46_qr_relays[i];
      nh_auth_broker_set_qr_default_relays(relay_ptrs,
                                           conf.nip46_qr_relays_count);
    }
    /* NIP-05 identifier resolution (B5-NIP-05, nostrc-bit0). Enabled
     * by default so a fresh install lets users type their NIP-05 at
     * the GDM greeter's "Not listed?" prompt out of the box; a site
     * admin can disable it via `nip05_resolve = off` in auth.conf.
     * Drop-user defaults to nip05_image_user, then profile_image_user,
     * then "nobody" — same helper is fine to reuse.
     * Helper path override lives in NH_NIP05_HELPER for headless
     * integration tests. */
    {
      int enabled = (conf.nip05_resolve != 2); /* 0 unset => on, 1 on, 2 off */
      const char *helper = getenv("NH_NIP05_HELPER");
      const char *duser = conf.nip05_image_user[0]
                             ? conf.nip05_image_user
                             : (conf.profile_image_user[0]
                                    ? conf.profile_image_user
                                    : NULL);
      nh_auth_broker_set_nip05(broker, enabled, helper, duser,
                               (int)conf.nip05_cache_ttl_seconds);
    }

    /* Portable-home (bead nostrc-pvha): install the identity store
     * handle so provider_nip46's post-verify hook can read/write
     * providers.wrapped_home_key. The enroll_wrap_key flag (auth.conf
     * `porthome_enroll_wrap_key = on|off`, 0=unset -> off) gates
     * first-login enrollment: without it the hook only reads existing
     * ciphertext and never mints or persists a new seed. Weak-linked
     * so a build without NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL
     * still links (the stub is a no-op). */
    extern void nh_auth_broker_porthome_install(
        nh_identity_store *store, int enroll_wrap_key,
        uint32_t nip46_decrypt_timeout_sec)
        __attribute__((weak));
    if (nh_auth_broker_porthome_install) {
      int enroll = (conf.porthome_enroll_wrap_key == 1) ? 1 : 0;
      /* nostrc-ck6i: 0 => broker uses its 30 s default; >120 s
       * is clamped inside the installer. */
      nh_auth_broker_porthome_install(
          store, enroll, conf.porthome_nip46_decrypt_timeout_sec);
    }
  }

  /* Optional persistent rate-limit backing. If NH_AUTH_RATELIMIT_PATH is set
   * (typically /var/lib/nostr-auth/ratelimit.db) the broker's per-account
   * failed-proof throttle is loaded from and persisted to that SQLite file so
   * an active cooldown survives a restart of nostr-authd. Unset => in-memory,
   * matching the pre-B1p behaviour. */
  {
    const char *rl_path = getenv("NH_AUTH_RATELIMIT_PATH");
    if (rl_path && *rl_path) {
      if (nh_auth_broker_set_ratelimit_path(broker, rl_path) != 0) {
        fprintf(stderr,
                "nostr-authd: cannot open rate-limit store %s; continuing in-memory\n",
                rl_path);
      }
    }
  }

#ifdef NH_AUTH_BROKER_ENABLE_SMB
  nh_smb_authority *smb = NULL;
  nh_smb_passdb_tdbsam *tdbsam = NULL;
  if (user_socket_path) {
    tdbsam = nh_smb_passdb_tdbsam_new(NULL, NULL);
    if (!tdbsam) {
      fprintf(stderr, "nostr-authd: cannot create tdbsam adapter\n");
      nh_auth_broker_free(broker); nh_identity_store_close(store); return 1;
    }
    nh_smb_rc srr = nh_smb_authority_open(smb_journal_path,
                                          &nh_smb_passdb_tdbsam_ops,
                                          tdbsam, &smb);
    if (srr != NH_SMB_OK) {
      fprintf(stderr, "nostr-authd: cannot open smb journal %s: %s\n",
              smb_journal_path, nh_smb_rc_name(srr));
      nh_smb_passdb_tdbsam_free(tdbsam);
      nh_auth_broker_free(broker); nh_identity_store_close(store); return 1;
    }
    nh_auth_broker_set_smb_authority(broker, smb);
  }
#else
  /* SMB compiled out: the user socket + journal are inert. Consume
   * smb_journal_path so -Werror=unused-but-set-variable does not fire on a
   * SMB-disabled build (the journal path is only read inside the SMB block). */
  (void)smb_journal_path;
  if (user_socket_path) {
    fprintf(stderr,
            "nostr-authd: this build has SMB disabled; user socket ignored\n");
    user_socket_path = NULL;
  }
#endif

  int listener_auth = bind_listener(auth_socket_path, 0600); /* privileged */
  if (listener_auth < 0) {
#ifdef NH_AUTH_BROKER_ENABLE_SMB
    if (smb) nh_smb_authority_close(smb);
    if (tdbsam) nh_smb_passdb_tdbsam_free(tdbsam);
#endif
    nh_auth_broker_free(broker); nh_identity_store_close(store); return 1;
  }
  int listener_user = -1;
  if (user_socket_path) {
    listener_user = bind_listener(user_socket_path, 0666); /* world-connectable */
    if (listener_user < 0) {
      close(listener_auth); unlink(auth_socket_path);
#ifdef NH_AUTH_BROKER_ENABLE_SMB
      if (smb) nh_smb_authority_close(smb);
      if (tdbsam) nh_smb_passdb_tdbsam_free(tdbsam);
#endif
      nh_auth_broker_free(broker); nh_identity_store_close(store); return 1;
    }
  }

  struct sigaction sa = {0};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);

  fprintf(stderr, "nostr-authd: auth listening on %s%s%s\n",
          auth_socket_path,
          user_socket_path ? ", user on " : "",
          user_socket_path ? user_socket_path : "");
  while (!g_stop) {
    struct pollfd pfd[2];
    nfds_t nfd = 0;
    pfd[nfd].fd = listener_auth; pfd[nfd].events = POLLIN; pfd[nfd].revents = 0; nfd++;
    if (listener_user >= 0) {
      pfd[nfd].fd = listener_user; pfd[nfd].events = POLLIN; pfd[nfd].revents = 0; nfd++;
    }
    int pr = poll(pfd, nfd, -1);
    if (pr < 0) {
      if (errno == EINTR) continue;
      perror("poll"); break;
    }
    for (nfds_t i = 0; i < nfd; i++) {
      if (!(pfd[i].revents & POLLIN)) continue;
      int fd = accept4(pfd[i].fd, NULL, NULL, SOCK_CLOEXEC);
      if (fd < 0) continue;
      if (pfd[i].fd == listener_auth) {
        (void)nh_auth_broker_handle_connection(broker, fd);
      } else {
        (void)nh_auth_broker_handle_user_connection(broker, fd);
      }
      close(fd);
    }
  }

  close(listener_auth);
  unlink(auth_socket_path);
  if (listener_user >= 0) {
    close(listener_user);
    if (user_socket_path) unlink(user_socket_path);
  }
#ifdef NH_AUTH_BROKER_ENABLE_SMB
  if (smb) { nh_auth_broker_set_smb_authority(broker, NULL); nh_smb_authority_close(smb); }
  if (tdbsam) nh_smb_passdb_tdbsam_free(tdbsam);
#endif
  /* Detach the porthome hook's borrowed store handle BEFORE we close
   * the store; a late-arriving login would otherwise deref a freed
   * sqlite handle. Weak-linked (see install site above). */
  extern void nh_auth_broker_porthome_install(
      nh_identity_store *store, int enroll_wrap_key,
      uint32_t nip46_decrypt_timeout_sec)
      __attribute__((weak));
  if (nh_auth_broker_porthome_install)
    nh_auth_broker_porthome_install(NULL, 0, 0);
  nh_auth_broker_free(broker);
  nh_identity_store_close(store);
  fprintf(stderr, "nostr-authd: stopped\n");
  return 0;
}
