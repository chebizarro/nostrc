/**
 * gnostr-timeline-view.h — App-level header for timeline view
 *
 * Re-exports the library widget from nostr-gtk and adds the app-specific
 * factory setup function.
 */

#ifndef GNOSTR_APP_TIMELINE_VIEW_H
#define GNOSTR_APP_TIMELINE_VIEW_H

/* Re-export the library widget and all its API */
#include <nostr-gtk-1.0/gnostr-timeline-view.h>

/* App-specific convenience includes */
#include "gnostr-avatar-cache.h"

G_BEGIN_DECLS

/**
 * nostr_gtk_timeline_view_setup_app_factory:
 * @view: The timeline view widget
 *
 * Creates and installs the app-specific GtkListItemFactory on the timeline view.
 * This factory handles NoteCardRow creation, binding with profile/embed/metadata
 * loading, and all NIP-specific UI behaviors.
 *
 * Must be called after creating the timeline view and before displaying items.
 */
void nostr_gtk_timeline_view_setup_app_factory(NostrGtkTimelineView *view);

G_END_DECLS

#endif /* GNOSTR_APP_TIMELINE_VIEW_H */
