/**
 * @file nip34.h
 * @brief NIP-34: Git Stuff — Nostr events for Git collaboration
 *
 * Event kinds:
 *   30617 — Repository announcement (addressable, d-tag = repo ID)
 *   30618 — Repository state (addressable, d-tag = repo ID, ref→OID tags)
 *   1617  — Patch (regular)
 *   1618  — Pull request (regular) [placeholder]
 *   1621  — Issue (regular) [placeholder]
 *   1630–1633 — Status events (open, applied/merged, closed, draft)
 *
 * @see https://github.com/nostr-protocol/nips/blob/master/34.md
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NOSTR_NIP34_H
#define NOSTR_NIP34_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations from libnostr */
struct _NostrEvent;
typedef struct _NostrEvent NostrEvent;

/* =========================================================================
 * Event kinds
 * ========================================================================= */

#define NIP34_KIND_REPOSITORY       30617
#define NIP34_KIND_REPOSITORY_STATE 30618
#define NIP34_KIND_PATCH            1617
#define NIP34_KIND_PULL_REQUEST     1618
#define NIP34_KIND_PR_UPDATE        1619
#define NIP34_KIND_ISSUE            1621
#define NIP34_KIND_ISSUE_REPLY      1622
#define NIP34_KIND_STATUS_OPEN      1630
#define NIP34_KIND_STATUS_APPLIED   1631
#define NIP34_KIND_STATUS_CLOSED    1632
#define NIP34_KIND_STATUS_DRAFT     1633

/* =========================================================================
 * Result codes
 * ========================================================================= */

typedef enum {
    NIP34_OK = 0,
    NIP34_ERR_NULL_PARAM   = -1,
    NIP34_ERR_ALLOC        = -2,
    NIP34_ERR_INVALID_KIND = -3,
    NIP34_ERR_MISSING_TAG  = -4,
    NIP34_ERR_PARSE        = -5,
} nip34_result_t;

/* =========================================================================
 * Repository (kind 30617) — parsing and creation
 * ========================================================================= */

/**
 * nip34_repository_t:
 * Parsed representation of a kind 30617 repository announcement.
 */
typedef struct {
    char *id;               /**< d-tag identifier */
    char *name;             /**< Repository name */
    char *description;      /**< Repository description */
    char **web;             /**< Web URLs (NULL-terminated) */
    size_t web_count;
    char **clone;           /**< Clone URLs (NULL-terminated) */
    size_t clone_count;
    char **relays;          /**< Preferred relays (NULL-terminated) */
    size_t relay_count;
    char **maintainers;     /**< Maintainer pubkeys (NULL-terminated) */
    size_t maintainer_count;
    char *earliest_unique_commit;  /**< Earliest unique commit (r tag) */
} nip34_repository_t;

/**
 * nip34_create_repo_announcement:
 * Create a kind 30617 repository announcement event.
 *
 * @param repo_id    d-tag identifier (required)
 * @param name       repository name (required)
 * @param desc       description (nullable)
 * @param clone_urls NULL-terminated array of clone URLs (nullable)
 * @param web_urls   NULL-terminated array of web URLs (nullable)
 * @param relay_urls NULL-terminated array of relay URLs (nullable)
 * @param maintainers NULL-terminated array of pubkey hex strings (nullable)
 *
 * Returns: newly allocated unsigned NostrEvent, or NULL on error.
 *          Caller must free with nostr_event_free().
 */
NostrEvent *nip34_create_repo_announcement(const char *repo_id,
                                           const char *name,
                                           const char *desc,
                                           const char *const *clone_urls,
                                           const char *const *web_urls,
                                           const char *const *relay_urls,
                                           const char *const *maintainers);

/**
 * nip34_parse_repository:
 * Parse a kind 30617 event into a repository struct.
 *
 * @param event  the event to parse
 * @param out    output repository (caller frees with nip34_repository_free)
 *
 * Returns: NIP34_OK on success
 */
nip34_result_t nip34_parse_repository(const NostrEvent *event,
                                      nip34_repository_t **out);

void nip34_repository_free(nip34_repository_t *repo);

/* =========================================================================
 * Repository State (kind 30618) — parsing and creation
 * ========================================================================= */

/**
 * nip34_ref_t:
 * A single ref → target mapping.
 */
typedef struct {
    char *refname;  /**< e.g. "refs/heads/main" or "HEAD" */
    char *target;   /**< Hex OID, or "ref: refs/heads/main" for symbolic */
} nip34_ref_t;

/**
 * nip34_repo_state_t:
 * Parsed representation of a kind 30618 repository state event.
 */
typedef struct {
    char *repo_id;          /**< d-tag identifier */
    nip34_ref_t *refs;      /**< Array of ref entries */
    size_t ref_count;
    char *head;             /**< HEAD target (e.g. "ref: refs/heads/main") */
} nip34_repo_state_t;

/**
 * nip34_create_repo_state:
 * Create a kind 30618 repository state event.
 *
 * @param repo_id   d-tag identifier (required)
 * @param refs      array of ref entries
 * @param ref_count number of refs
 * @param head      HEAD value (nullable; e.g. "ref: refs/heads/main")
 *
 * Returns: newly allocated unsigned NostrEvent, or NULL on error.
 */
NostrEvent *nip34_create_repo_state(const char *repo_id,
                                    const nip34_ref_t *refs,
                                    size_t ref_count,
                                    const char *head);

/**
 * nip34_parse_repo_state:
 * Parse a kind 30618 event into a repo state struct.
 *
 * Returns: NIP34_OK on success
 */
nip34_result_t nip34_parse_repo_state(const NostrEvent *event,
                                      nip34_repo_state_t **out);

void nip34_repo_state_free(nip34_repo_state_t *state);

/* =========================================================================
 * Patch (kind 1617)
 * ========================================================================= */

/**
 * nip34_patch_t:
 * Parsed representation of a kind 1617 patch event.
 */
typedef struct {
    char *repo_addr;    /**< "a" tag: kind:pubkey:d-tag */
    char *commit_id;    /**< Commit hash */
    char *parent_id;    /**< Parent commit hash (nullable) */
    char *subject;      /**< Patch subject/title */
    char *content;      /**< Full patch content (diff) */
    bool is_root;       /**< True if this is the root/cover letter */
} nip34_patch_t;

/**
 * nip34_create_patch:
 * Create a kind 1617 patch event.
 *
 * @param repo_addr  "a" tag value: "30617:<pubkey>:<repo_id>" (required)
 * @param content    patch content — diff or cover letter (required)
 * @param commit_id  commit hash (nullable)
 * @param parent_id  parent commit hash (nullable)
 * @param subject    patch subject line (nullable — first line of content used)
 *
 * Returns: newly allocated unsigned NostrEvent, or NULL on error.
 */
NostrEvent *nip34_create_patch(const char *repo_addr,
                               const char *content,
                               const char *commit_id,
                               const char *parent_id,
                               const char *subject);

nip34_result_t nip34_parse_patch(const NostrEvent *event,
                                 nip34_patch_t **out);

void nip34_patch_free(nip34_patch_t *patch);

/* =========================================================================
 * Status events (kinds 1630–1633)
 * ========================================================================= */

typedef enum {
    NIP34_STATUS_OPEN    = 1630,
    NIP34_STATUS_APPLIED = 1631,
    NIP34_STATUS_CLOSED  = 1632,
    NIP34_STATUS_DRAFT   = 1633,
} nip34_status_kind_t;

/**
 * nip34_create_status:
 * Create a status event (kind 1630–1633) targeting a patch or PR.
 *
 * @param target_event_id  event ID of the patch/PR being annotated (required)
 * @param status           status kind (1630–1633)
 * @param content          optional status message (nullable)
 *
 * Returns: newly allocated unsigned NostrEvent, or NULL on error.
 */
NostrEvent *nip34_create_status(const char *target_event_id,
                                nip34_status_kind_t status,
                                const char *content);

/* =========================================================================
 * Helpers
 * ========================================================================= */

const char *nip34_strerror(nip34_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP34_H */
