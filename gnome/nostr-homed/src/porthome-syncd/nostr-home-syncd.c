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
 *   NOSTR_HOMED_SYNCD_SEED_HEX   — 64-hex seed for home_key derivation.
 *                                  Env fallback only; headless tests
 *                                  and manual operator use.
 *   NOSTR_HOMED_SYNCD_SEED_FILE  — optional path override for the
 *                                  per-user broker seed drop
 *                                  (default: /run/nostr-auth/session/
 *                                  <uid>/home_seed).  W(3): read once,
 *                                  unlink, mlock; wraps in-process wipe.
 *
 * Signals:
 *   SIGTERM/SIGINT — flush final batch, release lock, exit 0.
 *   SIGHUP         — reload user ignore file.
 */

#include "nh_syncd.h"
/* nh_syncd_cache.h collides with nh_syncd.h on nh_syncd_notify_fn.
 * Forward-declare only what we need from the cache side. */
struct nh_syncd_pin_ring;
#define NH_SYNCD_PR_OK 0
#define NH_SYNCD_PR_ERR_JSON -303
extern char *nh_syncd_cache_default_pin_path(void);
extern int   nh_syncd_pin_ring_open(const char *json_path,
                                    struct nh_syncd_pin_ring **out);
extern void  nh_syncd_pin_ring_close(struct nh_syncd_pin_ring *r);
extern int   nh_syncd_pin_ring_promote_from_snapshot(struct nh_syncd_pin_ring *r,
                                                     const char *state_dir);
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
#include <sys/mman.h>
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


/* ────────────────────────────────────────────────────────────────────
 * W(3) — read the wrap seed from the broker's per-user runtime drop.
 *
 * On success:
 *   - returns 0
 *   - writes 64 lowercase hex chars + NUL to `out_hex` (>= 65 bytes)
 *   - unlink(2)s the source file so the next syncd start doesn't reuse
 *     a stale seed
 *   - mlock(2)s the on-heap buffer holding the hex value
 *
 * On failure (file missing, malformed, too short) returns -1 and leaves
 * `out_hex` empty — the caller falls back to the NOSTR_HOMED_SYNCD_SEED_HEX
 * env var (headless-test path).
 *
 * Path resolution: NOSTR_HOMED_SYNCD_SEED_FILE env override, else
 * /run/nostr-auth/session/<uid>/home_seed. The file is expected to
 * contain 64 hex chars (no newline required; a trailing whitespace is
 * tolerated). Never logs the seed value itself.
 * ──────────────────────────────────────────────────────────────────── */
static int read_seed_from_broker_drop(char *out_hex /* [65] */) {
    out_hex[0] = '\0';
    const char *ovr = getenv("NOSTR_HOMED_SYNCD_SEED_FILE");
    char path[256];
    if (ovr && *ovr) {
        snprintf(path, sizeof path, "%s", ovr);
    } else {
        snprintf(path, sizeof path,
                 "/run/nostr-auth/session/%u/home_seed",
                 (unsigned)getuid());
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[96];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    if (n < 64) { close(fd); return -1; }
    buf[n] = '\0';
    close(fd);
    /* Trim trailing whitespace. */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' ||
                     buf[n-1] == ' '  || buf[n-1] == '\t')) buf[--n] = '\0';
    if (n < 64) return -1;
    for (size_t i = 0; i < 64; i++) {
        char c = buf[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return -1;
    }
    memcpy(out_hex, buf, 64);
    out_hex[64] = '\0';
    /* Wipe the local buffer + unlink the source. */
    memset(buf, 0, sizeof buf);
    (void)unlink(path);
    fprintf(stderr, "syncd: read wrap seed from broker drop at %s (unlinked)\n",
            path);
    return 0;
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
    /* W(3): prefer the broker-side per-user drop; env is the fallback
     * kept for headless tests. Both paths land the seed at c->seed_hex
     * for the existing derivation in the batch loop. */
    char seed_from_drop[65] = {0};
    if (read_seed_from_broker_drop(seed_from_drop) == 0) {
        c->seed_hex = xstrdup(seed_from_drop);
    } else {
        c->seed_hex = xstrdup(getenv("NOSTR_HOMED_SYNCD_SEED_HEX"));
    }
    /* Best-effort mlock the heap copy; RLIMIT_MEMLOCK may reject it,
     * in which case the wipe-on-free in free_cfg is still the durable
     * defence. */
    if (c->seed_hex) (void)mlock(c->seed_hex, strlen(c->seed_hex) + 1);
    /* Wipe the local hex buffer immediately. */
    memset(seed_from_drop, 0, sizeof seed_from_drop);
    const char *bl = getenv("NOSTR_HOMED_SYNCD_BLOSSOM");
    const char *rl = getenv("NOSTR_HOMED_SYNCD_RELAYS");
    c->blossom = split_csv(bl, &c->n_blossom);
    c->relays  = split_csv(rl, &c->n_relays);
    const char *mr = getenv("NOSTR_HOMED_SYNCD_MIN_REPL");
    c->min_repl = mr ? (size_t)strtoul(mr, NULL, 10) : NH_SYNCD_DEFAULT_MIN_REPLICATION;
    return 0;
}

static void free_cfg(cfg_t *c) {
    free(c->home); free(c->state_dir); free(c->nsec_hex); free(c->d_tag);
    if (c->seed_hex) { memset(c->seed_hex, 0, strlen(c->seed_hex)); free(c->seed_hex); }
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

    nh_syncd_ignore *ig = NULL;
    if (nh_syncd_ignore_new(cfg.home, &ig) != NH_SYNCD_OK) {
        fprintf(stderr, "syncd: ignore init failed\n");
        nh_syncd_lock_release(lk); free_cfg(&cfg); return 6;
    }

    /* I2 §6.4: if the snapshot base is unknown, do an ADDITIVE rescan
     * of $HOME and mark NOSTR_HOME_STATE=partial. The push path will
     * refuse to run until a real pull path rebuilds base. */
    if (snap_unknown) {
        fprintf(stderr,
                "syncd: snapshot base unknown; running additive rescan + setting partial state\n");
        (void)nh_syncd_rescan_home_additive(state, cfg.home, ig, cfg.state_dir);
        (void)nh_syncd_partial_state_set(cfg.state_dir);
    }

    /* Interlocks now include the file marker in addition to the env
     * variable — the I2 marker file is authoritative because it
     * survives across daemon restarts. */
    const char *state_marker = nh_syncd_partial_state_is_set(cfg.state_dir)
                              ? "partial" : getenv("NOSTR_HOME_STATE");
    ilk.nostr_home_state = state_marker;
    nh_syncd_batcher *ba = NULL;
    if (nh_syncd_batcher_new(now_ns_monotonic, NULL, 0, 0, &ba) != NH_SYNCD_OK) {
        nh_syncd_ignore_free(ig); nh_syncd_lock_release(lk); free_cfg(&cfg); return 6;
    }

    /* Generation pin ring (design §6.5). Persisted at
     * $XDG_STATE_HOME/nostr-homed/pinned.json. If the JSON is corrupt we
     * unlink it and reopen empty — a lost ring costs at most N generations
     * of eviction safety, not user data. Failure to open at all is
     * warn-only (cache-hygiene, not correctness). */
    struct nh_syncd_pin_ring *pin_ring = NULL;
    char *pin_path = nh_syncd_cache_default_pin_path();
    if (pin_path) {
        int pr = nh_syncd_pin_ring_open(pin_path, &pin_ring);
        if (pr == NH_SYNCD_PR_ERR_JSON) {
            (void)unlink(pin_path);
            pr = nh_syncd_pin_ring_open(pin_path, &pin_ring);
        }
        if (pr != NH_SYNCD_PR_OK) {
            fprintf(stderr, "syncd: pin_ring open failed rc=%d (retention pinning disabled)\n", pr);
            pin_ring = NULL;
        }
    }

    install_signals(-1);

    if (dry_run) {
        fprintf(stderr, "syncd: --check OK (lock=held, interlocks=pass, state=%s)\n",
                snap_unknown ? "unknown" : "loaded");
        if (state) nh_syncd_state_free(state);
        nh_syncd_batcher_free(ba); nh_syncd_ignore_free(ig);
        if (pin_ring) nh_syncd_pin_ring_close(pin_ring);
        free(pin_path);
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
                    /* W(1)(a): promote current snapshot into the ring so
                     * every blob referenced by the last N=10 generations
                     * is pinned. Warn-only on failure. */
                    if (pin_ring) {
                        int prc = nh_syncd_pin_ring_promote_from_snapshot(pin_ring, cfg.state_dir);
                        if (prc != NH_SYNCD_PR_OK)
                            fprintf(stderr, "syncd: pin_ring promote (push) rc=%d\n", prc);
                    }
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
    if (pin_ring) nh_syncd_pin_ring_close(pin_ring);
    free(pin_path);
    nh_syncd_lock_release(lk);
    free_cfg(&cfg);
    return 0;
}
