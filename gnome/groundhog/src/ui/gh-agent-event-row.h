#ifndef GH_AGENT_EVENT_ROW_H
#define GH_AGENT_EVENT_ROW_H

#include <gtk/gtk.h>
#include "gh-message.h"

G_BEGIN_DECLS

#define GH_TYPE_AGENT_EVENT_ROW (gh_agent_event_row_get_type())
G_DECLARE_FINAL_TYPE(GhAgentEventRow, gh_agent_event_row, GH, AGENT_EVENT_ROW, GtkBox)

GtkWidget *gh_agent_event_row_new(void);
void gh_agent_event_row_set_message(GhAgentEventRow *self, GhMessage *message);
const gchar *gh_agent_event_row_get_summary(GhAgentEventRow *self);

G_END_DECLS
#endif
