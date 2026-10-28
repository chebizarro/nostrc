/*
 * nh_porthome_conflicts.h — reader side of portable-home conflicts
 * (nostrc-h10m.6).
 *
 * SPDX-License-Identifier: MIT
 *
 * nostr-home-syncd publishes unresolved conflicts in porthome-status.json:
 *   syncd.home_conflicts[]   personal home (sibling .conflict-* copies,
 *                            deletion markers)
 *   syncd.vault_conflicts[]  shared vaults (Portable/.conflicts/<path>@<writer>,
 *                            .overflow/<sha256>, deletion-only rows)
 * and the outcome of each resolution request in syncd.resolutions[].
 * This module normalises both arrays for the CLI, writes resolution
 * requests into syncd's inbox and reads their results back. It never edits
 * HOME: syncd applies the choice and publishes it.
 *
 * Choices (bit flags so a row can advertise the set syncd accepts):
 *   KEEP_CURRENT  keep what is at the original path now
 *   USE_SAVED     replace it with the saved version (a deletion conflict:
 *                 delete the file)
 *   KEEP_BOTH     add the saved version beside it as a copy
 * "keep mine" / "take theirs" map onto these through the row's `mine`
 * provenance, and are refused when provenance is unknown.
 */
#ifndef NH_PORTHOME_CONFLICTS_H
#define NH_PORTHOME_CONFLICTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NH_CONFLICT_CHOICE_KEEP_CURRENT 1u
#define NH_CONFLICT_CHOICE_USE_SAVED    2u
#define NH_CONFLICT_CHOICE_KEEP_BOTH    4u

typedef enum {
    NH_CONFLICT_SOURCE_HOME  = 1,
    NH_CONFLICT_SOURCE_VAULT = 2,
} nh_conflict_source;

typedef enum {
    NH_CONFLICT_MINE_UNKNOWN = 0,
    NH_CONFLICT_MINE_CURRENT = 1,
    NH_CONFLICT_MINE_SAVED   = 2,
} nh_conflict_mine;

typedef struct {
    char id[65];               /* 32 hex (home) or 64 hex (vault)          */
    nh_conflict_source source;
    char *path;                /* HOME-relative original path              */
    char *saved_path;          /* HOME-relative saved copy, or NULL        */
    char *marker;              /* personal deletion marker, or NULL        */
    char *kind;                /* home: edit|deleted-remotely|deleted-locally */
    char *writer;              /* vault: writer pubkey hex, else NULL      */
    char *vault_id;            /* vault: id hex, else NULL                 */
    char *expect;              /* freshness token to send back             */
    bool saved_present;        /* the other side has content (not a delete) */
    bool current_present;      /* something is at `path` now               */
    bool overflow;             /* saved copy uses .overflow/<sha256>       */
    bool whole_folder;         /* vault stale-local whole-subtree copy     */
    bool pending;              /* a resolution is applied, not yet shared  */
    nh_conflict_mine mine;
    unsigned choices;          /* NH_CONFLICT_CHOICE_* syncd accepts       */
    int64_t detected_at;       /* 0 when unknown                           */
} nh_porthome_conflict;

typedef struct {
    nh_porthome_conflict *items;
    size_t n;
} nh_porthome_conflict_list;

/* Parse the whole status document. Missing arrays are an empty list.
 * Malformed rows are skipped (they cannot be resolved). Returns 0, or
 * -EINVAL when the document is not a JSON object, -ENOMEM. */
int nh_porthome_conflicts_parse(const char *status_json, size_t len,
                                nh_porthome_conflict_list *out);
void nh_porthome_conflicts_clear(nh_porthome_conflict_list *list);

/* Unique lookup by id or id prefix (at least 6 hex characters).
 * Returns the index, -ENOENT when nothing matches, -EEXIST when the
 * prefix is ambiguous, -EINVAL for a malformed prefix. */
int nh_porthome_conflicts_find(const nh_porthome_conflict_list *list, const char *prefix);

/* Wire slug ("keep-current", ...) for one NH_CONFLICT_CHOICE_* flag. */
const char *nh_porthome_conflict_choice_slug(unsigned choice);

/* Command-line flag -> choice. "--keep-mine" / "--take-theirs" resolve
 * through c->mine; returns -ENOENT when provenance is unknown, -EINVAL
 * for an unknown flag. *alias is set when the flag was an alias. */
int nh_porthome_conflict_choice_from_flag(const nh_porthome_conflict *c, const char *flag,
                                          unsigned *choice, bool *alias);

/* Plain-language button/option label for `choice` on this conflict. */
const char *nh_porthome_conflict_choice_label(const nh_porthome_conflict *c, unsigned choice);

/* One-paragraph plain-language explanation (no trailing newline).
 * Returns the length written or -1 when it does not fit. */
int nh_porthome_conflict_describe(const nh_porthome_conflict *c, char *buf, size_t cap);

/* Newest syncd.resolutions[] entry for `request`. Returns 0 when found
 * (result is "pending", "ok" or "error"; reason may be empty), -ENOENT. */
int nh_porthome_resolution_lookup(const char *status_json, size_t len, const char *request,
                                  char result[16], char reason[32]);

/* Plain-language explanation for a syncd error/pending reason slug. */
const char *nh_porthome_resolution_reason_text(const char *reason);

/* Inbox directory next to the status file (heap), or NULL. */
char *nh_porthome_requests_default_dir(void);

/* Atomically drop a resolution request into `dir` (created 0700 when
 * missing). Writes the new request id into request_out. Returns 0 or
 * -errno. */
int nh_porthome_request_write(const char *dir, const char *conflict_id, unsigned choice,
                              const char *expect, char request_out[33]);

#ifdef __cplusplus
}
#endif
#endif /* NH_PORTHOME_CONFLICTS_H */
