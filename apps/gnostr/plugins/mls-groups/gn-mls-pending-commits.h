/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-mls-pending-commits.h - Resolve our pending MLS Commits
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_MLS_PENDING_COMMITS_H
#define GN_MLS_PENDING_COMMITS_H

#include "gn-mls-event-router.h"

G_BEGIN_DECLS

/*
 * What happened to a group's pending Commit (libmarmot keeps a Commit
 * pending until a relay confirmed it; MIP-03):
 * @GN_MLS_COMMIT_NONE: nothing was pending (any unsent Welcomes were sent)
 * @GN_MLS_COMMIT_MERGED: a relay accepted it; applied; its Welcomes sent
 * @GN_MLS_COMMIT_SUPERSEDED: a competing member's Commit won; discarded
 * @GN_MLS_COMMIT_REJECTED: every relay refused it (NIP-01 OK false) or the
 *   group has no relays: certainly unpublished, so discarded
 * @GN_MLS_COMMIT_UNCERTAIN: no relay confirmed it but one may have stored it
 *   (timeout, disconnect, lost OK): kept pending and retried; its echo from
 *   a relay also merges it
 * @GN_MLS_COMMIT_FAILED: it could not be applied (see the error); kept
 *   pending unless libmarmot found it can never apply
 */
typedef enum {
  GN_MLS_COMMIT_NONE,
  GN_MLS_COMMIT_MERGED,
  GN_MLS_COMMIT_SUPERSEDED,
  GN_MLS_COMMIT_REJECTED,
  GN_MLS_COMMIT_UNCERTAIN,
  GN_MLS_COMMIT_FAILED,
} GnMlsCommitOutcome;

/*
 * Publish the group's pending Commit (the signed event libmarmot kept) to
 * the group relays until one accepts it, then merge it and send the Welcomes
 * of an Add (each marked sent once its own send succeeded); clear it only
 * when certainly unpublished.  Concurrent calls for one group share one
 * resolution.  An UNCERTAIN outcome schedules retries with exponential
 * backoff (until gn_mls_pending_commits_stop()).
 */
void               gn_mls_resolve_pending_commit_async(GnMlsEventRouter    *router,
                                                       const gchar         *mls_group_id_hex,
                                                       GAsyncReadyCallback  callback,
                                                       gpointer             user_data);
GnMlsCommitOutcome gn_mls_resolve_pending_commit_finish(GAsyncResult  *result,
                                                        GError       **error);

/*
 * Plugin activation: resolve every group's leftover pending Commit and
 * unsent Welcomes (a crash or a lost relay answer must not wedge a group),
 * and flush a group's Welcome outbox whenever it changes epoch (e.g. our
 * pending Add merged by its relay echo).  gn_mls_pending_commits_stop() on
 * deactivation cancels every retry timer.
 */
void               gn_mls_pending_commits_start(GnMlsEventRouter *router);
void               gn_mls_pending_commits_stop(void);

G_END_DECLS

#endif /* GN_MLS_PENDING_COMMITS_H */
