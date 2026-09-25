/*
 * hanami-blossom-shim.c - PNG-shim wrapper for Blossom uploads (nostrc-bpum)
 *
 * SPDX-License-Identifier: MIT
 *
 * See hanami-blossom-shim.h for the wire layout and design rationale.
 */

#include "hanami/hanami-blossom-shim.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * PNG-standard CRC32 (poly 0xEDB88320, init 0xFFFFFFFF, xor-out
 * 0xFFFFFFFF). PNG chunks use this over "type || data".
 *
 * The IHDR CRC in the shim never changes because the IHDR body is
 * constant (1x1 grayscale 8-bit). We compute it once at module load
 * (thread-safe: the init is a pure function of static data and yields
 * the same bytes every time). Doing it at runtime avoids a
 * hand-computed magic constant that would rot silently if the IHDR
 * shape ever changed.
 * ---------------------------------------------------------------------- */

static uint32_t g_crc32_table[256];
static int      g_crc32_table_ready = 0;

static void crc32_table_init_once(void)
{
    if (g_crc32_table_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc32_table[i] = c;
    }
    /* Publish last — pattern is idempotent and every writer produces
     * identical table contents, so a torn race is harmless. */
    g_crc32_table_ready = 1;
}

static uint32_t png_crc32(const uint8_t *bytes, size_t len)
{
    crc32_table_init_once();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = g_crc32_table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* -----------------------------------------------------------------------
 * Shim prefix bytes.
 *
 *   [0..8)   PNG signature
 *   [8..12)  IHDR length = 13 (big-endian)
 *   [12..16) IHDR type "IHDR"
 *   [16..20) width  = 1
 *   [20..24) height = 1
 *   [24..29) bit depth, colour, compression, filter, interlace
 *   [29..33) IHDR CRC32 (computed over bytes [12..29))
 *   [33..37) IDAT length placeholder (patched with ciphertext length)
 *   [37..41) IDAT type "IDAT"
 * ---------------------------------------------------------------------- */

#define OFF_IHDR_TYPE  12u
#define OFF_IHDR_END   29u  /* one past last IHDR data byte */
#define OFF_IHDR_CRC   29u
#define OFF_IDAT_LEN   33u
#define OFF_IDAT_TYPE  37u

static const uint8_t kShimTemplate[HANAMI_BLOSSOM_PNG_SHIM_LEN] = {
    /* PNG signature */
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    /* IHDR length = 13 */
    0x00, 0x00, 0x00, 0x0D,
    /* IHDR type */
    'I', 'H', 'D', 'R',
    /* width = 1 */
    0x00, 0x00, 0x00, 0x01,
    /* height = 1 */
    0x00, 0x00, 0x00, 0x01,
    /* bit depth, colour, compression, filter, interlace */
    0x08, 0x00, 0x00, 0x00, 0x00,
    /* IHDR CRC32 placeholder */
    0x00, 0x00, 0x00, 0x00,
    /* IDAT length placeholder */
    0x00, 0x00, 0x00, 0x00,
    /* IDAT type */
    'I', 'D', 'A', 'T',
};

_Static_assert(sizeof(kShimTemplate) == HANAMI_BLOSSOM_PNG_SHIM_LEN,
               "shim template size drifted from header constant");

/* Fill the IHDR CRC + IDAT length fields of the shim prefix @out. */
static void write_shim_prefix(uint8_t *out, uint32_t idat_len)
{
    memcpy(out, kShimTemplate, HANAMI_BLOSSOM_PNG_SHIM_LEN);

    /* IHDR CRC is over "IHDR" + IHDR data — bytes [12..29). */
    uint32_t crc = png_crc32(out + OFF_IHDR_TYPE, OFF_IHDR_END - OFF_IHDR_TYPE);
    out[OFF_IHDR_CRC + 0] = (uint8_t)((crc >> 24) & 0xFF);
    out[OFF_IHDR_CRC + 1] = (uint8_t)((crc >> 16) & 0xFF);
    out[OFF_IHDR_CRC + 2] = (uint8_t)((crc >>  8) & 0xFF);
    out[OFF_IHDR_CRC + 3] = (uint8_t)( crc        & 0xFF);

    out[OFF_IDAT_LEN + 0] = (uint8_t)((idat_len >> 24) & 0xFF);
    out[OFF_IDAT_LEN + 1] = (uint8_t)((idat_len >> 16) & 0xFF);
    out[OFF_IDAT_LEN + 2] = (uint8_t)((idat_len >>  8) & 0xFF);
    out[OFF_IDAT_LEN + 3] = (uint8_t)( idat_len        & 0xFF);
}

/* -----------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

hanami_error_t
hanami_blossom_shim_encode(const uint8_t *ct, size_t ct_len,
                           uint8_t *out, size_t out_cap)
{
    if (!out) return HANAMI_ERR_INVALID_ARG;
    if (!ct && ct_len > 0) return HANAMI_ERR_INVALID_ARG;
    /* IDAT length field is 32-bit; refuse ciphertexts that would need
     * a bigger chunk (porthome chunks are capped well below 4 GiB). */
    if (ct_len > (size_t)UINT32_MAX) return HANAMI_ERR_INVALID_ARG;
    if (out_cap < hanami_blossom_shim_encoded_len(ct_len))
        return HANAMI_ERR_INVALID_ARG;

    write_shim_prefix(out, (uint32_t)ct_len);
    if (ct_len > 0)
        memcpy(out + HANAMI_BLOSSOM_PNG_SHIM_LEN, ct, ct_len);
    return HANAMI_OK;
}

bool
hanami_blossom_shim_detect(const uint8_t *buf, size_t len)
{
    if (!buf) return false;
    if (len < (size_t)HANAMI_BLOSSOM_PNG_SHIM_LEN) return false;

    /* Rebuild the fixed portions of the shim prefix into a scratch
     * buffer with the IDAT length set to (len - PNG_SHIM_LEN) and
     * memcmp against the incoming bytes. Comparing the full 41-byte
     * prefix (with the correct IDAT length) is stricter than a
     * partial match and never has false positives against porthome
     * ciphertext (which always starts with 0x01).
     *
     * The AEAD tag downstream is the belt-and-braces guard against
     * false positives from an adversarial upload — see
     * hanami_blossom_shim_strip's contract.
     */
    if (len - (size_t)HANAMI_BLOSSOM_PNG_SHIM_LEN > (size_t)UINT32_MAX)
        return false;
    uint32_t idat_len = (uint32_t)(len - (size_t)HANAMI_BLOSSOM_PNG_SHIM_LEN);
    uint8_t expected[HANAMI_BLOSSOM_PNG_SHIM_LEN];
    write_shim_prefix(expected, idat_len);
    return memcmp(buf, expected, HANAMI_BLOSSOM_PNG_SHIM_LEN) == 0;
}

hanami_error_t
hanami_blossom_shim_strip(const uint8_t *buf, size_t len,
                          const uint8_t **out_ct,
                          size_t *out_ct_len)
{
    if (!buf || !out_ct || !out_ct_len) return HANAMI_ERR_INVALID_ARG;
    if (!hanami_blossom_shim_detect(buf, len)) return HANAMI_ERR_INVALID_ARG;

    *out_ct     = buf + HANAMI_BLOSSOM_PNG_SHIM_LEN;
    *out_ct_len = len - (size_t)HANAMI_BLOSSOM_PNG_SHIM_LEN;
    return HANAMI_OK;
}
