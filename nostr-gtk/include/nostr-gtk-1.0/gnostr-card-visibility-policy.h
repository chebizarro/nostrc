/*
 * GnostrCardVisibilityPolicy - shared note-card visibility predicate
 *
 * Runtime helper for deciding whether a note card should be shown before
 * profile pairing is available.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GNOSTR_CARD_VISIBILITY_POLICY_H
#define GNOSTR_CARD_VISIBILITY_POLICY_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_CARD_VISIBILITY_POLICY (gnostr_card_visibility_policy_get_type())
G_DECLARE_FINAL_TYPE(GnostrCardVisibilityPolicy,
                     gnostr_card_visibility_policy,
                     GNOSTR,
                     CARD_VISIBILITY_POLICY,
                     GObject)

GnostrCardVisibilityPolicy *gnostr_card_visibility_policy_new(void);

/*
 * gnostr_card_visibility_policy_replace_follow_snapshot:
 * @self: a card visibility policy
 * @viewer_pubkey_hex: (nullable): current viewer pubkey
 * @followed_pubkeys: (nullable) (array zero-terminated=1): followed author pubkeys
 *
 * Replaces the current viewer follow snapshot. Returns %TRUE if the effective
 * viewer or followed-author membership changed.
 */
gboolean gnostr_card_visibility_policy_replace_follow_snapshot(
    GnostrCardVisibilityPolicy *self,
    const char *viewer_pubkey_hex,
    const char * const *followed_pubkeys);

/*
 * gnostr_card_visibility_policy_should_show:
 * @self: (nullable): a card visibility policy
 * @author_pubkey_hex: (nullable): event author pubkey
 * @has_profile: whether author profile pairing exists
 * @explicitly_loaded: whether the event was loaded by explicit user action
 *
 * Returns %TRUE when a card should be visible: explicit user load, existing
 * profile pairing, or author membership in the viewer follow snapshot. A NULL
 * policy degrades to explicit-or-profile visibility.
 */
gboolean gnostr_card_visibility_policy_should_show(
    GnostrCardVisibilityPolicy *self,
    const char *author_pubkey_hex,
    gboolean has_profile,
    gboolean explicitly_loaded);

G_END_DECLS

#endif /* GNOSTR_CARD_VISIBILITY_POLICY_H */
