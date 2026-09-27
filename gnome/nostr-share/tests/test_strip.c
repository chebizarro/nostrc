/* test_strip.c - metadata stripper (JPEG/PNG/WebP/GIF/ISO-BMFF) + dimensions
 *
 * SPDX-License-Identifier: MIT
 *
 * Fixtures are built byte-by-byte so the test needs no binary assets.
 * Each one carries a recognisable "GPS" marker string inside the
 * metadata we expect to be removed.
 */
#include "ns-strip.h"

#include <glib.h>
#include <string.h>

#define SECRET "GPSLatitude=48.8584N"

static gboolean
contains(GBytes *b, const gchar *needle)
{
  gsize n = 0;
  const guint8 *d = g_bytes_get_data(b, &n);
  gsize m = strlen(needle);
  for (gsize i = 0; i + m <= n; i++)
    if (memcmp(d + i, needle, m) == 0)
      return TRUE;
  return FALSE;
}

static void
seg(GByteArray *a, guint8 marker, const void *payload, gsize plen)
{
  guint8 h[4] = { 0xFF, marker, (guint8)((plen + 2) >> 8), (guint8)((plen + 2) & 0xFF) };
  g_byte_array_append(a, h, 4);
  g_byte_array_append(a, payload, (guint)plen);
}

static GByteArray *
make_jpeg(void)
{
  GByteArray *a = g_byte_array_new();
  const guint8 soi[2] = { 0xFF, 0xD8 };
  g_byte_array_append(a, soi, 2);
  seg(a, 0xE0, "JFIF\0\1\1\0\0\1\0\1\0\0", 14);                 /* keep */
  g_autofree gchar *exif = g_strdup_printf("Exif%c%c" SECRET, 0, 0);
  seg(a, 0xE1, exif, 6 + strlen(SECRET));                        /* drop */
  seg(a, 0xE1, "http://ns.adobe.com/xap/1.0/\0<x:xmpmeta>" SECRET, 40 + strlen(SECRET)); /* drop */
  seg(a, 0xE2, "ICC_PROFILE\0\1\1profile", 21);                  /* keep */
  seg(a, 0xE2, "MPF\0" SECRET, 4 + strlen(SECRET));              /* drop */
  seg(a, 0xED, "Photoshop 3.0\0" SECRET, 14 + strlen(SECRET));   /* drop */
  seg(a, 0xFE, SECRET, strlen(SECRET));                          /* drop */
  seg(a, 0xDB, "\0qtable", 7);                                   /* keep */
  const guint8 sof[] = { 8, 0x01, 0xE0, 0x02, 0x80, 1, 1, 0x11, 0 };  /* 640x480 */
  seg(a, 0xC0, sof, sizeof(sof));
  const guint8 sos[] = { 1, 1, 0, 0, 0x3F, 0 };
  seg(a, 0xDA, sos, sizeof(sos));
  /* entropy data with a stuffed FF00 and an RST marker */
  const guint8 scan[] = { 0x12, 0xFF, 0x00, 0x34, 0xFF, 0xD0, 0x56 };
  g_byte_array_append(a, scan, sizeof(scan));
  const guint8 eoi[2] = { 0xFF, 0xD9 };
  g_byte_array_append(a, eoi, 2);
  /* MPF secondary image / trailer after EOI */
  g_byte_array_append(a, (const guint8 *)"\xFF\xD8" SECRET, 2 + strlen(SECRET));
  return a;
}

static void
test_jpeg(void)
{
  g_autoptr(GByteArray) a = make_jpeg();
  guint w = 0, h = 0;
  g_assert_true(ns_image_dimensions("image/jpeg", a->data, a->len, &w, &h));
  g_assert_cmpuint(w, ==, 640);
  g_assert_cmpuint(h, ==, 480);

  GBytes *out = NULL;
  guint removed = 0;
  g_assert_cmpint(ns_strip_metadata("image/jpeg", a->data, a->len, &out, &removed),
                  ==, NS_STRIP_OK);
  g_assert_false(contains(out, SECRET));
  g_assert_false(contains(out, "Exif"));
  g_assert_false(contains(out, "xmpmeta"));
  g_assert_true(contains(out, "JFIF"));
  g_assert_true(contains(out, "ICC_PROFILE"));
  g_assert_true(contains(out, "qtable"));
  g_assert_cmpuint(removed, ==, 6);   /* 2×APP1, MPF, APP13, COM, trailer */

  gsize n = 0;
  const guint8 *d = g_bytes_get_data(out, &n);
  g_assert_cmpuint(d[n - 2], ==, 0xFF);
  g_assert_cmpuint(d[n - 1], ==, 0xD9);
  /* entropy-coded bytes survive verbatim */
  const guint8 scan[] = { 0x12, 0xFF, 0x00, 0x34, 0xFF, 0xD0, 0x56, 0xFF, 0xD9 };
  g_assert_cmpmem(d + n - sizeof(scan), sizeof(scan), scan, sizeof(scan));
  /* the cleaned file still parses and reports the same dimensions */
  g_assert_true(ns_image_dimensions("image/jpeg", d, n, &w, &h));
  g_assert_cmpuint(w, ==, 640);

  /* idempotent */
  GBytes *again = NULL;
  g_assert_cmpint(ns_strip_metadata("image/jpeg", d, n, &again, &removed), ==, NS_STRIP_OK);
  g_assert_cmpuint(removed, ==, 0);
  g_assert_true(g_bytes_equal(out, again));
  g_bytes_unref(again);
  g_bytes_unref(out);

  /* truncated inside a segment → malformed, never a partial copy */
  g_assert_cmpint(ns_strip_metadata("image/jpeg", a->data, 30, &out, NULL),
                  ==, NS_STRIP_MALFORMED);
  g_assert_null(out);
}

static void
png_chunk(GByteArray *a, const gchar *type, const void *data, guint32 len)
{
  guint8 l[4] = { (guint8)(len >> 24), (guint8)(len >> 16), (guint8)(len >> 8), (guint8)len };
  g_byte_array_append(a, l, 4);
  g_byte_array_append(a, (const guint8 *)type, 4);
  g_byte_array_append(a, data, len);
  const guint8 crc[4] = { 0, 0, 0, 0 };    /* not checked by the stripper */
  g_byte_array_append(a, crc, 4);
}

static void
test_png(void)
{
  GByteArray *a = g_byte_array_new();
  g_byte_array_append(a, (const guint8 *)"\x89PNG\r\n\x1a\n", 8);
  const guint8 ihdr[13] = { 0, 0, 1, 0, 0, 0, 0, 200, 8, 6, 0, 0, 0 };   /* 256x200 */
  png_chunk(a, "IHDR", ihdr, 13);
  png_chunk(a, "tEXt", "Comment\0" SECRET, 8 + (guint32)strlen(SECRET));
  png_chunk(a, "eXIf", "MM\0*" SECRET, 4 + (guint32)strlen(SECRET));
  png_chunk(a, "iTXt", "XML:com.adobe.xmp\0" SECRET, 18 + (guint32)strlen(SECRET));
  png_chunk(a, "tIME", "\x07\xe8\1\1\0\0\0", 7);
  png_chunk(a, "iCCP", "icc\0\0data", 9);
  png_chunk(a, "IDAT", "pixels", 6);
  png_chunk(a, "IEND", "", 0);
  g_byte_array_append(a, (const guint8 *)SECRET, strlen(SECRET));   /* trailer */

  guint w, h;
  g_assert_true(ns_image_dimensions("image/png", a->data, a->len, &w, &h));
  g_assert_cmpuint(w, ==, 256);
  g_assert_cmpuint(h, ==, 200);

  GBytes *out = NULL;
  guint removed = 0;
  g_assert_cmpint(ns_strip_metadata("image/png", a->data, a->len, &out, &removed), ==, NS_STRIP_OK);
  g_assert_cmpuint(removed, ==, 4);
  g_assert_false(contains(out, SECRET));
  g_assert_false(contains(out, "tIME"));
  g_assert_true(contains(out, "iCCP"));
  g_assert_true(contains(out, "IDAT"));
  g_assert_true(contains(out, "IEND"));
  g_bytes_unref(out);

  /* no IEND → malformed */
  g_assert_cmpint(ns_strip_metadata("image/png", a->data, 8 + 25, &out, NULL), ==,
                  NS_STRIP_MALFORMED);
  g_byte_array_unref(a);
}

static void
riff_chunk(GByteArray *a, const gchar *fourcc, const void *data, guint32 len)
{
  g_byte_array_append(a, (const guint8 *)fourcc, 4);
  guint8 l[4] = { (guint8)len, (guint8)(len >> 8), (guint8)(len >> 16), (guint8)(len >> 24) };
  g_byte_array_append(a, l, 4);
  g_byte_array_append(a, data, len);
  if (len & 1) {
    const guint8 pad = 0;
    g_byte_array_append(a, &pad, 1);
  }
}

static void
test_webp(void)
{
  GByteArray *a = g_byte_array_new();
  g_byte_array_append(a, (const guint8 *)"RIFF\0\0\0\0WEBP", 12);
  /* VP8X: flags EXIF|XMP|ICC, canvas 1024x768 */
  const guint8 vp8x[10] = { 0x08 | 0x04 | 0x20, 0, 0, 0, 0xFF, 0x03, 0, 0xFF, 0x02, 0 };
  riff_chunk(a, "VP8X", vp8x, 10);
  riff_chunk(a, "ICCP", "icc", 3);
  riff_chunk(a, "VP8L", "\x2f\0\0\0\0frame", 10);
  riff_chunk(a, "EXIF", "MM\0*" SECRET, 4 + (guint32)strlen(SECRET));
  riff_chunk(a, "XMP ", "<x:xmpmeta>" SECRET, 11 + (guint32)strlen(SECRET));
  guint32 sz = a->len - 8;
  a->data[4] = (guint8)sz; a->data[5] = (guint8)(sz >> 8);
  a->data[6] = (guint8)(sz >> 16); a->data[7] = (guint8)(sz >> 24);

  guint w, h;
  g_assert_true(ns_image_dimensions("image/webp", a->data, a->len, &w, &h));
  g_assert_cmpuint(w, ==, 1024);
  g_assert_cmpuint(h, ==, 768);

  GBytes *out = NULL;
  guint removed = 0;
  g_assert_cmpint(ns_strip_metadata("image/webp", a->data, a->len, &out, &removed), ==, NS_STRIP_OK);
  g_assert_cmpuint(removed, ==, 2);
  g_assert_false(contains(out, SECRET));
  gsize n = 0;
  const guint8 *d = g_bytes_get_data(out, &n);
  guint32 riff = (guint32)d[4] | ((guint32)d[5] << 8) | ((guint32)d[6] << 16) | ((guint32)d[7] << 24);
  g_assert_cmpuint(riff, ==, n - 8);
  g_assert_cmpuint(d[20] & 0x0C, ==, 0);      /* EXIF/XMP flags cleared */
  g_assert_cmpuint(d[20] & 0x20, ==, 0x20);   /* ICC flag kept */
  g_bytes_unref(out);
  g_byte_array_unref(a);
}

static void
test_gif(void)
{
  GByteArray *a = g_byte_array_new();
  g_byte_array_append(a, (const guint8 *)"GIF89a", 6);
  const guint8 lsd[7] = { 10, 0, 20, 0, 0x80, 0, 0 };   /* 10x20, GCT of 2 entries */
  g_byte_array_append(a, lsd, 7);
  g_byte_array_append(a, (const guint8 *)"\0\0\0\xff\xff\xff", 6);
  /* NETSCAPE2.0 loop block: keep */
  g_byte_array_append(a, (const guint8 *)"\x21\xff\x0bNETSCAPE2.0\x03\x01\0\0\0", 19);
  /* XMP application extension: drop */
  g_byte_array_append(a, (const guint8 *)"\x21\xff\x0bXMP DataXMP", 14);
  guint8 len = (guint8)strlen(SECRET);
  g_byte_array_append(a, &len, 1);
  g_byte_array_append(a, (const guint8 *)SECRET, len);
  g_byte_array_append(a, (const guint8 *)"\0", 1);
  /* comment extension: drop */
  g_byte_array_append(a, (const guint8 *)"\x21\xfe", 2);
  g_byte_array_append(a, &len, 1);
  g_byte_array_append(a, (const guint8 *)SECRET, len);
  g_byte_array_append(a, (const guint8 *)"\0", 1);
  /* image: descriptor, LZW size, one sub-block, terminator */
  g_byte_array_append(a, (const guint8 *)"\x2c\0\0\0\0\x0a\0\x14\0\0", 10);
  g_byte_array_append(a, (const guint8 *)"\x02\x02\x4c\x01\0", 5);
  g_byte_array_append(a, (const guint8 *)"\x3b", 1);

  guint w, h;
  g_assert_true(ns_image_dimensions("image/gif", a->data, a->len, &w, &h));
  g_assert_cmpuint(w, ==, 10);
  g_assert_cmpuint(h, ==, 20);

  GBytes *out = NULL;
  guint removed = 0;
  g_assert_cmpint(ns_strip_metadata("image/gif", a->data, a->len, &out, &removed), ==, NS_STRIP_OK);
  g_assert_cmpuint(removed, ==, 2);
  g_assert_false(contains(out, SECRET));
  g_assert_true(contains(out, "NETSCAPE2.0"));
  g_bytes_unref(out);
  g_byte_array_unref(a);
}

static void
test_unsupported(void)
{
  const guint8 junk[] = "whatever";
  GBytes *out = NULL;
  g_assert_cmpint(ns_strip_metadata("image/tiff", junk, sizeof(junk), &out, NULL), ==,
                  NS_STRIP_UNSUPPORTED);
  g_assert_cmpint(ns_strip_metadata("audio/mpeg", junk, sizeof(junk), &out, NULL), ==,
                  NS_STRIP_UNSUPPORTED);
  g_assert_cmpint(ns_strip_metadata("video/webm", junk, sizeof(junk), &out, NULL), ==,
                  NS_STRIP_UNSUPPORTED);
  g_assert_null(out);
  g_assert_false(ns_strip_supported("image/tiff"));
  g_assert_true(ns_strip_supported("image/jpeg"));
  g_assert_true(ns_strip_supported("video/mp4"));
  g_assert_true(ns_strip_supported("video/quicktime"));
  g_assert_true(ns_strip_supported("image/heic"));
  g_assert_true(ns_strip_supported("image/avif"));
  /* claims JPEG / MP4 / HEIC but is not */
  g_assert_cmpint(ns_strip_metadata("image/jpeg", junk, sizeof(junk), &out, NULL), ==,
                  NS_STRIP_MALFORMED);
  g_assert_cmpint(ns_strip_metadata("video/mp4", junk, sizeof(junk), &out, NULL), ==,
                  NS_STRIP_MALFORMED);
  g_assert_cmpint(ns_strip_metadata("image/heic", junk, sizeof(junk), &out, NULL), ==,
                  NS_STRIP_MALFORMED);
}

/* ---- ISO-BMFF (nostrc-wu3s): fixtures assembled box by box ---- */

#define GPS_ISO6709 "+37.7749-122.4194+010.000/"

static const guint8 XMP_UUID[16] = { 0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8,
                                     0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC };

static GByteArray *
bytes(const void *d, gsize n)
{
  GByteArray *a = g_byte_array_new();
  g_byte_array_append(a, d, (guint)n);
  return a;
}

static GByteArray *
str(const gchar *s)
{
  return bytes(s, strlen(s));
}

static GByteArray *
u32s(guint n, ...)
{
  GByteArray *a = g_byte_array_new();
  va_list ap;
  va_start(ap, n);
  for (guint i = 0; i < n; i++) {
    guint32 v = va_arg(ap, guint32);
    guint8 b[4] = { (guint8)(v >> 24), (guint8)(v >> 16), (guint8)(v >> 8), (guint8)v };
    g_byte_array_append(a, b, 4);
  }
  va_end(ap);
  return a;
}

/* Concatenate (and free) a NULL-terminated list of arrays. */
static GByteArray *
cat(GByteArray *first, ...)
{
  GByteArray *a = g_byte_array_new();
  va_list ap;
  va_start(ap, first);
  for (GByteArray *p = first; p != NULL; p = va_arg(ap, GByteArray *)) {
    g_byte_array_append(a, p->data, p->len);
    g_byte_array_unref(p);
  }
  va_end(ap);
  return a;
}

static GByteArray *
box(const gchar *type, GByteArray *payload)
{
  guint32 n = 8 + (payload ? payload->len : 0);
  guint8 h[8] = { (guint8)(n >> 24), (guint8)(n >> 16), (guint8)(n >> 8), (guint8)n,
                  (guint8)type[0], (guint8)type[1], (guint8)type[2], (guint8)type[3] };
  GByteArray *a = bytes(h, 8);
  if (payload != NULL) {
    g_byte_array_append(a, payload->data, payload->len);
    g_byte_array_unref(payload);
  }
  return a;
}

static GByteArray *
fullbox(const gchar *type, guint8 version, GByteArray *payload)
{
  guint8 vf[4] = { version, 0, 0, 0 };
  return box(type, cat(bytes(vf, 4), payload ? payload : g_byte_array_new(), NULL));
}

static gsize
find(GBytes *b, const void *needle, gsize m)
{
  gsize n = 0;
  const guint8 *d = g_bytes_get_data(b, &n);
  for (gsize i = 0; i + m <= n; i++)
    if (memcmp(d + i, needle, m) == 0)
      return i;
  return (gsize)-1;
}

/* A track: tkhd + mdia(mdhd, hdlr @handler, minf(stbl(stsz, stsc, stco))). */
static GByteArray *
trak(const gchar *handler, const gchar *name, guint32 width, guint32 height,
     guint32 n_samples, const guint32 *sizes, guint32 chunk_off)
{
  GByteArray *tkhd = cat(u32s(5, 0x5F5E1000u, 0x5F5E1000u, 1u, 0u, 1000u),
                         u32s(4, 0u, 0u, 0u, 0u),
                         u32s(9, 0x10000u, 0u, 0u, 0u, 0x10000u, 0u, 0u, 0u, 0x40000000u),
                         u32s(2, width << 16, height << 16), NULL);
  GByteArray *hdlr = cat(u32s(1, 0u), str(handler), u32s(3, 0u, 0u, 0u),
                         bytes(name, strlen(name) + 1), NULL);
  GByteArray *stsz = cat(u32s(2, 0u, n_samples), NULL);
  for (guint32 i = 0; i < n_samples; i++)
    stsz = cat(stsz, u32s(1, sizes[i]), NULL);
  GByteArray *stbl = cat(fullbox("stsz", 0, stsz),
                         fullbox("stsc", 0, u32s(4, 1u, 1u, n_samples, 1u)),
                         fullbox("stco", 0, u32s(2, 1u, chunk_off)), NULL);
  GByteArray *mdia = cat(fullbox("mdhd", 0, u32s(5, 0x5F5E1000u, 0x5F5E1000u, 1000u, 1000u, 0u)),
                         fullbox("hdlr", 0, hdlr),
                         box("minf", box("stbl", stbl)), NULL);
  return box("trak", cat(fullbox("tkhd", 0, tkhd), box("mdia", mdia),
                         box("udta", box("name", str("Main " SECRET))), NULL));
}

/* ftyp + moov(mvhd, video trak, GPS trak, udta, meta, XMP uuid) + free +
 * mdat("VIDEODATA" "GPS1LAT0" "GPS2LON0"). @offs: [video, gps] data
 * offsets (the fixture is built twice: sizes do not depend on them). */
static GByteArray *
make_mp4(const guint32 offs[2], gsize *mdat_payload_at)
{
  static const guint32 vsz[] = { 9 }, gsz[] = { 8, 8 };
  GByteArray *xyz = cat(u32s(1, (guint32)(strlen(GPS_ISO6709) << 16)), str(GPS_ISO6709), NULL);
  GByteArray *udta = cat(box("\xa9xyz", xyz), box("\xa9mak", str("Apple")),
                         box("\xa9mod", str("iPhone " SECRET)), NULL);
  GByteArray *keys = cat(u32s(1, 1u), u32s(1, 8u + 36u), str("mdta"),
                         str("com.apple.quicktime.location.ISO6709"), NULL);
  GByteArray *ilst = box("\0\0\0\1", box("data", cat(u32s(2, 1u, 0u), str(GPS_ISO6709), NULL)));
  GByteArray *qtmeta = fullbox("meta", 0, cat(
    fullbox("hdlr", 0, cat(u32s(1, 0u), str("mdta"), u32s(3, 0u, 0u, 0u), bytes("", 1), NULL)),
    fullbox("keys", 0, keys), box("ilst", ilst), NULL));
  GByteArray *xmp = box("uuid", cat(bytes(XMP_UUID, 16), str("<x:xmpmeta>" SECRET), NULL));
  GByteArray *mvhd = fullbox("mvhd", 0, cat(u32s(4, 0x5F5E1000u, 0x5F5E1000u, 1000u, 1000u),
                                            g_byte_array_sized_new(0), NULL));
  GByteArray *moov = box("moov", cat(mvhd,
                                     trak("vide", "VideoHandler", 640, 480, 1, vsz, offs[0]),
                                     trak("meta", "GoPro MET", 0, 0, 2, gsz, offs[1]),
                                     box("udta", udta), qtmeta, xmp, NULL));
  GByteArray *ftyp = box("ftyp", cat(str("isom"), u32s(1, 0x200u), str("isommp41"), NULL));
  GByteArray *head = cat(ftyp, moov, box("free", str("stale " SECRET)), NULL);
  *mdat_payload_at = head->len + 8;
  return cat(head, box("mdat", str("VIDEODATAGPS1LAT0GPS2LON0")), NULL);
}

static void
test_mp4(void)
{
  gsize at = 0;
  guint32 offs[2] = { 0, 0 };
  g_byte_array_unref(make_mp4(offs, &at));
  offs[0] = (guint32)at;
  offs[1] = (guint32)at + 9;
  g_autoptr(GByteArray) a = make_mp4(offs, &at);
  g_autoptr(GBytes) in = g_bytes_new(a->data, a->len);

  guint w = 0, h = 0;
  g_assert_true(ns_image_dimensions("video/mp4", a->data, a->len, &w, &h));
  g_assert_cmpuint(w, ==, 640);
  g_assert_cmpuint(h, ==, 480);

  GBytes *out = NULL;
  guint removed = 0;
  g_assert_cmpint(ns_strip_metadata("video/mp4", a->data, a->len, &out, &removed), ==,
                  NS_STRIP_OK);
  /* Same size, same offsets: nothing was re-muxed. */
  g_assert_cmpuint(g_bytes_get_size(out), ==, a->len);
  g_assert_cmpuint(find(out, "VIDEODATA", 9), ==, offs[0]);
  /* Gone: GPS (udta ©xyz, QuickTime keys, timed-metadata samples), device,
   * XMP, the stale free box, the metadata track itself. */
  g_assert_false(contains(out, SECRET));
  g_assert_false(contains(out, GPS_ISO6709));
  g_assert_false(contains(out, "ISO6709"));
  g_assert_false(contains(out, "Apple"));
  g_assert_false(contains(out, "xmpmeta"));
  g_assert_false(contains(out, "GPS1LAT0"));
  g_assert_false(contains(out, "GPS2LON0"));
  g_assert_false(contains(out, "GoPro MET"));
  g_assert_true(contains(out, "VideoHandler"));      /* the video track stays */
  g_assert_cmpuint(find(out, "udta", 4), ==, (gsize)-1);
  /* Creation / modification times zeroed. */
  const guint8 when[4] = { 0x5F, 0x5E, 0x10, 0x00 };
  g_assert_cmpuint(find(out, when, 4), ==, (gsize)-1);
  g_assert_cmpuint(removed, >=, 6);
  /* Idempotent: a second pass finds nothing left to remove but times. */
  GBytes *again = NULL;
  guint removed2 = 99;
  gsize n = 0;
  const guint8 *d = g_bytes_get_data(out, &n);
  g_assert_cmpint(ns_strip_metadata("video/mp4", d, n, &again, &removed2), ==, NS_STRIP_OK);
  g_assert_cmpuint(removed2, ==, 0);
  g_assert_true(g_bytes_equal(again, out));
  g_bytes_unref(again);
  g_bytes_unref(out);
}

/* QuickTime with the movie header after the media and a vendor trailer. */
static void
test_mov_trailer(void)
{
  GByteArray *ftyp = box("ftyp", cat(str("qt  "), u32s(1, 0u), str("qt  "), NULL));
  GByteArray *mdat = box("mdat", str("VIDEODATA"));
  guint32 data_at = ftyp->len + 8;
  static const guint32 vsz[] = { 9 };
  GByteArray *moov = box("moov", cat(fullbox("mvhd", 0, u32s(4, 1u, 1u, 600u, 600u)),
                                     trak("vide", "Core Media Video", 1920, 1080, 1, vsz, data_at),
                                     box("udta", box("\xa9xyz", str(GPS_ISO6709))), NULL));
  const gchar *trailer = "\x00\x00\x00\x03SEFH" SECRET;   /* not a box */
  g_autoptr(GByteArray) a = cat(ftyp, mdat, moov, bytes(trailer, 8 + strlen(SECRET)), NULL);
  GBytes *out = NULL;
  guint removed = 0;
  g_assert_cmpint(ns_strip_metadata("video/quicktime", a->data, a->len, &out, &removed), ==,
                  NS_STRIP_OK);
  g_assert_cmpuint(g_bytes_get_size(out), ==, a->len - 8 - strlen(SECRET));
  g_assert_false(contains(out, SECRET));
  g_assert_false(contains(out, GPS_ISO6709));
  g_assert_cmpuint(find(out, "VIDEODATA", 9), ==, data_at);
  g_bytes_unref(out);
}

/* HEIF: Exif item in mdat, XMP item in idat, the image in mdat. */
static GByteArray *
make_heif(const gchar *brand, const gchar *coding, guint32 img_off, guint32 exif_off,
          gsize *mdat_payload_at, guint cm_exif, gboolean exif_past_eof)
{
  static const gchar IMG[] = "HEVCIMAGEDATA";
  g_autofree gchar *exif_s = g_strdup_printf("%c%c%c%cMM%c*" SECRET, 0, 0, 0, 0, 0);
  GByteArray *exif = bytes(exif_s, 8 + strlen(SECRET));
  guint32 exif_len = exif->len + (exif_past_eof ? 1000 : 0);
  const gchar *xmp_s = "<x:xmpmeta>" SECRET "</x:xmpmeta>";
  GByteArray *infe1 = fullbox("infe", 2, cat(u32s(1, (1u << 16) | 0u), str(coding),
                                             bytes("", 1), NULL));
  GByteArray *infe2 = fullbox("infe", 2, cat(u32s(1, (2u << 16) | 0u), str("Exif"),
                                             bytes("", 1), NULL));
  GByteArray *infe3 = fullbox("infe", 2, cat(u32s(1, (3u << 16) | 0u), str("mime"),
                                             bytes("", 1), bytes("application/rdf+xml", 20),
                                             NULL));
  guint8 cnt3[2] = { 0, 3 };
  GByteArray *iinf = fullbox("iinf", 0, cat(bytes(cnt3, 2), infe1, infe2, infe3, NULL));
  guint8 sizes[4] = { 0x44, 0x00, 0, 3 };   /* offset 4, length 4, base 0, idx 0; 3 items */
  guint8 i1[8] = { 0, 1, 0, 0, 0, 0, 0, 1 };           /* id 1, cm 0, dref 0, 1 extent */
  guint8 i2[8] = { 0, 2, 0, (guint8)cm_exif, 0, 0, 0, 1 };
  guint8 i3[8] = { 0, 3, 0, 1, 0, 0, 0, 1 };           /* cm 1: idat */
  GByteArray *iloc = fullbox("iloc", 1, cat(bytes(sizes, 4),
                                            bytes(i1, 8), u32s(2, img_off, (guint32)strlen(IMG)),
                                            bytes(i2, 8), u32s(2, exif_off, exif_len),
                                            bytes(i3, 8), u32s(2, 0u, (guint32)strlen(xmp_s)),
                                            NULL));
  GByteArray *ipco = box("ipco", cat(fullbox("ispe", 0, u32s(2, 4032u, 3024u)),
                                     fullbox("ispe", 0, u32s(2, 320u, 240u)), NULL));
  guint8 pitm[2] = { 0, 1 };
  GByteArray *meta = fullbox("meta", 0, cat(
    fullbox("hdlr", 0, cat(u32s(1, 0u), str("pict"), u32s(3, 0u, 0u, 0u), bytes("", 1), NULL)),
    fullbox("pitm", 0, bytes(pitm, 2)), iinf, iloc, box("iprp", ipco),
    box("idat", str(xmp_s)), NULL));
  GByteArray *ftyp = box("ftyp", cat(str(brand), u32s(1, 0u), str("mif1"), str(brand), NULL));
  GByteArray *head = cat(ftyp, meta, NULL);
  *mdat_payload_at = head->len + 8;
  return cat(head, box("mdat", cat(str(IMG), exif, NULL)), NULL);
}

static void
check_heif(const gchar *mime, const gchar *brand, const gchar *coding)
{
  gsize at = 0;
  g_byte_array_unref(make_heif(brand, coding, 0, 0, &at, 0, FALSE));
  guint32 img_off = (guint32)at, exif_off = (guint32)at + 13;
  g_autoptr(GByteArray) a = make_heif(brand, coding, img_off, exif_off, &at, 0, FALSE);
  guint w = 0, h = 0;
  g_assert_true(ns_image_dimensions(mime, a->data, a->len, &w, &h));
  g_assert_cmpuint(w, ==, 4032);                    /* the image, not the thumbnail */
  g_assert_cmpuint(h, ==, 3024);

  GBytes *out = NULL;
  guint removed = 0;
  g_assert_cmpint(ns_strip_metadata(mime, a->data, a->len, &out, &removed), ==, NS_STRIP_OK);
  g_assert_cmpuint(removed, ==, 2);
  g_assert_cmpuint(g_bytes_get_size(out), ==, a->len);
  g_assert_false(contains(out, SECRET));
  g_assert_false(contains(out, "xmpmeta"));
  g_assert_cmpuint(find(out, "HEVCIMAGEDATA", 13), ==, img_off);   /* pixels untouched */
  /* The Exif item now reads as an empty, valid Exif block. */
  static const guint8 empty_exif[18] = { 0, 0, 0, 0, 'M', 'M', 0, 42, 0, 0, 0, 8,
                                         0, 0, 0, 0, 0, 0 };
  g_assert_cmpuint(find(out, empty_exif, sizeof(empty_exif)), ==, exif_off);
  g_assert_true(contains(out, "infe"));             /* item structure intact */
  g_bytes_unref(out);
}

static void
test_heic_avif(void)
{
  check_heif("image/heic", "heic", "hvc1");
  check_heif("image/avif", "avif", "av01");
}

static void
test_isobmff_refusals(void)
{
  GBytes *out = NULL;
  /* A movie needs its moov; an image its meta. */
  g_autoptr(GByteArray) only_ftyp = box("ftyp", cat(str("isom"), u32s(1, 0u), NULL));
  g_assert_cmpint(ns_strip_metadata("video/mp4", only_ftyp->data, only_ftyp->len, &out, NULL),
                  ==, NS_STRIP_MALFORMED);
  g_assert_cmpint(ns_strip_metadata("image/heic", only_ftyp->data, only_ftyp->len, &out, NULL),
                  ==, NS_STRIP_MALFORMED);
  /* A box claiming more bytes than the file has. */
  g_autoptr(GByteArray) cut = box("ftyp", cat(str("isom"), u32s(1, 0u), NULL));
  cut->data[3] = 0x7F;
  g_assert_cmpint(ns_strip_metadata("video/mp4", cut->data, cut->len, &out, NULL), ==,
                  NS_STRIP_MALFORMED);
  /* An Exif extent past the end of the file. */
  gsize at = 0;
  g_autoptr(GByteArray) far = make_heif("heic", "hvc1", 0, 0, &at, 0, TRUE);
  g_assert_cmpint(ns_strip_metadata("image/heic", far->data, far->len, &out, NULL), ==,
                  NS_STRIP_MALFORMED);
  /* Item-relative Exif (construction method 2): cannot be cleaned here. */
  g_autoptr(GByteArray) rel = make_heif("heic", "hvc1", 0, 0, &at, 2, FALSE);
  g_assert_cmpint(ns_strip_metadata("image/heic", rel->data, rel->len, &out, NULL), ==,
                  NS_STRIP_UNSUPPORTED);
  /* A compressed movie header cannot be inspected. */
  g_autoptr(GByteArray) cmov = cat(box("ftyp", cat(str("qt  "), u32s(1, 0u), NULL)),
                                   box("moov", box("cmov", str("zlib..."))), NULL);
  g_assert_cmpint(ns_strip_metadata("video/quicktime", cmov->data, cmov->len, &out, NULL), ==,
                  NS_STRIP_UNSUPPORTED);
  /* Timed metadata in a fragmented movie lives in moof/mdat pairs. */
  static const guint32 gsz[] = { 8 };
  g_autoptr(GByteArray) frag = cat(box("ftyp", cat(str("iso6"), u32s(1, 0u), NULL)),
                                   box("moov", trak("meta", "GPMF", 0, 0, 1, gsz, 0)),
                                   box("moof", box("traf", fullbox("tfhd", 0, u32s(1, 1u)))),
                                   box("mdat", str("GPS1LAT0")), NULL);
  g_assert_cmpint(ns_strip_metadata("video/mp4", frag->data, frag->len, &out, NULL), ==,
                  NS_STRIP_UNSUPPORTED);
  g_assert_null(out);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-share/strip/jpeg", test_jpeg);
  g_test_add_func("/nostr-share/strip/png", test_png);
  g_test_add_func("/nostr-share/strip/webp", test_webp);
  g_test_add_func("/nostr-share/strip/gif", test_gif);
  g_test_add_func("/nostr-share/strip/unsupported", test_unsupported);
  g_test_add_func("/nostr-share/strip/mp4", test_mp4);
  g_test_add_func("/nostr-share/strip/mov-trailer", test_mov_trailer);
  g_test_add_func("/nostr-share/strip/heic-avif", test_heic_avif);
  g_test_add_func("/nostr-share/strip/isobmff-refusals", test_isobmff_refusals);
  return g_test_run();
}
