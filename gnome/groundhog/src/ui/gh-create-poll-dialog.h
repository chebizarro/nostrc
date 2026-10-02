#ifndef GH_CREATE_POLL_DIALOG_H
#define GH_CREATE_POLL_DIALOG_H

#include <adwaita.h>
#include "gh-mls-poll.h"

G_BEGIN_DECLS

/*
 * GhCreatePollDialog (W26 slice C): a dialog for composing a NIP-88 poll
 * in an encrypted group. The user enters a question, 2-10 options,
 * single/multiple choice, and an optional end time (1-720 hours).
 * Emits "poll-created" with the poll parameters on success.
 *
 * Signals: "poll-created"(question, option_labels, n_options, poll_type, ends_at).
 */
#define GH_TYPE_CREATE_POLL_DIALOG (gh_create_poll_dialog_get_type())
G_DECLARE_FINAL_TYPE(GhCreatePollDialog, gh_create_poll_dialog, GH, CREATE_POLL_DIALOG, AdwDialog)

GtkWidget *gh_create_poll_dialog_new(void);

G_END_DECLS
#endif
