/* SPDX-License-Identifier: MIT
 *
 * nh_syncd_conflicts.h — conflict resolution inside nostr-home-syncd
 * (nostrc-h10m.6). Internal to src/porthome-syncd; not installed.
 *
 * Users (nostr-home-status CLI, the shell extension) never edit HOME to
 * resolve a conflict. They drop a request file into the inbox next to
 * porthome-status.json; syncd applies the choice with its own operations
 * (personal home: HOME edit + batcher -> pusher publishes a generation;
 * shared vault: nh_vault_engine_resolve publishes a head), then records the
 * outcome under status.syncd.resolutions[].
 *
 * Choices are expressed against the two versions syncd knows about:
 *   keep-current  the version at the original path stays (a saved copy or
 *                 a deletion record is discarded)
 *   use-saved     the saved version replaces it (for a deletion conflict:
 *                 the file is deleted)
 *   keep-both     the saved version becomes a sibling file next to it
 * Which of the two is "yours" is provenance (status field "mine").
 */
#ifndef NH_SYNCD_CONFLICTS_H
#define NH_SYNCD_CONFLICTS_H

#include "nh_syncd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NH_CONFLICT_KEEP_CURRENT = 1,
    NH_CONFLICT_USE_SAVED    = 2,
    NH_CONFLICT_KEEP_BOTH    = 3,
} nh_conflict_choice;

const char *nh_conflict_choice_slug(nh_conflict_choice c);
/* Accepts the slugs above. Returns 0 on success. */
int nh_conflict_choice_parse(const char *slug, nh_conflict_choice *out);

/* Resolution applied locally and waiting for publication (a positive
 * return from the resolvers below). */
#define NH_CONFLICT_QUEUED 1

/* Freshness token for the snapshot row at `path` (64 lowercase hex), or
 * "absent" when no row exists. A resolution request carries the token it
 * was shown; syncd refuses the request ("stale") when it no longer matches. */
void nh_syncd_state_version_token(const nh_syncd_state *s, const char *path, char out[65]);

/* "<dir>/<stem> (conflicted copy <label>)<ext>" beside `path`, numbered
 * " 2", " 3", ... until neither HOME nor the snapshot has it. The leaf
 * stays within NAME_MAX. Caller frees *out. Returns 0 on success. */
int nh_conflict_sibling_path(int home_fd, const nh_syncd_state *s, const char *path,
                             const char *label, char **out);

/* ── Personal-home conflict journal: <state_dir>/home-conflicts.json ── */

typedef enum {
    NH_HOME_CONFLICT_EDIT             = 1, /* both changed; loser in a sibling copy */
    NH_HOME_CONFLICT_DELETED_REMOTELY = 2, /* remote deleted, local edit kept       */
    NH_HOME_CONFLICT_DELETED_LOCALLY  = 3, /* local deleted, remote edit restored   */
} nh_home_conflict_kind;

typedef struct {
    const char *path;      /* HOME-relative original path                    */
    nh_home_conflict_kind kind;
    const char *saved;     /* EDIT: loser copy; deletions: marker file       */
    bool saved_is_local;   /* EDIT only: the copy holds this device's edit   */
} nh_home_conflict_record;

/* Append provenance rows (opaque 32-hex ids) atomically. */
int nh_syncd_home_conflicts_record(const char *state_dir, const nh_home_conflict_record *rows,
                                   size_t n, uint64_t generation, int64_t now);

/* Unresolved rows as a JSON array for nh_syncd_status_set_home_conflicts().
 * "current_present" is what HOME holds now (home_fd); with home_fd < 0 it
 * falls back to the snapshot, which lags untracked files. A missing journal
 * yields "[]". Caller frees *out. */
int nh_syncd_home_conflicts_project(const char *state_dir, int home_fd, const nh_syncd_state *s,
                                    char **out);

/* Apply a choice: HOME edits under home_fd plus batcher entries so the
 * normal pusher publishes them. Returns 0 when the row is resolved now (no
 * publication needed), NH_CONFLICT_QUEUED when it commits after the next
 * successful personal push, or a negative NH_SYNCD_ERR_* with *why set to
 * a status slug. Re-submitting the same request id is idempotent. */
int nh_syncd_home_conflicts_resolve(const char *state_dir, int home_fd, nh_syncd_state *s,
                                    nh_syncd_batcher *batcher, const char *id,
                                    nh_conflict_choice choice, const char *expect,
                                    const char *request, const char **why);

/* After a successful personal push: commit pending rows whose touched
 * paths the snapshot now reflects. `done` is called per committed row. */
typedef void (*nh_home_conflict_done_fn)(void *ud, const char *request, const char *id,
                                         const char *choice);
int nh_syncd_home_conflicts_commit_pending(const char *state_dir, const nh_syncd_state *s,
                                           nh_home_conflict_done_fn done, void *ud);

/* ── Request inbox: <status dir>/requests/<32 hex>.json ──
 *
 * {"schema":1,"op":"resolve-conflict","request":"<32 hex = file stem>",
 *  "conflict":"<32|64 hex>","choice":"<slug>","expect":"<token>"}
 *
 * Files must be regular, owned by the daemon's uid, at most 4 KiB and
 * strictly shaped; anything else is removed and reported invalid. */

typedef struct nh_syncd_requests nh_syncd_requests;

typedef struct {
    char request[33];
    char conflict[65];
    nh_conflict_choice choice;
    char expect[65];
} nh_syncd_request;

/* Default inbox directory (heap), derived from the status file path. */
char *nh_syncd_requests_default_dir(void);

int nh_syncd_requests_open(const char *dir, nh_syncd_requests **out);
/* inotify fd for poll(2); -1 if notifications are unavailable (callers
 * then rely on nh_syncd_requests_next() at startup and on rescans). */
int nh_syncd_requests_fd(const nh_syncd_requests *r);
/* Drain notifications and load the next request. Returns 1 with *out
 * filled, 0 when the inbox is empty, 2 when a malformed file was removed
 * (bad_request holds its id, possibly empty), <0 on I/O failure. */
int nh_syncd_requests_next(nh_syncd_requests *r, nh_syncd_request *out, char bad_request[33]);
/* Remove a handled request file. */
void nh_syncd_requests_done(nh_syncd_requests *r, const char *request);
void nh_syncd_requests_close(nh_syncd_requests *r);

#ifdef __cplusplus
}
#endif
#endif /* NH_SYNCD_CONFLICTS_H */
