/*
 * nh_porthome_provision.h — portable-home Phase 2: broker-side
 * provisioner. Materialize a decrypted manifest into an openat-relative
 * staging descriptor. Bead nostrc-89rj.
 *
 * SPDX-License-Identifier: MIT
 * EXPERIMENTAL. Gated behind NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL.
 *
 * Structural contract (design §D5): the caller invokes
 * nh_identity_home_prepare with an nh_identity_home_options.label
 * callback that resolves the staging directory descriptor. That
 * callback then calls nh_porthome_materialize_into_fd(...) with the
 * dirfd. Every write is done via openat/mkdirat/symlinkat relative to
 * that dirfd — RESOLVE_BENEATH/atomic install/crash recovery are
 * inherited from identity_home.c and NOT reimplemented here.
 *
 * Failure semantics (design §5.3 no-push interlock):
 *   Any fetch-side failure (relay unreachable, manifest decrypt fail,
 *   quorum shy) returns NH_PORTHOME_PROV_LIMITED. The caller MUST
 *   route this to LIMITED_MODE and MUST NOT clear an existing home.
 *   Only NH_PORTHOME_PROV_INVARIANT (crypto corruption etc.) is a
 *   hard fail that aborts the login.
 */

#ifndef NH_PORTHOME_PROVISION_H
#define NH_PORTHOME_PROVISION_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "nh_porthome_crypto.h"
#include "nh_porthome_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NH_PORTHOME_PROV_OK             =  0,
    NH_PORTHOME_PROV_ARG            = -1,
    /* Recoverable: relay/Blossom unreachable, decode/decrypt bounded
     * failure, or a per-file cap tripped. The provisioner did NOT
     * modify any existing local home. Route to LIMITED_MODE. */
    NH_PORTHOME_PROV_LIMITED        = -2,
    /* Hard: crypto tag mismatch, per-file invariant broken (path
     * escape, mode injection, decompression bomb). Fail the session. */
    NH_PORTHOME_PROV_INVARIANT      = -3,
    NH_PORTHOME_PROV_OOM            = -4,
    NH_PORTHOME_PROV_IO             = -5,
    NH_PORTHOME_PROV_BUDGET         = -6, /* per-file / total budget exceeded */
} nh_porthome_prov_status;

/* Per-transaction budget/policy (design §5.4 subset — Phase 2). */
typedef struct nh_porthome_prov_opts {
    uid_t   local_uid;              /* chown()d to this uid on apply */
    gid_t   local_gid;              /* chown()d to this gid on apply */
    size_t  max_bytes_per_load;     /* per-file cap; 0 -> 2 GiB default */
    unsigned load_timeout_sec;      /* per-file wall-clock cap; 0 -> 120 s */
    size_t  max_total_bytes;        /* per-home cap; 0 -> 20 GiB */
    size_t  max_entries;            /* fatal above; 0 -> NH_PORTHOME_MAX_ENTRIES */
    /* Progress artifact: absolute path written 0640 nostr-auth-greeter
     * (when supplied). NULL disables. */
    const char *progress_path;
} nh_porthome_prov_opts;

/* Sink for fetching sealed chunks. Real callers wire this to a
 * nh_porthome_blossom_t; tests can pass an in-memory map. Returns 0
 * with a heap buffer (caller frees with free()) on success; non-zero
 * means "not available" and the provisioner returns LIMITED. */
typedef int (*nh_porthome_prov_fetch_fn)(void *ctx, const char *sha256_hex,
                                         uint8_t **out_ct, size_t *out_ct_len);

/* Materialize `m` into `staging_fd`, decrypting chunks via `fetch`.
 * `home_key` is the derived 32-byte home key (see nh_porthome_wrapkey.h
 * + nh_porthome_key_derive).
 *
 * The manifest's path_enc components are treated as OPAQUE path names
 * (the entire point of §2.3 name encryption). We validate every name
 * against the same rules identity_home.c's `beneath()` enforces:
 * no '/', no NUL, ≤ 255 bytes, no ".", no "..", no leading '/'.
 *
 * Directories, files (chunk-reassembled), and symlinks are supported.
 * Mode is masked to 0777, sticky/setuid/setgid stripped unconditionally.
 * uid_hint/gid_hint are IGNORED; every write is chown'd to opts->local_uid
 * / opts->local_gid on apply (design §D17).
 *
 * Progress artifact JSON (if opts->progress_path):
 *   {"bytes":N,"total_bytes":T,"files":F,"total_files":TF,"state":"...","tx":"..."}
 *
 * Returns one of nh_porthome_prov_status. On LIMITED / INVARIANT the
 * staging_fd may hold partially-created files; the identity layer
 * takes care of the ambiguity/repair path — this function's job is
 * bounded, atomic writes only. */
nh_porthome_prov_status nh_porthome_materialize_into_fd(
    int staging_fd,
    const nh_porthome_manifest *m,
    const uint8_t home_key[NH_PORTHOME_KEY_LEN],
    nh_porthome_prov_fetch_fn fetch, void *fetch_ctx,
    const nh_porthome_prov_opts *opts,
    const char *tx_id_or_null);

/* Convenience wrapper: takes the *sealed* manifest bytes (as read from
 * relay content), decodes+unwraps under home_key, and dispatches to
 * nh_porthome_materialize_into_fd. Returns LIMITED on parse/AEAD
 * failure (never INVARIANT — a hostile relay is a data-source
 * problem, not a broken invariant in our code). */
nh_porthome_prov_status nh_porthome_materialize_sealed_into_fd(
    int staging_fd,
    const uint8_t *sealed_manifest, size_t sealed_len,
    const uint8_t home_key[NH_PORTHOME_KEY_LEN],
    nh_porthome_prov_fetch_fn fetch, void *fetch_ctx,
    const nh_porthome_prov_opts *opts,
    const char *tx_id_or_null);

#ifdef __cplusplus
}
#endif

#endif /* NH_PORTHOME_PROVISION_H */
