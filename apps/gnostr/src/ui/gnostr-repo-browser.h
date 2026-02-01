/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-repo-browser.h - NIP-34 Repository Browser View
 *
 * Displays published git repositories from Nostr relays.
 * Shows repository metadata, maintainers, clone URLs, and activity.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GNOSTR_REPO_BROWSER_H
#define GNOSTR_REPO_BROWSER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_REPO_BROWSER (gnostr_repo_browser_get_type())

G_DECLARE_FINAL_TYPE(GnostrRepoBrowser, gnostr_repo_browser, GNOSTR, REPO_BROWSER, GtkWidget)

/**
 * Signals:
 * "repo-selected" (gchar *repo_id, gpointer user_data)
 *   - Emitted when user selects a repository
 * "clone-requested" (gchar *clone_url, gpointer user_data)
 *   - Emitted when user clicks clone button
 */

/**
 * gnostr_repo_browser_new:
 *
 * Creates a new repository browser widget.
 *
 * Returns: (transfer full): A new repository browser
 */
GnostrRepoBrowser *gnostr_repo_browser_new(void);

/**
 * gnostr_repo_browser_add_repository:
 * @self: The repository browser
 * @id: Repository unique ID (d-tag)
 * @name: Repository name (nullable)
 * @description: Repository description (nullable)
 * @clone_url: Git clone URL (nullable)
 * @web_url: Web interface URL (nullable)
 * @maintainer_pubkey: Primary maintainer's pubkey (nullable)
 * @updated_at: Last update timestamp
 *
 * Adds a repository to the browser view.
 */
void gnostr_repo_browser_add_repository(GnostrRepoBrowser *self,
                                         const char        *id,
                                         const char        *name,
                                         const char        *description,
                                         const char        *clone_url,
                                         const char        *web_url,
                                         const char        *maintainer_pubkey,
                                         gint64             updated_at);

/**
 * gnostr_repo_browser_clear:
 * @self: The repository browser
 *
 * Removes all repositories from the view.
 */
void gnostr_repo_browser_clear(GnostrRepoBrowser *self);

/**
 * gnostr_repo_browser_set_loading:
 * @self: The repository browser
 * @loading: Whether to show loading state
 *
 * Shows or hides the loading indicator.
 */
void gnostr_repo_browser_set_loading(GnostrRepoBrowser *self, gboolean loading);

/**
 * gnostr_repo_browser_set_filter:
 * @self: The repository browser
 * @filter_text: Search/filter text (nullable to clear)
 *
 * Filters displayed repositories by name/description.
 */
void gnostr_repo_browser_set_filter(GnostrRepoBrowser *self, const char *filter_text);

/**
 * gnostr_repo_browser_get_selected_id:
 * @self: The repository browser
 *
 * Returns: (transfer none) (nullable): ID of selected repository, or NULL
 */
const char *gnostr_repo_browser_get_selected_id(GnostrRepoBrowser *self);

/**
 * gnostr_repo_browser_get_count:
 * @self: The repository browser
 *
 * Returns: Number of repositories in the browser
 */
guint gnostr_repo_browser_get_count(GnostrRepoBrowser *self);

G_END_DECLS

#endif /* GNOSTR_REPO_BROWSER_H */
