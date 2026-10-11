#ifndef GH_MESSAGE_ROW_H
#define GH_MESSAGE_ROW_H

#include <adwaita.h>
#include "gh-message.h"

G_BEGIN_DECLS

/* One message of a conversation (data/ui/gh-message-row.blp, charter §7.4,
 * §7.6): a bubble aligned to the end for your own messages and to the start
 * for others' (mirrored in right-to-left locales), with the body as
 * selectable text in which only gh-link-policy.h links are live (all other
 * markup is shown literally). Around it: the sender's name ("show-sender",
 * the first message of a run in a multi-party room), the "Show Preview"
 * button for the first https link (charter §2.1; nothing loads before it is
 * chosen, see GhConversationView), and a meta line with the
 * disappearing-message timer, the time ("run-end") and, for own messages,
 * the GhDeliveryIndicator and "Try Again" when not sent. Clicking a link
 * activates conversation.open-link; the button conversation.retry-message
 * and conversation.show-preview, with the rumor id.
 *
 * "undecryptable" shows "Unable to decrypt yet" instead of a body (charter
 * §7.15 state 13, encrypted groups). "summary" is the composed accessible
 * text (charter §7.14), e.g. "You, 10:43: Hi. Sent." or "Alice, Yesterday
 * 10:42: Hello"; the list binds it to its list item. Properties: "message",
 * "run-start", "run-end", "show-sender", "compact", "undecryptable",
 * "summary" (read-only). */
#define GH_TYPE_MESSAGE_ROW (gh_message_row_get_type())
G_DECLARE_FINAL_TYPE(GhMessageRow, gh_message_row, GH, MESSAGE_ROW, GtkWidget)

GtkWidget *gh_message_row_new(void);
void gh_message_row_set_message(GhMessageRow *self, GhMessage *message);
GhMessage *gh_message_row_get_message(GhMessageRow *self);
void gh_message_row_set_run(GhMessageRow *self, gboolean run_start, gboolean run_end);
void gh_message_row_set_show_sender(GhMessageRow *self, gboolean show_sender);
void gh_message_row_set_compact(GhMessageRow *self, gboolean compact);
void gh_message_row_set_undecryptable(GhMessageRow *self, gboolean undecryptable);
const gchar *gh_message_row_get_summary(GhMessageRow *self);

/* Set a poll card widget on a poll-kind message row (W26 slice C).
 * The widget is placed in the poll slot and body text is hidden.
 * Pass NULL to remove it. */
void gh_message_row_set_poll_widget(GhMessageRow *self, GtkWidget *poll_card);

/* W26 slice B (nostrc-191r): binds the reaction bar to a live-updated
 * summary from the reaction store. NULL clears it. */
typedef struct _GhReactionSummary GhReactionSummary;
void gh_message_row_set_reaction_summary(GhMessageRow *self, GhReactionSummary *summary);

/* The bubble's menu (owner request, groundhog/bubble-menu), in place of the
 * labels' default one: "Copy" (the selected text, only with a selection;
 * message.copy-selection activates conversation.copy-text), "Copy Message"
 * ("Copy Link" for a file; conversation.copy-message with the rumor id),
 * "Add Reaction…" (the reaction picker, while React is offered), and
 * "Delete for Me…" / "Delete for Everyone…" (conversation.delete-message
 * with (rumor id, for everyone)) where gh_conversation_view_can_delete_message()
 * allows. Right-click and long press on the bubble open it at the pointer;
 * the view opens it for Menu and Shift+F10 (popup_menu_for_focus, which
 * leaves the avatar's own menu alone). @x, @y in the bubble's coordinates,
 * negative: the whole bubble. FALSE when nothing was shown. */
gboolean gh_message_row_popup_menu(GhMessageRow *self, gdouble x, gdouble y);
gboolean gh_message_row_popup_menu_for_focus(GhMessageRow *self, GtkWidget *focus);
/* The menu once it has been opened, else NULL (for tests). */
GtkPopover *gh_message_row_get_menu(GhMessageRow *self);

/* The name shown for a pubkey (lowercase hex): its abbreviated npub
 * ("npub1abcde…wxyz"). Profile names are not fetched (charter PT-8). */
gchar *gh_message_row_display_name(const gchar *pubkey_hex);
/* The name shown for a message's sender: "You" for your own messages, else
 * gh_message_row_display_name() of the sender. */
gchar *gh_message_row_sender_name(GhMessage *message);
/* The accessible text of a message at @now: "Sender, time: text", then
 * "Disappearing message." when it expires and the status sentence for own
 * messages, e.g. "You, 10:43: Hi. Sent." (charter §7.14). */
gchar *gh_message_row_compose_summary(GhMessage *message, GDateTime *now);

G_END_DECLS
#endif
