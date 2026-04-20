/*
 * hanami-grasp.h - GRASP server compatibility layer
 *
 * SPDX-License-Identifier: MIT
 *
 * GRASP (Git Relay for Authenticated Server Pushes) servers combine:
 *   - A Nostr relay at ws(s)://<host>/
 *   - A Git Smart HTTP service at https://<host>/<npub>/<repo>.git
 *
 * Pushes are authorized by verifying the pusher's latest signed kind 30618
 * state event, so the state must be published BEFORE the git push.
 *
 * This module detects GRASP servers, coordinates the publish-then-push
 * workflow, and provides helpers for GRASP URL construction.
 */

#ifndef HANAMI_GRASP_H
#define HANAMI_GRASP_H

#include "hanami-types.h"
#include <git2/types.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct hanami_nostr_ctx hanami_nostr_ctx_t;

/* =========================================================================
 * GRASP server detection
 * ========================================================================= */

/**
 * hanami_grasp_info_t:
 *
 * Parsed GRASP server information extracted from clone/relay URLs.
 */
typedef struct {
    char *host;             /**< Server hostname (e.g. "relay.example.com") */
    char *npub;             /**< Owner npub in clone URL (bech32) */
    char *pubkey;           /**< Owner pubkey hex (decoded from npub) */
    char *repo_name;        /**< Repository name (without .git suffix) */
    char *clone_url;        /**< Full HTTPS clone URL */
    char *relay_url;        /**< WebSocket relay URL */
    bool uses_tls;          /**< Whether the server uses TLS */
} hanami_grasp_info_t;

/**
 * hanami_grasp_info_free:
 * @info: (nullable): info to free
 */
void hanami_grasp_info_free(hanami_grasp_info_t *info);

/**
 * hanami_is_grasp_server:
 * @clone_url: an HTTPS clone URL to check
 * @relay_urls: NULL-terminated array of relay URLs
 * @relay_count: number of relay URLs
 *
 * Detect whether a clone URL + relay URL pair represents a GRASP server.
 *
 * A GRASP server is detected when:
 *   1. clone_url matches pattern: http(s)://<host>/<npub>/<repo>.git
 *   2. A relay_url exists with the same host: ws(s)://<host> or ws(s)://<host>/
 *
 * Returns: true if the URL combination indicates a GRASP server
 */
bool hanami_is_grasp_server(const char *clone_url,
                            const char *const *relay_urls,
                            size_t relay_count);

/**
 * hanami_grasp_parse_clone_url:
 * @clone_url: HTTPS clone URL in GRASP format
 * @out: (out): parsed GRASP info (caller frees with hanami_grasp_info_free)
 *
 * Parse a GRASP clone URL into its components.
 * Expected format: http(s)://<host>/<npub>/<repo>.git
 *
 * Returns: HANAMI_OK on success, HANAMI_ERR_INVALID_ARG if not a valid GRASP URL
 */
hanami_error_t hanami_grasp_parse_clone_url(const char *clone_url,
                                             hanami_grasp_info_t **out);

/**
 * hanami_grasp_build_clone_url:
 * @host: server hostname
 * @npub: owner npub (bech32 encoded)
 * @repo_name: repository name (without .git)
 * @use_tls: whether to use https (true) or http (false)
 * @out: (out): allocated clone URL string (caller frees)
 *
 * Construct a GRASP clone URL from components.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_grasp_build_clone_url(const char *host,
                                             const char *npub,
                                             const char *repo_name,
                                             bool use_tls,
                                             char **out);

/**
 * hanami_grasp_build_relay_url:
 * @host: server hostname
 * @use_tls: whether to use wss (true) or ws (false)
 * @out: (out): allocated relay URL string (caller frees)
 *
 * Construct the WebSocket relay URL for a GRASP server.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_grasp_build_relay_url(const char *host,
                                             bool use_tls,
                                             char **out);

/* =========================================================================
 * GRASP push workflow
 * ========================================================================= */

/**
 * hanami_grasp_push_opts_t:
 *
 * Options for pushing to a GRASP server.
 */
typedef struct {
    const char *clone_url;              /**< GRASP clone URL */
    const char *const *relay_urls;      /**< NULL-terminated relay URLs */
    size_t relay_count;                 /**< Number of relay URLs */
    const hanami_signer_t *signer;      /**< Signer for 30618 state event */
    const char *repo_id;                /**< NIP-34 d-tag identifier */
    const char *remote_name;            /**< Remote name (default: "origin") */
    const char *const *refspecs;        /**< NULL-terminated refspecs to push */
    size_t refspec_count;               /**< Number of refspecs */
} hanami_grasp_push_opts_t;

/**
 * hanami_push_to_grasp:
 * @repo: repository to push from
 * @opts: push options
 *
 * Push to a GRASP server. This performs:
 *   1. Read current local refs matching refspecs
 *   2. Publish kind 30618 state event with desired ref state
 *   3. Push via standard Git Smart HTTP to the GRASP endpoint
 *
 * The 30618 event must be published BEFORE the push because GRASP
 * servers verify push authorization against the latest state event.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_push_to_grasp(git_repository *repo,
                                     const hanami_grasp_push_opts_t *opts);

/**
 * hanami_grasp_fetch:
 * @repo: (nullable): existing repo to fetch into, or NULL for info-only
 * @clone_url: GRASP clone URL
 * @relay_urls: NULL-terminated relay URLs for state lookup
 * @relay_count: number of relay URLs
 *
 * Fetch from a GRASP server using standard Git Smart HTTP.
 * This is essentially a normal git fetch since GRASP serves standard
 * Git protocol — but this function validates the URL format first.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_grasp_fetch(git_repository *repo,
                                   const char *clone_url,
                                   const char *const *relay_urls,
                                   size_t relay_count);

/* =========================================================================
 * Helpers for refs/nostr/ namespace
 * ========================================================================= */

/**
 * hanami_grasp_pr_refname:
 * @event_id: Nostr event ID of the patch/PR event
 * @out: (out): allocated refname string (caller frees)
 *
 * Build a refs/nostr/<event_id> refname for PR branches.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_grasp_pr_refname(const char *event_id, char **out);

#ifdef __cplusplus
}
#endif

#endif /* HANAMI_GRASP_H */
