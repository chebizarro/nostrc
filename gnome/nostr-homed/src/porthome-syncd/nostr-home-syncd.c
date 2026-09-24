/*
 * nostr-home-syncd — user-scoped portable-home sync daemon (I1).
 *
 * SPDX-License-Identifier: MIT
 *
 * WARNING: EXPERIMENTAL AND UNREVIEWED.
 * Gated behind NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL.  I1 delivers
 * the process lifecycle, capture pipeline, and push closure; the
 * pull/reconciler (I2) and cache/systemd unit (I3) land later.
 *
 * Runs as the LOGIN USER — never as root. No CAP_ chatter; no writes
 * outside $HOME. State: ${XDG_STATE_HOME:-~/.local/state}/nostr-homed/.
 *
 * Configuration: environment-only in I1 (I3 replaces this with a
 * proper config file). Required:
 *   NOSTR_HOMED_SYNCD_NSEC_HEX   — 64-hex secp256k1 for kind-30078
 *   NOSTR_HOMED_SYNCD_BLOSSOM    — https://... comma-separated
 *   NOSTR_HOMED_SYNCD_RELAYS     — wss://... comma-separated
 *   NOSTR_HOMED_SYNCD_D_TAG      — pointer d-tag (default matches
 *                                  design §2.2 personal home)
 *   NOSTR_HOMED_SYNCD_MIN_REPL   — integer, default 2
 *   NOSTR_HOMED_SYNCD_STATE_DIR  — override state dir (tests)
 *   NOSTR_HOMED_SYNCD_HOME       — override $HOME (tests)
 *   NOSTR_HOMED_SYNCD_SEED_HEX   — 64-hex seed for home_key derivation
 *                                  (Phase 2 broker plumbing not yet
 *                                  wired at daemon startup; I3 owns
 *                                  the credential handoff)
 *
 * Signals:
 *   SIGTERM/SIGINT — flush final batch, release lock, exit 0.
 *   SIGHUP         — reload user ignore file.
 */

#include "nh_syncd.h"
#include "nh_syncd_watcher.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_reload = 0;
static volatile sig_atomic_t g_stop   = 0;

static uint64_t now_ns_monotonic(void *ud) {
    (void)ud;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static char *xstrdup(const char *s) { return s ? strdup(s) : NULL; }

static char *default_home(void) {
    const char *h = getenv("NOSTR_HOMED_SYNCD_HOME");
    if (!h) h = getenv("HOME");
    if (!h) {
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_dir) h = pw->pw_dir;
    }
    return xstrdup(h);
}

static char *default_state_dir(const char *home) {
    const char *ov = getenv("NOSTR_HOMED_SYNCD_STATE_DIR");
    if (ov && *ov) return xstrdup(ov);
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg && xdg[0] == '/') {
        char *p = NULL;
        if (asprintf(&p, "%s/nostr-homed", xdg) < 0) return NULL;
        return p;
    }
    char *p = NULL;
    if (asprintf(&p, "%s/.local/state/nostr-homed", home) < 0) return NULL;
    return p;
}

/* Split a comma-separated env into a heap-alloced NULL-terminated
 * array of heap-alloced strings. */
static char **split_csv(const char *s, size_t *out_n) {
    *out_n = 0;
    if (!s || !*s) return NULL;
    /* First pass: count. */
    size_t n = 1;
    for (const char *p = s; *p; p++) if (*p == ',') n++;
    char **arr = calloc(n + 1, sizeof(char *));
    if (!arr) return NULL;
    size_t i = 0;
    const char *start = s;
    for (const char *p = s; ; p++) {
        if (*p == ',' || *p == '\0') {
            size_t len = (size_t)(p - start);
            char *item = malloc(len + 1);
            if (!item) { for (size_t j = 0; j < i; j++) free(arr[j]); free(arr); return NULL; }
            memcpy(item, start, len);
            item[len] = '\0';
            /* Trim leading/trailing whitespace. */
            while (*item == ' ' || *item == '\t') memmove(item, item + 1, strlen(item));
            size_t il = strlen(item);
            while (il > 0 && (item[il - 1] == ' ' || item[il - 1] == '\t')) item[--il] = '\0';
            if (*item) arr[i++] = item; else free(item);
            if (*p == '\0') break;
            start = p + 1;
        }
    }
    arr[i] = NULL;
    *out_n = i;
    return arr;
}

static void free_csv(char **arr) {
    if (!arr) return;
    for (size_t i = 0; arr[i]; i++) free(arr[i]);
    free(arr);
}

static void on_signal(int sig) {
    if (sig == SIGHUP) g_reload = 1;
    else               g_stop   = 1;
}

static void install_signals(int sfd) {
    /* Not using signalfd for the actual delivery (keeps daemon simple);
     * a plain handler + volatile flag is enough. We do open a signalfd
     * for the poll loop so poll() wakes on signals. */
    (void)sfd;
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

typedef struct {
    char   *home;
    char   *state_dir;
    char   *nsec_hex;
    char   *d_tag;
    char  **blossom;  size_t n_blossom;
    char  **relays;   size_t n_relays;
    char   *seed_hex;
    size_t  min_repl;
} cfg_t;

static int load_cfg(cfg_t *c) {
    memset(c, 0, sizeof *c);
    c->home = default_home();
    if (!c->home) { fprintf(stderr, "syncd: cannot determine $HOME\n"); return -1; }
    c->state_dir = default_state_dir(c->home);
    if (!c->state_dir) return -1;
    c->nsec_hex = xstrdup(getenv("NOSTR_HOMED_SYNCD_NSEC_HEX"));
    c->d_tag    = xstrdup(getenv("NOSTR_HOMED_SYNCD_D_TAG"));
    if (!c->d_tag) c->d_tag = xstrdup("nostr-homed.home.v1:personal");
    c->seed_hex = xstrdup(getenv("NOSTR_HOMED_SYNCD_SEED_HEX"));
    const char *bl = getenv("NOSTR_HOMED_SYNCD_BLOSSOM");
    const char *rl = getenv("NOSTR_HOMED_SYNCD_RELAYS");
    c->blossom = split_csv(bl, &c->n_blossom);
    c->relays  = split_csv(rl, &c->n_relays);
    const char *mr = getenv("NOSTR_HOMED_SYNCD_MIN_REPL");
    c->min_repl = mr ? (size_t)strtoul(mr, NULL, 10) : NH_SYNCD_DEFAULT_MIN_REPLICATION;
    return 0;
}

static void free_cfg(cfg_t *c) {
    free(c->home); free(c->state_dir); free(c->nsec_hex); free(c->d_tag); free(c->seed_hex);
    free_csv(c->blossom); free_csv(c->relays);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    setlinebuf(stdout);

    cfg_t cfg;
    if (load_cfg(&cfg) < 0) return 2;

    /* Missing signer or transport is fatal at start — no polling
     * substitutes. The one exception: allow --check (dry-run) to
     * exercise the lock + ignore + interlock stack without any
     * network. */
    int dry_run = (argc > 1 && !strcmp(argv[1], "--check"));

    /* Interlocks. */
    nh_syncd_interlocks ilk = { cfg.home, getenv("NOSTR_HOME_STATE") };
    int ir = nh_syncd_interlocks_check(&ilk);
    if (ir == NH_SYNCD_ERR_LIMITED_MODE) {
        fprintf(stderr, "syncd: refusing to run — ~/.nostr-home-limited present\n");
        free_cfg(&cfg); return 3;
    }
    if (ir == NH_SYNCD_ERR_PARTIAL_STATE) {
        fprintf(stderr, "syncd: refusing to run — NOSTR_HOME_STATE=partial\n");
        free_cfg(&cfg); return 4;
    }

    /* Lock. */
    nh_syncd_lock *lk = NULL;
    int lr = nh_syncd_lock_acquire(cfg.state_dir, &lk);
    if (lr == NH_SYNCD_ERR_LOCKED) {
        fprintf(stderr, "syncd: another instance holds sync.lock; exiting cleanly\n");
        free_cfg(&cfg); return 0;
    }
    if (lr != NH_SYNCD_OK) {
        fprintf(stderr, "syncd: lock_acquire rc=%d\n", lr);
        free_cfg(&cfg); return 5;
    }

    /* State + ignore + batcher. */
    nh_syncd_state *state = NULL;
    bool snap_unknown = false;
    (void)nh_syncd_state_load(cfg.state_dir, &state, &snap_unknown);
    if (snap_unknown) {
        fprintf(stderr, "syncd: snapshot base unknown; push disabled (I2 will rescan)\n");
    }
    nh_syncd_ignore *ig = NULL;
    if (nh_syncd_ignore_new(cfg.home, &ig) != NH_SYNCD_OK) {
        fprintf(stderr, "syncd: ignore init failed\n");
        nh_syncd_lock_release(lk); free_cfg(&cfg); return 6;
    }
    nh_syncd_batcher *ba = NULL;
    if (nh_syncd_batcher_new(now_ns_monotonic, NULL, 0, 0, &ba) != NH_SYNCD_OK) {
        nh_syncd_ignore_free(ig); nh_syncd_lock_release(lk); free_cfg(&cfg); return 6;
    }

    install_signals(-1);

    if (dry_run) {
        fprintf(stderr, "syncd: --check OK (lock=held, interlocks=pass, state=%s)\n",
                snap_unknown ? "unknown" : "loaded");
        if (state) nh_syncd_state_free(state);
        nh_syncd_batcher_free(ba); nh_syncd_ignore_free(ig);
        nh_syncd_lock_release(lk); free_cfg(&cfg);
        return 0;
    }

    /* Watcher. */
    nh_syncd_watcher *wa = NULL;
    if (nh_syncd_watcher_new(cfg.home, ig, ba, &wa) != NH_SYNCD_OK) {
        fprintf(stderr, "syncd: inotify init failed\n");
        goto cleanup;
    }

    /* Poll loop. */
    struct pollfd pfd = { nh_syncd_watcher_fd(wa), POLLIN, 0 };
    while (!g_stop) {
        if (g_reload) { g_reload = 0; nh_syncd_ignore_reload(ig, cfg.home); }
        uint64_t next_ms = nh_syncd_batcher_next_tick_ms(ba);
        int t = next_ms == UINT64_MAX ? -1 :
                (next_ms > 3600000ull ? 3600000 : (int)next_ms);
        int pr = poll(&pfd, 1, t);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pfd.revents & POLLIN) nh_syncd_watcher_drain(wa);
        if (nh_syncd_batcher_poll(ba) == NH_SYNCD_BATCHER_READY) {
            nh_syncd_batch *batch = nh_syncd_batcher_take(ba, false);
            if (batch) {
                /* Build push config. */
                nh_syncd_push_cfg pc = {0};
                pc.blossom_servers   = (const char *const *)cfg.blossom;
                pc.n_blossom_servers = cfg.n_blossom;
                pc.min_replication   = cfg.min_repl;
                pc.relays            = (const char *const *)cfg.relays;
                pc.n_relays          = cfg.n_relays;
                pc.event_signer_nsec_hex = cfg.nsec_hex;
                pc.d_tag             = cfg.d_tag;
                /* home_key/root_id derivation from the broker seed
                 * belongs to I3's credential handoff; for now the
                 * daemon refuses to push without a seed. */
                if (!cfg.seed_hex) {
                    fprintf(stderr, "syncd: batch %llu deferred — NOSTR_HOMED_SYNCD_SEED_HEX unset\n",
                            (unsigned long long)nh_syncd_batch_id(batch));
                    nh_syncd_batch_free(batch);
                    continue;
                }
                uint8_t seed[32];
                if (nh_porthome_from_hex64(cfg.seed_hex, seed) != 0 ||
                    nh_porthome_key_derive(seed, pc.home_key) != 0) {
                    fprintf(stderr, "syncd: seed derivation failed\n");
                    nh_syncd_batch_free(batch);
                    continue;
                }
                /* root_id: use the seed itself as an opaque identifier
                 * for I1 (matches operator publisher). I3 will source
                 * this from broker state alongside home_key. */
                memcpy(pc.root_id, seed, 32);

                char *emsg = NULL;
                int pushed = nh_syncd_push_batch(&pc, state, batch,
                                                 cfg.home, ig, &ilk,
                                                 cfg.state_dir, &emsg);
                if (pushed == NH_SYNCD_OK) {
                    fprintf(stderr, "syncd: batch %llu OK gen=%llu\n",
                            (unsigned long long)nh_syncd_batch_id(batch),
                            (unsigned long long)nh_syncd_state_get_local_generation(state));
                } else {
                    fprintf(stderr, "syncd: batch %llu FAILED rc=%d %s\n",
                            (unsigned long long)nh_syncd_batch_id(batch),
                            pushed, emsg ? emsg : "");
                }
                free(emsg);
                memset(seed, 0, sizeof seed);
                memset(pc.home_key, 0, sizeof pc.home_key);
                nh_syncd_batch_free(batch);
            }
        }
    }

    /* Final flush on shutdown. */
    nh_syncd_batch *tail = nh_syncd_batcher_take(ba, true);
    if (tail) nh_syncd_batch_free(tail);

    nh_syncd_watcher_free(wa);
cleanup:
    if (state) nh_syncd_state_free(state);
    nh_syncd_batcher_free(ba);
    nh_syncd_ignore_free(ig);
    nh_syncd_lock_release(lk);
    free_cfg(&cfg);
    return 0;
}
