#ifndef GH_REACTION_PICKER_H
#define GH_REACTION_PICKER_H

#include <adwaita.h>
#include "gh-reaction.h"

G_BEGIN_DECLS

/* A quick-reaction picker (GNOME HIG, chart §7.4): shown in a popover on
 * right-click, long-press or a hover button on a message row.
 *
 * Top row: six quick-set emoji buttons (gh_reaction_quick_set), then a
 * "…" button that opens GTK's GtkEmojiChooser for the full emoji palette.
 * Keyboard accessible: Tab through the quick set, Enter to pick.
 *
 * No network fetch for custom emoji images (privacy charter PD-2: no remote
 * images by default); custom shortcodes are shown as text. */
#define GH_TYPE_REACTION_PICKER (gh_reaction_picker_get_type())
G_DECLARE_FINAL_TYPE(GhReactionPicker, gh_reaction_picker, GH, REACTION_PICKER, GtkPopover)

GtkWidget *gh_reaction_picker_new(void);

/* Signal: "emoji-picked" (emoji: string) — the user chose an emoji. */

G_END_DECLS
#endif
