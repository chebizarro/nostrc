/*
 * nh_provision_cli.h — internal helpers for nostr-homed-provision.
 *
 * SPDX-License-Identifier: MIT
 * EXPERIMENTAL. Gated behind NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL.
 *
 * The public CLI (nostr-homed-provision) is a thin dispatcher over the
 * five subcommand implementations. Everything that is unit-testable
 * without opening a socket / touching disk lives here so
 * tests/unit/test_provision_cli.c can drive it directly.
 *
 * Bead: nostrc-5fdu (follow-up of nostrc-89rj).
 */

#ifndef NH_PROVISION_CLI_H
#define NH_PROVISION_CLI_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────
 * Exit codes — mirror nostr-home-fetch where the meaning maps 1:1.
 * ──────────────────────────────────────────────────────────────────── */
#define NH_PROV_CLI_EXIT_OK              0
#define NH_PROV_CLI_EXIT_ARG            64
#define NH_PROV_CLI_EXIT_SSRF           65
#define NH_PROV_CLI_EXIT_NETWORK        71 /* relay / blossom transport */
#define NH_PROV_CLI_EXIT_DECODE         72 /* manifest schema */
#define NH_PROV_CLI_EXIT_SIZE_CAP       73
#define NH_PROV_CLI_EXIT_TIMEOUT        74
#define NH_PROV_CLI_EXIT_CRYPTO         75 /* AEAD / key derivation */
#define NH_PROV_CLI_EXIT_INTERNAL       76
#define NH_PROV_CLI_EXIT_MISSING_CHUNK   5 /* verify: pointer OK, chunk absent */
#define NH_PROV_CLI_EXIT_NO_STATE        3 /* status: file absent */

/* ────────────────────────────────────────────────────────────────────
 * account-file schema — JSON body written by `enroll --out-dir`.
 *
 * Layout (v1, stable within v1):
 *   {
 *     "schema": 1,
 *     "account_pubkey_hex": "<64 hex>",
 *     "account_nsec_hex":   "<64 hex>",   // sensitive; file is 0600 uid-owned
 *     "wrap_seed_hex":      "<64 hex>",   // sensitive
 *     "home_key_hex":       "<64 hex>",   // derived; sensitive
 *     "root_id_hex":        "<64 hex>",   // matches syncd convention: seed
 *     "d_tag":              "nostr-homed.home.v1:personal",
 *     "home_relays":        ["wss://..."],
 *     "blossom_servers":    ["https://..."],
 *     "generation":         0
 *   }
 *
 * Two fields are secret and MUST NOT leave the file (account_nsec_hex,
 * wrap_seed_hex, home_key_hex). The file is written with fchmod(0600)
 * and — when NOT dry-run — chowned to the caller's uid.
 * ──────────────────────────────────────────────────────────────────── */

#define NH_PROV_MAX_URLS      8u
#define NH_PROV_MAX_URL_LEN 512u

typedef struct {
    char     account_pubkey_hex[65];
    char     account_nsec_hex[65];
    char     wrap_seed_hex[65];
    char     home_key_hex[65];
    char     root_id_hex[65];
    char     d_tag[96];
    char     home_relays[NH_PROV_MAX_URLS][NH_PROV_MAX_URL_LEN + 1];
    size_t   n_home_relays;
    char     blossom_servers[NH_PROV_MAX_URLS][NH_PROV_MAX_URL_LEN + 1];
    size_t   n_blossom_servers;
    uint64_t generation;
} nh_prov_account;

/* Zero-init an account and set the design default d_tag. */
void nh_prov_account_init(nh_prov_account *a);

/* Serialize a (possibly redacted) account as canonical JSON. Returns a
 * heap NUL-terminated string on success (caller frees). When `redact`
 * is non-zero, secret fields are replaced with "REDACTED". */
char *nh_prov_account_to_json(const nh_prov_account *a, int redact);

/* Parse account JSON. Refuses missing/malformed keys and any extra
 * top-level fields (strict). Returns 0 on success. */
int nh_prov_account_from_json(const char *json, size_t len,
                              nh_prov_account *out);

/* Read a file into a heap buffer (caller frees). Bounded (cap enforced,
 * refuses on overflow with -E2BIG). Returns 0 on success. */
int nh_prov_slurp_file(const char *path, size_t cap,
                       char **out, size_t *out_len);

/* Emit a per-chunk plan for the tree rooted at `home_abs`, printing one
 * JSON line per file to `out` and a footer with totals. Used by
 * `push --dry-run`. Returns 0 on success. */
int nh_prov_push_dry_run(const char *home_abs, uint64_t chunk_size,
                         FILE *out, uint64_t *out_total_bytes,
                         uint64_t *out_total_files,
                         uint64_t *out_total_chunks);

/* Split a comma-separated list into an array of trimmed non-empty
 * strings. Returns a heap array (caller free()s both arr and its
 * strings) or NULL on empty input. */
char **nh_prov_split_csv(const char *s, size_t *out_n);
void   nh_prov_free_csv (char **arr, size_t n);

/* Serialize an nh_prov_account into an nh_porthome_fetch_ctl JSON
 * payload (the shape nostr-home-fetch expects on stdin). The returned
 * buffer is heap (caller frees). Sets `*out_len`. */
char *nh_prov_render_fetch_ctl(const nh_prov_account *a,
                               uint32_t relay_timeout_ms,
                               uint64_t bandwidth_cap_bytes,
                               uint32_t per_file_timeout_sec,
                               uint64_t max_total_bytes,
                               int allow_insecure,
                               size_t *out_len);


/* ────────────────────────────────────────────────────────────────────
 * Real push helpers (nh_provision_cli.c) — shared between the CLI and
 * unit tests so the transport-heavy code in nostr-homed-provision.c can
 * stay narrow.
 * ──────────────────────────────────────────────────────────────────── */

/* Normalise a caller-supplied --chunk-size against the design defaults.
 * 0 (or unset) → 4 MiB. Values < 64 KiB clamp up to 64 KiB. Values
 * > 8 MiB clamp down to 8 MiB. The design's canonical chunk size is
 * 4 MiB and this normalisation never changes it. */
uint64_t nh_prov_normalize_chunk_size(uint64_t requested);

/* Snapshot-walk output row. `symlink_target` is populated iff kind ==
 * SYMLINK; `chunks` and `n_chunks` stay 0 (populated by the CLI's
 * transport step, not here). Owned strings must be freed via
 * nh_prov_free_walk. */
typedef enum {
    NH_PROV_WALK_KIND_FILE    = 1,
    NH_PROV_WALK_KIND_DIR     = 2,
    NH_PROV_WALK_KIND_SYMLINK = 3,
} nh_prov_walk_kind;

typedef struct {
    char             *rel_path;       /* Owned. */
    nh_prov_walk_kind kind;
    uint32_t          mode;           /* & 0777 */
    uint32_t          uid_hint;
    uint32_t          gid_hint;
    uint64_t          mtime_ns;
    uint64_t          size;           /* file size in bytes; 0 for dir/symlink */
    char             *symlink_target; /* Owned; only for SYMLINK. */
} nh_prov_walk_entry;

/* Walk `home_abs` recursively, capping depth at 32, skipping the
 * standard state / cache directories (.local, .cache, .git). Entries
 * with world-writable bits (0002), setuid (04000) or setgid (02000)
 * are refused with a warning printed to `warn` (may be NULL) and
 * counted in `*out_skipped`. Absolute-only paths inside the tree are
 * refused. Symlink targets are recorded, not traversed.
 *
 * Returns 0 on success; *out_entries points at a heap-allocated array
 * of `*out_n` entries (caller frees via nh_prov_free_walk). On
 * failure, returns -errno and no allocations. */
int nh_prov_walk_home(const char *home_abs,
                      FILE *warn,
                      nh_prov_walk_entry **out_entries,
                      size_t *out_n,
                      size_t *out_skipped);

/* Release everything nh_prov_walk_home allocated. */
void nh_prov_free_walk(nh_prov_walk_entry *entries, size_t n);

/* min_replication gate.
 *
 * Given `per_server[si].chunks_uploaded` values from a completed
 * nh_porthome_blossom_upload_batch call, count servers that hold a
 * full replica of every one of `n_chunks` blobs in the batch (i.e.
 * chunks_uploaded[si] == n_chunks AND chunks_failed[si] == 0). Return
 * that count.  Callers refuse to advance the generation when the
 * returned value is < min_replication. */
size_t nh_prov_count_full_replicas(const uint32_t *chunks_uploaded,
                                   const uint32_t *chunks_failed,
                                   size_t n_servers,
                                   uint64_t n_chunks);

/* Atomic 0600 rewrite of an account file.  Uses the same rename(2)-
 * from-<path>.tmp.<pid> discipline as the status writer.  Returns 0
 * on success, -errno on failure. */
int nh_prov_account_write_file(const char *path, const nh_prov_account *a);

/* Write / update the local push-side generation ring at `path` (typ.
 * <home>/.local/state/nostr-homed/pinned.json).  Idempotent: if the
 * last slot is the same generation, it is replaced; otherwise the new
 * slot is appended and the ring is trimmed to the last N generations
 * (default 10).  Hashes are 64-char lowercase hex (Blossom addresses).
 *
 * The file uses the same schema syncd's nh_syncd_pin_ring writer
 * emits (schema:1, capacity, generations:[{gen, hashes}]).  A
 * subsequent syncd start-up over the same $HOME picks it up without
 * loss.  Non-hex64 entries in `hashes` are ignored (this matches
 * syncd's promote path).  Returns 0 on success, -errno on failure. */
int nh_prov_pinned_ring_promote(const char *path,
                                uint64_t generation,
                                const char *const *hashes,
                                size_t n_hashes,
                                size_t ring_capacity);

#ifdef __cplusplus
}
#endif

#endif /* NH_PROVISION_CLI_H */
