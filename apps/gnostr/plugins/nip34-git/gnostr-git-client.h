/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-git-client.h - Local Git Repository Client
 *
 * Provides a GTK widget for interacting with local git repositories.
 * Requires libgit2 for git operations.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GNOSTR_GIT_CLIENT_H
#define GNOSTR_GIT_CLIENT_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_GIT_CLIENT (gnostr_git_client_get_type())

G_DECLARE_FINAL_TYPE(GnostrGitClient, gnostr_git_client, GNOSTR, GIT_CLIENT, GtkWidget)

/**
 * Signals:
 * "repo-opened" (gchar *path, gpointer user_data)
 *   - Emitted when a repository is successfully opened/cloned
 * "repo-closed" (gpointer user_data)
 *   - Emitted when the repository is closed
 * "commit-created" (gchar *commit_id, gpointer user_data)
 *   - Emitted when a new commit is created
 * "error" (gchar *message, gpointer user_data)
 *   - Emitted on error
 */

/**
 * gnostr_git_client_new:
 *
 * Creates a new git client widget.
 *
 * Returns: (transfer full): A new git client widget
 */
GnostrGitClient *gnostr_git_client_new(void);

/**
 * gnostr_git_client_clone:
 * @self: The git client
 * @url: Repository URL to clone
 * @path: Local path to clone to
 *
 * Clones a remote repository to the specified path.
 * Operation runs asynchronously; result is signaled via "repo-opened" or "error".
 */
void gnostr_git_client_clone(GnostrGitClient *self,
                              const char      *url,
                              const char      *path);

/**
 * gnostr_git_client_open:
 * @self: The git client
 * @path: Path to local repository
 *
 * Opens an existing local repository.
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean gnostr_git_client_open(GnostrGitClient *self, const char *path);

/**
 * gnostr_git_client_close:
 * @self: The git client
 *
 * Closes the currently open repository.
 */
void gnostr_git_client_close(GnostrGitClient *self);

/**
 * gnostr_git_client_get_path:
 * @self: The git client
 *
 * Returns: (transfer none) (nullable): Path of open repository, or NULL
 */
const char *gnostr_git_client_get_path(GnostrGitClient *self);

/**
 * gnostr_git_client_is_open:
 * @self: The git client
 *
 * Returns: TRUE if a repository is currently open
 */
gboolean gnostr_git_client_is_open(GnostrGitClient *self);

/**
 * gnostr_git_client_refresh:
 * @self: The git client
 *
 * Refreshes the repository state (status, commits, branches).
 */
void gnostr_git_client_refresh(GnostrGitClient *self);

/**
 * gnostr_git_client_stage_file:
 * @self: The git client
 * @path: File path relative to repo root
 *
 * Stages a file for commit.
 *
 * Returns: TRUE on success
 */
gboolean gnostr_git_client_stage_file(GnostrGitClient *self, const char *path);

/**
 * gnostr_git_client_unstage_file:
 * @self: The git client
 * @path: File path relative to repo root
 *
 * Unstages a file.
 *
 * Returns: TRUE on success
 */
gboolean gnostr_git_client_unstage_file(GnostrGitClient *self, const char *path);

/**
 * gnostr_git_client_commit:
 * @self: The git client
 * @message: Commit message
 *
 * Creates a new commit with staged changes.
 *
 * Returns: (transfer full) (nullable): Commit ID on success, NULL on error
 */
char *gnostr_git_client_commit(GnostrGitClient *self, const char *message);

G_END_DECLS

#endif /* GNOSTR_GIT_CLIENT_H */
