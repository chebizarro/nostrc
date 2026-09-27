/*
 * gnostr-publish-gate — per-note actions follow read-only mode (nostrc-46h7).
 *
 * nostrc-lwzv turns publishing off while a GNostr Signer (NIP-55L) session
 * has no signer to sign with, and guards every publish entry point with the
 * banner's explanation. This module also greys out the per-note actions
 * (reply, repost, like, zap, pin, bookmark) on every NostrGtkNoteCardRow:
 *   - gnostr_publish_gate_set_reason() re-applies the state to the cards
 *     under @root at once;
 *   - the timeline factory asks gnostr_publish_gate_effective_logged_in()
 *     at bind time, so recycled rows follow it;
 *   - while blocked, cards that other views (thread view, profile pane in
 *     nostr-gtk) create and map later are caught by a GtkWidget::map
 *     emission hook, which is removed again when publishing is back.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GNOSTR_PUBLISH_GATE_H
#define GNOSTR_PUBLISH_GATE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* @reason: why publishing is off (the banner text), or NULL when allowed.
 * @root: (nullable): widget tree whose note cards are updated now. */
void gnostr_publish_gate_set_reason(const char *reason, GtkWidget *root);

/* (transfer none) (nullable): the current reason. */
const char *gnostr_publish_gate_get_reason(void);

/* A card's "logged in" state (enables its actions): @signed_in and
 * publishing not blocked. */
gboolean gnostr_publish_gate_effective_logged_in(gboolean signed_in);

G_END_DECLS

#endif /* GNOSTR_PUBLISH_GATE_H */
