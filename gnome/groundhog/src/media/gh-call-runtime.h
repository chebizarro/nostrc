#ifndef GH_CALL_RUNTIME_H
#define GH_CALL_RUNTIME_H

#include "gh-call-service.h"
#include "gh-call-ui.h"

G_BEGIN_DECLS

/*
 * GhCallRuntime: the production media of calls (nostrc-mqrgk.8), one per
 * process. It installs the GhCallService media factory, which makes a
 * GhCallMedia (gh-call-media.h) per call after gh_call_capabilities_check(),
 * and offers the call UI its probe, headset choice and media controls. In a
 * build without calls (no GROUNDHOG_HAVE_CALLS) no factory is installed and
 * the probe says MISSING, so calls stay unavailable with a reason.
 *
 * Echo cancellation: a speaker route needs webrtcdsp/webrtcechoprobe. Without
 * them the probe says HEADSET_ONLY and the factory refuses a call until the
 * UI chose an explicit headset output (gh_call_runtime_choose_headset()),
 * which applies to the next call only (gh_call_media_set_headset()).
 *
 * The ringer loops a short synthesized ring (GStreamer audiotestsrc; no sound
 * file, no network) until stopped; gh-notifier.c bounds it and honours Do Not
 * Disturb.
 */
#define GH_TYPE_CALL_RUNTIME (gh_call_runtime_get_type())
G_DECLARE_FINAL_TYPE(GhCallRuntime, gh_call_runtime, GH, CALL_RUNTIME, GObject)

/* settings: org.nostr.Groundhog. */
GhCallRuntime *gh_call_runtime_new(GSettings *settings);
void gh_call_runtime_install(GhCallRuntime *self, GhCallService *service);
/* Checked once, then remembered for the process. *reason: English, nullable. */
GhCallUiMedia gh_call_runtime_probe(GhCallRuntime *self, gchar **reason);
/* Audio outputs (GstDevice) for the headset question; transfer none. */
GListModel *gh_call_runtime_get_outputs(GhCallRuntime *self);
gboolean gh_call_runtime_choose_headset(GhCallRuntime *self, GObject *output, GError **error);
void gh_call_runtime_set_next_camera(GhCallRuntime *self, gboolean enabled);
/* GhCallMedia controls; NULL without calls. */
const GhCallMediaOps *gh_call_runtime_get_media_ops(void);
void gh_call_runtime_set_ringing(GhCallRuntime *self, gboolean ringing);
gboolean gh_call_runtime_get_ringing(GhCallRuntime *self);

G_END_DECLS
#endif
