/* nostr_nip05.h — NIP-05 identifier resolution for the login broker.
 *
 * NIP-05 lets a user type "chebizarro@coinos.io" instead of a Linux
 * username at the GDM greeter's "Not listed?" prompt. The broker:
 *   1. syntactically validates the address (`<local>@<domain>`);
 *   2. fetches https://<domain>/.well-known/nostr.json?name=<local>
 *      through an UNPRIVILEGED helper (nostr-homed-nip05) that runs
 *      as `nobody`, refuses http://, refuses redirects to non-https,
 *      caps the body at 64 KiB, enforces a 10 s timeout, at most 3
 *      redirects, and — most importantly — verifies the resolved peer
 *      sockaddr against nh_profile_ssrf_check_sockaddr on every
 *      connect so the greeter cannot be turned into an SSRF pivot
 *      into RFC1918/loopback/link-local space;
 *   3. reads `names[<local>]` (must be 64 lc-hex) as the account
 *      pubkey and — best-effort — `relays[<pubkey>]` as an ordered
 *      relay-hint list;
 *   4. looks the pubkey up in the identity store (alias-only in v1;
 *      JIT enrolment is tracked separately);
 *   5. canonicalises PAM_USER to the account's local username and
 *      continues the normal QR / provider flow.
 *
 * This header exposes:
 *   - the syntactic validator + parser (also used by the CLI helper
 *     for its argv guard);
 *   - the resolver "client" the broker uses (fork/exec the helper,
 *     capture its JSON stdout, decode into a compact result struct);
 *   - a small per-address cache (positive + negative) so a burst of
 *     login attempts against the same NIP-05 doesn't hammer the
 *     issuer's .well-known/. Rate limiting per-address AND globally
 *     rides on top of this via the broker's existing throttle.
 *
 * NONE of the machinery in this header is authentication — the QR
 * challenge remains bound to the account pubkey; a forged NIP-05 that
 * happens to resolve to somebody else's pubkey cannot sign for them.
 * The security work here is entirely about (a) not letting the login
 * screen dial private-space HTTP targets, and (b) not letting the
 * login screen amplify traffic against arbitrary third parties. */
#ifndef NOSTR_HOMED_NIP05_H
#define NOSTR_HOMED_NIP05_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NIP-05 grammar bounds. RFC 1035 caps a DNS name at 253 bytes; the
 * NIP-05 local-part is deliberately generous (identifier UX matters).
 * The full address is bounded well below the PAM username cap so the
 * broker can pass the NIP-05 through as PAM_USER without truncation
 * before the canonicalisation step overwrites it. */
#define NH_NIP05_LOCAL_MAX   64u
#define NH_NIP05_DOMAIN_MAX  253u
#define NH_NIP05_ADDRESS_MAX (NH_NIP05_LOCAL_MAX + 1u + NH_NIP05_DOMAIN_MAX)

/* Relay-hint cap. The .well-known payload may list any number under
 * relays[<pubkey>]; we bound the copy to keep the resolver reply
 * bounded and to match the existing NH_AUTH_CONF_RELAYS_MAX shape. */
#define NH_NIP05_RELAY_HINTS_MAX 4u
#define NH_NIP05_RELAY_URL_MAX   255u

/* Return codes from nh_nip05_resolve() / nh_nip05_client_resolve().
 *
 * These map 1:1 to the CLI helper's exit codes (see
 * src/nip05/nostr-homed-nip05.c) so the parent can distinguish
 * failure classes without parsing text — matches the pattern already
 * used by the image-downloader helper. */
typedef enum nh_nip05_rc {
    NH_NIP05_OK              = 0,
    NH_NIP05_ERR_ARG         = 64,  /* address failed validation           */
    NH_NIP05_ERR_SSRF        = 65,  /* peer address refused                */
    NH_NIP05_ERR_TRANSPORT   = 66,  /* HTTP/network failure, size overflow */
    NH_NIP05_ERR_CONTENT     = 67,  /* wrong content-type or unreadable    */
    NH_NIP05_ERR_JSON        = 68,  /* JSON parse / schema failure         */
    NH_NIP05_ERR_NAME        = 69,  /* names[local] missing or not hex     */
    NH_NIP05_ERR_PRIV        = 70,  /* helper still running as root        */
    NH_NIP05_ERR_INTERNAL    = 71,  /* OOM / fork failure / etc.           */
} nh_nip05_rc;

/* Parsed address, always NUL-terminated. Filled by nh_nip05_parse.
 * `address` echoes the canonicalised input (local part left as typed
 * for case-insensitive comparisons; domain lower-cased). */
typedef struct nh_nip05_address {
    char local[NH_NIP05_LOCAL_MAX + 1];
    char domain[NH_NIP05_DOMAIN_MAX + 1];
    char address[NH_NIP05_ADDRESS_MAX + 1];
} nh_nip05_address;

/* Result of a successful resolution. `relays` is best-effort — the
 * caller MUST fall back to its own configured relays when
 * relays_count == 0. */
typedef struct nh_nip05_result {
    char pubkey_hex[65];
    char relays[NH_NIP05_RELAY_HINTS_MAX][NH_NIP05_RELAY_URL_MAX + 1];
    size_t relays_count;
} nh_nip05_result;

/* ── Validation / parsing ─────────────────────────────────────── */

/* True iff @s starts with a syntactically valid NIP-05 address.
 * Accepts:
 *   - local part matching [a-z0-9._-]+ (case-insensitive; upper-case
 *     letters are accepted and treated as their lower-case peers per
 *     NIP-05 §"Names are case-insensitive"),
 *   - local part == "_" (root identifier, per NIP-05 §"root"),
 *   - single '@',
 *   - domain matching the RFC 1035 preferred-name grammar with a
 *     leading letter/digit and no leading/trailing dot; each label
 *     bounded 63 bytes, total <= 253.
 * A leading/trailing whitespace is refused (the broker trims before
 * calling this). */
int nh_nip05_is_valid(const char *s);

/* Parse @s into @out (zeroed first). Returns 0 on success, -1 on any
 * validation failure. Case: local part is preserved as-typed (case
 * folding happens at query time), domain is lower-cased. */
int nh_nip05_parse(const char *s, nh_nip05_address *out);

/* ── Wire helpers (also used by the CLI child) ────────────────── */

/* Assemble the well-known GET URL for @addr into @out (cap = @cap
 * bytes incl. NUL). URL-encodes the local part per RFC 3986
 * unreserved (RFC 3986 §2.3) — anything outside [A-Za-z0-9._~-] is
 * pct-encoded. Returns 0 on success, -1 on argument failure or if
 * the assembled URL would overflow the caller's buffer. */
int nh_nip05_wellknown_url(const nh_nip05_address *addr, char *out, size_t cap);

/* Parse a .well-known/nostr.json response body @json (length @len)
 * for the local part @local. On success writes the 64-hex pubkey to
 * @out (result_out->pubkey_hex) and any relays[<pubkey>] entries
 * (bounded). Returns NH_NIP05_OK on success and a specific rc on
 * failure — never crashes on hostile input. */
nh_nip05_rc nh_nip05_parse_wellknown(const char *json, size_t len,
                                     const char *local,
                                     nh_nip05_result *result_out);

/* ── Resolver: parent side (broker) ───────────────────────────── */

/* Fork/exec the nostr-homed-nip05 helper with @addr on argv, dropping
 * privileges to @drop_user (defaults to "nobody"). The helper writes
 * a compact JSON object to stdout describing the resolution; the
 * parent parses it into @result_out. Returns NH_NIP05_OK on success
 * or one of the NH_NIP05_ERR_* codes.
 *
 * @helper_path may be NULL to look up "nostr-homed-nip05" on PATH;
 * tests set it to the build-tree binary. */
nh_nip05_rc nh_nip05_client_resolve(const char *helper_path,
                                    const char *drop_user,
                                    const nh_nip05_address *addr,
                                    nh_nip05_result *result_out);

/* ── Cache (broker-owned, in-memory) ──────────────────────────── */

typedef struct nh_nip05_cache nh_nip05_cache;

/* Positive TTL: how long a resolved (address -> pubkey) stays hot.
 * Negative TTL: how long a NAME_NOT_FOUND / SSRF / transport failure
 * suppresses a re-resolution of the same address. Defaults match the
 * task brief (positive 10 min, negative 60 s) and can be overridden
 * via nh_auth_conf.nip05_cache_ttl_seconds (positive) — the negative
 * TTL is kept fixed to keep the config surface small; the numbers are
 * short enough that a legitimate typo self-heals in a minute. */
#define NH_NIP05_CACHE_POSITIVE_TTL_DEFAULT_SEC 600
#define NH_NIP05_CACHE_NEGATIVE_TTL_SEC          60
#define NH_NIP05_CACHE_CAPACITY                  64u

/* Create an empty cache. Returns NULL on OOM. */
nh_nip05_cache *nh_nip05_cache_new(int positive_ttl_seconds);
void nh_nip05_cache_free(nh_nip05_cache *cache);
/* Reset the positive TTL (negative TTL is fixed). Safe to call at
 * any time; entries already in the cache keep their existing
 * timestamps and just re-evaluate on next lookup. */
void nh_nip05_cache_set_positive_ttl(nh_nip05_cache *cache, int seconds);

/* Lookup by canonicalised address (address->address). On a positive
 * hit fills @result_out and returns NH_NIP05_OK. On a suppressed
 * negative hit returns the cached NH_NIP05_ERR_* code. On a miss
 * returns NH_NIP05_ERR_INTERNAL (a sentinel — caller should proceed
 * to actually resolve). @now_seconds is the current wall-clock time;
 * pass time(NULL). */
nh_nip05_rc nh_nip05_cache_lookup(nh_nip05_cache *cache, const char *address,
                                  int64_t now_seconds,
                                  nh_nip05_result *result_out);

/* Insert a positive result. Overwrites any prior entry for the same
 * address. */
void nh_nip05_cache_put_positive(nh_nip05_cache *cache, const char *address,
                                 int64_t now_seconds,
                                 const nh_nip05_result *result);

/* Insert a negative result (rc must not be NH_NIP05_OK). */
void nh_nip05_cache_put_negative(nh_nip05_cache *cache, const char *address,
                                 int64_t now_seconds, nh_nip05_rc rc);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_HOMED_NIP05_H */
