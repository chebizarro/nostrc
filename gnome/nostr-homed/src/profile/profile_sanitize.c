/* profile_sanitize.c — text/URL/SSRF hygiene for the profile pipeline.
 *
 * These functions are the security boundary between untrusted
 * relay-supplied kind-0 metadata and the AccountsService D-Bus writes
 * that end up shown on the greeter. Every string that reaches the
 * greeter passes through nh_profile_sanitize_text; every URL that the
 * image-downloader child dials passes through
 * nh_profile_validate_picture_url and then, on the resolved peer,
 * nh_profile_ssrf_check_sockaddr. Keeping the three helpers in one
 * translation unit lets the unit tests exercise them without pulling
 * in libnostr or GdkPixbuf. */
#define _GNU_SOURCE
#include "nostr_profile.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* ── UTF-8 aware text sanitiser ─────────────────────────────── */

/* Decode one UTF-8 codepoint starting at *p (length @avail). Returns:
 *   > 0  number of bytes consumed
 *     0  incomplete / invalid sequence
 * *cp receives the decoded codepoint on success. Rejects overlong
 * encodings and surrogates. */
static size_t utf8_decode(const unsigned char *p, size_t avail, uint32_t *cp) {
  if (avail == 0) return 0;
  unsigned char b0 = p[0];
  if (b0 < 0x80) { *cp = b0; return 1; }
  if ((b0 & 0xe0) == 0xc0) {
    if (avail < 2 || (p[1] & 0xc0) != 0x80) return 0;
    uint32_t v = ((uint32_t)(b0 & 0x1f) << 6) | (uint32_t)(p[1] & 0x3f);
    if (v < 0x80) return 0; /* overlong */
    *cp = v; return 2;
  }
  if ((b0 & 0xf0) == 0xe0) {
    if (avail < 3 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80) return 0;
    uint32_t v = ((uint32_t)(b0 & 0x0f) << 12) |
                 ((uint32_t)(p[1] & 0x3f) << 6) |
                 (uint32_t)(p[2] & 0x3f);
    if (v < 0x800) return 0;                    /* overlong */
    if (v >= 0xd800 && v <= 0xdfff) return 0;   /* surrogate */
    *cp = v; return 3;
  }
  if ((b0 & 0xf8) == 0xf0) {
    if (avail < 4 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80 ||
        (p[3] & 0xc0) != 0x80)
      return 0;
    uint32_t v = ((uint32_t)(b0 & 0x07) << 18) |
                 ((uint32_t)(p[1] & 0x3f) << 12) |
                 ((uint32_t)(p[2] & 0x3f) << 6) |
                 (uint32_t)(p[3] & 0x3f);
    if (v < 0x10000 || v > 0x10ffff) return 0;
    *cp = v; return 4;
  }
  return 0;
}

/* Refused codepoints: ASCII controls (< 0x20 except space + tab), DEL,
 * C1 controls (0x80..0x9f), U+2028 / U+2029 line separators, and BOM. */
static int cp_is_forbidden(uint32_t cp) {
  if (cp < 0x20 && cp != ' ' && cp != '\t') return 1;
  if (cp == 0x7f) return 1;
  if (cp >= 0x80 && cp <= 0x9f) return 1;
  if (cp == 0x2028 || cp == 0x2029) return 1;
  if (cp == 0xfeff) return 1;
  return 0;
}

int nh_profile_sanitize_text(const char *src, char *dst, size_t dst_cap) {
  if (!src || !dst || dst_cap < 2) return -1;
  dst[0] = '\0';

  /* Skip leading ASCII whitespace. */
  const unsigned char *p = (const unsigned char *)src;
  while (*p && (*p == ' ' || *p == '\t')) p++;

  size_t src_len = strlen((const char *)p);
  size_t out = 0;
  size_t i = 0;
  int have_visible = 0;
  while (i < src_len) {
    uint32_t cp = 0;
    size_t n = utf8_decode(p + i, src_len - i, &cp);
    /* Any rejection path must NUL-out @dst before returning so callers
     * (take_str in profile_metadata) always see an empty string, never
     * a half-written prefix. */
    if (n == 0)              { dst[0] = '\0'; return -1; } /* invalid UTF-8 */
    if (cp_is_forbidden(cp)) { dst[0] = '\0'; return -1; }
    /* Won't fit (need room for NUL). Stop cleanly at the codepoint boundary. */
    if (out + n + 1 > dst_cap) break;
    memcpy(dst + out, p + i, n);
    out += n;
    i += n;
    if (cp != ' ' && cp != '\t') have_visible = 1;
  }
  /* Trim trailing ASCII whitespace. Multi-byte "whitespace" (NBSP etc.)
   * is deliberately left alone: it's rare in display names and the
   * downstream renderer handles it. */
  while (out > 0 && (dst[out - 1] == ' ' || dst[out - 1] == '\t')) out--;
  dst[out] = '\0';
  if (out == 0 || !have_visible) { dst[0] = '\0'; return -1; }
  return 0;
}

/* ── Picture URL validator ──────────────────────────────────── */

int nh_profile_validate_picture_url(const char *url) {
  if (!url) return -1;
  size_t n = strlen(url);
  if (n < 9 || n > NH_PROFILE_PICTURE_URL_MAX) return -1;
  if (strncmp(url, "https://", 8) != 0) return -1;

  /* Reject whitespace, control chars and userinfo. */
  const unsigned char *p = (const unsigned char *)url;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = p[i];
    if (c < 0x20 || c == 0x7f) return -1;
    if (c == ' ' || c == '\t') return -1;
  }
  /* Authority section: everything between "https://" and the first '/',
   * '?' or '#'. Refuse '@' (userinfo) or empty host. */
  const char *auth = url + 8;
  const char *auth_end = auth;
  while (*auth_end && *auth_end != '/' && *auth_end != '?' && *auth_end != '#')
    auth_end++;
  if (auth_end == auth) return -1; /* empty authority */
  for (const char *q = auth; q < auth_end; q++) {
    if (*q == '@') return -1; /* userinfo */
  }
  return 0;
}

/* ── SSRF: peer address check ───────────────────────────────── */

/* IPv4 refused ranges (network byte order semantics handled below). */
static int v4_is_public_unicast(uint32_t hb /* host-byte-order */) {
  uint8_t a = (uint8_t)(hb >> 24);
  uint8_t b = (uint8_t)(hb >> 16);
  /* 0.0.0.0/8 */                  if (a == 0) return 0;
  /* 10.0.0.0/8 */                 if (a == 10) return 0;
  /* 127.0.0.0/8 */                if (a == 127) return 0;
  /* 169.254.0.0/16 link-local */  if (a == 169 && b == 254) return 0;
  /* 172.16.0.0/12 */              if (a == 172 && (b & 0xf0) == 16) return 0;
  /* 192.168.0.0/16 */             if (a == 192 && b == 168) return 0;
  /* 100.64.0.0/10 CGNAT */        if (a == 100 && (b & 0xc0) == 64) return 0;
  /* 192.0.2.0/24 TEST-NET-1 */    if (a == 192 && b == 0 && ((hb >> 8) & 0xff) == 2) return 0;
  /* 198.18.0.0/15 benchmark */    if (a == 198 && (b & 0xfe) == 18) return 0;
  /* 198.51.100.0/24 TEST-NET-2 */ if (a == 198 && b == 51 && ((hb >> 8) & 0xff) == 100) return 0;
  /* 203.0.113.0/24 TEST-NET-3 */  if (a == 203 && b == 0 && ((hb >> 8) & 0xff) == 113) return 0;
  /* 224.0.0.0/4 multicast */      if ((a & 0xf0) == 0xe0) return 0;
  /* 240.0.0.0/4 reserved */       if ((a & 0xf0) == 0xf0) return 0;
  /* 255.255.255.255 broadcast */  if (hb == 0xffffffffu) return 0;
  return 1;
}

static int v6_is_public_unicast(const struct in6_addr *a) {
  const uint8_t *b = a->s6_addr;
  /* ::/128 unspecified */
  int all_zero = 1;
  for (int i = 0; i < 16; i++) if (b[i]) { all_zero = 0; break; }
  if (all_zero) return 0;
  /* ::1/128 loopback */
  int loopback = 1;
  for (int i = 0; i < 15; i++) if (b[i]) { loopback = 0; break; }
  if (loopback && b[15] == 1) return 0;
  /* IPv4-mapped ::ffff:0:0/96 → check the embedded v4 */
  if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0 && b[4] == 0 &&
      b[5] == 0 && b[6] == 0 && b[7] == 0 && b[8] == 0 && b[9] == 0 &&
      b[10] == 0xff && b[11] == 0xff) {
    uint32_t hb = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                  ((uint32_t)b[14] << 8) | (uint32_t)b[15];
    return v4_is_public_unicast(hb);
  }
  /* IPv4-compatible ::0:0:0:0:X.Y.Z.W (deprecated, treat as v4) */
  int ipv4_compat = 1;
  for (int i = 0; i < 12; i++) if (b[i]) { ipv4_compat = 0; break; }
  if (ipv4_compat) {
    uint32_t hb = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                  ((uint32_t)b[14] << 8) | (uint32_t)b[15];
    return v4_is_public_unicast(hb);
  }
  /* fe80::/10 link-local */
  if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return 0;
  /* fec0::/10 site-local (deprecated) */
  if (b[0] == 0xfe && (b[1] & 0xc0) == 0xc0) return 0;
  /* fc00::/7 ULA */
  if ((b[0] & 0xfe) == 0xfc) return 0;
  /* ff00::/8 multicast */
  if (b[0] == 0xff) return 0;
  /* 2001:db8::/32 documentation */
  if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return 0;
  /* 100::/64 discard-only */
  if (b[0] == 0x01 && b[1] == 0x00) {
    int rest_zero = 1;
    for (int i = 2; i < 8; i++) if (b[i]) { rest_zero = 0; break; }
    if (rest_zero) return 0;
  }
  return 1;
}

int nh_profile_ssrf_check_sockaddr(const struct sockaddr *sa) {
  if (!sa) return -1;
  if (sa->sa_family == AF_INET) {
    const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
    uint32_t hb = ntohl(in->sin_addr.s_addr);
    return v4_is_public_unicast(hb) ? 0 : -1;
  }
  if (sa->sa_family == AF_INET6) {
    const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
    return v6_is_public_unicast(&in6->sin6_addr) ? 0 : -1;
  }
  return -1;
}
