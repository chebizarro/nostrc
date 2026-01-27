#ifndef GNOSTR_SESSION_VIEW_H
#define GNOSTR_SESSION_VIEW_H

#include <adwaita.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_SESSION_VIEW (gnostr_session_view_get_type())

G_DECLARE_FINAL_TYPE(GnostrSessionView, gnostr_session_view, GNOSTR, SESSION_VIEW, AdwBin)

/* Constructor */
GnostrSessionView *gnostr_session_view_new(void);

/* Responsive mode (drives template bindings) */
gboolean gnostr_session_view_get_compact(GnostrSessionView *self);
void gnostr_session_view_set_compact(GnostrSessionView *self, gboolean compact);

/* Guest-mode gating for Notifications / Messages */
gboolean gnostr_session_view_get_authenticated(GnostrSessionView *self);
void gnostr_session_view_set_authenticated(GnostrSessionView *self, gboolean authenticated);

/* Navigation */
void gnostr_session_view_show_page(GnostrSessionView *self, const char *page_name);

/* Side panel (profile/thread) controls */
void gnostr_session_view_show_profile_panel(GnostrSessionView *self);
void gnostr_session_view_show_thread_panel(GnostrSessionView *self);
void gnostr_session_view_hide_side_panel(GnostrSessionView *self);
gboolean gnostr_session_view_is_side_panel_visible(GnostrSessionView *self);

/* Toast forwarding (main window owns the overlay; session view can be given a weak reference) */
void gnostr_session_view_set_toast_overlay(GnostrSessionView *self, AdwToastOverlay *overlay);
void gnostr_session_view_show_toast(GnostrSessionView *self, const char *message);

/* Access to child widgets for main window to wire up models/signals */
GtkWidget *gnostr_session_view_get_timeline(GnostrSessionView *self);
GtkWidget *gnostr_session_view_get_notifications_view(GnostrSessionView *self);
GtkWidget *gnostr_session_view_get_dm_inbox(GnostrSessionView *self);
GtkWidget *gnostr_session_view_get_discover_page(GnostrSessionView *self);
GtkWidget *gnostr_session_view_get_classifieds_view(GnostrSessionView *self);
GtkWidget *gnostr_session_view_get_profile_pane(GnostrSessionView *self);
GtkWidget *gnostr_session_view_get_thread_view(GnostrSessionView *self);

/* Panel state queries */
gboolean gnostr_session_view_is_showing_profile(GnostrSessionView *self);

/* New notes indicator */
void gnostr_session_view_set_new_notes_count(GnostrSessionView *self, guint count);

G_END_DECLS

#endif /* GNOSTR_SESSION_VIEW_H */