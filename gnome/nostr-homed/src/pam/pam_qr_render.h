/* Unicode half-block QR renderer for the PAM module — design §3.4.
 *
 * Wraps the vendored Nayuki qrcodegen encoder. Byte mode, ECC L, version
 * capped at 9 (decision D11), quiet zone 2 modules on every side (design
 * §3.4 pragmatic screen minimum).
 *
 * The Phase-0 spike (docs/reviews/qr-greeter-render-spike-2026-09-22.md)
 * ruled the in-dialog QR NO-GO in the graphical GDM greeter because the
 * proportional Cantarell label wraps and mis-tiles half-block rows. The
 * PAM module therefore emits this render ONLY when it detects a text
 * console (tty1, ssh, pamtester) — the graphical greeter gets the URI +
 * pairing code only. The renderer stays wired up because it works
 * correctly on text consoles today. */
#ifndef NH_PAM_QR_RENDER_H
#define NH_PAM_QR_RENDER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rendering result codes. All are returned via `render`'s return value; the
 * output buffer contract is documented alongside. */
typedef enum nh_qr_render_rc {
  NH_QR_RENDER_OK = 0,
  NH_QR_RENDER_TOO_LONG = 1,    /* URI larger than the max-version budget */
  NH_QR_RENDER_INVALID_INPUT = 2,
  NH_QR_RENDER_OOM = 3
} nh_qr_render_rc;

/* Render `text` (any UTF-8 byte sequence; usually the nostrconnect:// URI)
 * as a NUL-terminated string of half-block glyphs (`▀`, `▄`, `█`, ` `) with
 * a quiet zone of 2 modules on every side. Rows contain (size + 2*quiet)
 * columns and (ceil((size + 2*quiet) / 2)) text rows. The last text row is
 * padded with the light half-block if the module height is odd.
 *
 *   max_version: upper version cap. Design D11 is 9 (53x53 modules); values
 *                > 40 clamp to 40.
 *   *out_buf:    heap-allocated on OK; NULL otherwise. Caller free()s.
 *   *out_len:    strlen of *out_buf on OK; 0 otherwise.
 *
 * Returns NH_QR_RENDER_OK on success, TOO_LONG if the text does not fit in
 * any version up to max_version at ECC L. Never crashes on malformed
 * input. */
nh_qr_render_rc nh_pam_qr_render_halfblock(const char *text, int max_version,
                                           char **out_buf, size_t *out_len);

/* Render `text` into a monochrome PNG (8-bit greyscale, no filter/PLTE
 * tricks — the greeter extension consumes it verbatim). Same encoder
 * parameters as `nh_pam_qr_render_halfblock`. Each QR module is emitted as
 * a `scale x scale` block (design does not fix a scale; 8-12 renders well
 * at typical greeter DPIs — the PAM module chooses).
 *
 *   *out_buf: heap-allocated PNG bytes on OK; caller free()s. NULL on error.
 *   *out_len: PNG byte length on OK; 0 otherwise.
 *
 * Uses a bundled tiny CRC32 + deflate-store PNG writer — no external image
 * library. */
nh_qr_render_rc nh_pam_qr_render_png(const char *text, int max_version,
                                     int scale, unsigned char **out_buf,
                                     size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* NH_PAM_QR_RENDER_H */
