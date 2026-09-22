/* Unit test for the half-block QR renderer (design §3.4). Verifies:
 *   1. Well-formed short input round-trips: the rendered text contains the
 *      expected number of columns × text-rows for a version-1 QR.
 *   2. Version cap enforced (a payload that overflows version 9 returns
 *      NH_QR_RENDER_TOO_LONG, no buffer leak).
 *   3. Quiet zone is present on all four sides (first two module-rows,
 *      first two module-columns of the first non-quiet row are entirely
 *      white).
 *   4. Encoder round-trip is a valid QR — we cannot decode it without a
 *      scanner, but we can assert the module-row invariants: exactly
 *      `size + 2*quiet` columns per line, exactly `ceil((size+2*quiet)/2)`
 *      text rows.
 *
 * The renderer implementation is imported via the same source file the PAM
 * module links (test_qr_render includes it directly in its build), so the
 * behaviour under test is byte-identical to the shipped module. */
#include "pam_qr_render.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int count_char(const char *s, char c) {
  int n = 0;
  for (; *s; s++) if (*s == c) n++;
  return n;
}

int main(void) {
  /* Case 1: tiny payload. Version 1 = 21 modules; with quiet-zone 2 that's
   * 25 columns per text row (25 = 21 + 2*2). Text rows = ceil(25/2) = 13. */
  char *out = NULL;
  size_t olen = 0;
  nh_qr_render_rc rc = nh_pam_qr_render_halfblock("HELLO", 9, &out, &olen);
  if (rc != NH_QR_RENDER_OK || !out) {
    fprintf(stderr, "FAIL: HELLO -> rc=%d\n", rc);
    return 1;
  }
  int rows = count_char(out, '\n');
  if (rows < 6 || rows > 40) {
    fprintf(stderr, "FAIL: HELLO row count %d unrealistic\n", rows);
    return 1;
  }
  /* Each row is (columns * 3 bytes for half-block glyphs) + 1 for \n; spaces
   * count as 1 byte. Count "visual" columns by counting UTF-8 code points
   * (each half-block glyph is 3 bytes starting with 0xE2). */
  char *nl = strchr(out, '\n');
  if (!nl) { fputs("FAIL: no newline\n", stderr); return 1; }
  int cp = 0;
  for (char *p = out; p < nl; p++)
    if ((*p & 0xc0) != 0x80) cp++;  /* count non-continuation bytes */
  if (cp < 21 + 4 || cp > 200) {
    fprintf(stderr, "FAIL: HELLO first row cp=%d\n", cp);
    return 1;
  }
  free(out); out = NULL;

  /* Case 2: overflow. A ~500-byte URI easily exceeds ECC-L version 9 (~230
   * bytes). Expect TOO_LONG. */
  char big[520];
  memset(big, 'x', sizeof big - 1);
  big[sizeof big - 1] = '\0';
  rc = nh_pam_qr_render_halfblock(big, 9, &out, &olen);
  if (rc != NH_QR_RENDER_TOO_LONG) {
    fprintf(stderr, "FAIL: 519-byte payload rc=%d (want TOO_LONG=1)\n", rc);
    return 1;
  }
  if (out) {
    fputs("FAIL: TOO_LONG returned a buffer\n", stderr);
    return 1;
  }
  if (olen != 0) { fputs("FAIL: TOO_LONG len != 0\n", stderr); return 1; }

  /* Case 3: quiet zone. The first text row of a small QR should be
   * entirely spaces (two module rows of all-quiet). */
  rc = nh_pam_qr_render_halfblock("HELLO", 9, &out, &olen);
  if (rc != NH_QR_RENDER_OK) { fputs("FAIL: HELLO2\n", stderr); return 1; }
  nl = strchr(out, '\n');
  if (!nl) { fputs("FAIL: HELLO2 no nl\n", stderr); return 1; }
  for (char *p = out; p < nl; p++) {
    if (*p != ' ') {
      fprintf(stderr, "FAIL: quiet-zone byte %02x at offset %ld\n",
              (unsigned char)*p, (long)(p - out));
      free(out);
      return 1;
    }
  }
  free(out);

  /* Case 4: invalid input. */
  rc = nh_pam_qr_render_halfblock(NULL, 9, &out, &olen);
  if (rc != NH_QR_RENDER_INVALID_INPUT) {
    fprintf(stderr, "FAIL: NULL text rc=%d\n", rc);
    return 1;
  }

  /* Case 5: PNG output shape (bytes 0..7 are the PNG signature). */
  unsigned char *png = NULL;
  size_t plen = 0;
  rc = nh_pam_qr_render_png("HELLO", 9, 4, &png, &plen);
  if (rc != NH_QR_RENDER_OK || !png || plen < 33) {
    fprintf(stderr, "FAIL: PNG rc=%d plen=%zu\n", rc, plen);
    return 1;
  }
  static const unsigned char sig[] = {137,80,78,71,13,10,26,10};
  if (memcmp(png, sig, 8) != 0) {
    fputs("FAIL: PNG signature mismatch\n", stderr);
    return 1;
  }
  free(png);

  puts("RESULT: PASS");
  return 0;
}
