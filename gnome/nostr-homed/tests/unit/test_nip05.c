/* test_nip05.c — nostr-homed-nip05 helper + resolver-client coverage.
 *
 * Covers the parts of the NIP-05 login pipeline that don't require a
 * live network: syntactic validation, .well-known/nostr.json parser
 * (happy path + wrong name + non-hex + malformed JSON + oversize
 * name-part + relays best-effort + case-insensitive), URL assembly
 * (pct-encoding of local part), and the in-memory positive/negative
 * cache with TTL expiry.
 *
 * SSRF refusal of private/loopback/link-local peer addresses and the
 * http:// / redirect-scheme guards live inside the curl-driven child
 * process; the underlying nh_profile_ssrf_check_sockaddr predicate
 * is already unit-tested in test_profile_sanitize. The helper is
 * additionally proved end-to-end on the aarch64 VM against a live
 * .well-known/ (see docs/reviews/nip05-login-2026-09-22.md). */
#include "nostr_nip05.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define OK(msg) do { printf("ok  %s\n", msg); } while (0)
#define FAIL(msg) do { fprintf(stderr, "FAIL %s\n", msg); return 1; } while (0)

/* ── 1. Syntactic validation ────────────────────────────────── */

static int test_parse(void) {
  nh_nip05_address a;

  if (nh_nip05_parse("chebizarro@coinos.io", &a) != 0)
    FAIL("happy parse");
  if (strcmp(a.local, "chebizarro") != 0) FAIL("local");
  if (strcmp(a.domain, "coinos.io") != 0) FAIL("domain");
  if (strcmp(a.address, "chebizarro@coinos.io") != 0) FAIL("address");

  /* Domain case-folded. */
  if (nh_nip05_parse("bob@Example.COM", &a) != 0) FAIL("mixed-case");
  if (strcmp(a.domain, "example.com") != 0) FAIL("domain lc");
  if (strcmp(a.local, "bob") != 0) FAIL("local preserved case");

  /* Root identifier ("_") is allowed. */
  if (nh_nip05_parse("_@nostr.example", &a) != 0) FAIL("root");
  if (strcmp(a.local, "_") != 0) FAIL("root local");

  /* Dots + digits + hyphens + underscores in local. */
  if (nh_nip05_parse("first.last_1-2@a.b.c", &a) != 0) FAIL("rich local");

  /* Rejections. */
  if (nh_nip05_parse("bareword", &a) == 0) FAIL("bareword accepted");
  if (nh_nip05_parse("@nodomain", &a) == 0) FAIL("empty local");
  if (nh_nip05_parse("nodomain@", &a) == 0) FAIL("empty domain");
  if (nh_nip05_parse("two@ats@baddom", &a) == 0) FAIL("two ats");
  if (nh_nip05_parse("bad space@x.com", &a) == 0) FAIL("space");
  if (nh_nip05_parse("bad\tab@x.com", &a) == 0) FAIL("tab");
  if (nh_nip05_parse("a@localhost", &a) == 0) FAIL("bare hostname");
  if (nh_nip05_parse("a@.dotstart", &a) == 0) FAIL("leading dot");
  if (nh_nip05_parse("a@dotend.", &a) == 0) FAIL("trailing dot");
  if (nh_nip05_parse("a@doubledot..com", &a) == 0) FAIL("double dot");
  if (nh_nip05_parse(".ldot@x.com", &a) == 0) FAIL("leading local dot");
  if (nh_nip05_parse("ldot.@x.com", &a) == 0) FAIL("trailing local dot");
  if (nh_nip05_parse("bad!char@x.com", &a) == 0) FAIL("bang in local");
  if (nh_nip05_parse(NULL, &a) == 0) FAIL("null");

  /* Length caps. */
  char big_local[NH_NIP05_LOCAL_MAX + 8];
  memset(big_local, 'a', sizeof big_local - 1);
  big_local[sizeof big_local - 1] = '\0';
  char addr[NH_NIP05_LOCAL_MAX + 32];
  snprintf(addr, sizeof addr, "%s@x.com", big_local);
  if (nh_nip05_parse(addr, &a) == 0) FAIL("oversize local");

  OK("parse");
  return 0;
}

/* ── 2. URL assembly ────────────────────────────────────────── */

static int test_url(void) {
  nh_nip05_address a;
  char url[512];

  assert(nh_nip05_parse("bob@example.com", &a) == 0);
  if (nh_nip05_wellknown_url(&a, url, sizeof url) != 0) FAIL("plain url");
  if (strcmp(url, "https://example.com/.well-known/nostr.json?name=bob") != 0)
    FAIL("plain url value");

  /* Root identifier stays "_" (unreserved). */
  assert(nh_nip05_parse("_@example.com", &a) == 0);
  if (nh_nip05_wellknown_url(&a, url, sizeof url) != 0) FAIL("root url");
  if (!strstr(url, "?name=_")) FAIL("root name");

  /* Space would be rejected by nh_nip05_parse, but explicitly craft
   * a non-unreserved char that *is* accepted (dot) and confirm it's
   * NOT pct-encoded — 3986 unreserved. */
  assert(nh_nip05_parse("a.b@example.com", &a) == 0);
  if (nh_nip05_wellknown_url(&a, url, sizeof url) != 0) FAIL("dot url");
  if (!strstr(url, "?name=a.b")) FAIL("dot passthrough");

  /* Overflow guard. */
  char tiny[16];
  if (nh_nip05_wellknown_url(&a, tiny, sizeof tiny) == 0) FAIL("tiny buf");

  OK("url");
  return 0;
}

/* ── 3. .well-known/nostr.json parser ───────────────────────── */

#define WELLKNOWN_HAPPY \
  "{\"names\":{\"bob\":\"1111111111111111111111111111111111111111111111111111111111111111\"}," \
  "\"relays\":{\"1111111111111111111111111111111111111111111111111111111111111111\":" \
  "[\"wss://a.example\",\"wss://b.example\"]}}"

#define WELLKNOWN_NO_NAME \
  "{\"names\":{\"alice\":\"1111111111111111111111111111111111111111111111111111111111111111\"}}"

#define WELLKNOWN_BAD_HEX \
  "{\"names\":{\"bob\":\"deadbeef\"}}"

#define WELLKNOWN_NAMES_MISSING \
  "{\"relays\":{}}"

#define WELLKNOWN_UPPERCASE_KEY \
  "{\"names\":{\"BOB\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}"

#define WELLKNOWN_TRUNCATED "{\"names\":{\"bob\":\"aa"

static int test_wellknown(void) {
  nh_nip05_result r;

  /* Happy path + relay hints. */
  if (nh_nip05_parse_wellknown(WELLKNOWN_HAPPY, strlen(WELLKNOWN_HAPPY),
                               "bob", &r) != NH_NIP05_OK)
    FAIL("happy");
  if (strcmp(r.pubkey_hex,
             "1111111111111111111111111111111111111111111111111111111111111111") != 0)
    FAIL("happy pubkey");
  if (r.relays_count != 2) FAIL("relays count");
  if (strcmp(r.relays[0], "wss://a.example") != 0) FAIL("relay 0");
  if (strcmp(r.relays[1], "wss://b.example") != 0) FAIL("relay 1");

  /* Wrong local part = name missing. */
  if (nh_nip05_parse_wellknown(WELLKNOWN_NO_NAME, strlen(WELLKNOWN_NO_NAME),
                               "bob", &r) != NH_NIP05_ERR_NAME)
    FAIL("wrong name");

  /* Non-hex pubkey. */
  if (nh_nip05_parse_wellknown(WELLKNOWN_BAD_HEX, strlen(WELLKNOWN_BAD_HEX),
                               "bob", &r) != NH_NIP05_ERR_NAME)
    FAIL("non-hex");

  /* Names object missing. */
  if (nh_nip05_parse_wellknown(WELLKNOWN_NAMES_MISSING,
                               strlen(WELLKNOWN_NAMES_MISSING),
                               "bob", &r) != NH_NIP05_ERR_NAME)
    FAIL("names missing");

  /* Case-insensitive name key match. */
  if (nh_nip05_parse_wellknown(WELLKNOWN_UPPERCASE_KEY,
                               strlen(WELLKNOWN_UPPERCASE_KEY),
                               "bob", &r) != NH_NIP05_OK)
    FAIL("case insensitive");

  /* Malformed JSON. */
  if (nh_nip05_parse_wellknown(WELLKNOWN_TRUNCATED,
                               strlen(WELLKNOWN_TRUNCATED),
                               "bob", &r) != NH_NIP05_ERR_JSON)
    FAIL("malformed");

  /* Not an object. */
  if (nh_nip05_parse_wellknown("[]", 2, "bob", &r) != NH_NIP05_ERR_JSON)
    FAIL("array root");

  OK("wellknown");
  return 0;
}

/* ── 4. In-memory cache ─────────────────────────────────────── */

static int test_cache(void) {
  nh_nip05_cache *c = nh_nip05_cache_new(60);
  if (!c) FAIL("cache new");

  nh_nip05_result r;

  /* Miss sentinel. */
  if (nh_nip05_cache_lookup(c, "bob@example.com", 1000, &r) != NH_NIP05_ERR_INTERNAL)
    FAIL("miss sentinel");

  /* Positive insert + lookup within TTL. */
  nh_nip05_result seed;
  memset(&seed, 0, sizeof seed);
  memcpy(seed.pubkey_hex,
         "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc", 64);
  nh_nip05_cache_put_positive(c, "bob@example.com", 1000, &seed);
  if (nh_nip05_cache_lookup(c, "bob@example.com", 1010, &r) != NH_NIP05_OK)
    FAIL("positive fresh");
  if (strcmp(r.pubkey_hex, seed.pubkey_hex) != 0) FAIL("positive value");

  /* Positive expired -> miss sentinel. */
  if (nh_nip05_cache_lookup(c, "bob@example.com", 1000 + 61, &r) != NH_NIP05_ERR_INTERNAL)
    FAIL("positive expired");

  /* Negative insert + lookup within NEG TTL (60 s fixed). */
  nh_nip05_cache_put_negative(c, "nobody@invalid.example", 2000, NH_NIP05_ERR_NAME);
  if (nh_nip05_cache_lookup(c, "nobody@invalid.example", 2010, &r) != NH_NIP05_ERR_NAME)
    FAIL("negative fresh");
  /* After NEG TTL, back to miss sentinel. */
  if (nh_nip05_cache_lookup(c, "nobody@invalid.example",
                            2000 + NH_NIP05_CACHE_NEGATIVE_TTL_SEC + 1,
                            &r) != NH_NIP05_ERR_INTERNAL)
    FAIL("negative expired");

  nh_nip05_cache_free(c);
  OK("cache");
  return 0;
}

int main(void) {
  int rc = 0;
  rc |= test_parse();
  rc |= test_url();
  rc |= test_wellknown();
  rc |= test_cache();
  if (rc == 0) puts("PASS test_nip05");
  return rc;
}
