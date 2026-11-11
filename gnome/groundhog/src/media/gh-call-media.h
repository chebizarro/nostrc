#ifndef GH_CALL_MEDIA_H
#define GH_CALL_MEDIA_H
#include "../app/gh-call-media-backend.h"
#include <gtk/gtk.h>
#include <gst/gst.h>
G_BEGIN_DECLS
#define GH_TYPE_CALL_MEDIA (gh_call_media_get_type())
G_DECLARE_FINAL_TYPE(GhCallMedia, gh_call_media, GH, CALL_MEDIA, GObject)
/* One instance per call, created/used on the GTK owning main context. settings
 * must use org.nostr.Groundhog. Construction opens no capture or ICE sockets. */
GhCallMedia *gh_call_media_new(GSettings *settings);
void gh_call_media_set_muted(GhCallMedia *self, gboolean muted);
void gh_call_media_set_camera_enabled(GhCallMedia *self, gboolean enabled);
/* Default FALSE: speakers require webrtcdsp + webrtcechoprobe. Select TRUE only
 * for an explicitly selected headset output, never the system default route.
 * Selecting a different output resets this assertion. */
gboolean gh_call_media_set_headset(GhCallMedia *self, gboolean headset, GError **error);
/* Device discovery is explicit, not constructor side-effect. Items are
 * GstDevice objects. NULL selection means platform default. Capture devices
 * and AEC-protected outputs can change between negotiations during a call;
 * changing a headset output requires a new call to re-evaluate AEC. */
gboolean gh_call_media_start_devices(GhCallMedia *self, GError **error);
GListModel *gh_call_media_get_devices(GhCallMedia *self); /* transfer none */
gboolean gh_call_media_select_device(GhCallMedia *self, const gchar *device_class,
                                     GstDevice *device, GError **error);
GdkPaintable *gh_call_media_get_remote_video(GhCallMedia *self); /* transfer none */
GdkPaintable *gh_call_media_get_local_preview(GhCallMedia *self); /* transfer none */
/* device-removed (GstDevice): selected capture stops; the call remains alive.
 * Media/pipeline errors emit connection-state FAILED and tear down sockets.
 * TURN secrets are retrieved asynchronously by the backend through
 * gh-call-settings.h. configure snapshots server preferences per call. */
G_END_DECLS
#endif
