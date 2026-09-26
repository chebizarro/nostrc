/* ns-strip.c - Container-level image metadata stripper
 *
 * SPDX-License-Identifier: MIT
 *
 * See ns-strip.h for what is kept and what is dropped. All parsers are
 * bounds-checked against @len; a truncated or inconsistent container is
 * NS_STRIP_MALFORMED rather than a best-effort partial copy, so we never
 * upload something we could not fully walk.
 */
#include "ns-strip.h"

#include <string.h>

static guint16 be16(const guint8 *p) { return (guint16)((p[0] << 8) | p[1]); }
static guint32 be32(const guint8 *p)
{
  return ((guint32)p[0] << 24) | ((guint32)p[1] << 16) | ((guint32)p[2] << 8) | p[3];
}
static guint32 le32(const guint8 *p)
{
  return ((guint32)p[3] << 24) | ((guint32)p[2] << 16) | ((guint32)p[1] << 8) | p[0];
}
static guint32 le24(const guint8 *p)
{
  return ((guint32)p[2] << 16) | ((guint32)p[1] << 8) | p[0];
}
static guint16 le16(const guint8 *p) { return (guint16)((p[1] << 8) | p[0]); }

gboolean
ns_strip_supported(const gchar *mime)
{
  return g_strcmp0(mime, "image/jpeg") == 0 || g_strcmp0(mime, "image/png") == 0 ||
         g_strcmp0(mime, "image/webp") == 0 || g_strcmp0(mime, "image/gif") == 0;
}

/* ---- JPEG ---- */

static gboolean
jpeg_keep_segment(guint8 marker, const guint8 *payload, gsize plen)
{
  if (marker == 0xFE)                     /* COM */
    return FALSE;
  if (marker < 0xE0 || marker > 0xEF)     /* not APPn: structural, keep */
    return TRUE;
  if (marker == 0xE0 || marker == 0xEE)   /* APP0 JFIF/JFXX, APP14 Adobe */
    return TRUE;
  if (marker == 0xE2)                     /* APP2: keep ICC, drop MPF/FPXR */
    return plen >= 12 && memcmp(payload, "ICC_PROFILE\0", 12) == 0;
  return FALSE;                           /* APP1 Exif/XMP, APP13 IPTC, ... */
}

static NsStripResult
strip_jpeg(const guint8 *d, gsize len, GBytes **out, guint *n_removed)
{
  if (len < 4 || d[0] != 0xFF || d[1] != 0xD8)
    return NS_STRIP_MALFORMED;

  GByteArray *o = g_byte_array_sized_new((guint)len);
  g_byte_array_append(o, d, 2);
  gsize i = 2;
  guint removed = 0;

  while (i < len) {
    if (d[i] != 0xFF)
      goto malformed;
    while (i < len && d[i] == 0xFF)      /* fill bytes */
      i++;
    if (i >= len)
      goto malformed;
    guint8 m = d[i++];

    if (m == 0xD9) {                     /* EOI: drop any trailer */
      const guint8 eoi[2] = { 0xFF, 0xD9 };
      g_byte_array_append(o, eoi, 2);
      if (n_removed) *n_removed = removed + (i < len ? 1 : 0);
      *out = g_byte_array_free_to_bytes(o);
      return NS_STRIP_OK;
    }
    if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {  /* standalone markers */
      const guint8 mk[2] = { 0xFF, m };
      g_byte_array_append(o, mk, 2);
      continue;
    }
    if (i + 2 > len)
      goto malformed;
    guint16 seglen = be16(d + i);
    if (seglen < 2 || i + seglen > len)
      goto malformed;
    const guint8 *payload = d + i + 2;
    gsize plen = seglen - 2u;

    if (jpeg_keep_segment(m, payload, plen)) {
      const guint8 mk[2] = { 0xFF, m };
      g_byte_array_append(o, mk, 2);
      g_byte_array_append(o, d + i, seglen);
    } else {
      removed++;
    }
    i += seglen;

    if (m == 0xDA) {
      /* Entropy-coded data follows until the next marker that is neither
       * a stuffed 0xFF00 nor RSTn. Copy it verbatim. */
      gsize start = i;
      while (i + 1 < len) {
        if (d[i] == 0xFF && d[i + 1] != 0x00 &&
            !(d[i + 1] >= 0xD0 && d[i + 1] <= 0xD7) && d[i + 1] != 0xFF)
          break;
        i++;
      }
      if (i + 1 >= len)
        goto malformed;
      g_byte_array_append(o, d + start, (guint)(i - start));
    }
  }

malformed:
  g_byte_array_unref(o);
  return NS_STRIP_MALFORMED;
}

/* ---- PNG ---- */

static const guint8 PNG_SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

static NsStripResult
strip_png(const guint8 *d, gsize len, GBytes **out, guint *n_removed)
{
  if (len < 8 || memcmp(d, PNG_SIG, 8) != 0)
    return NS_STRIP_MALFORMED;
  GByteArray *o = g_byte_array_sized_new((guint)len);
  g_byte_array_append(o, d, 8);
  gsize i = 8;
  guint removed = 0;
  while (i + 12 <= len) {
    guint32 clen = be32(d + i);
    if (clen > len - i - 12)
      break;
    const guint8 *type = d + i + 4;
    gsize total = 12u + clen;
    gboolean drop = memcmp(type, "eXIf", 4) == 0 || memcmp(type, "tEXt", 4) == 0 ||
                    memcmp(type, "zTXt", 4) == 0 || memcmp(type, "iTXt", 4) == 0 ||
                    memcmp(type, "tIME", 4) == 0;
    if (drop)
      removed++;
    else
      g_byte_array_append(o, d + i, (guint)total);
    i += total;
    if (memcmp(type, "IEND", 4) == 0) {
      if (n_removed) *n_removed = removed;
      *out = g_byte_array_free_to_bytes(o);
      return NS_STRIP_OK;
    }
  }
  g_byte_array_unref(o);
  return NS_STRIP_MALFORMED;
}

/* ---- WebP ---- */

static NsStripResult
strip_webp(const guint8 *d, gsize len, GBytes **out, guint *n_removed)
{
  if (len < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WEBP", 4) != 0)
    return NS_STRIP_MALFORMED;
  gsize riff_end = (gsize)le32(d + 4) + 8u;
  if (riff_end > len || riff_end < 12)
    return NS_STRIP_MALFORMED;

  GByteArray *o = g_byte_array_sized_new((guint)len);
  g_byte_array_append(o, d, 12);
  gsize i = 12;
  guint removed = 0;
  gssize vp8x_off = -1;
  while (i + 8 <= riff_end) {
    guint32 clen = le32(d + i + 4);
    gsize padded = (gsize)clen + (clen & 1u);
    if (padded > riff_end - i - 8) {
      g_byte_array_unref(o);
      return NS_STRIP_MALFORMED;
    }
    if (memcmp(d + i, "EXIF", 4) == 0 || memcmp(d + i, "XMP ", 4) == 0) {
      removed++;
    } else {
      if (memcmp(d + i, "VP8X", 4) == 0 && clen >= 10)
        vp8x_off = (gssize)o->len;
      g_byte_array_append(o, d + i, (guint)(8 + padded));
    }
    i += 8 + padded;
  }
  if (i != riff_end) {
    g_byte_array_unref(o);
    return NS_STRIP_MALFORMED;
  }
  if (vp8x_off >= 0)
    o->data[vp8x_off + 8] &= (guint8)~(0x08u | 0x04u);   /* EXIF | XMP flags */
  guint32 riff_size = o->len - 8u;
  o->data[4] = (guint8)(riff_size & 0xFF);
  o->data[5] = (guint8)((riff_size >> 8) & 0xFF);
  o->data[6] = (guint8)((riff_size >> 16) & 0xFF);
  o->data[7] = (guint8)((riff_size >> 24) & 0xFF);
  if (n_removed) *n_removed = removed;
  *out = g_byte_array_free_to_bytes(o);
  return NS_STRIP_OK;
}

/* ---- GIF ---- */

/* Advance past a GIF data sub-block chain starting at @i. Returns the
 * offset after the zero-length terminator, or 0 on truncation. */
static gsize
gif_skip_subblocks(const guint8 *d, gsize len, gsize i)
{
  while (i < len) {
    guint8 n = d[i];
    if (n == 0)
      return i + 1;
    i += 1u + n;
  }
  return 0;
}

static NsStripResult
strip_gif(const guint8 *d, gsize len, GBytes **out, guint *n_removed)
{
  if (len < 13 || (memcmp(d, "GIF87a", 6) != 0 && memcmp(d, "GIF89a", 6) != 0))
    return NS_STRIP_MALFORMED;
  gsize i = 13;
  if (d[10] & 0x80)
    i += 3u * (1u << ((d[10] & 0x07) + 1));
  if (i > len)
    return NS_STRIP_MALFORMED;

  GByteArray *o = g_byte_array_sized_new((guint)len);
  g_byte_array_append(o, d, (guint)i);
  guint removed = 0;
  while (i < len) {
    guint8 b = d[i];
    if (b == 0x3B) {                       /* trailer */
      g_byte_array_append(o, d + i, 1);
      if (n_removed) *n_removed = removed;
      *out = g_byte_array_free_to_bytes(o);
      return NS_STRIP_OK;
    }
    if (b == 0x2C) {                       /* image descriptor */
      if (i + 10 > len) break;
      gsize j = i + 10;
      if (d[i + 9] & 0x80)
        j += 3u * (1u << ((d[i + 9] & 0x07) + 1));
      j += 1;                              /* LZW minimum code size */
      if (j > len) break;
      gsize end = gif_skip_subblocks(d, len, j);
      if (end == 0) break;
      g_byte_array_append(o, d + i, (guint)(end - i));
      i = end;
      continue;
    }
    if (b == 0x21) {                       /* extension */
      if (i + 2 > len) break;
      guint8 label = d[i + 1];
      gsize end = gif_skip_subblocks(d, len, i + 2);
      if (end == 0) break;
      gboolean keep = TRUE;
      if (label == 0xFE) {
        keep = FALSE;                      /* comment */
      } else if (label == 0xFF) {          /* application extension */
        keep = i + 14 <= len && d[i + 2] == 11 &&
               (memcmp(d + i + 3, "NETSCAPE2.0", 11) == 0 ||
                memcmp(d + i + 3, "ANIMEXTS1.0", 11) == 0);
      }
      if (keep)
        g_byte_array_append(o, d + i, (guint)(end - i));
      else
        removed++;
      i = end;
      continue;
    }
    break;
  }
  g_byte_array_unref(o);
  return NS_STRIP_MALFORMED;
}

NsStripResult
ns_strip_metadata(const gchar *mime, const guint8 *data, gsize len,
                  GBytes **out, guint *n_removed)
{
  g_return_val_if_fail(out != NULL, NS_STRIP_MALFORMED);
  *out = NULL;
  if (n_removed) *n_removed = 0;
  if (g_strcmp0(mime, "image/jpeg") == 0) return strip_jpeg(data, len, out, n_removed);
  if (g_strcmp0(mime, "image/png") == 0)  return strip_png(data, len, out, n_removed);
  if (g_strcmp0(mime, "image/webp") == 0) return strip_webp(data, len, out, n_removed);
  if (g_strcmp0(mime, "image/gif") == 0)  return strip_gif(data, len, out, n_removed);
  return NS_STRIP_UNSUPPORTED;
}

/* ---- Dimensions ---- */

static gboolean
jpeg_dims(const guint8 *d, gsize len, guint *w, guint *h)
{
  if (len < 4 || d[0] != 0xFF || d[1] != 0xD8)
    return FALSE;
  gsize i = 2;
  while (i + 4 <= len) {
    if (d[i] != 0xFF)
      return FALSE;
    guint8 m = d[i + 1];
    if (m == 0xFF) { i++; continue; }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
    if (m == 0xD9 || m == 0xDA)
      return FALSE;
    guint16 seglen = be16(d + i + 2);
    if (seglen < 2 || i + 2 + seglen > len)
      return FALSE;
    gboolean sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
    if (sof) {
      if (seglen < 7)
        return FALSE;
      *h = be16(d + i + 5);
      *w = be16(d + i + 7);
      return *w > 0 && *h > 0;
    }
    i += 2u + seglen;
  }
  return FALSE;
}

gboolean
ns_image_dimensions(const gchar *mime, const guint8 *d, gsize len,
                    guint *width, guint *height)
{
  guint w = 0, h = 0;
  gboolean ok = FALSE;
  if (g_strcmp0(mime, "image/jpeg") == 0) {
    ok = jpeg_dims(d, len, &w, &h);
  } else if (g_strcmp0(mime, "image/png") == 0) {
    if (len >= 24 && memcmp(d, PNG_SIG, 8) == 0 && memcmp(d + 12, "IHDR", 4) == 0) {
      w = be32(d + 16);
      h = be32(d + 20);
      ok = TRUE;
    }
  } else if (g_strcmp0(mime, "image/gif") == 0) {
    if (len >= 10 && memcmp(d, "GIF8", 4) == 0) {
      w = le16(d + 6);
      h = le16(d + 8);
      ok = TRUE;
    }
  } else if (g_strcmp0(mime, "image/webp") == 0) {
    if (len >= 30 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) {
      const guint8 *c = d + 12;
      if (memcmp(c, "VP8X", 4) == 0) {
        w = le24(c + 12) + 1;
        h = le24(c + 15) + 1;
        ok = TRUE;
      } else if (memcmp(c, "VP8 ", 4) == 0 && c[11] == 0x9d && c[12] == 0x01 &&
                 c[13] == 0x2a) {
        w = le16(c + 14) & 0x3FFF;
        h = le16(c + 16) & 0x3FFF;
        ok = TRUE;
      } else if (memcmp(c, "VP8L", 4) == 0 && c[8] == 0x2f) {
        guint32 bits = le32(c + 9);
        w = (bits & 0x3FFF) + 1;
        h = ((bits >> 14) & 0x3FFF) + 1;
        ok = TRUE;
      }
    }
  }
  if (!ok || w == 0 || h == 0)
    return FALSE;
  *width = w;
  *height = h;
  return TRUE;
}
