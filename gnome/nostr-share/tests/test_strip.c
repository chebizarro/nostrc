/* test_strip.c - metadata stripper (JPEG/PNG/WebP/GIF) + dimensions
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
  g_assert_cmpint(ns_strip_metadata("image/heic", junk, sizeof(junk), &out, NULL), ==,
                  NS_STRIP_UNSUPPORTED);
  g_assert_cmpint(ns_strip_metadata("video/mp4", junk, sizeof(junk), &out, NULL), ==,
                  NS_STRIP_UNSUPPORTED);
  g_assert_null(out);
  g_assert_false(ns_strip_supported("image/tiff"));
  g_assert_true(ns_strip_supported("image/jpeg"));
  /* claims JPEG but is not */
  g_assert_cmpint(ns_strip_metadata("image/jpeg", junk, sizeof(junk), &out, NULL), ==,
                  NS_STRIP_MALFORMED);
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
  return g_test_run();
}
