#ifndef GN_MEDIA_SOURCE_H
#define GN_MEDIA_SOURCE_H
#include <gtk/gtk.h>
G_BEGIN_DECLS
/*
 * GnMediaSource: how a host application lets nostr-gtk's media widgets
 * (GnMediaViewer, GnVideoPlayer) obtain remote media (nostrc-8xfib.4).
 *
 * The widgets never open a network connection or hand a remote URL to a media
 * backend themselves. Without a source they only emit "load-requested" and the
 * host supplies the result. With a source:
 *  - get_policy() is asked first. ALLOW: the widget may load as soon as an
 *    item is shown. ASK: nothing is fetched until the user activates the
 *    widget's Load action. BLOCKED: nothing is fetched; the widget says so.
 *  - fetch_async()/fetch_finish() return the bytes, at most max_bytes; the
 *    widget decodes them with gn_media_decode_async() under its limits and
 *    cancels the request when the item is navigated away from or closed.
 *  - open_stream() (optional) turns a video URL into a GtkMediaStream. How is
 *    the host's decision (in memory, a vetted file, its own backend policy);
 *    without it videos load only through the "load-requested" path.
 *  - adopt() (optional) is called when a widget starts using this source, so
 *    the host can bind its settings (for example a player's autoplay).
 */
typedef enum {
  GN_MEDIA_KIND_IMAGE,
  GN_MEDIA_KIND_VIDEO
} GnMediaKind;

typedef enum {
  GN_MEDIA_POLICY_ALLOW,
  GN_MEDIA_POLICY_ASK,
  GN_MEDIA_POLICY_BLOCKED
} GnMediaPolicy;

#define GN_TYPE_MEDIA_SOURCE (gn_media_source_get_type())
G_DECLARE_INTERFACE(GnMediaSource, gn_media_source, GN, MEDIA_SOURCE, GObject)

struct _GnMediaSourceInterface {
  GTypeInterface parent_iface;
  GnMediaPolicy (*get_policy)(GnMediaSource *self, const char *url, GnMediaKind kind);
  void (*fetch_async)(GnMediaSource *self, const char *url, GnMediaKind kind, gsize max_bytes,
                      GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
  GBytes *(*fetch_finish)(GnMediaSource *self, GAsyncResult *result, GError **error);
  GtkMediaStream *(*open_stream)(GnMediaSource *self, const char *url, GError **error);
  void (*adopt)(GnMediaSource *self, GtkWidget *widget);
  gpointer padding[8];
};

/* BLOCKED when the implementation has no get_policy or url is empty. */
GnMediaPolicy gn_media_source_get_policy(GnMediaSource *self, const char *url, GnMediaKind kind);
void gn_media_source_fetch_async(GnMediaSource *self, const char *url, GnMediaKind kind,
                                 gsize max_bytes, GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer user_data);
GBytes *gn_media_source_fetch_finish(GnMediaSource *self, GAsyncResult *result, GError **error);
/* (transfer full): NULL with G_IO_ERROR_NOT_SUPPORTED when not implemented. */
GtkMediaStream *gn_media_source_open_stream(GnMediaSource *self, const char *url, GError **error);
void gn_media_source_adopt(GnMediaSource *self, GtkWidget *widget);

/* A process-wide source for widgets that are built deep inside other
 * nostr-gtk widgets (note cards, profile panes) and cannot be handed one.
 * Unset (NULL) by default: such widgets then load nothing by themselves.
 * The library never consults it for widgets the host creates directly. */
void gn_media_source_set_default(GnMediaSource *source);
/* (transfer none) (nullable) */
GnMediaSource *gn_media_source_get_default(void);

G_END_DECLS
#endif
