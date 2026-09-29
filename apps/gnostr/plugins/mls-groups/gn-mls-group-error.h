/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-mls-group-error.h - When a rejected group event means "out of sync"
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_MLS_GROUP_ERROR_H
#define GN_MLS_GROUP_ERROR_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * TRUE when @error from marmot_gobject_client_process_message_finish() is an
 * MLS-level failure.  libmarmot reaches the MLS layer only after the outer
 * NIP-44 layer decrypted with a group exporter secret, i.e. the event came
 * from a member; relay backfill from before we joined and events anyone can
 * publish with the public h tag fail earlier (MARMOT_ERR_NIP44,
 * GROUP_NOT_FOUND, parse errors) and are not divergence.
 */
gboolean gn_mls_group_error_is_divergence(const GError *error);

/*
 * Per-group gate for "group may be out of sync" reports: @reported is a set
 * of group ids (g_str_hash/g_str_equal, owning keys).  Returns TRUE -- and
 * records @group_id -- only the first time for that group; a NULL or empty id
 * is never reported.  gn_mls_group_error_gate_reset() re-arms a group once it
 * has successfully moved to a new epoch.
 */
gboolean gn_mls_group_error_gate_admit(GHashTable *reported, const gchar *group_id);
void     gn_mls_group_error_gate_reset(GHashTable *reported, const gchar *group_id);

G_END_DECLS

#endif /* GN_MLS_GROUP_ERROR_H */
