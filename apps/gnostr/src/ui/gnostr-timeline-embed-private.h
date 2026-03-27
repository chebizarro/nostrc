/**
 * gnostr-timeline-embed-private.h — Embed resolution system declarations
 *
 * Exposes the embed resolution subsystem extracted from gnostr-timeline-view.c.
 * Handles bech32 reference resolution (note1, nevent, naddr, nprofile),
 * inflight request deduplication, and embed result caching.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GNOSTR_TIMELINE_EMBED_PRIVATE_H
#define GNOSTR_TIMELINE_EMBED_PRIVATE_H

#include <nostr-gtk-1.0/nostr-note-card-row.h>
#include <gtk/gtk.h>
#include <glib.h>

G_BEGIN_DECLS

/**
 * on_row_request_embed:
 * Signal handler for "request-embed" on NostrGtkNoteCardRow.
 * Resolves bech32 note/nevent/naddr references via local store and relay fetch.
 */
void gnostr_timeline_embed_on_row_request_embed (NostrGtkNoteCardRow *row,
                                                  const char          *target,
                                                  gpointer             user_data);

/**
 * inflight_detach_row:
 * Remove a row widget from all inflight embed requests. Cancels requests
 * that become unused (no more attached rows).
 */
void gnostr_timeline_embed_inflight_detach_row (GtkWidget *row);

/**
 * get_current_user_pubkey_hex:
 * Returns the current user's pubkey as 64-char hex from GSettings.
 * Returns newly allocated string or NULL if not signed in. Caller must free.
 */
char *gnostr_timeline_embed_get_current_user_pubkey_hex (void);

/**
 * hex32_from_string:
 * Convert a 64-char hex string to a 32-byte binary array.
 * Returns TRUE on success.
 */
gboolean gnostr_timeline_embed_hex32_from_string (const char    *hex,
                                                   unsigned char  out[32]);

G_END_DECLS

#endif /* GNOSTR_TIMELINE_EMBED_PRIVATE_H */
