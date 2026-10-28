/*
 * hanami.h - Public API for libhanami
 *
 * SPDX-License-Identifier: MIT
 *
 * libhanami: Blossom filesystem extension & tools for libgit2
 *
 * Provides custom libgit2 backends enabling Git repositories to use
 * Blossom servers as a decentralized, content-addressed object store
 * and Nostr events (NIP-34) as the coordination/ref layer.
 */

#ifndef HANAMI_H
#define HANAMI_H

#include "hanami-export.h"

#include "hanami-types.h"
#include <git2.h>
#include <porthome/crypto.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Library lifecycle
 * ========================================================================= */

/**
 * hanami_init:
 *
 * Initialize libhanami. Call after git_libgit2_init().
 * Safe to call multiple times (reference counted).
 *
 * Returns: HANAMI_OK on success
 */
HANAMI_API hanami_error_t hanami_init(void);

/**
 * hanami_shutdown:
 *
 * Shut down libhanami. Decrements reference count; resources are freed
 * when count reaches zero. Call before git_libgit2_shutdown().
 */
HANAMI_API void hanami_shutdown(void);

/**
 * hanami_version:
 * @major: (out) (optional): major version
 * @minor: (out) (optional): minor version
 * @patch: (out) (optional): patch version
 *
 * Returns: version string (e.g. "0.1.0")
 */
HANAMI_API const char *hanami_version(int *major, int *minor, int *patch);

/* hanami_config — see hanami/hanami-config.h for the full API */

/* =========================================================================
 * Transport registration
 * ========================================================================= */

/* hanami_transport_register — see hanami/hanami-transport.h for the full API */

/* =========================================================================
 * ODB backend
 * ========================================================================= */

/* hanami_odb_backend_new — see hanami/hanami-odb-backend.h for the full API */

/* hanami_refdb_backend_new — see hanami/hanami-refdb-backend.h for the full API */

/* =========================================================================
 * High-level operations
 * ========================================================================= */

/**
 * hanami_repo_open:
 * @out: (out): the opened git_repository
 * @endpoint: Blossom server URL
 * @relay_urls: NULL-terminated array of Nostr relay URLs
 * @repo_id: repository identifier
 * @owner_pubkey: repository owner's hex public key
 * @signer: (nullable): signer for write operations
 * @config: (nullable): configuration (uses defaults if NULL)
 * @vault: (nullable): root ring; copied, latest epoch used for writes
 *
 * With a ring, repo_id MUST be the 64-hex random vault_id. Opens the owner's
 * signed head (or this authorized writer's head when present), sealed ODB and
 * private refdb. Missing/unreadable history fails closed; only the owner may
 * initialize an empty vault. No public 30618 is fetched or published.
 * Repositories returned here are temporary bare repositories; the caller owns
 * removal of git_repository_path() after freeing the repository.
 *
 * Open or create a repository backed by Blossom ODB + Nostr RefDB.
 *
 * Returns: HANAMI_OK on success
 */
HANAMI_API hanami_error_t hanami_repo_open(git_repository **out,
                                const char *endpoint,
                                const char **relay_urls,
                                const char *repo_id,
                                const char *owner_pubkey,
                                const hanami_signer_t *signer,
                                const hanami_config_t *config,
                                const porthome_ring_t *vault);

/* Clone a private vault using libgit2's clone machinery and a per-operation
 * transport, with a sealed remote ODB attached to the destination. No global
 * transport credentials, public discovery or 30618. Local working files/refs
 * are plaintext, as for any Git checkout. Use repo_sync to publish local refs.
 * repo_id is the random 64-hex vault_id, not a key-derived label. */
HANAMI_API hanami_error_t hanami_clone_sealed(git_repository **out, const char *local_path,
    const char *endpoint, const char **relay_urls, const char *repo_id,
    const char *owner_pubkey, const hanami_signer_t *signer,
    const porthome_ring_t *vault, const hanami_config_t *config);

/* Seal/upload every local object before publishing the private snapshot.
 * Refdb writes on repo_open repositories already do this per ref mutation. */
HANAMI_API hanami_error_t hanami_repo_sync(git_repository *repo);

/* Import an authorized writer's snapshot as refs/remotes/<writer>/<branch>.
 * Does not advance local branches or fold conflicts. Use libgit2 merge APIs,
 * then sync the chosen/merged refs. Policy comes from the owner's signed root,
 * never from the writer's own asserted membership. */
HANAMI_API hanami_error_t hanami_repo_fetch_writer(git_repository *repo, const char *writer_pubkey);

/* Explicit irreversible disclosure: mirror plaintext objects and ACK a public
 * 30618 projection, then mark this writer's private snapshot lifted. Future
 * public Blossom/GRASP pushes are allowed; sealing remains the source of truth.
 * A failed lift may already have disclosed objects; failure never authorizes
 * public pushes. No API restores privacy. */
HANAMI_API hanami_error_t hanami_repo_lift_privacy(git_repository *repo);

/**
 * hanami_clone:
 * @out: (out): the cloned git_repository
 * @nostr_uri: nostr:// URI (e.g. "nostr://npub1.../repo-name")
 * @local_path: local directory to clone into
 * @signer: (nullable): signer for authenticated fetch
 * @config: (nullable): configuration (uses defaults if NULL)
 *
 * Clone a repository discovered via Nostr events (kind 30617 on the
 * configured relays, else public defaults). The Blossom server comes from
 * the config endpoint or the owner's BUD-03 kind-10063 list and must pass
 * libblossom's capability probe; NIP-34 `clone` URLs are Git remotes and are
 * never used as Blossom APIs.
 *
 * The kind-10063 and kind-30618 reads cover the discovery relays, the
 * announcement's relays and up to 4 of the owner's NIP-65 (kind-10002) write
 * relays, in that order; a missing kind-10002 adds no relays. The configured
 * endpoint still takes precedence over the kind-10063 list.
 *
 * The clone is an ordinary self-contained repository: every object reachable
 * from the owner's kind-30618 refs is located through the hanami manifest
 * chain, verified against its Git OID and written to the local ODB; refs are
 * written under remote "origin" (URL = @nostr_uri) and HEAD is checked out.
 * `hanami.endpoint`, `hanami.relays` and the .git/hanami/index.sqlite cache
 * are recorded for hanami_fetch(). No Blossom/Nostr backends stay attached.
 *
 * Returns: HANAMI_OK on success; HANAMI_ERR_CONFIG when no Blossom server is
 * known, HANAMI_ERR_BLOSSOM when no candidate is a Blossom server or an
 * object cannot be fetched/verified, HANAMI_ERR_NOT_FOUND when the 30618 has
 * no hanami-manifest tag, HANAMI_ERR_INDEX for an invalid or incomplete
 * manifest chain, HANAMI_ERR_LIBGIT2 for clone/checkout failures. A relay
 * query (30617, kind-10063, kind-30618) where no relay returned the event
 * concludes absence only if every relay sent EOSE. Otherwise it fails with
 * HANAMI_ERR_AUTH for an "auth-required:" (after one NIP-42 attempt when
 * @signer is set) or "restricted:" CLOSED, else HANAMI_ERR_NOSTR for
 * another CLOSED, else HANAMI_ERR_NETWORK when a relay was unreachable, or
 * HANAMI_ERR_TIMEOUT. A refusing or unreachable relay does not fail the
 * clone while another relay returns the event. Nothing is left at
 * @local_path on failure; git_error_last() names the cause.
 */
HANAMI_API hanami_error_t hanami_clone(git_repository **out,
                            const char *nostr_uri,
                            const char *local_path,
                            const hanami_signer_t *signer,
                            const hanami_config_t *config);

/**
 * hanami_fetch:
 * @repo: repository created by hanami_clone()
 * @remote_name: (nullable): remote with a nostr:// URL; NULL means "origin"
 * @signer: (nullable): signer for authenticated relay access
 * @config: (nullable): overrides for endpoint/relays/allow_insecure; otherwise
 *   the repository's `hanami.endpoint` and `hanami.relays` are used
 *
 * Re-read the owner's kind-30618, import only the manifest segments newer
 * than the cached head, download (and verify) only objects missing locally,
 * and update refs/remotes/<remote>/ via libgit2. Local branches are not
 * moved. Private (sealed) repositories are refused with HANAMI_ERR_AUTH; use
 * hanami_repo_fetch_writer() for those.
 *
 * Returns: HANAMI_OK on success (also when every relay sent EOSE without a
 * 30618); error codes as for hanami_clone(), including HANAMI_ERR_NETWORK
 * when a relay was unreachable and no relay returned the 30618, with
 * git_error_last() naming the cause.
 */
HANAMI_API hanami_error_t hanami_fetch(git_repository *repo,
                            const char *remote_name,
                            const hanami_signer_t *signer,
                            const hanami_config_t *config);

/**
 * hanami_push_to_blossom:
 * @repo: the repository to push from
 * @endpoint: Blossom server URL
 * @signer: signer for upload authorization (required)
 * @relay_urls: NULL-terminated array of relay URLs for state publishing
 * @repo_id: repository identifier for NIP-34 state event
 *
 * Publish in order (docs/libhanami-design.md 4.4b):
 * 1. every local object, uploaded unless the server already has it and
 *    recorded in <gitdir>/hanami/index.sqlite;
 * 2. a manifest segment with the rows the published chain does not cover
 *    yet. It is a full checkpoint on the first push, after a tagless 30618,
 *    or once the chain has 16 segments. The writer first catches up to the
 *    signer's published 30618 (a base check, not compare-and-swap);
 * 3. kind-30618 with local refs (refs/remotes/ excluded), HEAD and
 *    ["hanami-manifest", <segment>], through the relay-accepted publish.
 * Success requires every upload, the segment upload and at least one relay
 * OK true; refs pointing at objects that were not uploaded are refused.
 * Private (sealed) repositories are refused with HANAMI_ERR_AUTH.
 *
 * Returns: HANAMI_OK on success; HANAMI_ERR_UPLOAD/HANAMI_ERR_BLOSSOM for
 * object or segment uploads, relay errors (HANAMI_ERR_NOSTR, _AUTH,
 * _NETWORK, _TIMEOUT) for the 30618, HANAMI_ERR_INDEX for an uncoverable
 * state or row limits; git_error_last() names the cause.
 */
HANAMI_API hanami_error_t hanami_push_to_blossom(git_repository *repo,
                                      const char *endpoint,
                                      const hanami_signer_t *signer,
                                      const char **relay_urls,
                                      const char *repo_id);

/* hanami_push_to_blossom with configuration. Only allow_insecure is
 * consulted: the explicit opt-in for http:// or loopback Blossom servers
 * (local fixtures). No URL weakens transport guards automatically. */
HANAMI_API hanami_error_t hanami_push_to_blossom_ex(git_repository *repo,
                                      const char *endpoint,
                                      const hanami_signer_t *signer,
                                      const char **relay_urls,
                                      const char *repo_id,
                                      const hanami_config_t *config);

/**
 * hanami_announce_repo:
 * @repo_id: repository identifier (d-tag)
 * @name: human-readable name
 * @description: brief description
 * @clone_urls: NULL-terminated array of clone URLs
 * @relay_urls: NULL-terminated array of relay URLs
 * @signer: signer for the announcement event (required)
 *
 * Publish a NIP-34 repository announcement (kind 30617).
 *
 * Returns: HANAMI_OK on success
 */
HANAMI_API hanami_error_t hanami_announce_repo(const char *repo_id,
                                    const char *name,
                                    const char *description,
                                    const char **clone_urls,
                                    const char **relay_urls,
                                    const hanami_signer_t *signer);

/**
 * hanami_publish_state:
 * @repo_id: repository identifier (d-tag)
 * @refs: array of ref entries (refname → target)
 * @ref_count: number of entries
 * @relay_urls: NULL-terminated array of relay URLs
 * @signer: signer for the state event (required)
 *
 * Publish a NIP-34 repository state announcement (kind 30618) without
 * uploading objects. The signer's currently published ["hanami-manifest", …]
 * head is carried forward, so cold readers can still locate objects; they
 * reject refs that chain does not cover. Use hanami_push_to_blossom() to
 * publish new objects. Requires relay OK true.
 *
 * Returns: HANAMI_OK on success; the error of the published-state query or
 * of the publication otherwise
 */
HANAMI_API hanami_error_t hanami_publish_state(const char *repo_id,
                                    const hanami_ref_entry_t *refs,
                                    size_t ref_count,
                                    const char **relay_urls,
                                    const hanami_signer_t *signer);

#ifdef __cplusplus
}
#endif

#endif /* HANAMI_H */
