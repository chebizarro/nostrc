/* ns-strip.h - Remove identifying metadata from images before upload
 *
 * SPDX-License-Identifier: MIT
 *
 * GExiv2 is not a dependency anywhere in this tree, so this is a minimal
 * container-level stripper written for nostr-share. It never decodes or
 * re-encodes pixels; it walks the container and drops the segments /
 * chunks that carry EXIF (GPS, camera serial, timestamps), XMP, IPTC and
 * free-text comments:
 *
 *   JPEG  drops APP1 (Exif, XMP), APP3–APP13 and APP15 (incl. IPTC /
 *         Photoshop), APP2 "MPF" and COM; keeps APP0 (JFIF), APP2
 *         ICC_PROFILE and APP14 (Adobe colour transform). Everything
 *         after EOI (MPF secondary images, trailers) is dropped.
 *   PNG   drops eXIf, tEXt, zTXt, iTXt, tIME; stops at IEND.
 *   WebP  drops EXIF and "XMP " chunks, clears the VP8X EXIF/XMP flags
 *         and rewrites the RIFF size.
 *   GIF   drops comment extensions and application extensions other
 *         than the NETSCAPE2.0 / ANIMEXTS1.0 loop block (XMP lives in an
 *         application extension); stops at the trailer.
 *
 * Anything else (HEIC/AVIF/TIFF/SVG images, all video and audio) is
 * reported as NS_STRIP_UNSUPPORTED. Callers must not upload those
 * without an explicit user override (--keep-metadata / the dialog's
 * acknowledgement switch).
 */
#ifndef NS_STRIP_H
#define NS_STRIP_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  NS_STRIP_OK = 0,        /* @out holds the cleaned bytes */
  NS_STRIP_UNSUPPORTED,   /* format not handled; nothing was changed */
  NS_STRIP_MALFORMED,     /* claimed format but the container is corrupt */
} NsStripResult;

gboolean ns_strip_supported(const gchar *mime);

/* @out: (out): cleaned bytes on NS_STRIP_OK. @n_removed: (out) (optional):
 * number of segments/chunks dropped. */
NsStripResult ns_strip_metadata(const gchar  *mime,
                                const guint8 *data,
                                gsize         len,
                                GBytes      **out,
                                guint        *n_removed);

/* Pixel dimensions from the container header (JPEG SOFn, PNG IHDR, GIF
 * logical screen, WebP VP8/VP8L/VP8X). FALSE when unknown. */
gboolean ns_image_dimensions(const gchar  *mime,
                             const guint8 *data,
                             gsize         len,
                             guint        *width,
                             guint        *height);

G_END_DECLS

#endif /* NS_STRIP_H */
