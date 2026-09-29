#include "gh-metadata-strip.h"

#include <string.h>

static const guint8 png_signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

/* PNG ancillary chunks that only affect how the image is drawn; every other
 * ancillary chunk (text, time, EXIF, private chunks) is dropped. */
static const gchar *const png_render_chunks[] = {
  "cHRM", "gAMA", "iCCP", "sBIT", "sRGB", "cICP", "mDCv", "cLLi", "bKGD", "tRNS", "pHYs",
  "acTL", "fcTL", "fdAT", NULL
};

/* Output buffer whose contents (plaintext) are wiped whenever it is freed. */
typedef struct {
  guint8 *data;
  gsize len;
  gsize cap;
} Out;

static void
wipe(gpointer data, gsize size)
{
  volatile guint8 *p = data;
  while (size--)
    *p++ = 0;
}

typedef struct {
  guint8 *data;
  gsize size;
} Secret;

static void
secret_free(gpointer data)
{
  Secret *secret = data;
  wipe(secret->data, secret->size);
  g_free(secret->data);
  g_free(secret);
}

static void
out_append(Out *out, const guint8 *data, gsize size)
{
  g_assert(out->len + size <= out->cap);
  memcpy(out->data + out->len, data, size);
  out->len += size;
}

static void
out_clear(Out *out)
{
  if (out->data)
    wipe(out->data, out->cap);
  g_free(out->data);
  out->data = NULL;
}

static GBytes *
out_to_bytes(Out *out)
{
  Secret *secret = g_new0(Secret, 1);
  secret->data = g_steal_pointer(&out->data);
  secret->size = out->cap;
  return g_bytes_new_with_free_func(secret->data, out->len, secret_free, secret);
}

GhMediaFormat
gh_media_sniff(GBytes *bytes)
{
  gsize size = 0;
  const guint8 *data = bytes ? g_bytes_get_data(bytes, &size) : NULL;
  if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
    return GH_MEDIA_FORMAT_JPEG;
  if (size >= sizeof png_signature && memcmp(data, png_signature, sizeof png_signature) == 0)
    return GH_MEDIA_FORMAT_PNG;
  return GH_MEDIA_FORMAT_OTHER;
}

const gchar *
gh_media_format_mime(GhMediaFormat format)
{
  switch (format) {
  case GH_MEDIA_FORMAT_JPEG:
    return "image/jpeg";
  case GH_MEDIA_FORMAT_PNG:
    return "image/png";
  default:
    return NULL;
  }
}

static gboolean
damaged(GError **error, const gchar *what)
{
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
              "This %s is damaged or incomplete, so its hidden details can't be removed", what);
  return FALSE;
}

static guint32
be32(const guint8 *p)
{
  return ((guint32)p[0] << 24) | ((guint32)p[1] << 16) | ((guint32)p[2] << 8) | p[3];
}

/* ---- JPEG ------------------------------------------------------------------------ */

static gboolean
jpeg_sof(guint8 marker)
{
  return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}

/* What happens to one segment. */
typedef enum { SEGMENT_DROP, SEGMENT_KEEP, SEGMENT_DAMAGED } Segment;

static gboolean
dqt_valid(const guint8 *p, gsize n)
{
  while (n > 0) {
    gsize table = (p[0] >> 4) ? 128 : 64;
    if ((p[0] >> 4) > 1 || n < 1 + table)
      return FALSE;
    p += 1 + table;
    n -= 1 + table;
  }
  return TRUE;
}

static gboolean
dht_valid(const guint8 *p, gsize n)
{
  while (n > 0) {
    if (n < 17)
      return FALSE;
    gsize values = 0;
    for (guint i = 1; i <= 16; i++)
      values += p[i];
    if (n < 17 + values)
      return FALSE;
    p += 17 + values;
    n -= 17 + values;
  }
  return TRUE;
}

/* A kept segment must be exactly what its marker says, so no segment carries
 * bytes beyond its own structure (e.g. a length that swallows the metadata
 * segment after it). keep_size receives how much of the payload is kept:
 * APP0 JFIF and APP14 Adobe are cut to their fixed headers (the JFIF
 * thumbnail goes); everything else whole. COM, every other APPn, the
 * reserved and JPGn markers are dropped; hierarchical JPEG (DHP, EXP) and
 * anything malformed is refused. */
static Segment
jpeg_segment(guint8 marker, const guint8 *payload, gsize size, gsize *keep_size)
{
  *keep_size = size;
  if (marker == 0xFE || marker < 0xC0 || (marker >= 0xF0 && marker <= 0xFD))
    return SEGMENT_DROP; /* COM, reserved, JPGn */
  if (marker >= 0xE0 && marker <= 0xEF) {
    if (marker == 0xE0 && size >= 14 && memcmp(payload, "JFIF\0", 5) == 0) {
      *keep_size = 14;   /* version, units, density; no thumbnail */
      return SEGMENT_KEEP;
    }
    if (marker == 0xEE && size >= 12 && memcmp(payload, "Adobe", 5) == 0) {
      *keep_size = 12;   /* version, flags, colour transform */
      return SEGMENT_KEEP;
    }
    if (marker == 0xE2 && size >= 14 && memcmp(payload, "ICC_PROFILE\0", 12) == 0 &&
        payload[12] >= 1 && payload[12] <= payload[13]) {
      /* A single-chunk profile must be exactly its declared size. */
      if (payload[13] == 1 && (size < 18 || be32(payload + 14) != size - 14))
        return SEGMENT_DROP;
      return SEGMENT_KEEP;
    }
    return SEGMENT_DROP; /* APP1 EXIF/XMP, APP13 IPTC, and the rest */
  }
  switch (marker) {
  case 0xDB:
    return dqt_valid(payload, size) ? SEGMENT_KEEP : SEGMENT_DAMAGED;
  case 0xC4:
    return dht_valid(payload, size) ? SEGMENT_KEEP : SEGMENT_DAMAGED;
  case 0xDA:
    return size >= 1 && payload[0] >= 1 && size == 1 + 2 * (gsize)payload[0] + 3
             ? SEGMENT_KEEP : SEGMENT_DAMAGED;
  case 0xDD:
  case 0xDC:
    return size == 2 ? SEGMENT_KEEP : SEGMENT_DAMAGED;
  case 0xCC:
    return size % 2 == 0 ? SEGMENT_KEEP : SEGMENT_DAMAGED;
  case 0xDE:
  case 0xDF:
    return SEGMENT_DAMAGED; /* hierarchical: not decoded, not passed on */
  default: /* SOFn */
    return size >= 6 && payload[5] >= 1 && size == 6 + 3 * (gsize)payload[5]
             ? SEGMENT_KEEP : SEGMENT_DAMAGED;
  }
}

/* Walks the JPEG. out (nullable) receives the kept segments; width/height
 * the first frame header's. */
static gboolean
jpeg_walk(const guint8 *d, gsize n, Out *out, guint *width, guint *height, GError **error)
{
  gboolean have_size = FALSE;
  if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
    return damaged(error, "JPEG");
  if (out)
    out_append(out, d, 2);
  gsize pos = 2;
  for (;;) {
    if (pos >= n || d[pos] != 0xFF)
      return damaged(error, "JPEG");
    while (pos < n && d[pos] == 0xFF)
      pos++; /* fill bytes */
    if (pos >= n)
      return damaged(error, "JPEG");
    guint8 marker = d[pos++];
    if (marker == 0xD9) { /* EOI: anything after it is dropped */
      if (out)
        out_append(out, (const guint8[]){ 0xFF, 0xD9 }, 2);
      break;
    }
    if (marker == 0x00 || marker == 0xD8)
      return damaged(error, "JPEG");
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { /* standalone */
      if (out)
        out_append(out, (const guint8[]){ 0xFF, marker }, 2);
      continue;
    }
    if (pos + 2 > n)
      return damaged(error, "JPEG");
    gsize length = ((gsize)d[pos] << 8) | d[pos + 1];
    if (length < 2 || pos + length > n)
      return damaged(error, "JPEG");
    const guint8 *payload = d + pos + 2;
    gsize payload_size = length - 2;
    gsize keep = 0;
    Segment fate = jpeg_segment(marker, payload, payload_size, &keep);
    if (fate == SEGMENT_DAMAGED)
      return damaged(error, "JPEG");
    if (jpeg_sof(marker)) {
      if (have_size) /* one frame only */
        return damaged(error, "JPEG");
      *height = ((guint)payload[1] << 8) | payload[2];
      *width = ((guint)payload[3] << 8) | payload[4];
      have_size = TRUE;
    }
    if (out && fate == SEGMENT_KEEP) {
      guint8 head[4] = { 0xFF, marker, (guint8)((keep + 2) >> 8), (guint8)((keep + 2) & 0xff) };
      out_append(out, head, sizeof head);
      if (marker == 0xE0) {
        /* JFIF without its thumbnail: say so (0 x 0). */
        out_append(out, payload, 12);
        out_append(out, (const guint8[]){ 0, 0 }, 2);
      } else {
        out_append(out, payload, keep);
      }
    }
    pos += length;
    if (marker != 0xDA)
      continue;
    if (!have_size)
      return damaged(error, "JPEG"); /* a scan before its frame */
    /* Entropy-coded data up to the next marker: FF 00 is a stuffed byte,
     * FF D0..D7 a restart marker inside the scan, FF FF fill. */
    gsize start = pos;
    for (;;) {
      if (pos + 1 >= n)
        return damaged(error, "JPEG");
      if (d[pos] != 0xFF) {
        pos++;
        continue;
      }
      guint8 next = d[pos + 1];
      if (next == 0x00 || (next >= 0xD0 && next <= 0xD7))
        pos += 2;
      else if (next == 0xFF)
        pos++;
      else
        break;
    }
    if (out)
      out_append(out, d + start, pos - start);
  }
  if (!have_size)
    return damaged(error, "JPEG");
  return TRUE;
}

/* ---- PNG ------------------------------------------------------------------------- */

/* CRC-32 (ISO 3309, as PNG uses it) of the chunk type and data. */
static guint32
png_crc(const guint8 *data, gsize size)
{
  static guint32 table[256];
  static gsize ready = 0;
  if (g_once_init_enter(&ready)) {
    for (guint32 n = 0; n < 256; n++) {
      guint32 c = n;
      for (guint k = 0; k < 8; k++)
        c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
      table[n] = c;
    }
    g_once_init_leave(&ready, 1);
  }
  guint32 c = 0xffffffffu;
  for (gsize i = 0; i < size; i++)
    c = table[(c ^ data[i]) & 0xff] ^ (c >> 8);
  return c ^ 0xffffffffu;
}

static gboolean
png_type_valid(const guint8 *type)
{
  for (guint i = 0; i < 4; i++)
    if (!g_ascii_isalpha(type[i]))
      return FALSE;
  return TRUE;
}

/* PNG's critical chunks. Any other critical chunk makes a PNG undecodable
 * (a decoder must refuse it), so such a file is refused, not passed on with
 * whatever it carries. */
static const gchar *const png_critical_chunks[] = { "IHDR", "PLTE", "IDAT", "IEND", NULL };

static gboolean
png_critical_known(const guint8 *type)
{
  for (guint i = 0; png_critical_chunks[i]; i++)
    if (memcmp(type, png_critical_chunks[i], 4) == 0)
      return TRUE;
  return FALSE;
}

static gboolean
png_keep(const guint8 *type)
{
  if (g_ascii_isupper(type[0]))
    return TRUE; /* critical, known (png_walk refuses the rest) */
  for (guint i = 0; png_render_chunks[i]; i++)
    if (memcmp(type, png_render_chunks[i], 4) == 0)
      return TRUE;
  return FALSE;
}

static gboolean
png_walk(const guint8 *d, gsize n, Out *out, guint *width, guint *height, GError **error)
{
  if (n < sizeof png_signature || memcmp(d, png_signature, sizeof png_signature) != 0)
    return damaged(error, "PNG");
  if (out)
    out_append(out, d, sizeof png_signature);
  gsize pos = sizeof png_signature;
  gboolean first = TRUE;
  for (;;) {
    if (pos + 12 > n)
      return damaged(error, "PNG");
    guint32 length = be32(d + pos);
    const guint8 *type = d + pos + 4;
    if (length > 0x7fffffffu || (gsize)length > n - pos - 12 || !png_type_valid(type) ||
        (g_ascii_isupper(type[0]) && !png_critical_known(type)))
      return damaged(error, "PNG");
    if (first) {
      if (memcmp(type, "IHDR", 4) != 0 || length != 13)
        return damaged(error, "PNG");
      *width = be32(d + pos + 8);
      *height = be32(d + pos + 12);
      first = FALSE;
    }
    gsize chunk = 12 + (gsize)length;
    /* A kept chunk must be intact: its CRC covers type and data, so a
     * length that swallows the next chunk does not pass. */
    gboolean keep = png_keep(type);
    if (keep && png_crc(type, 4 + (gsize)length) != be32(d + pos + 8 + length))
      return damaged(error, "PNG");
    if (out && keep)
      out_append(out, d + pos, chunk);
    pos += chunk;
    if (memcmp(type, "IEND", 4) == 0)
      break; /* anything after it is dropped */
  }
  return TRUE;
}

/* ---- public ---------------------------------------------------------------------- */

gboolean
gh_media_probe_dimensions(GBytes *bytes, GhMediaFormat *out_format, guint *out_width,
                          guint *out_height, GError **error)
{
  g_return_val_if_fail(bytes != NULL, FALSE);
  GhMediaFormat format = gh_media_sniff(bytes);
  if (out_format)
    *out_format = format;
  gsize n = 0;
  const guint8 *d = g_bytes_get_data(bytes, &n);
  guint width = 0, height = 0;
  gboolean ok;
  switch (format) {
  case GH_MEDIA_FORMAT_JPEG:
    ok = jpeg_walk(d, n, NULL, &width, &height, error);
    break;
  case GH_MEDIA_FORMAT_PNG:
    /* The header is enough: IHDR is the first chunk. */
    if (n < 33 || memcmp(d + 12, "IHDR", 4) != 0 || be32(d + 8) != 13)
      ok = damaged(error, "PNG");
    else {
      width = be32(d + 16);
      height = be32(d + 20);
      ok = TRUE;
    }
    break;
  default:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "Only PNG and JPEG images can be previewed");
    return FALSE;
  }
  if (ok && (width == 0 || height == 0))
    ok = damaged(error, format == GH_MEDIA_FORMAT_PNG ? "PNG" : "JPEG");
  if (ok && out_width)
    *out_width = width;
  if (ok && out_height)
    *out_height = height;
  return ok;
}

GBytes *
gh_metadata_strip(GBytes *bytes, GhMediaFormat *out_format, gboolean *out_stripped,
                  GError **error)
{
  g_return_val_if_fail(bytes != NULL, NULL);
  GhMediaFormat format = gh_media_sniff(bytes);
  if (out_format)
    *out_format = format;
  if (out_stripped)
    *out_stripped = FALSE;
  if (format == GH_MEDIA_FORMAT_OTHER)
    return g_bytes_ref(bytes);
  gsize n = 0;
  const guint8 *d = g_bytes_get_data(bytes, &n);
  Out out = { g_malloc(n), 0, n }; /* the output never grows */
  guint width = 0, height = 0;
  gboolean ok = format == GH_MEDIA_FORMAT_JPEG ? jpeg_walk(d, n, &out, &width, &height, error)
                                               : png_walk(d, n, &out, &width, &height, error);
  if (!ok) {
    out_clear(&out);
    return NULL;
  }
  if (out_stripped)
    *out_stripped = TRUE;
  return out_to_bytes(&out);
}
