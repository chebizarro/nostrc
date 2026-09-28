#ifndef GH_CONVERSATION_ROW_H
#define GH_CONVERSATION_ROW_H

#include <adwaita.h>
#include "gh-conversation.h"

G_BEGIN_DECLS

/* One row of the conversation list (data/ui/gh-conversation-row.blp): an
 * initials-only avatar, the title (bold while unread), the relative time of
 * the last activity, the message preview (only with "show-preview"), a
 * "Request" tag for message requests and the unread count badge. It follows
 * the bound conversation's notifications. "summary" is the composed
 * accessible text (charter §7.5), e.g. "Alice. Private conversation.
 * 2 unread. 10:42. Hello there" (the preview only with show-preview); the
 * list binds it to its GtkListItem:accessible-label. */
#define GH_TYPE_CONVERSATION_ROW (gh_conversation_row_get_type())
G_DECLARE_FINAL_TYPE(GhConversationRow, gh_conversation_row, GH, CONVERSATION_ROW, GtkWidget)

GtkWidget *gh_conversation_row_new(void);
void gh_conversation_row_set_conversation(GhConversationRow *self,
                                          GhConversation *conversation);
GhConversation *gh_conversation_row_get_conversation(GhConversationRow *self);
void gh_conversation_row_set_show_preview(GhConversationRow *self, gboolean show_preview);
const gchar *gh_conversation_row_get_summary(GhConversationRow *self);

/* The list's time for a unix timestamp relative to now (local time): the
 * time of day today, "Yesterday", the weekday within the last week, else the
 * locale's date. "" for 0. The clock follows org.gnome.desktop.interface
 * clock-format when that schema is installed, else 24-hour. */
gchar *gh_conversation_row_format_time(gint64 timestamp, GDateTime *now);
/* A message's time: the time of day today, else the day as above followed
 * by the time of day. */
gchar *gh_conversation_row_format_message_time(gint64 timestamp, GDateTime *now);

G_END_DECLS
#endif
