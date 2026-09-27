/* ns-strip-isobmff.h - ISO base media file format metadata stripper (private)
 *
 * SPDX-License-Identifier: MIT
 *
 * Used by ns-strip.c for MP4 / QuickTime / 3GP / M4A and HEIF / HEIC /
 * AVIF (bead nostrc-wu3s). See ns-strip.h for what is removed.
 */
#ifndef NS_STRIP_ISOBMFF_H
#define NS_STRIP_ISOBMFF_H

#include "ns-strip.h"

G_BEGIN_DECLS

/* TRUE for a MIME type this walker handles; @is_image says whether the
 * top-level `meta` box is the file's structure (HEIF/AVIF items). */
gboolean ns_isobmff_mime(const gchar *mime, gboolean *is_image);

NsStripResult ns_strip_isobmff(gboolean      is_image,
                               const guint8 *data,
                               gsize         len,
                               GBytes      **out,
                               guint        *n_removed);

/* HEIF/AVIF: the largest `ispe` (the full image; thumbnails and grid tiles
 * are smaller). Video: the largest `tkhd` width x height. */
gboolean ns_isobmff_dimensions(gboolean      is_image,
                               const guint8 *data,
                               gsize         len,
                               guint        *width,
                               guint        *height);

G_END_DECLS

#endif /* NS_STRIP_ISOBMFF_H */
