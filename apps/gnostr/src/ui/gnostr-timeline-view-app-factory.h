/**
 * gnostr-timeline-view-app-factory.h — App adapter for timeline view rendering
 *
 * The core NostrGtkTimelineView widget lives in nostr-gtk.
 * This header exposes the GNostr app's list-item factory setup helper.
 */

#ifndef GNOSTR_TIMELINE_VIEW_APP_FACTORY_H
#define GNOSTR_TIMELINE_VIEW_APP_FACTORY_H

#include <nostr-gtk-1.0/gnostr-timeline-view.h>

G_BEGIN_DECLS

/**
 * gnostr_timeline_view_setup_app_factory:
 * @view: The timeline view widget
 *
 * Creates and installs the GNostr app-specific GtkListItemFactory on the
 * timeline view. This factory handles NoteCardRow creation, binding with
 * profile/embed/metadata loading, and app-level UI behaviors.
 *
 * Must be called after creating the timeline view and before displaying items.
 */
void gnostr_timeline_view_setup_app_factory(NostrGtkTimelineView *view);

G_END_DECLS

#endif /* GNOSTR_TIMELINE_VIEW_APP_FACTORY_H */
