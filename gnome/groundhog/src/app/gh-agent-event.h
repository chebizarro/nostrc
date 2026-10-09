#ifndef GH_AGENT_EVENT_H
#define GH_AGENT_EVENT_H

#include <glib.h>

G_BEGIN_DECLS

#define GH_AGENT_STREAM_START_KIND 1200
#define GH_AGENT_ACTIVITY_KIND 1201
#define GH_AGENT_OPERATION_KIND 1202
#define GH_AGENT_EVENT_MAX_JSON_BYTES 4096

typedef struct {
  gchar *text;
  gchar *detail;
  gchar *type; /* normalized event_type/status for icon selection */
  const gchar *icon_name;
} GhAgentEvent;

/* A peer-controlled inner-event body. NULL means it must not be displayed. */
GhAgentEvent *gh_agent_event_parse(gint kind, const gchar *content);
typedef struct _GhMessage GhMessage;
/* Presentation also reads optional authenticated inner-event tags. */
GhAgentEvent *gh_agent_event_parse_message(GhMessage *message);
void gh_agent_event_free(GhAgentEvent *event);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhAgentEvent, gh_agent_event_free)

gboolean gh_agent_event_is_kind(gint kind);
/* Stream starts and malformed activity are stored, but never shown. */
gboolean gh_agent_event_is_visible(gint kind, const gchar *content);

G_END_DECLS
#endif
