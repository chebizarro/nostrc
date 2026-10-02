#ifndef GH_REACTION_BAR_H
#define GH_REACTION_BAR_H

#include <adwaita.h>
#include "gh-reaction.h"

G_BEGIN_DECLS

/* A row of emoji chips under a message bubble (GNOME HIG, charter §7.4):
 * each chip shows the emoji and its count, highlighted when the user reacted
 * with it. Clicking a chip toggles the user's own reaction (add or remove).
 * A "+" button at the end opens the reaction picker.
 *
 * The bar binds to a GhReactionSummary (live-updated by the reaction store).
 * It hides itself when the summary has no reactions (total-count 0). When
 * there are reactions it is visible and accessible (tab-focusable, with
 * screen reader text per chip: "thumbs up, 3, including you" or
 * "heart, 2"). */
#define GH_TYPE_REACTION_BAR (gh_reaction_bar_get_type())
G_DECLARE_FINAL_TYPE(GhReactionBar, gh_reaction_bar, GH, REACTION_BAR, GtkWidget)

GtkWidget *gh_reaction_bar_new(void);
void gh_reaction_bar_set_summary(GhReactionBar *self, GhReactionSummary *summary);
GhReactionSummary *gh_reaction_bar_get_summary(GhReactionBar *self);

/* Signal: "reaction-toggled" (emoji: string, add: boolean) —
 * the user clicked a chip to add or remove their reaction. */

G_END_DECLS
#endif
