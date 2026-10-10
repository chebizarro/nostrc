/*
 * gnostr-media-source.h - Gnostr policy and plumbing for nostr-gtk media
 * widgets (nostrc-8xfib.4).
 *
 * Implements GnMediaSource for GnMediaViewer and GnVideoPlayer:
 *  - policy: "load-remote-media" on -> ALLOW (images and videos load when
 *    shown, as Gnostr always did); off -> images ASK (the viewer offers Load)
 *    and videos BLOCKED ("Remote media loading is disabled");
 *  - images: fetched with the shared SoupSession, HTTP status and empty
 *    bodies checked, at most max_bytes kept;
 *  - videos: handed to GTK media backend as a GFile for the URL (unchanged
 *    Gnostr behaviour: the backend streams it itself);
 *  - adopt: binds a player autoplay and loop to org.gnostr.Client
 *    video-autoplay and video-loop, gives the viewer the WebP/AVIF fallback
 *    Gnostr always decoded, and turns "link-copied" into a toast.
 */
#pragma once

#include <nostr-gtk-1.0/gn-media-source.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_MEDIA_SOURCE (gnostr_media_source_get_type())
G_DECLARE_FINAL_TYPE(GnostrMediaSource, gnostr_media_source, GNOSTR, MEDIA_SOURCE, GObject)

GnostrMediaSource *gnostr_media_source_new(void);
/* The process-wide instance, installed as gn_media_source_get_default() by
 * gnostr_media_source_install(). (transfer none) */
GnMediaSource *gnostr_media_source_get(void);
void gnostr_media_source_install(void);

G_END_DECLS
