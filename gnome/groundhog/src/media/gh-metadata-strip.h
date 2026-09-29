#ifndef GH_METADATA_STRIP_H
#define GH_METADATA_STRIP_H

#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * Metadata stripping for attachments (privacy charter §6 "Strip metadata",
 * D6: mandatory for JPEG and PNG). The file is rewritten segment by segment
 * and chunk by chunk, never re-encoded, so the image data is byte-identical.
 *
 *  - JPEG: APP1 (EXIF, XMP: camera, GPS position, time), APP13 (IPTC,
 *    Photoshop) and COM are removed, as the charter requires, and so is every
 *    other APPn except APP0 JFIF and APP14 Adobe, cut to their fixed headers
 *    (the JFIF thumbnail goes), and an APP2 ICC profile (colours), which
 *    decoders need; reserved and JPGn segments and anything after EOI (phone
 *    trailers, appended videos or depth maps) go too. Markers between scans
 *    are handled, so a progressive JPEG's later segments are stripped as
 *    well. Every kept segment must be exactly its structure (quantization
 *    and Huffman tables, frame and scan headers), so none can carry the
 *    bytes of a segment after it; one frame only (hierarchical JPEG is
 *    refused).
 *  - PNG: tEXt, iTXt, zTXt, eXIf and tIME (the charter's list) are removed,
 *    and so is every other ancillary chunk except the rendering ones listed
 *    in gh-metadata-strip.c (colour, transparency, pixel size, animation),
 *    each of which must pass its CRC;
 *    the four critical chunks are kept, and nothing after IEND. A PNG with
 *    another critical chunk cannot be decoded and is refused.
 *  - Anything else is returned unchanged with *out_stripped FALSE: the UI
 *    shows the one-time "Files can contain hidden details such as location."
 *
 * The format comes from the magic bytes, never from a name or MIME type. A
 * JPEG or PNG that cannot be walked to its end (truncated, a bad length) is
 * refused with G_IO_ERROR_INVALID_DATA rather than sent with its metadata.
 * Memory only; the stripped copy is wiped when freed.
 */

typedef enum {
  GH_MEDIA_FORMAT_OTHER,
  GH_MEDIA_FORMAT_JPEG,
  GH_MEDIA_FORMAT_PNG
} GhMediaFormat;

GhMediaFormat gh_media_sniff(GBytes *bytes);
/* "image/jpeg", "image/png", or NULL for OTHER. */
const gchar *gh_media_format_mime(GhMediaFormat format);

/* The pixel size a JPEG (first SOFn) or PNG (IHDR) header declares, read
 * without decoding anything. G_IO_ERROR_NOT_SUPPORTED for another format,
 * G_IO_ERROR_INVALID_DATA when the header is missing or malformed. */
gboolean gh_media_probe_dimensions(GBytes *bytes, GhMediaFormat *out_format, guint *out_width,
                                   guint *out_height, GError **error);

/* The file without its metadata (see above), a new reference. out_format and
 * out_stripped are nullable. */
GBytes *gh_metadata_strip(GBytes *bytes, GhMediaFormat *out_format, gboolean *out_stripped,
                          GError **error);

G_END_DECLS
#endif
