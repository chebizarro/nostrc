/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip34-git-plugin.h - NIP-34 Git Repository Plugin
 *
 * Implements NIP-34 (Git Stuff) for Nostr-based git repository integration.
 * Handles event kinds 30617 (repo), 1617 (patches), 1621 (issues), 1622 (replies).
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef NIP34_GIT_PLUGIN_H
#define NIP34_GIT_PLUGIN_H

#include <glib-object.h>

G_BEGIN_DECLS

#define NIP34_TYPE_GIT_PLUGIN (nip34_git_plugin_get_type())

G_DECLARE_FINAL_TYPE(Nip34GitPlugin, nip34_git_plugin, NIP34, GIT_PLUGIN, GObject)

G_END_DECLS

#endif /* NIP34_GIT_PLUGIN_H */
