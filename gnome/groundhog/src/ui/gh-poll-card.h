#ifndef GH_POLL_CARD_H
#define GH_POLL_CARD_H

#include <adwaita.h>
#include "gh-mls-poll.h"

G_BEGIN_DECLS

/*
 * GhPollCard: renders a NIP-88 poll inside a message bubble (W26 slice C).
 * Shows the question, option buttons with vote tallies, total voter count,
 * remaining time / closed state. When the user taps an option, emits
 * "vote-cast" with the selected option IDs. The widget binds to a GhMlsPoll
 * object and redraws when tallies change. Built entirely in code (no Blueprint
 * template) because the option list is dynamic (2-10 items).
 *
 * Properties: "poll" (GhMlsPoll, nullable).
 * Signals: "vote-cast" (gchar **option_ids, guint n_options).
 */
#define GH_TYPE_POLL_CARD (gh_poll_card_get_type())
G_DECLARE_FINAL_TYPE(GhPollCard, gh_poll_card, GH, POLL_CARD, GtkWidget)

GtkWidget  *gh_poll_card_new(void);
void        gh_poll_card_set_poll(GhPollCard *self, GhMlsPoll *poll);
GhMlsPoll  *gh_poll_card_get_poll(GhPollCard *self);

G_END_DECLS
#endif
