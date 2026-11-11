#ifndef GH_CALL_VIEW_H
#define GH_CALL_VIEW_H

#include <adwaita.h>

G_BEGIN_DECLS

/*
 * GhCallView: the in-call page (data/ui/gh-call-view.blp, nostrc-mqrgk.8).
 * Presentation only: gh-call-ui.c sets what it shows and installs the win.call-*
 * actions its controls name. It never touches the call service or media.
 */
typedef enum {
  GH_CALL_VIEW_MUTE = 1 << 0,
  GH_CALL_VIEW_CAMERA = 1 << 1,
  GH_CALL_VIEW_SWITCH_VIDEO = 1 << 2,
  GH_CALL_VIEW_DEVICES = 1 << 3,
  GH_CALL_VIEW_DECLINE = 1 << 4,
  GH_CALL_VIEW_ANSWER = 1 << 5,
  GH_CALL_VIEW_ANSWER_VIDEO = 1 << 6,
  GH_CALL_VIEW_HANG_UP = 1 << 7,
} GhCallViewControls;

#define GH_TYPE_CALL_VIEW (gh_call_view_get_type())
G_DECLARE_FINAL_TYPE(GhCallView, gh_call_view, GH, CALL_VIEW, AdwBreakpointBin)

GtkWidget *gh_call_view_new(void);
/* The peer's name (avatar initials, title); plain text. */
void gh_call_view_set_peer(GhCallView *self, const gchar *name);
const gchar *gh_call_view_get_peer(GhCallView *self);
/* "Calling…", "0:42", "Call ended", …; plain text. */
void gh_call_view_set_status(GhCallView *self, const gchar *status);
const gchar *gh_call_view_get_status(GhCallView *self);
/* Exactly these controls show. */
void gh_call_view_set_controls(GhCallView *self, GhCallViewControls controls);
/* Icon, tooltip and accessible label follow the state: "Mute Microphone" /
 * "Unmute Microphone", "Turn Off Camera" / "Turn On Camera". */
void gh_call_view_set_muted(GhCallView *self, gboolean muted);
void gh_call_view_set_camera(GhCallView *self, gboolean on);
/* Either may be NULL; remote replaces the avatar while it shows. */
void gh_call_view_set_video(GhCallView *self, GdkPaintable *remote, GdkPaintable *local);
/* One control's widget, for focus and tests. */
GtkWidget *gh_call_view_get_control(GhCallView *self, GhCallViewControls control);
/* The Devices popover's box, filled by the controller when it opens. */
GtkBox *gh_call_view_get_devices_box(GhCallView *self);
GtkPopover *gh_call_view_get_devices_popover(GhCallView *self);

G_END_DECLS
#endif
