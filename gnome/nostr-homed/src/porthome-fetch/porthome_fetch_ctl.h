/*
 * porthome_fetch_ctl.h — control-payload / progress-line codec for the
 * nostr-home-fetch helper. Separate from the helper's main so unit
 * tests can drive the parsers without linking libnostr / libhanami /
 * libcurl. Bead nostrc-9k4g.
 *
 * SPDX-License-Identifier: MIT
 * EXPERIMENTAL. Gated behind NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL.
 *
 * Control payload (broker → helper, stdin):
 *   {
 *     "account_pubkey_hex":  "<64 lowercase hex>",
 *     "home_root_id_hex":    "<64 lowercase hex>",
 *     "home_key_hex":        "<64 lowercase hex>",
 *     "d_tag":               "<opaque printable, 1..96 chars>",
 *     "relays":              ["wss://..."],
 *     "blossom_servers":     ["https://..."],
 *     "bandwidth_cap_bytes": <uint>,
 *     "per_file_timeout_sec":<uint>,
 *     "max_total_bytes":     <uint>,
 *     "relay_timeout_ms":    <uint>,
 *     "allow_insecure":      false        // opt-in for local tests only
 *   }
 *
 * Every field is required except allow_insecure (default false). Any
 * extra top-level fields are refused (strict) so a hostile broker
 * cannot smuggle policy through a spelling error. The JSON parser is
 * intentionally tiny and bounded: no arbitrary nesting, no float
 * exponents, no numeric type wider than 64-bit unsigned, and no string
 * longer than 1 KiB.
 *
 * Progress line (helper → broker, stdout, one JSON object per line):
 *   {"bytes":N,"files":K,"phase":"manifest"|"chunk"|"decode"|"done"}\n
 *
 * A bounded lexer accepts exactly the shape above; anything else is a
 * hard reject. Chunk-level updates are emitted at most every 64 KiB
 * or every second, whichever is more frequent, so the broker's pipe
 * doesn't back up.
 */

#ifndef NH_PORTHOME_FETCH_CTL_H
#define NH_PORTHOME_FETCH_CTL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NH_PORTHOME_FETCH_MAX_RELAYS         8u
#define NH_PORTHOME_FETCH_MAX_SERVERS        8u
#define NH_PORTHOME_FETCH_MAX_URL_LEN        512u
#define NH_PORTHOME_FETCH_MAX_DTAG_LEN        96u
#define NH_PORTHOME_FETCH_MAX_CTL_BYTES     (16u * 1024u)
#define NH_PORTHOME_FETCH_MAX_PROGRESS_LINE  256u

/* Exit codes. The parent MUST use these to map to LIMITED_MODE vs
 * FAILED. Matches the profile-image helper convention where possible. */
#define NH_PORTHOME_FETCH_EXIT_OK              0
#define NH_PORTHOME_FETCH_EXIT_ARG            64
#define NH_PORTHOME_FETCH_EXIT_SSRF           65
#define NH_PORTHOME_FETCH_EXIT_NETWORK_FAIL   71
#define NH_PORTHOME_FETCH_EXIT_DECODE_FAIL    72
#define NH_PORTHOME_FETCH_EXIT_SIZE_CAP       73
#define NH_PORTHOME_FETCH_EXIT_TIMEOUT        74
#define NH_PORTHOME_FETCH_EXIT_DECRYPT_FAIL   75
#define NH_PORTHOME_FETCH_EXIT_INTERNAL       76

typedef struct nh_porthome_fetch_ctl {
    /* All hex fields are validated as exactly 64 lowercase hex chars. */
    char account_pubkey_hex[65];
    char home_root_id_hex[65];
    char home_key_hex[65];
    /* d-tag (public, per design §2.2). */
    char d_tag[NH_PORTHOME_FETCH_MAX_DTAG_LEN + 1];

    char   relays[NH_PORTHOME_FETCH_MAX_RELAYS][NH_PORTHOME_FETCH_MAX_URL_LEN + 1];
    size_t relays_count;
    char   blossom_servers[NH_PORTHOME_FETCH_MAX_SERVERS][NH_PORTHOME_FETCH_MAX_URL_LEN + 1];
    size_t blossom_servers_count;

    uint64_t bandwidth_cap_bytes;    /* per-file cap; 0 → helper default */
    uint32_t per_file_timeout_sec;   /* per-file wall-clock; 0 → default */
    uint64_t max_total_bytes;        /* per-home cap; 0 → default */
    uint32_t relay_timeout_ms;       /* pointer-fetch total budget; 0 → default */
    uint8_t  allow_insecure;         /* 1 iff json literal `true` */
} nh_porthome_fetch_ctl;

typedef enum {
    NH_PORTHOME_FETCH_CTL_OK               = 0,
    NH_PORTHOME_FETCH_CTL_ERR_ARG          = -1,
    NH_PORTHOME_FETCH_CTL_ERR_TOO_LARGE    = -2,
    NH_PORTHOME_FETCH_CTL_ERR_JSON         = -3,
    NH_PORTHOME_FETCH_CTL_ERR_MISSING      = -4,
    NH_PORTHOME_FETCH_CTL_ERR_URL          = -5,
    NH_PORTHOME_FETCH_CTL_ERR_HEX          = -6,
    NH_PORTHOME_FETCH_CTL_ERR_UNKNOWN_KEY  = -7,
} nh_porthome_fetch_ctl_status;

/* Parse a bounded JSON control payload. On success `*out` is filled;
 * on failure `*out` is zeroed. `data`/`len` need not be NUL-terminated.
 * Refuses:
 *   - len > NH_PORTHOME_FETCH_MAX_CTL_BYTES,
 *   - any top-level key not listed above,
 *   - any field of the wrong JSON type,
 *   - missing required field,
 *   - malformed hex, URL, or numeric range,
 *   - relays / servers list empty or > MAX_RELAYS / MAX_SERVERS. */
nh_porthome_fetch_ctl_status
nh_porthome_fetch_ctl_parse(const char *data, size_t len,
                            nh_porthome_fetch_ctl *out);

/* Return a static-string label for the status (for tests and logs).
 * Never NULL. */
const char *nh_porthome_fetch_ctl_strerror(nh_porthome_fetch_ctl_status s);

/* ────────────────────────────────────────────────────────────────────
 * Progress line lexer.
 *
 * Accepts exactly the shape:
 *   {"bytes":N,"files":K,"phase":"<enum>"}\n
 * where <enum> ∈ {"manifest","chunk","decode","done"}, N and K are
 * unsigned decimals fitting in uint64_t. Any other shape → ERR. */

typedef enum {
    NH_PORTHOME_FETCH_PHASE_MANIFEST = 1,
    NH_PORTHOME_FETCH_PHASE_CHUNK    = 2,
    NH_PORTHOME_FETCH_PHASE_DECODE   = 3,
    NH_PORTHOME_FETCH_PHASE_DONE     = 4,
} nh_porthome_fetch_phase;

typedef struct nh_porthome_fetch_progress {
    uint64_t bytes;
    uint64_t files;
    nh_porthome_fetch_phase phase;
} nh_porthome_fetch_progress;

typedef enum {
    NH_PORTHOME_FETCH_PROG_OK       = 0,
    NH_PORTHOME_FETCH_PROG_ERR      = -1,
    NH_PORTHOME_FETCH_PROG_TOO_LONG = -2,
} nh_porthome_fetch_progress_status;

/* Parse ONE progress line. `line` must NOT include a trailing '\n'; the
 * caller splits on newlines. On success fills `*out`. On failure `*out`
 * is untouched. `line_len` must be < NH_PORTHOME_FETCH_MAX_PROGRESS_LINE
 * or NH_PORTHOME_FETCH_PROG_TOO_LONG is returned. */
nh_porthome_fetch_progress_status
nh_porthome_fetch_progress_parse(const char *line, size_t line_len,
                                 nh_porthome_fetch_progress *out);

/* Render a progress line into `buf` (NUL-terminated, no trailing '\n').
 * Returns the number of bytes written excluding the NUL, or -1 on
 * buffer-too-small. */
int nh_porthome_fetch_progress_format(char *buf, size_t buf_cap,
                                      const nh_porthome_fetch_progress *p);

#ifdef __cplusplus
}
#endif

#endif /* NH_PORTHOME_FETCH_CTL_H */
