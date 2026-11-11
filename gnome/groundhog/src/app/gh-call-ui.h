#ifndef GH_CALL_UI_H
#define GH_CALL_UI_H

#include "gh-call-service.h"
#include "gh-window.h"

G_BEGIN_DECLS

/*
 * The call UI of one window (nostrc-mqrgk.8): entry points, the in-call page
 * and window behaviour around the process-owned GhCallService.
 *
 *   - Header: Start Voice Call / Start Video Call (gh_content_page_get_call_button())
 *     show for a shown one-to-one private (NIP-17) conversation that is not a
 *     message request; no group calls. They are insensitive, with the reason
 *     as tooltip and accessible description, when calls are unavailable: no
 *     call support installed (probe MISSING), a build without media, Tor mode,
 *     no account, a recipient without a message inbox
 *     (gh_conversation_view_get_recipient_without_inbox()) or another call in
 *     progress. Probe HEADSET_ONLY (no echo cancellation) keeps them sensitive,
 *     says "use a headset" in the tooltip, and asks for the headset output
 *     before placing or answering.
 *   - The in-call page (GhCallView) is the root stack's "call" page: shown on
 *     placing, answering and an incoming ring, and when a window attaches
 *     during a call. Back (win.call-leave-view) leaves the call running; the
 *     sidebar's and content page's "Return to Call" banners (win.call-show)
 *     come back. State texts: Incoming…, Calling…, Connecting…, the duration,
 *     Reconnecting…, and the end reason.
 *   - Actions (win.): call-voice, call-video, call-answer, call-answer-video,
 *     call-decline, call-hang-up, call-toggle-mute and call-toggle-camera
 *     (boolean state), call-switch-video, call-show, call-leave-view. Enabled
 *     only when they apply; the accelerators are gh-window.c's.
 *   - Closing the window during a call asks first ("End Call?") unless
 *     keeps_running() says the process stays (run-in-background), in which
 *     case the call continues with no window.
 *   - An Answer from the call notification ("answer-call" on the notifier) is
 *     handled here, so the headset question and the camera choice apply.
 * Nothing here is stored: names come from the conversation model.
 */

/* Whether the media runtime can place calls. */
typedef enum {
  GH_CALL_UI_MEDIA_READY,
  GH_CALL_UI_MEDIA_HEADSET_ONLY, /* no echo cancellation: a headset is needed */
  GH_CALL_UI_MEDIA_MISSING,      /* the GStreamer call elements are missing */
} GhCallUiMedia;

/* Controls of the concrete backend of gh_call_get_media(); every member is
 * nullable (that control then does nothing). device models hold GObjects
 * with "display-name" and "device-class" ("Audio/Source", "Audio/Sink",
 * "Video/Source") string properties; a NULL device is the system default. A
 * backend with a "device-removed" (GObject) signal reports unplugged devices. */
typedef struct {
  void (*set_muted)(GhCallMediaBackend *media, gboolean muted);
  void (*set_camera)(GhCallMediaBackend *media, gboolean enabled);
  GdkPaintable *(*remote_video)(GhCallMediaBackend *media);
  GdkPaintable *(*local_preview)(GhCallMediaBackend *media);
  GListModel *(*devices)(GhCallMediaBackend *media, GError **error);
  gboolean (*select_device)(GhCallMediaBackend *media, const gchar *device_class,
                            GObject *device, GError **error);
} GhCallMediaOps;

typedef struct {
  GhCallService *service;             /* required */
  GhConversationStore *conversations; /* nullable: peers are then unnamed */
  const GhCallMediaOps *ops;          /* nullable: no media controls */
  /* nullable: READY. *reason (nullable out, English) is for the log. */
  GhCallUiMedia (*probe)(gpointer data, gchar **reason);
  /* Headset choice (HEADSET_ONLY): the outputs, and the one for the next call. */
  GListModel *(*headset_outputs)(gpointer data);
  gboolean (*choose_headset)(gpointer data, GObject *output, GError **error);
  /* Whether the next call's media starts with the camera on (nullable). Set
   * before answering, so a video offer answered by voice never opens the
   * camera. */
  void (*set_next_camera)(gpointer data, gboolean enabled);
  gboolean (*keeps_running)(gpointer data); /* nullable: FALSE */
  GObject *notifier;                  /* nullable GhNotifier ("answer-call") */
  gpointer data;                      /* for every function above; borrowed */
} GhCallUiConfig;

/* Once per window; released with it. */
void gh_call_ui_attach(GhWindow *window, const GhCallUiConfig *config);

/* For tests: the window's call page, and its last toast text. */
GtkWidget *gh_call_ui_get_view(GhWindow *window);
const gchar *gh_call_ui_get_last_toast(GhWindow *window);
/* The status text for a call (translated), as the page shows it. */
gchar *gh_call_ui_describe(GhCall *call, gint64 now_monotonic);

G_END_DECLS
#endif
