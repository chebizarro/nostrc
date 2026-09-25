/*
 * hanami-blossom-shim.h - PNG-shim wrapper for Blossom uploads (nostrc-bpum)
 *
 * SPDX-License-Identifier: MIT
 *
 * Community Blossom servers (blossom.band, blossom.primal.net) run
 * body-sniffing content-policy filters that reject uploads whose bytes
 * do NOT look like a supported media type — regardless of the request's
 * Content-Type header. See docs/reviews/porthome-blossom-content-type-
 * 2026-09-25.md §2 for the empirical evidence.
 *
 * Porthome chunks are `0x01 || nonce || ct || tag` — random-looking bytes
 * that libmagic/nostr.build classify as `application/octet-stream` and
 * reject 415. This module produces a DETERMINISTIC 41-byte PNG prefix
 * that, when prepended to a chunk, satisfies the "magic bytes + one
 * well-formed IHDR chunk" test that all common content-sniffers apply.
 *
 * Wire layout of a shimmed blob (offsets in bytes):
 *
 *     [0..8)   PNG signature   89 50 4E 47 0D 0A 1A 0A
 *     [8..12)  IHDR length     00 00 00 0D                     (13, big-endian)
 *     [12..16) IHDR type       "IHDR"
 *     [16..20) width           00 00 00 01                     (1 pixel)
 *     [20..24) height          00 00 00 01                     (1 pixel)
 *     [24]     bit depth       08
 *     [25]     colour type     00                              (grayscale)
 *     [26]     compression     00
 *     [27]     filter          00
 *     [28]     interlace       00
 *     [29..33) IHDR CRC32      constant (CRC of "IHDR" + IHDR data)
 *     [33..37) IDAT length     BE(ciphertext_len)              (varies)
 *     [37..41) IDAT type       "IDAT"
 *     [41..]   ciphertext bytes (raw — sniffers do not decompress)
 *
 * Total overhead: HANAMI_BLOSSOM_PNG_SHIM_LEN (41 bytes). No IDAT CRC and
 * no IEND chunk — a strict PNG parser would reject the file as truncated,
 * but content-sniffers walk only enough to classify (signature + first
 * chunk header) and pass. This is exactly the "PNG header + fabricated
 * IHDR + IDAT chunk with the raw ciphertext" shape called out in yo44's
 * D8 probe design.
 *
 * The blob identifier (`sha256_hex`) that goes in the porthome manifest
 * is `sha256(shim || ciphertext)` — the shim IS part of the addressable
 * blob. Convergence at the manifest level is preserved because the shim
 * is a deterministic function of ciphertext length and byte content.
 *
 * Symmetric fetch: a downloader recognises a shimmed blob by matching
 * the first bytes against the PNG signature (real D4 wire always starts
 * with 0x01), strips HANAMI_BLOSSOM_PNG_SHIM_LEN bytes, and forwards the
 * tail to the AEAD decrypter. A mis-strip fails the AEAD tag — this is
 * the belt-and-braces guard.
 *
 * NOTE: this module ships the primitives. Pusher/fetcher wiring lives
 * in nh_porthome_blossom.c and nostr-home-fetch.c respectively; the
 * decision to shim or not is currently gated by the environment
 * variable NOSTR_HOMED_BLOSSOM_PNG_SHIM=1 (see review §4). An automatic
 * capability-probe-driven decision is filed as a follow-up bead.
 */

#ifndef HANAMI_BLOSSOM_SHIM_H
#define HANAMI_BLOSSOM_SHIM_H

#include "hanami-types.h"
#include "hanami-server-capability.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * HANAMI_BLOSSOM_PNG_SHIM_LEN:
 * Compile-time constant — number of bytes the shim prepends to a
 * ciphertext blob. This is the number a fetcher strips off after
 * detecting a shimmed blob.
 */
#define HANAMI_BLOSSOM_PNG_SHIM_LEN 41

/**
 * hanami_blossom_shim_encoded_len:
 * @ciphertext_len: length of the ciphertext body to be shimmed
 *
 * Returns: the total byte length of the shimmed blob
 *          (HANAMI_BLOSSOM_PNG_SHIM_LEN + ciphertext_len).
 */
static inline size_t
hanami_blossom_shim_encoded_len(size_t ciphertext_len)
{
    return (size_t)HANAMI_BLOSSOM_PNG_SHIM_LEN + ciphertext_len;
}

/**
 * hanami_blossom_shim_encode:
 * @ct: (nullable if @ct_len == 0): ciphertext bytes
 * @ct_len: length of @ct
 * @out: (out): buffer to receive shim + ciphertext; caller sizes to at
 *              least hanami_blossom_shim_encoded_len(@ct_len)
 * @out_cap: capacity of @out
 *
 * Writes HANAMI_BLOSSOM_PNG_SHIM_LEN bytes of PNG shim followed by a
 * verbatim copy of @ct into @out. The IDAT length field inside the
 * shim is set to @ct_len (big-endian). The output is deterministic
 * for any given (@ct_len, @ct byte pattern) — same input → identical
 * output → identical sha256. This preserves porthome D4 convergence.
 *
 * Returns: HANAMI_OK on success;
 *          HANAMI_ERR_INVALID_ARG if @out is NULL or @out_cap is too small
 *              or @ct is NULL while @ct_len > 0 or @ct_len > UINT32_MAX
 *              (the IDAT length field is 32-bit).
 */
hanami_error_t
hanami_blossom_shim_encode(const uint8_t *ct, size_t ct_len,
                           uint8_t *out, size_t out_cap);

/**
 * hanami_blossom_shim_active:
 *
 * Returns: true iff the pusher should shim-wrap every ciphertext blob
 *          before hashing + uploading to Blossom. Callers on both sides
 *          of the "compute-hash / upload" boundary (the porthome
 *          provisioner in cmd_push, the syncd pusher's per-chunk loop,
 *          and the Blossom-client wrapper in nh_porthome_blossom.c) MUST
 *          consult this function to make the same decision — otherwise
 *          the manifest chunk_hash and the Blossom URL disagree and
 *          pulls 404 on every chunk (regression that bit nostrc-jmx0).
 *
 *          v1 impl: reads the environment variable
 *          `NOSTR_HOMED_BLOSSOM_PNG_SHIM`; returns true iff the value is
 *          exactly the single character "1". The check is uncached so a
 *          test process can toggle the variable between calls. When the
 *          eventual capability-cache-driven auto-shim (nostrc-si30)
 *          lands, this function is where the per-server signal folds in.
 *
 *          The env-var check is O(1) and side-effect-free.
 */
bool
hanami_blossom_shim_active(void);

/**
 * hanami_blossom_shim_sha256:
 * @ct: (nullable if @ct_len == 0): ciphertext bytes
 * @ct_len: length of @ct
 * @out_hash: (out): 32-byte buffer to receive sha256(shim_prefix || ct)
 *
 * Computes the SHA-256 of the concatenation of the deterministic
 * 41-byte PNG shim prefix (built by hanami_blossom_shim_encode with
 * IDAT length = @ct_len) and @ct, without allocating a shim||ct
 * buffer. The resulting hash is the "authoritative Blossom address"
 * of the shimmed chunk — the exact 32 bytes that MUST appear in the
 * porthome manifest for the pull to find the chunk on Blossom.
 *
 * Returns: HANAMI_OK on success;
 *          HANAMI_ERR_INVALID_ARG if @out_hash is NULL,
 *              @ct is NULL while @ct_len > 0, or
 *              @ct_len > UINT32_MAX (the shim's IDAT length field is
 *              32-bit).
 */
hanami_error_t
hanami_blossom_shim_sha256(const uint8_t *ct, size_t ct_len,
                           uint8_t out_hash[32]);

/**
 * hanami_blossom_shim_detect:
 * @buf: bytes to inspect
 * @len: length of @buf
 *
 * Returns: true iff @buf is at least HANAMI_BLOSSOM_PNG_SHIM_LEN bytes
 *          AND begins with the exact shim prefix produced by
 *          hanami_blossom_shim_encode (PNG signature + IHDR chunk header
 *          + IHDR body + IDAT chunk type). The IDAT length field and
 *          the trailing ciphertext are NOT inspected — that is the
 *          fetcher's job (compare the strip'd tail length + AEAD tag).
 *
 *          Porthome D4 ciphertext always begins with the version byte
 *          0x01, so any real ciphertext will fail this check and no
 *          strip will happen — the two representations are unambiguous.
 */
bool
hanami_blossom_shim_detect(const uint8_t *buf, size_t len);

/**
 * hanami_blossom_shim_strip:
 * @buf: full downloaded blob (shim || ciphertext)
 * @len: length of @buf
 * @out_ct: (out): pointer into @buf, past the shim; borrowed —
 *          do NOT free
 * @out_ct_len: (out): length of the ciphertext tail
 *
 * On a shimmed blob (hanami_blossom_shim_detect() returns true), sets
 * @*out_ct to `buf + HANAMI_BLOSSOM_PNG_SHIM_LEN` and @*out_ct_len to
 * `len - HANAMI_BLOSSOM_PNG_SHIM_LEN`. The AEAD tag check downstream
 * is the belt-and-braces guard against a false-positive detect.
 *
 * Returns: HANAMI_OK on success;
 *          HANAMI_ERR_INVALID_ARG if @buf is NULL, @out_ct is NULL,
 *              @out_ct_len is NULL, or @buf is not a shimmed blob.
 */
hanami_error_t
hanami_blossom_shim_strip(const uint8_t *buf, size_t len,
                          const uint8_t **out_ct,
                          size_t *out_ct_len);

/**
 * hanami_blossom_shim_active_for:
 * @server_urls: array of destination Blossom endpoint URLs
 * @count: number of URLs in @server_urls
 *
 * Probe-driven auto-shim decision (nostrc-si30). Returns true iff the
 * pusher should shim-wrap every ciphertext blob before hashing +
 * uploading for THIS push, given the destination server set.
 *
 * Precedence:
 *   1. Explicit env-var override always wins:
 *        NOSTR_HOMED_BLOSSOM_PNG_SHIM=1  -> true  (force shim)
 *        NOSTR_HOMED_BLOSSOM_PNG_SHIM=0  -> false (force raw)
 *   2. Otherwise the decision is per-server, driven by the capability
 *      cache populated by hanami_server_probe_capabilities:
 *        - ANY server with raw_random_ok=NO AND png_shim_ok=YES -> true
 *        - ALL servers with raw_random_ok=YES                    -> false
 *        - otherwise (UNKNOWN or ambiguous)                      -> false
 *   3. If some server's capability is UNKNOWN, this function will run
 *      hanami_server_probe_capabilities inline against that server so
 *      the auto-decide has real data — UNLESS
 *      NOSTR_HOMED_HANAMI_SKIP_CAPABILITY_PROBE=1 is set, in which case
 *      it falls back to hanami_blossom_shim_active() (env-var only).
 *
 * The URL-keyed capability cache is session-scoped (process-local) and
 * lives inside libhanami. Callers do NOT need to plumb hanami clients
 * through — @server_urls is enough to drive the decision.
 *
 * @count == 0 or @server_urls == NULL falls back to
 * hanami_blossom_shim_active() (the env-var-only reader).
 */
bool
hanami_blossom_shim_active_for(const char *const *server_urls, size_t count);

/**
 * hanami_blossom_shim_cache_reset:
 *
 * Clears the URL-keyed capability cache used by
 * hanami_blossom_shim_active_for. For tests and diagnostics.
 */
void hanami_blossom_shim_cache_reset(void);

/**
 * hanami_blossom_shim_cache_set:
 * @server_url: NUL-terminated Blossom endpoint URL
 * @raw_random_ok: cached raw-random probe outcome for this URL
 * @png_shim_ok:   cached PNG-shim probe outcome for this URL
 *
 * Explicitly seeds the shim module's per-URL cache. Used by tests and
 * by callers that already ran the probe (via hanami_server_probe_
 * capabilities) and want to hand the result back to the auto-decide
 * helper without triggering a duplicate probe.
 */
void hanami_blossom_shim_cache_set(const char *server_url,
                                   hanami_capability_state_t raw_random_ok,
                                   hanami_capability_state_t png_shim_ok);

/**
 * hanami_blossom_shim_reason_t:
 * Diagnostic label describing WHY the last hanami_blossom_shim_active_for
 * call returned the value it did. Written by the helper into a small
 * thread-unsafe scratch buffer that callers can pass to the log line.
 */
typedef enum {
    HANAMI_SHIM_REASON_UNKNOWN   = 0,
    HANAMI_SHIM_REASON_ENV_FORCE_ON,  /* NOSTR_HOMED_BLOSSOM_PNG_SHIM=1 */
    HANAMI_SHIM_REASON_ENV_FORCE_OFF, /* NOSTR_HOMED_BLOSSOM_PNG_SHIM=0 */
    HANAMI_SHIM_REASON_AUTO_REQUIRED, /* probe: server needs shim */
    HANAMI_SHIM_REASON_AUTO_RAW_OK,   /* probe: all servers accept raw */
    HANAMI_SHIM_REASON_AUTO_FALLBACK, /* probe skipped / UNKNOWN caps */
} hanami_blossom_shim_reason_t;

/**
 * hanami_blossom_shim_active_for_ex:
 *
 * As hanami_blossom_shim_active_for but also fills in @out_reason and
 * (if non-NULL) writes the URL that tipped the decision into
 * @out_reason_url_buf (size @url_buf_cap). Used by the pusher log line.
 */
bool
hanami_blossom_shim_active_for_ex(const char *const *server_urls, size_t count,
                                  hanami_blossom_shim_reason_t *out_reason,
                                  char *out_reason_url_buf,
                                  size_t url_buf_cap);

#ifdef __cplusplus
}
#endif

#endif /* HANAMI_BLOSSOM_SHIM_H */
