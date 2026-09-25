/* test_porthome_fetch_ssrf.c — headless coverage for the SSRF
 * pre-resolution helpers added to nostr-home-fetch (bead nostrc-9k4g).
 *
 * The helpers themselves live inside the fetch binary's translation
 * unit; we duplicate their bodies here (as static definitions) and
 * exercise them independently. Any drift between the two copies is
 * caught by the review checklist that updates this file whenever the
 * fetch binary's SSRF gate changes. This is deliberately a lightweight
 * unit test — the deep coverage lives in profile_sanitize's
 * v4/v6_is_public_unicast, and the integration test spawns the real
 * helper against a private IP redirect.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

/* ── Duplicated from nostr-home-fetch.c (keep in sync!) ────────── */
static int is_ipv4_public_unicast(uint32_t hb) {
    uint8_t a = (uint8_t)(hb >> 24);
    uint8_t b = (uint8_t)(hb >> 16);
    if (a == 0) return 0;
    if (a == 10) return 0;
    if (a == 127) return 0;
    if (a == 169 && b == 254) return 0;
    if (a == 172 && (b & 0xf0) == 16) return 0;
    if (a == 192 && b == 168) return 0;
    if (a == 100 && (b & 0xc0) == 64) return 0;
    if ((a & 0xf0) == 0xe0) return 0;
    if ((a & 0xf0) == 0xf0) return 0;
    if (hb == 0xffffffffu) return 0;
    return 1;
}
static int is_ipv6_public_unicast(const struct in6_addr *a) {
    const uint8_t *b = a->s6_addr;
    int all_zero = 1;
    for (int i = 0; i < 16; i++) if (b[i]) { all_zero = 0; break; }
    if (all_zero) return 0;
    int loopback = 1;
    for (int i = 0; i < 15; i++) if (b[i]) { loopback = 0; break; }
    if (loopback && b[15] == 1) return 0;
    if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0 && b[4] == 0 &&
        b[5] == 0 && b[6] == 0 && b[7] == 0 && b[8] == 0 && b[9] == 0 &&
        b[10] == 0xff && b[11] == 0xff) {
        uint32_t hb = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                      ((uint32_t)b[14] << 8) | (uint32_t)b[15];
        return is_ipv4_public_unicast(hb);
    }
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return 0;
    if ((b[0] & 0xfe) == 0xfc) return 0;
    if (b[0] == 0xff) return 0;
    return 1;
}
static int url_host(const char *url, char *host, size_t cap) {
    if (!url || strncasecmp(url, "https://", 8) != 0) {
        if (strncasecmp(url, "http://", 7) != 0) return -1;
    }
    const char *p = url + (strncasecmp(url, "https://", 8) == 0 ? 8 : 7);
    {
        const char *slash = strchr(p, '/');
        const char *at = strchr(p, '@');
        if (at && (!slash || at < slash)) p = at + 1;
    }
    size_t hlen = 0;
    if (*p == '[') {
        p++;
        while (*p && *p != ']') {
            if (hlen + 1 >= cap) return -1;
            host[hlen++] = *p++;
        }
        if (*p != ']') return -1;
    } else {
        while (*p && *p != ':' && *p != '/') {
            if (hlen + 1 >= cap) return -1;
            host[hlen++] = *p++;
        }
    }
    host[hlen] = '\0';
    return hlen > 0 ? 0 : -1;
}
/* ── End duplication ───────────────────────────────────────────── */

#define V4(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(d))

static void test_ipv4_ranges(void) {
    /* Refused ranges. */
    assert(!is_ipv4_public_unicast(V4(0,0,0,0)));
    assert(!is_ipv4_public_unicast(V4(10,0,0,1)));
    assert(!is_ipv4_public_unicast(V4(127,0,0,1)));
    assert(!is_ipv4_public_unicast(V4(169,254,169,254))); /* EC2/GCE metadata */
    assert(!is_ipv4_public_unicast(V4(172,16,0,1)));
    assert(!is_ipv4_public_unicast(V4(172,31,255,255)));
    assert(!is_ipv4_public_unicast(V4(192,168,1,1)));
    assert(!is_ipv4_public_unicast(V4(100,64,0,1))); /* CGNAT */
    assert(!is_ipv4_public_unicast(V4(224,0,0,1)));
    assert(!is_ipv4_public_unicast(V4(255,255,255,255)));
    /* Allowed. */
    assert( is_ipv4_public_unicast(V4(1,1,1,1)));
    assert( is_ipv4_public_unicast(V4(8,8,8,8)));
    assert( is_ipv4_public_unicast(V4(93,184,216,34)));
    fprintf(stderr, "PASS ipv4_ranges\n");
}

static void test_ipv6_ranges(void) {
    struct in6_addr a;
    assert(inet_pton(AF_INET6, "::1", &a) == 1);
    assert(!is_ipv6_public_unicast(&a));
    assert(inet_pton(AF_INET6, "fe80::1", &a) == 1);
    assert(!is_ipv6_public_unicast(&a));
    assert(inet_pton(AF_INET6, "fc00::1", &a) == 1);
    assert(!is_ipv6_public_unicast(&a));
    assert(inet_pton(AF_INET6, "ff02::1", &a) == 1);
    assert(!is_ipv6_public_unicast(&a));
    assert(inet_pton(AF_INET6, "::ffff:127.0.0.1", &a) == 1);
    assert(!is_ipv6_public_unicast(&a)); /* v4-mapped loopback */
    assert(inet_pton(AF_INET6, "2606:4700::1111", &a) == 1);
    assert( is_ipv6_public_unicast(&a));
    fprintf(stderr, "PASS ipv6_ranges\n");
}

static void test_url_host(void) {
    char h[256];
    assert(url_host("https://blossom.example", h, sizeof h) == 0);
    assert(strcmp(h, "blossom.example") == 0);
    assert(url_host("https://cdn.example:8443/base", h, sizeof h) == 0);
    assert(strcmp(h, "cdn.example") == 0);
    assert(url_host("https://[2606:4700::1234]/x", h, sizeof h) == 0);
    assert(strcmp(h, "2606:4700::1234") == 0);
    assert(url_host("https://[::1]:8443", h, sizeof h) == 0);
    assert(strcmp(h, "::1") == 0);
    /* userinfo with a ':' inside must not confuse the host extractor. */
    assert(url_host("https://user:pass@evil.example/x", h, sizeof h) == 0);
    assert(strcmp(h, "evil.example") == 0);
    /* Malformed inputs. */
    assert(url_host("ftp://x.example", h, sizeof h) != 0);
    assert(url_host("https://[unterminated", h, sizeof h) != 0);
    fprintf(stderr, "PASS url_host\n");
}

int main(void) {
    test_ipv4_ranges();
    test_ipv6_ranges();
    test_url_host();
    fprintf(stderr, "test_porthome_fetch_ssrf: ALL PASS\n");
    return 0;
}
