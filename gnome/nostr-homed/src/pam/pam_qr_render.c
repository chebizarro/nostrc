/* Unicode half-block + PNG renderer for the PAM module — design §3.4.
 *
 * The encoder is the vendored Nayuki qrcodegen (MIT). Byte mode, ECC L,
 * quiet zone 2 modules, version cap = the caller's max_version (design
 * decision D11 is 9). Each two module-rows collapse into one text row of
 * half-block characters:
 *   ' '  both modules light
 *   '▀'  top module dark  (U+2580)
 *   '▄'  bottom module dark (U+2584)
 *   '█'  both dark (U+2588)
 *
 * PNG output is a monochrome greyscale image with each module rendered as
 * a `scale x scale` pixel block. The writer is a hand-rolled stored-block
 * deflate + CRC32 — small enough to sit next to the qrcodegen encoder
 * without pulling in libpng/zlib. */
#include "pam_qr_render.h"

#include "../../third_party/qrcodegen/qrcodegen.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define QR_QUIET 2

static const char HALF_TOP[]    = "\xe2\x96\x80"; /* ▀ U+2580 */
static const char HALF_BOTTOM[] = "\xe2\x96\x84"; /* ▄ U+2584 */
static const char HALF_FULL[]   = "\xe2\x96\x88"; /* █ U+2588 */

static int module_at(const uint8_t *qr, int x, int y, int size) {
  if (x < 0 || y < 0 || x >= size || y >= size) return 0; /* quiet zone */
  return qrcodegen_getModule(qr, x, y);
}

/* Try to encode text into `qr` at the smallest version <= max_version that
 * accepts it. Returns 1 on success, 0 on TOO_LONG. */
static int encode_bytes(const char *text, int max_version, uint8_t *qr,
                        uint8_t *tmp) {
  if (max_version < 1) max_version = 1;
  if (max_version > qrcodegen_VERSION_MAX) max_version = qrcodegen_VERSION_MAX;
  /* qrcodegen_encodeText consumes text as bytes internally when the
   * segmenter cannot use numeric/alphanumeric — but explicitly using the
   * binary path guarantees byte mode regardless of the URI's byte content. */
  size_t n = strlen(text);
  /* qrcodegen requires the input buffer be at least
   * qrcodegen_BUFFER_LEN_FOR_VERSION(max_version) — we already pass that. */
  if (n > (size_t)qrcodegen_BUFFER_LEN_FOR_VERSION(max_version)) return 0;
  memcpy(tmp, text, n);
  return qrcodegen_encodeBinary(tmp, n, qr, qrcodegen_Ecc_LOW, 1,
                                max_version, qrcodegen_Mask_AUTO, 1);
}

nh_qr_render_rc nh_pam_qr_render_halfblock(const char *text, int max_version,
                                           char **out_buf, size_t *out_len) {
  if (out_buf) *out_buf = NULL;
  if (out_len) *out_len = 0;
  if (!text || !out_buf || !out_len) return NH_QR_RENDER_INVALID_INPUT;
  if (max_version < 1) max_version = 1;
  if (max_version > qrcodegen_VERSION_MAX) max_version = qrcodegen_VERSION_MAX;

  size_t bufcap = qrcodegen_BUFFER_LEN_FOR_VERSION(max_version);
  uint8_t *qr = malloc(bufcap);
  uint8_t *tmp = malloc(bufcap);
  if (!qr || !tmp) { free(qr); free(tmp); return NH_QR_RENDER_OOM; }
  if (!encode_bytes(text, max_version, qr, tmp)) {
    free(qr); free(tmp);
    return NH_QR_RENDER_TOO_LONG;
  }
  free(tmp);

  int size = qrcodegen_getSize(qr);
  int cols = size + 2 * QR_QUIET;
  int rows_modules = size + 2 * QR_QUIET;
  int rows_text = (rows_modules + 1) / 2; /* two module rows per text row */

  /* Worst case: every cell is a 3-byte UTF-8 glyph + 1 for the newline. */
  size_t need = (size_t)rows_text * (size_t)(cols * 3 + 1) + 1;
  char *out = malloc(need);
  if (!out) { free(qr); return NH_QR_RENDER_OOM; }
  size_t off = 0;

  for (int ry = 0; ry < rows_modules; ry += 2) {
    int qy_top = ry - QR_QUIET;
    int qy_bot = ry + 1 - QR_QUIET;
    for (int rx = 0; rx < cols; rx++) {
      int qx = rx - QR_QUIET;
      int top = module_at(qr, qx, qy_top, size);
      int bot = (ry + 1 < rows_modules) ? module_at(qr, qx, qy_bot, size) : 0;
      const char *g;
      if (top && bot) g = HALF_FULL;
      else if (top) g = HALF_TOP;
      else if (bot) g = HALF_BOTTOM;
      else g = " ";
      size_t gl = strlen(g);
      memcpy(out + off, g, gl);
      off += gl;
    }
    out[off++] = '\n';
  }
  out[off] = '\0';
  free(qr);

  *out_buf = out;
  *out_len = off;
  return NH_QR_RENDER_OK;
}

/* -------------------- tiny PNG writer -------------------- */

/* CRC32 (IEEE 802.3), computed from the standard polynomial. Not
 * table-driven; PNGs we write are tiny (a QR at scale=8, version 9 is well
 * under 200 kB) so the per-byte loop is not a hot spot. */
static uint32_t crc32_of(const uint8_t *data, size_t len, uint32_t seed) {
  uint32_t c = seed ^ 0xffffffffu;
  for (size_t i = 0; i < len; i++) {
    c ^= data[i];
    for (int k = 0; k < 8; k++)
      c = (c >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(c & 1)));
  }
  return c ^ 0xffffffffu;
}

/* Adler-32 for the zlib wrapper. */
static uint32_t adler32_of(const uint8_t *data, size_t len) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < len; i++) {
    a = (a + data[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Append a PNG chunk (type + data), computing CRC over type||data. */
static int append_chunk(uint8_t **buf, size_t *cap, size_t *len,
                        const char type[4], const uint8_t *data, size_t dlen) {
  size_t need = *len + 4 + 4 + dlen + 4;
  if (need > *cap) {
    size_t ncap = *cap ? *cap : 512;
    while (ncap < need) ncap *= 2;
    uint8_t *nb = realloc(*buf, ncap);
    if (!nb) return -1;
    *buf = nb;
    *cap = ncap;
  }
  put_be32(*buf + *len, (uint32_t)dlen);
  memcpy(*buf + *len + 4, type, 4);
  if (dlen) memcpy(*buf + *len + 8, data, dlen);
  uint32_t crc = crc32_of(*buf + *len + 4, 4 + dlen, 0);
  put_be32(*buf + *len + 8 + dlen, crc);
  *len += 4 + 4 + dlen + 4;
  return 0;
}

/* Encode `raw` (already includes per-row filter bytes) as a series of
 * uncompressed deflate blocks + adler32 (a "stored" zlib stream). Deflate
 * stored blocks cap payload at 65535 bytes per block. Returns malloc'd
 * buffer + length. */
static uint8_t *zlib_store_encode(const uint8_t *raw, size_t rlen,
                                  size_t *out_len) {
  /* zlib header (2) + N * (5-byte stored-block header + payload) + adler32 (4) */
  size_t nblocks = (rlen + 65534) / 65535;
  if (nblocks == 0) nblocks = 1; /* at least one, possibly empty (rlen==0) */
  size_t cap = 2 + nblocks * (5 + 65535) + 4;
  uint8_t *out = malloc(cap);
  if (!out) return NULL;
  size_t off = 0;
  /* zlib header: CMF=0x78 (deflate, 32K window), FLG=0x01 (fastest) so
   * (CMF*256+FLG) % 31 == 0. */
  out[off++] = 0x78;
  out[off++] = 0x01;
  size_t left = rlen;
  const uint8_t *p = raw;
  do {
    size_t take = left > 65535 ? 65535 : left;
    int last = (take == left);
    out[off++] = last ? 0x01 : 0x00; /* BFINAL + BTYPE=00 */
    out[off++] = (uint8_t)(take & 0xff);
    out[off++] = (uint8_t)((take >> 8) & 0xff);
    out[off++] = (uint8_t)((~take) & 0xff);
    out[off++] = (uint8_t)(((~take) >> 8) & 0xff);
    if (take) { memcpy(out + off, p, take); off += take; p += take; }
    left -= take;
  } while (left);
  uint32_t adler = adler32_of(raw, rlen);
  put_be32(out + off, adler);
  off += 4;
  *out_len = off;
  return out;
}

nh_qr_render_rc nh_pam_qr_render_png(const char *text, int max_version,
                                     int scale, unsigned char **out_buf,
                                     size_t *out_len) {
  if (out_buf) *out_buf = NULL;
  if (out_len) *out_len = 0;
  if (!text || !out_buf || !out_len) return NH_QR_RENDER_INVALID_INPUT;
  if (scale < 1) scale = 1;
  if (scale > 64) scale = 64;
  if (max_version < 1) max_version = 1;
  if (max_version > qrcodegen_VERSION_MAX) max_version = qrcodegen_VERSION_MAX;

  size_t bufcap = qrcodegen_BUFFER_LEN_FOR_VERSION(max_version);
  uint8_t *qr = malloc(bufcap);
  uint8_t *tmp = malloc(bufcap);
  if (!qr || !tmp) { free(qr); free(tmp); return NH_QR_RENDER_OOM; }
  if (!encode_bytes(text, max_version, qr, tmp)) {
    free(qr); free(tmp);
    return NH_QR_RENDER_TOO_LONG;
  }
  free(tmp);

  int size = qrcodegen_getSize(qr);
  int mods = size + 2 * QR_QUIET;
  size_t pixels = (size_t)mods * (size_t)scale;
  /* Raw scanlines: 1 filter byte + `pixels` greyscale bytes per row. */
  size_t row_bytes = 1 + pixels;
  size_t raw_len = row_bytes * pixels;
  uint8_t *raw = malloc(raw_len);
  if (!raw) { free(qr); return NH_QR_RENDER_OOM; }
  /* Build one module row at a time, then replicate `scale` times. */
  for (size_t my = 0; my < (size_t)mods; my++) {
    uint8_t *row = raw + my * (size_t)scale * row_bytes;
    row[0] = 0; /* filter = None */
    for (size_t mx = 0; mx < (size_t)mods; mx++) {
      int qx = (int)mx - QR_QUIET;
      int qy = (int)my - QR_QUIET;
      uint8_t v = module_at(qr, qx, qy, size) ? 0x00 : 0xff;
      memset(row + 1 + mx * (size_t)scale, v, (size_t)scale);
    }
    for (int r = 1; r < scale; r++)
      memcpy(row + (size_t)r * row_bytes, row, row_bytes);
  }
  free(qr);

  size_t zlen = 0;
  uint8_t *zdata = zlib_store_encode(raw, raw_len, &zlen);
  free(raw);
  if (!zdata) return NH_QR_RENDER_OOM;

  uint8_t *png = NULL;
  size_t cap = 0, len = 0;
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  if ((cap = 0, len = 0, 0)) {} /* silence warn */
  cap = 8; len = 8; png = malloc(cap);
  if (!png) { free(zdata); return NH_QR_RENDER_OOM; }
  memcpy(png, sig, 8);

  uint8_t ihdr[13];
  put_be32(ihdr, (uint32_t)pixels);
  put_be32(ihdr + 4, (uint32_t)pixels);
  ihdr[8] = 8;   /* bit depth */
  ihdr[9] = 0;   /* colour type: greyscale */
  ihdr[10] = 0;  /* compression: deflate */
  ihdr[11] = 0;  /* filter */
  ihdr[12] = 0;  /* interlace: none */
  if (append_chunk(&png, &cap, &len, "IHDR", ihdr, sizeof ihdr) != 0) {
    free(png); free(zdata); return NH_QR_RENDER_OOM;
  }
  if (append_chunk(&png, &cap, &len, "IDAT", zdata, zlen) != 0) {
    free(png); free(zdata); return NH_QR_RENDER_OOM;
  }
  free(zdata);
  if (append_chunk(&png, &cap, &len, "IEND", NULL, 0) != 0) {
    free(png); return NH_QR_RENDER_OOM;
  }
  *out_buf = png;
  *out_len = len;
  return NH_QR_RENDER_OK;
}
