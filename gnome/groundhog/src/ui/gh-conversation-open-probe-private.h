/* Private, main-thread-only conversation-open timing hooks. Built only with
 * GROUNDHOG_CONVERSATION_OPEN_PROBE; never part of the installed API. */
#pragma once

#ifdef GROUNDHOG_CONVERSATION_OPEN_PROBE
#include "gh-conversation-view.h"
#include "gh-message-row.h"

typedef struct {
  guint64 generation;
  gboolean trace;
  gint64 entry_us;
  gint64 cleanup_end_us;
  gint64 timeline_start_us;
  gint64 timeline_end_us;
  gint64 attach_end_us;
  gint64 return_us;
  gint64 first_bind_us;
  gint64 last_bind_us;
  gint64 first_allocation_us;
  gint64 first_paint_us;
  gint64 last_scroll_us;
  gint64 settled_us;
  guint timeline_items;
  guint bind_count;
  gint64 bind_wall_us;
  gint64 bind_wall_max_us;
  guint open_scroll_count;
  guint pin_count;
  guint adjustment_changes;
  guint older_requests;
} GhConversationOpenProbe;

GhConversationOpenProbe *gh_conversation_open_probe_arm(GhConversationView *view, gboolean trace);
void gh_conversation_open_probe_disarm(GhConversationView *view);
gboolean gh_conversation_open_probe_scroll_pending(GhConversationView *view);
void gh_conversation_open_probe_bind(GhMessageRow *row, gint64 elapsed_us);
#endif
