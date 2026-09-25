/*
 * hanami-blossom-shim.c - PNG-shim wrapper for Blossom uploads (nostrc-bpum)
 *
 * SPDX-License-Identifier: MIT
 *
 * See hanami-blossom-shim.h for the wire layout and design rationale.
 */

#include "hanami/hanami-blossom-shim.h"
#include "hanami/hanami-blossom-client.h"

#include <openssl/evp.h>

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
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

bool
hanami_blossom_shim_active(void)
{
    /* Uncached — a single getenv() per chunk is cheap and lets tests
     * toggle the flag between calls. See hanami_blossom_shim_active_for
     * for the probe-driven auto-decide (nostrc-si30) that folds
     * capability data into the decision. */
    const char *e = getenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM");
    return (e && e[0] == '1' && e[1] == '\0');
}

/* -----------------------------------------------------------------------
 * Auto-shim decision (nostrc-si30) — URL-keyed capability cache.
 *
 * A tiny process-local map keyed by the exact endpoint URL. Each entry
 * remembers the raw_random_ok / png_shim_ok flags observed by a probe.
 * The map is grow-only within a process (Blossom endpoint sets are
 * bounded to a small handful for real-world pushers), thread-guarded
 * by a mutex around insert + lookup.
 *
 * The cache is populated in two ways:
 *   1. Explicit seed via hanami_blossom_shim_cache_set — used by tests
 *      and by callers that already ran a probe on a real client.
 *   2. Lazy probe from hanami_blossom_shim_active_for — mints a
 *      short-lived hanami_blossom_client_t against the URL and calls
 *      hanami_server_probe_capabilities, then copies the flags back.
 * ---------------------------------------------------------------------- */

typedef struct shim_cache_entry {
    char *url;                                    /* strdup'd, owned */
    hanami_capability_state_t raw_random_ok;
    hanami_capability_state_t png_shim_ok;
    int64_t last_probe_ts;
    struct shim_cache_entry *next;
} shim_cache_entry_t;

static shim_cache_entry_t *g_shim_cache_head = NULL;
static pthread_mutex_t     g_shim_cache_mu   = PTHREAD_MUTEX_INITIALIZER;

/* Refresh window on the URL-keyed cache; matches the probe review
 * discipline (§porthome-blossom-shim §6). Set to 24 h; entries older
 * than this are re-probed. Session-scoped so this only matters for
 * long-running processes (syncd). */
#define SHIM_CACHE_REFRESH_SECONDS ((int64_t)24 * 60 * 60)

/* MUST be called with g_shim_cache_mu held. */
static shim_cache_entry_t *shim_cache_find_locked(const char *url)
{
    for (shim_cache_entry_t *e = g_shim_cache_head; e; e = e->next) {
        if (e->url && strcmp(e->url, url) == 0) return e;
    }
    return NULL;
}

/* MUST be called with g_shim_cache_mu held. */
static shim_cache_entry_t *shim_cache_insert_locked(const char *url)
{
    shim_cache_entry_t *e = (shim_cache_entry_t *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->url = url ? strdup(url) : NULL;
    if (url && !e->url) { free(e); return NULL; }
    e->raw_random_ok = HANAMI_CAP_UNKNOWN;
    e->png_shim_ok   = HANAMI_CAP_UNKNOWN;
    e->last_probe_ts = 0;
    e->next = g_shim_cache_head;
    g_shim_cache_head = e;
    return e;
}

void hanami_blossom_shim_cache_reset(void)
{
    pthread_mutex_lock(&g_shim_cache_mu);
    shim_cache_entry_t *e = g_shim_cache_head;
    while (e) {
        shim_cache_entry_t *n = e->next;
        free(e->url);
        free(e);
        e = n;
    }
    g_shim_cache_head = NULL;
    pthread_mutex_unlock(&g_shim_cache_mu);
}

void hanami_blossom_shim_cache_set(const char *url,
                                   hanami_capability_state_t raw_random_ok,
                                   hanami_capability_state_t png_shim_ok)
{
    if (!url) return;
    pthread_mutex_lock(&g_shim_cache_mu);
    shim_cache_entry_t *e = shim_cache_find_locked(url);
    if (!e) e = shim_cache_insert_locked(url);
    if (e) {
        e->raw_random_ok = raw_random_ok;
        e->png_shim_ok   = png_shim_ok;
        /* Explicit seed counts as a fresh observation — the caller
         * either ran a real probe or asserted a known truth. */
        e->last_probe_ts = 1; /* non-zero sentinel */
    }
    pthread_mutex_unlock(&g_shim_cache_mu);
}

/* Read env-var override. Returns:
 *   +1 -> explicit "1" (force shim ON)
 *    0 -> explicit "0" (force shim OFF)
 *   -1 -> unset or malformed value (auto-decide)
 */
static int shim_env_override(void)
{
    const char *e = getenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM");
    if (!e || !e[0]) return -1;
    if (e[0] == '1' && e[1] == '\0') return 1;
    if (e[0] == '0' && e[1] == '\0') return 0;
    return -1;
}

/* Probe kill-switch check — matches upload_batch's behaviour so
 * NOSTR_HOMED_HANAMI_SKIP_CAPABILITY_PROBE=1 blocks EVERY probe. */
static bool shim_probe_disabled(void)
{
    const char *e = getenv("NOSTR_HOMED_HANAMI_SKIP_CAPABILITY_PROBE");
    return (e && e[0] == '1' && e[1] == '\0');
}

/* Run the extended probe against @url and copy raw/png flags back into
 * the cache under the URL. No-op if url is NULL/empty or the probe is
 * disabled. Uses an ephemeral key inside libhanami's probe — no signer
 * is required. */
static void shim_probe_url(const char *url)
{
    if (!url || !url[0]) return;
    if (shim_probe_disabled()) return;

    hanami_blossom_client_opts_t opts = {0};
    opts.endpoint        = url;
    opts.timeout_seconds = 10;
    opts.user_agent      = "libhanami/shim-probe (nostrc-si30)";
    hanami_blossom_client_t *c = NULL;
    if (hanami_blossom_client_new(&opts, NULL, &c) != HANAMI_OK) return;

    (void)hanami_server_probe_capabilities(c);
    const hanami_server_capabilities_t *caps =
        hanami_blossom_get_capabilities(c);

    if (caps) {
        pthread_mutex_lock(&g_shim_cache_mu);
        shim_cache_entry_t *e = shim_cache_find_locked(url);
        if (!e) e = shim_cache_insert_locked(url);
        if (e) {
            e->raw_random_ok = caps->raw_random_ok;
            e->png_shim_ok   = caps->png_shim_ok;
            e->last_probe_ts = caps->last_probe_ts ? caps->last_probe_ts : 1;
        }
        pthread_mutex_unlock(&g_shim_cache_mu);
    }

    hanami_blossom_client_free(c);
}

bool
hanami_blossom_shim_active_for_ex(const char *const *server_urls, size_t count,
                                  hanami_blossom_shim_reason_t *out_reason,
                                  char *out_reason_url_buf,
                                  size_t url_buf_cap)
{
    if (out_reason) *out_reason = HANAMI_SHIM_REASON_UNKNOWN;
    if (out_reason_url_buf && url_buf_cap > 0) out_reason_url_buf[0] = '\0';

    int env = shim_env_override();
    if (env == 1) {
        if (out_reason) *out_reason = HANAMI_SHIM_REASON_ENV_FORCE_ON;
        return true;
    }
    if (env == 0) {
        if (out_reason) *out_reason = HANAMI_SHIM_REASON_ENV_FORCE_OFF;
        return false;
    }

    /* No URL context or all callers explicitly opted out of probing:
     * fall back to the env-var-only reader. */
    if (!server_urls || count == 0) {
        if (out_reason) *out_reason = HANAMI_SHIM_REASON_AUTO_FALLBACK;
        return hanami_blossom_shim_active();
    }

    /* Ensure a cache entry per URL; probe UNKNOWNs lazily. */
    for (size_t i = 0; i < count; i++) {
        const char *u = server_urls[i];
        if (!u || !u[0]) continue;

        pthread_mutex_lock(&g_shim_cache_mu);
        shim_cache_entry_t *e = shim_cache_find_locked(u);
        bool need_probe = false;
        if (!e) {
            e = shim_cache_insert_locked(u);
            need_probe = true;
        } else if (e->last_probe_ts == 0) {
            need_probe = true;
        }
        pthread_mutex_unlock(&g_shim_cache_mu);

        if (need_probe) shim_probe_url(u);
    }

    /* Evaluate. Rule 1: any (raw=NO && shim=YES) wins → shim ON. */
    for (size_t i = 0; i < count; i++) {
        const char *u = server_urls[i];
        if (!u || !u[0]) continue;
        pthread_mutex_lock(&g_shim_cache_mu);
        shim_cache_entry_t *e = shim_cache_find_locked(u);
        bool require_shim = (e &&
                             e->raw_random_ok == HANAMI_CAP_NO &&
                             e->png_shim_ok   == HANAMI_CAP_YES);
        pthread_mutex_unlock(&g_shim_cache_mu);
        if (require_shim) {
            if (out_reason) *out_reason = HANAMI_SHIM_REASON_AUTO_REQUIRED;
            if (out_reason_url_buf && url_buf_cap > 0) {
                size_t n = strlen(u);
                if (n >= url_buf_cap) n = url_buf_cap - 1;
                memcpy(out_reason_url_buf, u, n);
                out_reason_url_buf[n] = '\0';
            }
            return true;
        }
    }

    /* Rule 2: all servers raw_random_ok=YES → shim OFF. */
    bool all_raw_ok = true;
    for (size_t i = 0; i < count; i++) {
        const char *u = server_urls[i];
        if (!u || !u[0]) { all_raw_ok = false; break; }
        pthread_mutex_lock(&g_shim_cache_mu);
        shim_cache_entry_t *e = shim_cache_find_locked(u);
        bool ok = (e && e->raw_random_ok == HANAMI_CAP_YES);
        pthread_mutex_unlock(&g_shim_cache_mu);
        if (!ok) { all_raw_ok = false; break; }
    }
    if (all_raw_ok) {
        if (out_reason) *out_reason = HANAMI_SHIM_REASON_AUTO_RAW_OK;
        return false;
    }

    /* Rule 3: at least one server UNKNOWN and probe couldn't (or wouldn't)
     * fill it in. Default: shim OFF (conservative — the env-var override
     * exists for the operator who KNOWS they need it). */
    if (out_reason) *out_reason = HANAMI_SHIM_REASON_AUTO_FALLBACK;
    return false;
}

bool
hanami_blossom_shim_active_for(const char *const *server_urls, size_t count)
{
    return hanami_blossom_shim_active_for_ex(server_urls, count,
                                             NULL, NULL, 0);
}

hanami_error_t
hanami_blossom_shim_sha256(const uint8_t *ct, size_t ct_len,
                           uint8_t out_hash[32])
{
    if (!out_hash) return HANAMI_ERR_INVALID_ARG;
    if (!ct && ct_len > 0) return HANAMI_ERR_INVALID_ARG;
    /* Match the encode-side cap so callers get the same rejection on
     * both sides of the boundary. */
    if (ct_len > (size_t)UINT32_MAX) return HANAMI_ERR_INVALID_ARG;

    uint8_t prefix[HANAMI_BLOSSOM_PNG_SHIM_LEN];
    write_shim_prefix(prefix, (uint32_t)ct_len);

    /* Use the EVP interface — the low-level SHA256_* API is deprecated
     * in OpenSSL 3. Streams the 41-byte prefix and the ciphertext into
     * the same digest so we never allocate shim||ct in memory. */
    EVP_MD_CTX *ctx_md = EVP_MD_CTX_new();
    if (!ctx_md) return HANAMI_ERR_NOMEM;
    hanami_error_t err = HANAMI_ERR_NOMEM;
    if (EVP_DigestInit_ex(ctx_md, EVP_sha256(), NULL) != 1) goto out;
    if (EVP_DigestUpdate(ctx_md, prefix, sizeof prefix) != 1) goto out;
    if (ct_len > 0 && EVP_DigestUpdate(ctx_md, ct, ct_len) != 1) goto out;
    unsigned int outlen = 0;
    if (EVP_DigestFinal_ex(ctx_md, out_hash, &outlen) != 1) goto out;
    err = (outlen == 32) ? HANAMI_OK : HANAMI_ERR_NOMEM;
out:
    EVP_MD_CTX_free(ctx_md);
    return err;
}
