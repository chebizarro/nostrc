/* G21 offline tests (privacy charter §6, §9.2 AT-1, AT-2, AT-4): metadata
 * stripping, the decode guard, AES-256-GCM with x checked before decryption,
 * an independent AES-GCM vector, the kind-15 tags, GhMessage's kind 15 and
 * the encrypted media cache. Nothing here touches the network. */
#include "gh-attachment.h"
#include "gh-message.h"
#include "gh-store-media.h"

#include <glib/gstdio.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <stdlib.h>
#include <string.h>

#define GPS_CANARY "GPS-CANARY-47.3769N-8.5417E"
#define XMP_CANARY "XMP-CANARY-camera-owner"
#define IPTC_CANARY "IPTC-CANARY-byline"
#define COM_CANARY "COM-CANARY-comment"
#define TRAILER_CANARY "TRAILER-CANARY-motion-photo"
#define MPF_CANARY "MPF-CANARY-depth-map"
#define TEXT_CANARY "PNG-TEXT-CANARY-author"
#define EXIF_CANARY "PNG-EXIF-CANARY-gps"
#define PRIVATE_CANARY "PNG-PRIVATE-CANARY"
#define THUMB_CANARY "\x54\x48\x55" /* "THU": the 1x1 JFIF thumbnail's pixel */

static gboolean
contains(GBytes *bytes, const gchar *needle)
{
  gsize size = 0;
  const guint8 *data = g_bytes_get_data(bytes, &size);
  gsize n = strlen(needle);
  for (gsize i = 0; n <= size && i <= size - n; i++)
    if (memcmp(data + i, needle, n) == 0)
      return TRUE;
  return FALSE;
}

static gboolean
contains_raw(GBytes *bytes, const guint8 *needle, gsize n)
{
  gsize size = 0;
  const guint8 *data = g_bytes_get_data(bytes, &size);
  for (gsize i = 0; n <= size && i <= size - n; i++)
    if (memcmp(data + i, needle, n) == 0)
      return TRUE;
  return FALSE;
}

/* ---- synthetic JPEG and PNG ------------------------------------------------------ */

static void
jpeg_segment(GByteArray *out, guint8 marker, const void *payload, gsize size)
{
  guint8 head[4] = { 0xFF, marker, (guint8)((size + 2) >> 8), (guint8)((size + 2) & 0xff) };
  g_byte_array_append(out, head, 4);
  g_byte_array_append(out, payload, (guint)size);
}

static void
jpeg_tagged(GByteArray *out, guint8 marker, const gchar *id, gsize id_size, const gchar *text)
{
  g_autoptr(GByteArray) payload = g_byte_array_new();
  g_byte_array_append(payload, (const guint8 *)id, (guint)id_size);
  g_byte_array_append(payload, (const guint8 *)text, (guint)strlen(text));
  jpeg_segment(out, marker, payload->data, payload->len);
}

/* Entropy-coded bytes with stuffed FF 00 and restart markers, never a real
 * marker. */
static void
jpeg_scan(GByteArray *out, gsize size, guint32 seed)
{
  for (gsize i = 0; i < size; i++) {
    seed = seed * 1103515245u + 12345u;
    guint8 byte = (guint8)(seed >> 16);
    if (i % 4099 == 17) {
      guint8 rst[2] = { 0xFF, (guint8)(0xD0 + (i % 8)) };
      g_byte_array_append(out, rst, 2);
    } else if (byte == 0xFF) {
      guint8 stuffed[2] = { 0xFF, 0x00 };
      g_byte_array_append(out, stuffed, 2);
    } else {
      g_byte_array_append(out, &byte, 1);
    }
  }
}

/* A JPEG of about scan_size bytes, width x height, carrying every kind of
 * metadata the stripper must remove, in and between two scans. */
static GBytes *
make_jpeg(guint width, guint height, gsize scan_size)
{
  GByteArray *out = g_byte_array_new();
  const guint8 soi[2] = { 0xFF, 0xD8 };
  g_byte_array_append(out, soi, 2);
  /* JFIF with a 1x1 thumbnail (3 bytes): kept as a bare 14-byte header. */
  jpeg_segment(out, 0xE0, "JFIF\0\x01\x02\x00\x00\x48\x00\x48\x01\x01" THUMB_CANARY, 14 + 3);
  jpeg_tagged(out, 0xE1, "Exif\0\0", 6, "MM\x00\x2a GPSInfo " GPS_CANARY);
  jpeg_tagged(out, 0xE1, "http://ns.adobe.com/xap/1.0/\0", 29, XMP_CANARY);
  /* One ICC chunk: the profile's own size field (16) matches its bytes. */
  jpeg_segment(out, 0xE2, "ICC_PROFILE\0\x01\x01\x00\x00\x00\x10icc-colours!", 12 + 2 + 16);
  jpeg_tagged(out, 0xE2, "MPF\0", 4, MPF_CANARY);
  jpeg_tagged(out, 0xED, "Photoshop 3.0\0", 14, IPTC_CANARY);
  jpeg_segment(out, 0xEE, "Adobe\x00\x64\x00\x00\x00\x00\x01", 12);
  jpeg_segment(out, 0xFE, COM_CANARY, strlen(COM_CANARY));
  const guint8 dqt[65] = { 0 };
  jpeg_segment(out, 0xDB, dqt, sizeof dqt);
  guint8 sof[15] = { 8, (guint8)(height >> 8), (guint8)height, (guint8)(width >> 8),
                     (guint8)width, 3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1 };
  jpeg_segment(out, 0xC2, sof, sizeof sof); /* progressive */
  const guint8 dht[17] = { 0 };
  jpeg_segment(out, 0xC4, dht, sizeof dht);
  const guint8 sos[10] = { 3, 1, 0, 2, 0x11, 3, 0x11, 0, 63, 0 };
  jpeg_segment(out, 0xDA, sos, sizeof sos);
  jpeg_scan(out, scan_size / 2, 1);
  /* Metadata between scans, then a second scan. */
  jpeg_tagged(out, 0xE1, "Exif\0\0", 6, "late " GPS_CANARY);
  jpeg_segment(out, 0xC4, dht, sizeof dht);
  jpeg_segment(out, 0xDA, sos, sizeof sos);
  jpeg_scan(out, scan_size / 2, 2);
  const guint8 eoi[2] = { 0xFF, 0xD9 };
  g_byte_array_append(out, eoi, 2);
  g_byte_array_append(out, (const guint8 *)TRAILER_CANARY, strlen(TRAILER_CANARY));
  return g_byte_array_free_to_bytes(out);
}

/* PNG's CRC-32, computed independently of the code under test. */
static guint32
crc32_of(const guint8 *data, gsize size)
{
  guint32 c = 0xffffffffu;
  for (gsize i = 0; i < size; i++) {
    c ^= data[i];
    for (guint k = 0; k < 8; k++)
      c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
  }
  return c ^ 0xffffffffu;
}

static void
png_chunk(GByteArray *out, const gchar *type, const void *data, gsize size)
{
  guint8 head[8] = { (guint8)(size >> 24), (guint8)(size >> 16), (guint8)(size >> 8),
                     (guint8)size, (guint8)type[0], (guint8)type[1], (guint8)type[2],
                     (guint8)type[3] };
  g_byte_array_append(out, head, 8);
  if (size)
    g_byte_array_append(out, data, (guint)size);
  g_autoptr(GByteArray) covered = g_byte_array_new();
  g_byte_array_append(covered, head + 4, 4);
  if (size)
    g_byte_array_append(covered, data, (guint)size);
  guint32 c = crc32_of(covered->data, covered->len);
  const guint8 crc[4] = { (guint8)(c >> 24), (guint8)(c >> 16), (guint8)(c >> 8), (guint8)c };
  g_byte_array_append(out, crc, 4);
}

static GBytes *
make_png(guint32 width, guint32 height)
{
  GByteArray *out = g_byte_array_new();
  const guint8 signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
  g_byte_array_append(out, signature, 8);
  guint8 ihdr[13] = { (guint8)(width >> 24), (guint8)(width >> 16), (guint8)(width >> 8),
                      (guint8)width, (guint8)(height >> 24), (guint8)(height >> 16),
                      (guint8)(height >> 8), (guint8)height, 8, 6, 0, 0, 0 };
  png_chunk(out, "IHDR", ihdr, sizeof ihdr);
  png_chunk(out, "tEXt", "Author\0" TEXT_CANARY, 7 + strlen(TEXT_CANARY));
  png_chunk(out, "gAMA", "\x00\x00\xb1\x8f", 4);
  png_chunk(out, "iTXt", "Comment\0\0\0\0\0" TEXT_CANARY, 12 + strlen(TEXT_CANARY));
  png_chunk(out, "zTXt", "Title\0\0" TEXT_CANARY, 7 + strlen(TEXT_CANARY));
  png_chunk(out, "eXIf", "MM\x00\x2a" EXIF_CANARY, 4 + strlen(EXIF_CANARY));
  png_chunk(out, "tIME", "\x07\xea\x09\x1d\x0c\x00\x00", 7);
  png_chunk(out, "pHYs", "\x00\x00\x0b\x13\x00\x00\x0b\x13\x01", 9);
  png_chunk(out, "prVt", PRIVATE_CANARY, strlen(PRIVATE_CANARY));
  png_chunk(out, "IDAT", "\x78\x9c\x63\x60\x00\x00\x00\x02\x00\x01", 10);
  png_chunk(out, "IEND", NULL, 0);
  g_byte_array_append(out, (const guint8 *)TRAILER_CANARY, strlen(TRAILER_CANARY));
  return g_byte_array_free_to_bytes(out);
}

/* ---- metadata stripping ---------------------------------------------------------- */

static void
test_strip_jpeg(void)
{
  g_autoptr(GBytes) jpeg = make_jpeg(640, 480, 64 * 1024);
  g_assert_true(contains(jpeg, GPS_CANARY));
  GhMediaFormat format = GH_MEDIA_FORMAT_OTHER;
  gboolean stripped = FALSE;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) out = gh_metadata_strip(jpeg, &format, &stripped, &error);
  g_assert_no_error(error);
  g_assert_cmpint(format, ==, GH_MEDIA_FORMAT_JPEG);
  g_assert_true(stripped);
  const gchar *gone[] = { GPS_CANARY, XMP_CANARY, IPTC_CANARY, COM_CANARY, TRAILER_CANARY,
                          MPF_CANARY, "Exif", "Photoshop" };
  for (guint i = 0; i < G_N_ELEMENTS(gone); i++)
    g_assert_false(contains(out, gone[i]));
  /* JFIF stays as a bare header saying "no thumbnail". */
  static const guint8 jfif[] = { 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0x02,
                                 0x00, 0x00, 0x48, 0x00, 0x48, 0x00, 0x00 };
  g_assert_true(contains_raw(out, jfif, sizeof jfif));
  const gchar *kept[] = { "ICC_PROFILE", "icc-colours", "Adobe" };
  for (guint i = 0; i < G_N_ELEMENTS(kept); i++)
    g_assert_true(contains(out, kept[i]));
  gsize size = 0;
  const guint8 *data = g_bytes_get_data(out, &size);
  g_assert_cmpuint(data[0], ==, 0xFF);
  g_assert_cmpuint(data[1], ==, 0xD8);
  g_assert_cmpuint(data[size - 2], ==, 0xFF);
  g_assert_cmpuint(data[size - 1], ==, 0xD9);
  /* The image data itself is unchanged: both scans survive byte for byte. */
  g_autoptr(GByteArray) scan = g_byte_array_new();
  jpeg_scan(scan, 32 * 1024, 2);
  g_assert_true(contains_raw(out, scan->data, scan->len));
  guint width = 0, height = 0;
  g_assert_true(gh_media_probe_dimensions(out, &format, &width, &height, &error));
  g_assert_cmpuint(width, ==, 640);
  g_assert_cmpuint(height, ==, 480);
  /* Stripping is idempotent. */
  g_autoptr(GBytes) again = gh_metadata_strip(out, NULL, NULL, &error);
  g_assert_true(g_bytes_equal(again, out));
}

static void
test_strip_png(void)
{
  g_autoptr(GBytes) png = make_png(300, 200);
  gboolean stripped = FALSE;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) out = gh_metadata_strip(png, NULL, &stripped, &error);
  g_assert_no_error(error);
  g_assert_true(stripped);
  const gchar *gone[] = { TEXT_CANARY, EXIF_CANARY, PRIVATE_CANARY, TRAILER_CANARY, "tEXt",
                          "iTXt", "zTXt", "eXIf", "tIME", "prVt" };
  for (guint i = 0; i < G_N_ELEMENTS(gone); i++)
    g_assert_false(contains(out, gone[i]));
  const gchar *kept[] = { "IHDR", "gAMA", "pHYs", "IDAT", "IEND" };
  for (guint i = 0; i < G_N_ELEMENTS(kept); i++)
    g_assert_true(contains(out, kept[i]));
  guint width = 0, height = 0;
  g_assert_true(gh_media_probe_dimensions(out, NULL, &width, &height, &error));
  g_assert_cmpuint(width, ==, 300);
  g_assert_cmpuint(height, ==, 200);
}

/* A JPEG or PNG that cannot be walked is refused, never sent as it is. */
static void
test_strip_damaged(void)
{
  g_autoptr(GBytes) jpeg = make_jpeg(64, 64, 4096);
  gsize size = g_bytes_get_size(jpeg);
  g_autoptr(GBytes) truncated = g_bytes_new_from_bytes(jpeg, 0, size / 2);
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_metadata_strip(truncated, NULL, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);

  g_autoptr(GBytes) png = make_png(8, 8);
  gsize png_size = 0;
  guint8 *bad = g_memdup2(g_bytes_get_data(png, &png_size), png_size);
  bad[8 + 4 + 4 + 13 + 4 + 3] = 0xff; /* tEXt's length points past the end */
  g_autoptr(GBytes) bad_png = g_bytes_new_take(bad, png_size);
  g_assert_null(gh_metadata_strip(bad_png, NULL, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);

  /* Through the attachment path too. */
  g_assert_null(gh_attachment_prepare(truncated, "image/jpeg", 1024 * 1024, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

/* Other types pass unchanged, with the notice. */
static void
test_strip_other(void)
{
  static const gchar text[] = "%PDF-1.7 Author: someone, Location: somewhere";
  g_autoptr(GBytes) pdf = g_bytes_new_static(text, sizeof text - 1);
  gboolean stripped = TRUE;
  GhMediaFormat format = GH_MEDIA_FORMAT_JPEG;
  g_autoptr(GBytes) out = gh_metadata_strip(pdf, &format, &stripped, NULL);
  g_assert_true(g_bytes_equal(out, pdf));
  g_assert_false(stripped);
  g_assert_cmpint(format, ==, GH_MEDIA_FORMAT_OTHER);
  g_assert_true(gh_attachment_may_have_metadata(pdf));
  g_autoptr(GError) error = NULL;
  g_autoptr(GhAttachmentPrepared) prepared =
    gh_attachment_prepare(pdf, "application/pdf; charset=binary", 1024, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(prepared->mime, ==, "application/pdf");
  g_assert_true(prepared->may_have_metadata);
  g_assert_false(prepared->stripped);
  /* A hint claiming an image the bytes deny is not believed. */
  g_autoptr(GhAttachmentPrepared) liar = gh_attachment_prepare(pdf, "image/png", 1024, &error);
  g_assert_cmpstr(liar->mime, ==, "application/octet-stream");
  /* A JPEG's type comes from its bytes, whatever the hint. */
  g_autoptr(GBytes) jpeg = make_jpeg(16, 16, 2048);
  g_autoptr(GhAttachmentPrepared) photo = gh_attachment_prepare(jpeg, "text/plain", 1 << 20,
                                                                &error);
  g_assert_cmpstr(photo->mime, ==, "image/jpeg");
  g_assert_cmpuint(photo->width, ==, 16);
  g_assert_false(photo->may_have_metadata);
  /* Size bounds. */
  g_autoptr(GBytes) empty = g_bytes_new_static("", 0);
  g_assert_null(gh_attachment_prepare(empty, NULL, 1024, &error));
  g_assert_error(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_EMPTY);
  g_clear_error(&error);
  g_assert_null(gh_attachment_prepare(pdf, NULL, 10, &error));
  g_assert_error(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_TOO_LARGE);
}

/* The walkers take arbitrary bytes: 4000 deterministic mutations (bit flips,
 * truncations, length bytes set to extremes) of a JPEG and a PNG never crash
 * or read out of bounds (run under ASan in CI), and whatever is accepted
 * still carries none of the metadata canaries. */
static void
test_strip_mutations(void)
{
  GBytes *sources[] = { make_jpeg(320, 240, 8 * 1024), make_png(64, 64) };
  /* Fixed for CI; GH_TEST_MUTATION_SEED explores others. */
  const gchar *seed = g_getenv("GH_TEST_MUTATION_SEED");
  GRand *rand = g_rand_new_with_seed(seed ? (guint32)g_ascii_strtoull(seed, NULL, 10) : 0x6721);
  const gchar *canaries[] = { GPS_CANARY, XMP_CANARY, IPTC_CANARY, COM_CANARY, TEXT_CANARY,
                              EXIF_CANARY, PRIVATE_CANARY };
  guint accepted = 0, refused = 0;
  for (guint i = 0; i < 4000; i++) {
    GBytes *source = sources[i % G_N_ELEMENTS(sources)];
    gsize size = 0;
    guint8 *data = g_memdup2(g_bytes_get_data(source, &size), size);
    guint edits = 1 + g_rand_int_range(rand, 0, 4);
    for (guint e = 0; e < edits && size > 3; e++) {
      gsize at = (gsize)g_rand_int_range(rand, 2, (gint32)size);
      switch (g_rand_int_range(rand, 0, 4)) {
      case 0: data[at] ^= (guint8)(1u << g_rand_int_range(rand, 0, 8)); break;
      case 1: data[at] = 0xFF; break;
      case 2: data[at] = 0x00; break;
      default: size = at; break; /* truncate */
      }
    }
    g_autoptr(GBytes) mutated = g_bytes_new_take(data, size);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) out = gh_metadata_strip(mutated, NULL, NULL, &error);
    guint width = 0, height = 0;
    (void)gh_media_probe_dimensions(mutated, NULL, &width, &height, NULL);
    (void)gh_attachment_check_preview(mutated, NULL, NULL, NULL, NULL);
    if (!out) {
      g_assert_nonnull(error);
      refused++;
      continue;
    }
    accepted++;
    /* Metadata a decoder would still see as metadata never passes: every
     * whole canary of a PNG, and of a JPEG those in its header, before the
     * first scan (a mutation that destroys a later segment's marker turns
     * its bytes into scan data, which no stripper can tell from pixels
     * without decoding). */
    gsize limit = size;
    if (gh_media_sniff(mutated) == GH_MEDIA_FORMAT_JPEG) {
      const guint8 *d = g_bytes_get_data(mutated, NULL);
      for (gsize k = 2; k + 1 < size; k++)
        if (d[k] == 0xFF && d[k + 1] == 0xDA) {
          limit = k;
          break;
        }
    }
    g_autoptr(GBytes) head = g_bytes_new_from_bytes(mutated, 0, limit);
    for (guint c = 0; c < G_N_ELEMENTS(canaries); c++)
      if (gh_media_sniff(mutated) != GH_MEDIA_FORMAT_OTHER && contains(head, canaries[c]))
        g_assert_false(contains(out, canaries[c]));
    /* And stripping what came out changes nothing. */
    g_autoptr(GBytes) again = gh_metadata_strip(out, NULL, NULL, NULL);
    g_assert_true(again && g_bytes_equal(again, out));
  }
  g_rand_free(rand);
  for (guint i = 0; i < G_N_ELEMENTS(sources); i++)
    g_bytes_unref(sources[i]);
  g_test_message("mutations: %u accepted, %u refused", accepted, refused);
  g_assert_cmpuint(refused, >, 0);
}

/* AT-4: the header decides, before anything is decoded. */
static void
test_decode_guard(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) huge = make_png(50000, 50000);
  g_assert_false(gh_attachment_check_preview(huge, NULL, NULL, NULL, &error));
  g_assert_error(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_PREVIEW_TOO_LARGE);
  g_clear_error(&error);
  g_autoptr(GBytes) wide = make_jpeg(9000, 10, 1024);
  g_assert_false(gh_attachment_check_preview(wide, NULL, NULL, NULL, &error));
  g_assert_error(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_PREVIEW_TOO_LARGE);
  g_clear_error(&error);
  g_autoptr(GBytes) limit = make_png(8192, 8192);
  GhMediaFormat format = GH_MEDIA_FORMAT_OTHER;
  guint width = 0, height = 0;
  g_assert_true(gh_attachment_check_preview(limit, &format, &width, &height, &error));
  g_assert_cmpint(format, ==, GH_MEDIA_FORMAT_PNG);
  g_assert_cmpuint(width, ==, 8192);
  /* Another type (even one named image/...) gets a card only. */
  static const gchar gif[] = "GIF89a\x10\x00\x10\x00";
  g_autoptr(GBytes) other = g_bytes_new_static(gif, sizeof gif - 1);
  g_assert_false(gh_attachment_check_preview(other, NULL, NULL, NULL, &error));
  g_assert_error(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_NO_PREVIEW);
  g_clear_error(&error);
  /* A JPEG whose header cannot be read. */
  static const guint8 stub[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00 };
  g_autoptr(GBytes) broken = g_bytes_new_static(stub, sizeof stub);
  g_assert_false(gh_attachment_check_preview(broken, NULL, NULL, NULL, &error));
  g_assert_error(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_NO_PREVIEW);
}

/* ---- encryption ------------------------------------------------------------------ */

static gchar *
sha256_hex(GBytes *bytes)
{
  gsize size = 0;
  gconstpointer data = g_bytes_get_data(bytes, &size);
  return g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, size);
}

static GhAttachmentSealed *
seal_jpeg(GBytes **out_stripped)
{
  g_autoptr(GBytes) jpeg = make_jpeg(1024, 768, 256 * 1024);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhAttachmentPrepared) prepared = gh_attachment_prepare(jpeg, NULL, 1 << 20, &error);
  g_assert_no_error(error);
  GhAttachmentSealed *sealed = gh_attachment_encrypt(prepared, &error);
  g_assert_no_error(error);
  if (out_stripped)
    *out_stripped = g_bytes_ref(prepared->plaintext);
  return sealed;
}

/* AT-1's crypto half: x and ox are right and decryption gives back the
 * stripped file. */
static void
test_encrypt_round_trip(void)
{
  g_autoptr(GBytes) stripped = NULL;
  g_autoptr(GhAttachmentSealed) sealed = seal_jpeg(&stripped);
  GhNip17File *file = sealed->file;
  g_autofree gchar *x = sha256_hex(sealed->ciphertext);
  g_autofree gchar *ox = sha256_hex(stripped);
  g_assert_cmpstr(file->x, ==, x);
  g_assert_cmpstr(file->ox, ==, ox);
  g_assert_cmpuint(file->nonce_size, ==, 12);
  g_assert_cmpuint(file->size, ==, g_bytes_get_size(sealed->ciphertext));
  g_assert_cmpuint(file->size, ==, g_bytes_get_size(stripped) + 16);
  g_assert_cmpstr(file->file_type, ==, "image/jpeg");
  g_assert_cmpuint(file->width, ==, 1024);
  g_assert_cmpuint(file->height, ==, 768);
  g_assert_null(file->url);
  g_assert_false(contains(sealed->ciphertext, "JFIF"));
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) plain = gh_attachment_decrypt(file, sealed->ciphertext, &error);
  g_assert_no_error(error);
  g_assert_true(g_bytes_equal(plain, stripped));
  g_assert_false(contains(plain, GPS_CANARY));
  /* A fresh key and nonce every time. */
  g_autoptr(GhAttachmentSealed) again = seal_jpeg(NULL);
  g_assert_true(memcmp(again->file->key, file->key, 32) != 0);
  g_assert_true(memcmp(again->file->nonce, file->nonce, 12) != 0);
}

/* AT-2: a flipped ciphertext bit fails the x check; a sender-consistent x
 * with a bad GCM tag is still rejected; so is a wrong ox. */
static void
test_tampering(void)
{
  g_autoptr(GhAttachmentSealed) sealed = seal_jpeg(NULL);
  gsize size = 0;
  guint8 *flipped = g_memdup2(g_bytes_get_data(sealed->ciphertext, &size), size);
  flipped[size / 3] ^= 0x01;
  g_autoptr(GBytes) tampered = g_bytes_new_take(flipped, size);
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_attachment_decrypt(sealed->file, tampered, &error));
  g_assert_error(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_DAMAGED);
  g_assert_cmpstr(error->message, ==, "This file was changed or damaged.");
  g_clear_error(&error);
  /* x is checked before the cipher runs: with the key destroyed too, the
   * answer is the same x failure, not a cipher one. */
  g_autoptr(GhNip17File) no_key = gh_nip17_file_copy(sealed->file);
  memset(no_key->key, 0, sizeof no_key->key);
  g_assert_null(gh_attachment_decrypt(no_key, tampered, &error));
  g_assert_error(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_DAMAGED);
  g_clear_error(&error);

  /* The sender names the tampered bytes' x: the GCM tag still refuses. */
  g_autoptr(GhNip17File) consistent = gh_nip17_file_copy(sealed->file);
  g_autofree gchar *x = sha256_hex(tampered);
  g_strlcpy(consistent->x, x, sizeof consistent->x);
  g_assert_null(gh_attachment_decrypt(consistent, tampered, &error));
  g_assert_error(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_DAMAGED);
  g_clear_error(&error);

  /* A tag-only flip (last byte), sender-consistent x. */
  guint8 *tag_flip = g_memdup2(g_bytes_get_data(sealed->ciphertext, NULL), size);
  tag_flip[size - 1] ^= 0x80;
  g_autoptr(GBytes) bad_tag = g_bytes_new_take(tag_flip, size);
  g_autofree gchar *x2 = sha256_hex(bad_tag);
  g_strlcpy(consistent->x, x2, sizeof consistent->x);
  g_assert_null(gh_attachment_decrypt(consistent, bad_tag, &error));
  g_assert_error(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_DAMAGED);
  g_clear_error(&error);

  /* A wrong ox. */
  g_autoptr(GhNip17File) wrong_ox = gh_nip17_file_copy(sealed->file);
  wrong_ox->ox[0] = wrong_ox->ox[0] == 'a' ? 'b' : 'a';
  g_assert_null(gh_attachment_decrypt(wrong_ox, sealed->ciphertext, &error));
  g_assert_error(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_DAMAGED);
  g_clear_error(&error);
  /* No ox given: fine. */
  g_autoptr(GhNip17File) no_ox = gh_nip17_file_copy(sealed->file);
  no_ox->ox[0] = '\0';
  g_autoptr(GBytes) plain = gh_attachment_decrypt(no_ox, sealed->ciphertext, &error);
  g_assert_no_error(error);
  /* Truncated to less than a tag. */
  g_autoptr(GBytes) stub = g_bytes_new_from_bytes(sealed->ciphertext, 0, 10);
  g_assert_null(gh_attachment_decrypt(sealed->file, stub, &error));
  g_assert_error(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_DAMAGED);
}

/* Interop: AES-256-GCM output of an independent implementation (Node crypto
 * and Go, 16-byte nonce: what Amethyst and 0xchat upload), from gnostr's
 * tests/test_dm_files_key_encoding.c, read through a kind-15 rumor. */
#define VECTOR_KEY "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
#define VECTOR_NONCE "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
#define VECTOR_PLAIN "kind-15 interop: amethyst/0xchat 16-byte nonce\n"
#define VECTOR_CT                                                        \
  "41ca4dd835fcf856aaf06bd49f7bfdaaa2242e1c0a5c28df893d5ace7a5d47f32ca0d0" \
  "4ef7aacbd8ce675fcc7b27bde7211e6286028720d314fa489bb9d31d"

static GBytes *
unhex(const gchar *hex)
{
  gsize n = strlen(hex) / 2;
  guint8 *out = g_malloc(n);
  for (gsize i = 0; i < n; i++)
    out[i] = (guint8)((g_ascii_xdigit_value(hex[2 * i]) << 4) |
                      g_ascii_xdigit_value(hex[2 * i + 1]));
  return g_bytes_new_take(out, n);
}

static gchar *
rumor_json(gint kind, const gchar *content, NostrTags *tags)
{
  NostrEvent *rumor = nostr_event_new();
  nostr_event_set_kind(rumor, kind);
  nostr_event_set_pubkey(rumor, "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
  nostr_event_set_created_at(rumor, 1700000000);
  nostr_event_set_content(rumor, content);
  nostr_tags_append(tags, nostr_tag_new("p",
    "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5", NULL));
  nostr_event_set_tags(rumor, tags);
  rumor->id = nostr_event_get_id(rumor);
  char *json = nostr_event_serialize_compact(rumor);
  nostr_event_free(rumor);
  gchar *copy = g_strdup(json);
  free(json);
  return copy;
}

static void
test_interop_vector(void)
{
  g_autoptr(GBytes) ciphertext = unhex(VECTOR_CT);
  g_autofree gchar *x = sha256_hex(ciphertext);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("file-type", "text/plain", NULL));
  nostr_tags_append(tags, nostr_tag_new("encryption-algorithm", "aes-gcm", NULL));
  nostr_tags_append(tags, nostr_tag_new("decryption-key", VECTOR_KEY, NULL));
  nostr_tags_append(tags, nostr_tag_new("decryption-nonce", VECTOR_NONCE, NULL));
  nostr_tags_append(tags, nostr_tag_new("x", x, NULL));
  g_autofree gchar *json = rumor_json(15, "https://blossom.example.com/file", tags);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip17File) file = gh_nip17_file_from_rumor(json, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(file->nonce_size, ==, 16);
  g_assert_cmpstr(file->ox, ==, "");
  g_autoptr(GBytes) plain = gh_attachment_decrypt(file, ciphertext, &error);
  g_assert_no_error(error);
  g_assert_cmpmem(g_bytes_get_data(plain, NULL), g_bytes_get_size(plain), VECTOR_PLAIN,
                  strlen(VECTOR_PLAIN));
}

/* ---- kind-15 tags and messages ---------------------------------------------------- */

static const gchar *const alice = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
static const gchar *const bob = "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5";

static void
test_file_tags(void)
{
  g_autoptr(GhAttachmentSealed) sealed = seal_jpeg(NULL);
  GhNip17File *file = sealed->file;
  file->url = g_strdup("https://blossom.example.com/abc");
  g_autofree gchar *id = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *json = gh_nip17_file_rumor_new(alice, bob, file, 1700000000, 1700003600, &id,
                                                   &error);
  g_assert_no_error(error);
  g_assert_nonnull(id);
  g_autoptr(GhNip17File) back = gh_nip17_file_from_rumor(json, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(back->url, ==, file->url);
  g_assert_cmpstr(back->file_type, ==, "image/jpeg");
  g_assert_cmpmem(back->key, 32, file->key, 32);
  g_assert_cmpmem(back->nonce, back->nonce_size, file->nonce, file->nonce_size);
  g_assert_cmpstr(back->x, ==, file->x);
  g_assert_cmpstr(back->ox, ==, file->ox);
  g_assert_cmpuint(back->size, ==, file->size);
  g_assert_cmpuint(back->width, ==, 1024);
  g_assert_cmpuint(back->height, ==, 768);
  /* No thumb or blurhash in v1; the expiration rides along. */
  g_assert_null(strstr(json, "thumb"));
  g_assert_null(strstr(json, "blurhash"));
  g_assert_nonnull(strstr(json, "[\"expiration\",\"1700003600\"]"));
  g_assert_nonnull(strstr(json, "\"kind\":15"));

  /* GhMessage takes it as a kind-15 message and hands its file back. */
  g_autoptr(GhMessage) message = gh_message_new_from_rumor(bob, json, &error);
  g_assert_no_error(error);
  g_assert_cmpint(gh_message_get_kind(message), ==, 15);
  g_assert_cmpstr(gh_message_get_content(message), ==, file->url);
  g_autoptr(GhNip17File) from_message = gh_message_dup_file(message);
  g_assert_nonnull(from_message);
  g_assert_cmpstr(from_message->x, ==, file->x);
  g_assert_cmpint(gh_message_get_expires_at(message), ==, 1700003600);

  /* A kind-14 message has no file. */
  NostrTags *none = nostr_tags_new(0);
  g_autofree gchar *chat = rumor_json(14, "hello", none);
  g_autoptr(GhMessage) text = gh_message_new_from_rumor(bob, chat, &error);
  g_assert_no_error(error);
  g_assert_null(gh_message_dup_file(text));

  /* Incomplete input is refused. */
  GhNip17File incomplete = *file;
  incomplete.x[0] = 'z';
  g_assert_null(gh_nip17_file_rumor_new(alice, bob, &incomplete, 1, 0, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  incomplete = *file;
  incomplete.url = (gchar *)"javascript:alert(1)";
  g_assert_null(gh_nip17_file_rumor_new(alice, bob, &incomplete, 1, 0, NULL, &error));
  g_clear_error(&error);
}

typedef struct {
  const gchar *name;
  const gchar *tags[8][2];
  const gchar *content;
  gint code;
} BadFile;

static void
test_file_tags_invalid(void)
{
  const gchar *key = VECTOR_KEY, *nonce = "a0a1a2a3a4a5a6a7a8a9aaab";
  const gchar *x = "7d865e959b2466918c9863afca942d0fb89d7c9ac0c99bafc3749504ded97730";
  const BadFile cases[] = {
    { "no file-type", { { "encryption-algorithm", "aes-gcm" }, { "decryption-key", key },
                        { "decryption-nonce", nonce }, { "x", x } }, NULL,
      G_IO_ERROR_INVALID_DATA },
    { "other algorithm", { { "file-type", "image/png" }, { "encryption-algorithm", "chacha" },
                           { "decryption-key", key }, { "decryption-nonce", nonce },
                           { "x", x } }, NULL, G_IO_ERROR_NOT_SUPPORTED },
    { "short key", { { "file-type", "image/png" }, { "encryption-algorithm", "aes-gcm" },
                     { "decryption-key", "0011" }, { "decryption-nonce", nonce }, { "x", x } },
      NULL, G_IO_ERROR_INVALID_DATA },
    { "base64 nonce", { { "file-type", "image/png" }, { "encryption-algorithm", "aes-gcm" },
                        { "decryption-key", key }, { "decryption-nonce", "oKGio6Slpqeoqaqr" },
                        { "x", x } }, NULL, G_IO_ERROR_INVALID_DATA },
    { "no x", { { "file-type", "image/png" }, { "encryption-algorithm", "aes-gcm" },
                { "decryption-key", key }, { "decryption-nonce", nonce } }, NULL,
      G_IO_ERROR_INVALID_DATA },
    { "two x", { { "file-type", "image/png" }, { "encryption-algorithm", "aes-gcm" },
                 { "decryption-key", key }, { "decryption-nonce", nonce }, { "x", x },
                 { "x", x } }, NULL, G_IO_ERROR_INVALID_DATA },
    { "bad dim", { { "file-type", "image/png" }, { "encryption-algorithm", "aes-gcm" },
                   { "decryption-key", key }, { "decryption-nonce", nonce }, { "x", x },
                   { "dim", "0x100" } }, NULL, G_IO_ERROR_INVALID_DATA },
    { "bad size", { { "file-type", "image/png" }, { "encryption-algorithm", "aes-gcm" },
                    { "decryption-key", key }, { "decryption-nonce", nonce }, { "x", x },
                    { "size", "-5" } }, NULL, G_IO_ERROR_INVALID_DATA },
    { "file url", { { "file-type", "image/png" }, { "encryption-algorithm", "aes-gcm" },
                    { "decryption-key", key }, { "decryption-nonce", nonce }, { "x", x } },
      "file:///etc/passwd", G_IO_ERROR_INVALID_DATA },
    { "user info", { { "file-type", "image/png" }, { "encryption-algorithm", "aes-gcm" },
                     { "decryption-key", key }, { "decryption-nonce", nonce }, { "x", x } },
      "https://user:pw@blossom.example.com/x", G_IO_ERROR_INVALID_DATA },
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_test_message("invalid file message: %s", cases[i].name);
    NostrTags *tags = nostr_tags_new(0);
    for (guint t = 0; t < 8 && cases[i].tags[t][0]; t++)
      nostr_tags_append(tags, nostr_tag_new(cases[i].tags[t][0], cases[i].tags[t][1], NULL));
    g_autofree gchar *json = rumor_json(15, cases[i].content ? cases[i].content
                                                             : "https://blossom.example.com/x",
                                        tags);
    g_autoptr(GError) error = NULL;
    g_assert_null(gh_nip17_file_from_rumor(json, &error));
    g_assert_error(error, G_IO_ERROR, cases[i].code);
    g_clear_error(&error);
    /* GhMessage refuses what the unwrap would. */
    g_assert_null(gh_message_new_from_rumor(bob, json, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  }
}

/* ---- the encrypted media cache ---------------------------------------------------- */

static void
remove_tree(const gchar *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  if (dir) {
    const gchar *name;
    while ((name = g_dir_read_name(dir))) {
      g_autofree gchar *child = g_build_filename(path, name, NULL);
      if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
        remove_tree(child);
      else
        g_unlink(child);
    }
    g_dir_close(dir);
  }
  g_rmdir(path);
}

static gchar *
hex_of(guint n)
{
  g_autofree gchar *seed = g_strdup_printf("media-%u", n);
  return g_compute_checksum_for_string(G_CHECKSUM_SHA256, seed, -1);
}

static void
test_media_cache(void)
{
  g_autofree gchar *dir = g_dir_make_tmp("groundhog-media-XXXXXX", NULL);
  g_assert_nonnull(dir);
  guint8 raw_key[32];
  for (guint i = 0; i < sizeof raw_key; i++)
    raw_key[i] = (guint8)g_random_int();
  g_autoptr(GBytes) key = g_bytes_new(raw_key, sizeof raw_key);
  GhClock *clock = gh_clock_new_fake(G_GINT64_CONSTANT(1700000000) * G_USEC_PER_SEC);
  GhStoreConfig config = { .data_dir = dir, .account_pubkey = alice, .clock = clock };
  g_autofree gchar *store_id = g_uuid_string_random();
  g_autoptr(GError) error = NULL;
  GhStore *store = gh_store_open_with_key(&config, key, store_id, GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);

  static const gchar canary[] = "MEDIA-CACHE-PLAINTEXT-CANARY-5c1e";
  g_autofree gchar *a = hex_of(1), *b = hex_of(2), *c = hex_of(3);
  g_autoptr(GBytes) photo = g_bytes_new_static(canary, sizeof canary - 1);
  g_assert_null(gh_store_media_get(store, a, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(gh_store_media_put(store, a, "image/jpeg", photo, &error));
  g_autoptr(GBytes) big = g_bytes_new_take(g_malloc0(1000), 1000);
  gh_clock_fake_advance(clock, G_USEC_PER_SEC);
  g_assert_true(gh_store_media_put(store, b, NULL, big, &error));
  gh_clock_fake_advance(clock, G_USEC_PER_SEC);
  g_assert_true(gh_store_media_put(store, c, NULL, big, &error));
  /* Reading a marks it used: b is now the least recently used. */
  gh_clock_fake_advance(clock, G_USEC_PER_SEC);
  g_autofree gchar *mime = NULL;
  g_autoptr(GBytes) back = gh_store_media_get(store, a, &mime, &error);
  g_assert_true(g_bytes_equal(back, photo));
  g_assert_cmpstr(mime, ==, "image/jpeg");
  gint64 total = 0;
  g_assert_true(gh_store_media_get_total(store, &total, &error));
  g_assert_cmpint(total, ==, 2000 + (gint64)g_bytes_get_size(photo));
  g_assert_true(gh_store_media_prune(store, 2000, &error));
  g_assert_true(gh_store_media_get_total(store, &total, &error));
  g_assert_cmpint(total, <=, 2000);
  g_autoptr(GBytes) gone = gh_store_media_get(store, b, NULL, &error);
  g_assert_null(gone); /* the least recently used went */
  g_autoptr(GBytes) kept = gh_store_media_get(store, a, NULL, &error);
  g_assert_nonnull(kept);
  g_assert_true(gh_store_media_remove(store, c, &error));
  g_assert_true(gh_store_media_prune(store, 0, &error));
  g_assert_true(gh_store_media_get_total(store, &total, &error));
  g_assert_cmpint(total, ==, 0);

  /* Bad input. */
  g_assert_false(gh_store_media_put(store, "not-hex", NULL, photo, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);

  /* The plaintext was in the store, which is encrypted: not on disk. */
  g_assert_true(gh_store_media_put(store, a, "image/jpeg", photo, &error));
  g_autofree gchar *path = g_strdup(gh_store_get_path(store));
  gh_store_close(store);
  gh_clock_unref(clock);
  const gchar *suffixes[] = { "", "-wal", "-shm" };
  for (guint i = 0; i < G_N_ELEMENTS(suffixes); i++) {
    g_autofree gchar *file = g_strconcat(path, suffixes[i], NULL);
    gchar *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(file, &contents, &length, NULL))
      continue;
    g_autoptr(GBytes) bytes = g_bytes_new_take(contents, length);
    g_assert_false(contains(bytes, canary));
  }
  remove_tree(dir);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/media/strip-jpeg", test_strip_jpeg);
  g_test_add_func("/groundhog/media/strip-png", test_strip_png);
  g_test_add_func("/groundhog/media/strip-damaged", test_strip_damaged);
  g_test_add_func("/groundhog/media/strip-other", test_strip_other);
  g_test_add_func("/groundhog/media/strip-mutations", test_strip_mutations);
  g_test_add_func("/groundhog/media/at4-decode-guard", test_decode_guard);
  g_test_add_func("/groundhog/media/at1-encrypt-round-trip", test_encrypt_round_trip);
  g_test_add_func("/groundhog/media/at2-tampering", test_tampering);
  g_test_add_func("/groundhog/media/interop-vector", test_interop_vector);
  g_test_add_func("/groundhog/media/file-tags", test_file_tags);
  g_test_add_func("/groundhog/media/file-tags-invalid", test_file_tags_invalid);
  g_test_add_func("/groundhog/media/cache", test_media_cache);
  return g_test_run();
}
