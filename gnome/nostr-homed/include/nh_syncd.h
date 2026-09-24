/*
 * nh_syncd.h — portable-home Phase 3 user-session sync daemon (I1).
 *
 * SPDX-License-Identifier: MIT
 *
 * WARNING: EXPERIMENTAL AND UNREVIEWED.
 * Gated behind NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL (requires
 * NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL). Do not deploy.
 *
 * See docs/designs/home-from-relay.md §6 for the design.
 *
 * This header is the STABLE seam between I1 (daemon core + push path)
 * and later items:
 *   - I2 (pull/reconciler) plugs into the `on_remote_pointer` callback
 *     and reads/writes the state accessors below.
 *   - I3 (cache/retention/packaging) will add /var/cache paths and the
 *     systemd --user unit; it does NOT modify these types.
 *
 * Beads: nostrc-p6qp (I1), nostrc-h10m (portable-home epic).
 */

#ifndef NH_SYNCD_H
#define NH_SYNCD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "nh_porthome_crypto.h"
#include "nh_porthome_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────
 * Error codes.  All success returns 0; failure returns negative. Values
 * are disjoint from the porthome/blossom ranges so callers can dispatch
 * on rc across the whole stack.
 * ──────────────────────────────────────────────────────────────────── */

typedef enum {
    NH_SYNCD_OK                    = 0,
    NH_SYNCD_ERR_ARG               = -200,
    NH_SYNCD_ERR_OOM               = -201,
    NH_SYNCD_ERR_IO                = -202,
    NH_SYNCD_ERR_JSON              = -203,
    NH_SYNCD_ERR_LOCKED            = -204, /* another daemon holds sync.lock */
    NH_SYNCD_ERR_INTERLOCK         = -205, /* §6.4 refuses this push */
    NH_SYNCD_ERR_SNAPSHOT_UNKNOWN  = -206, /* snapshot.json missing/corrupt */
    NH_SYNCD_ERR_LIMITED_MODE      = -207, /* ~/.nostr-home-limited present */
    NH_SYNCD_ERR_PARTIAL_STATE     = -208, /* NOSTR_HOME_STATE=partial */
    NH_SYNCD_ERR_UPLOAD            = -209, /* blossom upload failure */
    NH_SYNCD_ERR_PUBLISH           = -210, /* no relay OK'd the pointer */
    NH_SYNCD_ERR_CRYPTO            = -211,
    NH_SYNCD_ERR_MANIFEST          = -212,
    NH_SYNCD_ERR_PATH              = -213,
} nh_syncd_status;

/* Constant reserved for the I3 local blob cache directory (§6.5). The
 * ignore matcher already excludes this path from capture, even though
 * I3 will be the one to create/populate it. Relative to $HOME. */
#define NH_SYNCD_LOCAL_CACHE_REL ".cache/nostr-homed"

/* Chunk size used for capture. Matches the operator publisher
 * (tests/integration/nostr_home_publisher.c) so a fixture captured by
 * one is byte-identical to the other. 4 MiB.
 *
 * Not a nh_porthome_crypto constant: the crypto layer is oblivious to
 * chunk boundaries — it accepts any plaintext up to its overhead cap.
 * The 4 MiB choice is a sync-daemon policy: bigger reduces manifest
 * entry count for large files; smaller reduces re-upload cost when one
 * middle chunk changes. */
#define NH_SYNCD_CHUNK_SIZE (4u * 1024u * 1024u)

/* Force-flush period (§6.2 bullet 2). */
#define NH_SYNCD_FORCE_FLUSH_SECS  (15u * 60u)
/* Debounce idle window (§6.2 bullet 2). */
#define NH_SYNCD_DEBOUNCE_SECS     5u

/* Default replication target for Blossom (design §5.4, §6.2). */
#define NH_SYNCD_DEFAULT_MIN_REPLICATION 2u

/* Per-relay OK wait when publishing the pointer. */
#define NH_SYNCD_DEFAULT_PUBLISH_TIMEOUT_MS 10000u

/* ────────────────────────────────────────────────────────────────────
 * State (opaque). Persisted at:
 *   ${XDG_STATE_HOME:-~/.local/state}/nostr-homed/snapshot.json
 *   ${XDG_STATE_HOME:-~/.local/state}/nostr-homed/generation
 *   ${XDG_STATE_HOME:-~/.local/state}/nostr-homed/remote.json  (I2 fills)
 *
 * snapshot.json schema (v1):
 *   {
 *     "schema": 1,
 *     "generation": <u64>,
 *     "root":       "<abs $HOME>",
 *     "d_tag":      "<string>",
 *     "account_pubkey_hex": "<64 hex or empty>",
 *     "root_id_hex":        "<64 hex>",
 *     "files": {
 *       "<rel-plaintext-path>": {
 *         "kind":     "file"|"dir"|"symlink",
 *         "mode":     <u32>,
 *         "uid":      <u32>,
 *         "gid":      <u32>,
 *         "mtime_ns": <u64>,
 *         "size":     <u64>,
 *         "content_hash_hex":  "<64 hex or empty>",
 *         "chunk_addrs_hex":   ["<64 hex>", ...],   // omit for dirs/symlinks
 *         "symlink_target":    "..."                // only for symlinks
 *       }
 *     }
 *   }
 *
 * `generation` is also written as its own single-line ASCII decimal file
 * for atomic monotonic reads by external observers (systemd status,
 * later CLI). snapshot.json is the source of truth on daemon startup.
 * ──────────────────────────────────────────────────────────────────── */

typedef enum {
    NH_SYNCD_KIND_FILE    = 1,
    NH_SYNCD_KIND_DIR     = 2,
    NH_SYNCD_KIND_SYMLINK = 3,
} nh_syncd_kind;

typedef struct nh_syncd_state nh_syncd_state;

/* Load state from `state_dir`. If files are missing, the returned state
 * is empty with generation=0 and `*out_snapshot_unknown = true`.
 * If files exist but the JSON is malformed, returns NH_SYNCD_ERR_JSON
 * and sets `*out_snapshot_unknown = true` (I1 push path will then
 * refuse to run — I2 owns the "rescan" additive-only reconcile). */
int  nh_syncd_state_load(const char *state_dir,
                         nh_syncd_state **out,
                         bool *out_snapshot_unknown);

/* Persist to disk. Writes to `snapshot.json.tmp` then rename(2) atomic
 * swap; same for `generation.tmp`. */
int  nh_syncd_state_save(const nh_syncd_state *s, const char *state_dir);

void nh_syncd_state_free(nh_syncd_state *s);

/* Accessors used by I2 (pull). */
uint64_t nh_syncd_state_get_local_generation(const nh_syncd_state *s);
uint64_t nh_syncd_state_get_remote_generation(const nh_syncd_state *s);
void     nh_syncd_state_set_remote_generation(nh_syncd_state *s, uint64_t g);

/* Root/D-tag/pubkey accessors (may return NULL for empty). */
const char *nh_syncd_state_get_root(const nh_syncd_state *s);
const char *nh_syncd_state_get_d_tag(const nh_syncd_state *s);
const char *nh_syncd_state_get_account_pubkey_hex(const nh_syncd_state *s);

/* Number of files/dirs/symlinks recorded. */
size_t   nh_syncd_state_file_count(const nh_syncd_state *s);

/* Look up a single entry (returns NULL if absent). Fields via getters
 * below. The pointer is owned by the state; do not free. */
typedef struct nh_syncd_entry nh_syncd_entry;
const nh_syncd_entry *nh_syncd_state_find(const nh_syncd_state *s,
                                          const char *rel_path);
nh_syncd_kind  nh_syncd_entry_kind(const nh_syncd_entry *e);
uint64_t       nh_syncd_entry_size(const nh_syncd_entry *e);
uint64_t       nh_syncd_entry_mtime_ns(const nh_syncd_entry *e);
/* content_hash_hex is NUL-terminated (64 hex) or "" for dirs/symlinks. */
const char    *nh_syncd_entry_content_hash_hex(const nh_syncd_entry *e);

/* ────────────────────────────────────────────────────────────────────
 * Additive getters for external readers (Phase 4 P4-I, bead nostrc-1u55).
 *
 * The FUSE overlay (nostr-home-fuse) needs to walk snapshot.json to
 * build its in-memory namespace without duplicating the JSON schema.
 * These getters expose read-only introspection of every entry — no
 * behaviour change, no new writers.
 *
 * `nh_syncd_state_at`: iterate entries by insertion order (jansson's
 * default). `out_rel_path` receives a borrowed pointer valid for the
 * lifetime of the state. Returns NULL when `i` is out of range.
 *
 * These getters use thread-local storage (like `_find`) so callers
 * must consume the result before calling any state accessor from the
 * same thread. The FUSE mount snapshots the whole table at load
 * time; it does not hold multiple entries live.
 * ──────────────────────────────────────────────────────────────────── */
const nh_syncd_entry *nh_syncd_state_at(const nh_syncd_state *s, size_t i,
                                        const char **out_rel_path);
uint32_t     nh_syncd_entry_mode          (const nh_syncd_entry *e);
uint32_t     nh_syncd_entry_uid           (const nh_syncd_entry *e);
uint32_t     nh_syncd_entry_gid           (const nh_syncd_entry *e);
size_t       nh_syncd_entry_chunk_count   (const nh_syncd_entry *e);
/* Returns a borrowed 64-hex + NUL pointer (or NULL when `i` is out of
 * range). Valid until the next call on this state from the same
 * thread. */
const char  *nh_syncd_entry_chunk_at      (const nh_syncd_entry *e, size_t i);
/* Returns "" (never NULL) for non-symlinks. */
const char  *nh_syncd_entry_symlink_target(const nh_syncd_entry *e);

/* Initialise a fresh state (empty). Caller frees with _free(). Used by
 * `nostr-home-syncd --init` and by tests. */
int nh_syncd_state_new(const char *root_abs,
                       const char *d_tag,
                       const char *account_pubkey_hex_or_empty,
                       const uint8_t root_id[NH_PORTHOME_SHA256_LEN],
                       nh_syncd_state **out);

/* ────────────────────────────────────────────────────────────────────
 * Ignore matcher (§6.2 bullet 1 + user file).
 *
 * The matcher owns:
 *   - a static exclude set (baked into I1; §6.2)
 *   - the user file ~/.config/nostr-homed/ignore (globs, `#` comments)
 *   - the $HOME device id (xdev check) captured at load time
 *
 * Matching uses fnmatch(FNM_PATHNAME | FNM_LEADING_DIR) against paths
 * RELATIVE to $HOME (no leading slash). A directory match implicitly
 * covers everything beneath.
 * ──────────────────────────────────────────────────────────────────── */

typedef struct nh_syncd_ignore nh_syncd_ignore;

/* Build from a $HOME root. Reads ~/.config/nostr-homed/ignore if
 * present; missing file is not an error. The current st_dev of
 * `home_dir` is remembered for xdev decisions. */
int  nh_syncd_ignore_new(const char *home_dir,
                         nh_syncd_ignore **out);
void nh_syncd_ignore_free(nh_syncd_ignore *ig);

/* Reload the user ignore file (call on config-change / SIGHUP; I3
 * hooks this). */
int  nh_syncd_ignore_reload(nh_syncd_ignore *ig, const char *home_dir);

/* Test seams: append a single glob or override the recorded st_dev.
 * Used by unit tests; production code should not call these. */
int  nh_syncd_ignore_add_user_glob(nh_syncd_ignore *ig, const char *glob);
void nh_syncd_ignore_set_home_dev(nh_syncd_ignore *ig, uint64_t dev);

/* Result of matching. */
typedef enum {
    NH_SYNCD_IGNORE_PASS   = 0, /* not ignored */
    NH_SYNCD_IGNORE_STATIC = 1, /* matched a static rule */
    NH_SYNCD_IGNORE_USER   = 2, /* matched the user file */
    NH_SYNCD_IGNORE_XDEV   = 3, /* on a different filesystem */
    NH_SYNCD_IGNORE_SPECIAL= 4, /* socket / fifo / device — from stat */
    NH_SYNCD_IGNORE_CONTROL= 5, /* .nostr-home-limited, state dir, cache dir */
} nh_syncd_ignore_kind;

/* Full check.  `rel_path` MUST be relative to $HOME, no leading '/'.
 * `st_dev` and `st_mode` are the stat(2) values (pass 0/0 if unknown —
 * the xdev + special-file checks will be skipped). */
nh_syncd_ignore_kind nh_syncd_ignore_check(const nh_syncd_ignore *ig,
                                           const char *rel_path,
                                           uint64_t st_dev,
                                           uint32_t st_mode);

/* Path-only variant used when stat() is not yet available (inotify
 * IN_CREATE for a plain file). */
nh_syncd_ignore_kind nh_syncd_ignore_check_path(const nh_syncd_ignore *ig,
                                                const char *rel_path);

/* ────────────────────────────────────────────────────────────────────
 * Lazy-subtree matcher (Phase 4 P4-I, bead nostrc-1u55).
 *
 * Strictly opt-in: an empty matcher matches nothing and reconcile
 * behaves identically to earlier versions (behavior-preserving).
 *
 * Prefixes are $HOME-relative directory paths (no leading '/'). A
 * prefix matches an entry rel_path when the entry is exactly the
 * prefix or begins with `<prefix>/`. Empty / whitespace-only
 * lines are ignored; '#' starts a comment.
 *
 * Load order (any of the following, in decreasing priority):
 *   1. `nh_syncd_lazy_new_from_string(csv_prefixes, ...)` — used by
 *      tests and to expose an env-var / config-file knob;
 *   2. `nh_syncd_lazy_new(home_dir, ...)` — reads
 *      `~/.config/nostr-homed/lazy` if present.
 *
 * When plugged into `nh_syncd_reconcile_cfg.lazy` the reconciler
 * SUPPRESSES eager materialization of matching entries — their
 * metadata is still recorded in snapshot.json (so the FUSE mount can
 * serve them on demand) but no bytes are written to $HOME. Existing
 * files under a lazy prefix from prior reconciles are NEVER deleted:
 * remote-delete records that fall under a lazy prefix become
 * warn-logged no-ops (additive suppression only, per maintainer
 * approval note at the foot of docs/designs/nostrfs-porthome-overlay.md).
 * ──────────────────────────────────────────────────────────────────── */

typedef struct nh_syncd_lazy nh_syncd_lazy;

/* Empty matcher (never matches). */
int  nh_syncd_lazy_new_empty(nh_syncd_lazy **out);
/* Read `~/.config/nostr-homed/lazy` if present. Missing file → empty
 * matcher (not an error). */
int  nh_syncd_lazy_new(const char *home_dir, nh_syncd_lazy **out);
/* Parse comma- and/or newline-separated prefixes from a string. */
int  nh_syncd_lazy_new_from_string(const char *prefixes, nh_syncd_lazy **out);

void nh_syncd_lazy_free(nh_syncd_lazy *lz);

/* Test seams. */
int    nh_syncd_lazy_add_prefix(nh_syncd_lazy *lz, const char *prefix);
size_t nh_syncd_lazy_prefix_count(const nh_syncd_lazy *lz);

/* Returns true iff `rel_path` falls under any lazy prefix (or IS one). */
bool nh_syncd_lazy_covers(const nh_syncd_lazy *lz, const char *rel_path);

/* ────────────────────────────────────────────────────────────────────
 * Batcher (debounce + coalesce, fixed-clock testable).
 *
 * A change is (rel_path, changekind). Changes coalesce: multiple hits
 * on one path collapse into one; the last kind wins EXCEPT that a
 * later DELETE always overrides prior MODIFY/CREATE, and a CREATE
 * after DELETE overrides back to CREATE (an inotify move-in reversing
 * a move-out is not this common case; both are captured under CREATE).
 * ──────────────────────────────────────────────────────────────────── */

typedef enum {
    NH_SYNCD_CHANGE_CREATE = 1,
    NH_SYNCD_CHANGE_MODIFY = 2,
    NH_SYNCD_CHANGE_DELETE = 3,
} nh_syncd_change_kind;

typedef struct nh_syncd_batch nh_syncd_batch;
typedef struct nh_syncd_batcher nh_syncd_batcher;

/* Monotonic clock in nanoseconds. The batcher takes a `now_fn`
 * callback so tests can drive it deterministically. */
typedef uint64_t (*nh_syncd_now_fn)(void *ud);

/* `debounce_ns` and `max_hold_ns` default to
 * NH_SYNCD_DEBOUNCE_SECS/NH_SYNCD_FORCE_FLUSH_SECS in seconds if 0. */
int  nh_syncd_batcher_new(nh_syncd_now_fn now_fn, void *now_ud,
                          uint64_t debounce_ns,
                          uint64_t max_hold_ns,
                          nh_syncd_batcher **out);
void nh_syncd_batcher_free(nh_syncd_batcher *b);

/* Record a change. Silently deduplicates. */
int  nh_syncd_batcher_push(nh_syncd_batcher *b,
                           const char *rel_path,
                           nh_syncd_change_kind kind);

typedef enum {
    NH_SYNCD_BATCHER_IDLE       = 0, /* nothing pending */
    NH_SYNCD_BATCHER_COALESCING = 1, /* pending, but debounce not up */
    NH_SYNCD_BATCHER_READY      = 2, /* flush now: idle or max-hold expired */
} nh_syncd_batcher_state;

nh_syncd_batcher_state nh_syncd_batcher_poll(nh_syncd_batcher *b);

/* Milliseconds until the next state change (0 if READY, UINT64_MAX
 * if IDLE). Use to size the main-loop timerfd / epoll_wait timeout. */
uint64_t nh_syncd_batcher_next_tick_ms(nh_syncd_batcher *b);

/* Consume the pending batch. Caller frees with nh_syncd_batch_free.
 * Returns NULL if not READY (unless `force` is true, in which case
 * whatever is pending — possibly empty — is returned).  Increments
 * the monotonic batch id on the daemon side. */
nh_syncd_batch *nh_syncd_batcher_take(nh_syncd_batcher *b, bool force);

uint64_t nh_syncd_batch_id(const nh_syncd_batch *b);
size_t   nh_syncd_batch_len(const nh_syncd_batch *b);
int      nh_syncd_batch_at(const nh_syncd_batch *b, size_t i,
                           const char **out_rel, nh_syncd_change_kind *out_k);
void     nh_syncd_batch_free(nh_syncd_batch *b);

/* ────────────────────────────────────────────────────────────────────
 * Interlocks (§6.4).
 * ──────────────────────────────────────────────────────────────────── */

typedef struct {
    /* Absolute path to $HOME. */
    const char *home_dir;
    /* Value of NOSTR_HOME_STATE (may be NULL). "partial" refuses push. */
    const char *nostr_home_state;
} nh_syncd_interlocks;

/* Returns NH_SYNCD_OK if push is allowed, otherwise one of:
 *   NH_SYNCD_ERR_LIMITED_MODE, NH_SYNCD_ERR_PARTIAL_STATE.
 * The snapshot-base check (NH_SYNCD_ERR_SNAPSHOT_UNKNOWN) is done by
 * nh_syncd_push_batch since it needs the state pointer. */
int nh_syncd_interlocks_check(const nh_syncd_interlocks *ilk);

/* ────────────────────────────────────────────────────────────────────
 * Single-instance lock: ~/.local/state/nostr-homed/sync.lock via
 * flock(LOCK_EX | LOCK_NB). Returns NH_SYNCD_ERR_LOCKED if held.
 * ──────────────────────────────────────────────────────────────────── */

typedef struct nh_syncd_lock nh_syncd_lock;

int  nh_syncd_lock_acquire(const char *state_dir, nh_syncd_lock **out);
void nh_syncd_lock_release(nh_syncd_lock *l);

/* ────────────────────────────────────────────────────────────────────
 * Push closure.
 *
 * Test injection points:
 *   - `bud02_signer`: a hanami_signer_t for BUD-02 (fake or real).
 *   - `event_signer_nsec_hex`: 64-hex secp256k1 nsec used to sign the
 *     kind-30078 pointer. Env-configured in prod; test-injected in
 *     integration. A NULL nsec makes the push return NH_SYNCD_ERR_ARG.
 *   - `event_publish_fn` (optional override): if set, the push closure
 *     calls this INSTEAD of doing its own libnostr relay round-trip.
 *     Used by pure-unit tests that must not open sockets.
 *
 * When `event_publish_fn` is NULL the push closure uses libnostr:
 *   - Opens NostrRelay per URL, connects, calls
 *     nostr_relay_publish_and_wait with `relay_publish_timeout_ms`.
 *   - Success = at least one relay returns OK true.
 *   - Never sleeps as a wait primitive — it uses the relay's own
 *     event-driven OK wait.
 * ──────────────────────────────────────────────────────────────────── */

/* libhanami's signer type is a typedef of an anonymous struct in
 * hanami-types.h, which cannot be forward-declared. Include the
 * header directly. This is the same tradeoff nh_porthome_blossom.h
 * makes — every caller of this API already has libhanami on the
 * include path. */
#include <hanami/hanami-types.h>

/* Signature used by test-only publish override. Return 0 on OK. */
typedef int (*nh_syncd_publish_fn)(void *ud,
                                   const char *pubkey_hex,
                                   int64_t created_at,
                                   const char *d_tag,
                                   const uint8_t *sealed,
                                   size_t sealed_len,
                                   char out_event_id[65]);

typedef struct {
    /* Blossom transport. */
    const char *const *blossom_servers;
    size_t             n_blossom_servers;
    size_t             min_replication;             /* 0 => default 2 */
    const hanami_signer_t *bud02_signer;            /* required for real Blossom */

    /* Encryption / manifest. */
    uint8_t            home_key[NH_PORTHOME_KEY_LEN];
    uint8_t            root_id[NH_PORTHOME_SHA256_LEN];

    /* Relay transport (used only when event_publish_fn == NULL). */
    const char *const *relays;
    size_t             n_relays;
    uint32_t           relay_publish_timeout_ms;    /* 0 => default 10s */

    /* Kind-30078 signer. Exactly one of these MUST be set. */
    const char        *event_signer_nsec_hex;       /* 64-hex OR NULL */
    /* Test-only publish override. If non-NULL, `event_signer_nsec_hex`
     * still supplies the account pubkey (for its `pubkey` tag), so the
     * caller must also set `account_pubkey_hex_override` — or provide
     * nsec_hex so the derivation runs. */
    nh_syncd_publish_fn event_publish_fn;
    void               *event_publish_ud;
    const char        *account_pubkey_hex_override; /* NULL => derive from nsec */

    /* Address of the kind-30078 pointer. */
    const char        *d_tag;                       /* NULL => default */
} nh_syncd_push_cfg;

/* Execute the push pipeline once for `batch`. On success the state's
 * files map and local_generation are advanced and persisted to disk.
 * On failure, state is left untouched (caller decides retry/backoff). */
int nh_syncd_push_batch(const nh_syncd_push_cfg *cfg,
                        nh_syncd_state           *state,
                        const nh_syncd_batch     *batch,
                        const char               *root_dir,
                        const nh_syncd_ignore    *ignore,
                        const nh_syncd_interlocks *ilk,
                        const char               *state_dir_for_persist,
                        char                    **out_error_msg);

/* ────────────────────────────────────────────────────────────────────
 * Remote-pointer subscription seam (I1 wires a minimal REQ; I2 owns
 * the reconcile decoder).
 * ──────────────────────────────────────────────────────────────────── */

typedef void (*nh_syncd_on_remote_pointer_fn)(void *ud,
                                              const char *event_id_hex,
                                              int64_t     created_at,
                                              const char *d_tag,
                                              const char *content_hex_or_b64);

/* I1's default hook: records `created_at` in-memory (bumps
 * remote_generation to the max observed) and persists to
 * `${state_dir}/remote.json`. I2 replaces this with a real decoder. */
void nh_syncd_default_remote_pointer_recorder(void *ud,
                                              const char *event_id_hex,
                                              int64_t     created_at,
                                              const char *d_tag,
                                              const char *content_hex_or_b64);

/* Opaque subscription handle. */
typedef struct nh_syncd_subscription nh_syncd_subscription;

int  nh_syncd_subscription_start(const char *const *relays, size_t n_relays,
                                 const char *account_pubkey_hex,
                                 const char *d_tag,
                                 nh_syncd_on_remote_pointer_fn cb,
                                 void *cb_ud,
                                 nh_syncd_subscription **out);
void nh_syncd_subscription_stop(nh_syncd_subscription *s);

/* ────────────────────────────────────────────────────────────────────
 * I2 additions — pull path + three-way reconciler + conflict policy.
 *
 * Sibling to the push seam above. All declarations below are owned by
 * Item 2 (bead nostrc-p6qp, parent nostrc-h10m). Do not extend without
 * coordinating with the I1/I3 owners.
 *
 * See docs/designs/home-from-relay.md §6.3 (pull + merge) and §6.4
 * (interlocks) for behaviour; docs/reviews/porthome-syncd-pull-2026-09-23.md
 * for v1 punts + reproducers.
 * ──────────────────────────────────────────────────────────────────── */

/* Persisted marker for §6.4 "unknown / rebuilding base". Written at
 * `${state_dir}/state` as a single line: "partial\n" or "ok\n". Absent
 * file counts as "ok". The daemon main loads this before checking
 * interlocks and passes it into `nh_syncd_interlocks.nostr_home_state`. */
#define NH_SYNCD_STATE_MARKER_FILE   "state"
#define NH_SYNCD_STATE_MARKER_PARTIAL "partial"
#define NH_SYNCD_STATE_MARKER_OK      "ok"

int  nh_syncd_partial_state_set  (const char *state_dir);
int  nh_syncd_partial_state_clear(const char *state_dir);
/* Returns 1 iff the marker file exists AND says "partial", 0 otherwise
 * (missing / unreadable / "ok"). Never returns < 0. */
int  nh_syncd_partial_state_is_set(const char *state_dir);

/* Result of a single reconcile pass. Owned by caller; free with
 * nh_syncd_reconcile_result_free. */
typedef struct nh_syncd_reconcile_result nh_syncd_reconcile_result;

/* Introspection: counts of what happened. */
size_t nh_syncd_reconcile_result_applied     (const nh_syncd_reconcile_result *r);
size_t nh_syncd_reconcile_result_deleted     (const nh_syncd_reconcile_result *r);
size_t nh_syncd_reconcile_result_push_queued (const nh_syncd_reconcile_result *r);
size_t nh_syncd_reconcile_result_conflicts   (const nh_syncd_reconcile_result *r);

/* Access the i-th push-queue candidate (rel-path). */
const char *nh_syncd_reconcile_result_push_at (const nh_syncd_reconcile_result *r, size_t i);
/* Access the i-th conflict record. `out_rel` is the original relpath,
 * `out_conflict_path` is the sibling `.conflict-*` path that got
 * written (rel to $HOME). Either may be NULL if the caller doesn't
 * need it. Returns 0 on success. */
int nh_syncd_reconcile_result_conflict_at(const nh_syncd_reconcile_result *r,
                                          size_t i,
                                          const char **out_rel,
                                          const char **out_conflict_path);

void nh_syncd_reconcile_result_free(nh_syncd_reconcile_result *r);

/* Notification hook. If set, called ONCE at the end of a reconcile
 * pass that produced at least one conflict, with a
 * space-separated list of rel-paths (NUL-terminated). The default
 * behaviour when this is NULL and NOSTR_HOMED_SYNCD_NOTIFY!=0 is to
 * exec `notify-send` if it is on $PATH; if that fails the notification
 * is dropped (best effort — the conflict files themselves are the
 * durable record). */
typedef void (*nh_syncd_notify_fn)(void *ud, const char *summary,
                                   const char *body);

/* Reconcile config. All fields optional except manifest + home_key. */
typedef struct {
    const nh_porthome_manifest *manifest; /* remote (decoded) */
    uint64_t                    remote_generation;
    uint8_t                     home_key[NH_PORTHOME_KEY_LEN];

    /* Sink for fetching sealed chunk bytes by 64-hex address.
     * Signature matches nh_porthome_prov_fetch_fn — we accept the exact
     * function pointer so callers can share a Blossom-backed sink with
     * the fetch helper. Returns 0 on success (heap buffer via free()). */
    int (*fetch_chunk)(void *ctx, const char *sha256_hex,
                       uint8_t **out_ct, size_t *out_ct_len);
    void *fetch_chunk_ctx;

    /* Device name embedded in `.conflict-<device>-<ISO8601>` file
     * names. NULL → gethostname(). */
    const char *device_name;

    /* Batcher receives local-only changes so the push path picks them
     * up. May be NULL — in that case local-only changes are logged
     * only (used by the "cold start, additive-only" rescan path). */
    nh_syncd_batcher *push_queue;

    /* Notifier for conflict summary. NULL → default (see above). */
    nh_syncd_notify_fn notify;
    void              *notify_ud;

    /* Test seam: overrides the wall clock used to stamp conflict file
     * names.  0 → gettimeofday(). Unix time in seconds. */
    int64_t            clock_epoch_secs_override;

    /* Phase 4 P4-I (bead nostrc-1u55): opt-in lazy subtree matcher.
     * When non-NULL, entries whose rel_path falls under any prefix
     * held by `lazy` are recorded in snapshot.json but their bytes
     * are NOT materialized to $HOME. Remote-delete records for paths
     * under a lazy prefix become warn-logged no-ops (additive
     * suppression only — never destroy files that already exist).
     * NULL preserves prior behaviour exactly. */
    const nh_syncd_lazy *lazy;
} nh_syncd_reconcile_cfg;

/* Run one reconcile pass. `state` is mutated in place: applied entries
 * are upserted, deleted-from-remote entries are removed, and the local
 * generation is set to max(local, remote_generation). Persists state
 * to `state_dir_for_persist` if non-NULL. Returns 0 on success. */
int nh_syncd_reconcile_from_manifest(const nh_syncd_reconcile_cfg *cfg,
                                     nh_syncd_state           *state,
                                     const char               *root_dir,
                                     const nh_syncd_ignore    *ignore,
                                     const char               *state_dir_for_persist,
                                     nh_syncd_reconcile_result **out_result,
                                     char                    **out_error_msg);

/* Rescan $HOME additively — every regular file becomes a "candidate
 * for push" record. The state's `files` map is populated (kind/mode/
 * mtime_ns/size/content_hash_hex; chunk_addrs_hex left empty until a
 * push actually chunks + uploads). Writes the partial-state marker.
 * Never deletes anything. Used when snapshot.json is missing/corrupt
 * per §6.4. */
int nh_syncd_rescan_home_additive(nh_syncd_state    *state,
                                  const char        *root_dir,
                                  const nh_syncd_ignore *ignore,
                                  const char        *state_dir_for_persist);

/* Pull glue: build a reconcile config and pointer callback that feeds
 * a live subscription. The callback verifies the event (kind, author,
 * d-tag, signature), decrypts the sealed CBOR under `home_key`, and
 * — if the decoded generation strictly exceeds state's local — runs
 * `nh_syncd_reconcile_from_manifest`. Compose with
 * nh_syncd_subscription_start(). */
typedef struct nh_syncd_pull_ctx nh_syncd_pull_ctx;

typedef struct {
    /* Same as nh_syncd_reconcile_cfg less `manifest`+`remote_generation`. */
    uint8_t                     home_key[NH_PORTHOME_KEY_LEN];
    int (*fetch_chunk)(void *ctx, const char *sha256_hex,
                       uint8_t **out_ct, size_t *out_ct_len);
    void *fetch_chunk_ctx;
    const char *device_name;
    nh_syncd_batcher *push_queue;
    nh_syncd_notify_fn notify;
    void              *notify_ud;

    /* Live-relay context. */
    nh_syncd_state *state;             /* mutated on every applied EVENT */
    const char     *root_dir;
    const nh_syncd_ignore *ignore;
    const char     *state_dir_for_persist;

    /* The pubkey we expect to see on inbound pointers. */
    const char     *account_pubkey_hex;
    /* The `d` tag we expect. */
    const char     *d_tag;

    /* W(1)(b): optional generation pin ring (design §6.5). When set,
     * every successful reconcile promotes the current post-reconcile
     * snapshot into the ring so blobs referenced by the last N=10
     * generations are pinned against LRU eviction. NULL disables. */
    struct nh_syncd_pin_ring *pin_ring;
} nh_syncd_pull_opts;

int  nh_syncd_pull_ctx_new (const nh_syncd_pull_opts *opts,
                            nh_syncd_pull_ctx **out);
void nh_syncd_pull_ctx_free(nh_syncd_pull_ctx *c);

/* The nh_syncd_on_remote_pointer_fn compatible entry point. Wire this
 * as the subscription's callback with `c` as `cb_ud`. */
void nh_syncd_pull_on_pointer(void *ud,
                              const char *event_id_hex,
                              int64_t     created_at,
                              const char *d_tag,
                              const char *content_hex_or_b64);

/* Introspection for tests. */
uint64_t nh_syncd_pull_ctx_last_applied_generation(const nh_syncd_pull_ctx *c);
size_t   nh_syncd_pull_ctx_total_conflicts(const nh_syncd_pull_ctx *c);
size_t   nh_syncd_pull_ctx_total_events(const nh_syncd_pull_ctx *c);


#ifdef __cplusplus
}
#endif

#endif /* NH_SYNCD_H */
