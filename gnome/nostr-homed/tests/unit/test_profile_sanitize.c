/* test_profile_sanitize.c — profile_sanitize.c coverage (text +
 * URL + SSRF check). No external deps beyond libc + POSIX. */
#include "nostr_profile.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#define OK(msg) do { printf("ok  %s\n", msg); } while (0)
#define FAIL(msg) do { fprintf(stderr, "FAIL %s\n", msg); return 1; } while (0)

static int test_sanitize_text(void) {
  char out[16];

  if (nh_profile_sanitize_text("hello", out, sizeof out) != 0) FAIL("simple ascii");
  if (strcmp(out, "hello") != 0) FAIL("simple ascii value");

  if (nh_profile_sanitize_text("  spaced  ", out, sizeof out) != 0) FAIL("trim");
  if (strcmp(out, "spaced") != 0) FAIL("trim value");

  if (nh_profile_sanitize_text("bad\x01" "ctl", out, sizeof out) == 0) FAIL("ctl accepted");
  if (nh_profile_sanitize_text("bad\x1b" "ctl", out, sizeof out) == 0) FAIL("esc accepted");
  if (nh_profile_sanitize_text("bad\ttab", out, sizeof out) != 0) FAIL("tab rejected");
  if (nh_profile_sanitize_text("", out, sizeof out) == 0) FAIL("empty accepted");
  if (nh_profile_sanitize_text("     ", out, sizeof out) == 0) FAIL("whitespace only accepted");

  /* UTF-8: "café" is 5 bytes, fits in out[16] */
  if (nh_profile_sanitize_text("caf\xc3\xa9", out, sizeof out) != 0) FAIL("utf-8 cafe");
  if (strcmp(out, "caf\xc3\xa9") != 0) FAIL("utf-8 cafe value");

  /* Overlong UTF-8 (0xc0 0x80 for NUL) is rejected */
  if (nh_profile_sanitize_text("\xc0\x80" "x", out, sizeof out) == 0) FAIL("overlong nul");

  /* U+2028 line separator rejected */
  if (nh_profile_sanitize_text("a\xe2\x80\xa8" "b", out, sizeof out) == 0) FAIL("U+2028");

  /* Truncation on codepoint boundary: 16-byte buffer → 15 useful bytes. */
  char tiny[6]; /* 5 usable + NUL */
  if (nh_profile_sanitize_text("abcdefghij", tiny, sizeof tiny) != 0) FAIL("truncate");
  if (strlen(tiny) != 5) FAIL("truncate len");

  /* Truncation must not split a multi-byte codepoint. 4-char buffer
   * for input "aébcd" → we should get exactly "aé" (a + 2-byte é = 3
   * bytes + NUL); any other outcome (e.g. "a\xc3" or "aéb") means we
   * either truncated inside a codepoint or overran the buffer. */
  char split[4]; /* 3 usable + NUL */
  if (nh_profile_sanitize_text("a\xc3\xa9" "bcd", split, sizeof split) != 0)
    FAIL("truncate utf8");
  if (strcmp(split, "a\xc3\xa9") != 0) FAIL("truncate utf8 value");
  OK("sanitize_text");
  return 0;
}

static int test_url(void) {
  if (nh_profile_validate_picture_url(NULL) == 0) FAIL("NULL accepted");
  if (nh_profile_validate_picture_url("") == 0) FAIL("empty accepted");
  if (nh_profile_validate_picture_url("http://ex.com/a.png") == 0) FAIL("http accepted");
  if (nh_profile_validate_picture_url("javascript:alert(1)") == 0) FAIL("javascript accepted");
  if (nh_profile_validate_picture_url("file:///etc/passwd") == 0) FAIL("file accepted");
  if (nh_profile_validate_picture_url("https://") == 0) FAIL("empty authority accepted");
  if (nh_profile_validate_picture_url("https://user:pass@x/y") == 0) FAIL("userinfo accepted");
  if (nh_profile_validate_picture_url("https://ex\n.com/a") == 0) FAIL("newline accepted");
  if (nh_profile_validate_picture_url("https://ex.com/x y") == 0) FAIL("space accepted");
  if (nh_profile_validate_picture_url("https://ex.com/a.png") != 0) FAIL("good url");
  if (nh_profile_validate_picture_url("https://ex.com:8443/a?b=c") != 0) FAIL("port+query");
  OK("validate_picture_url");
  return 0;
}

static int check_v4(const char *ip, int expect_ok) {
  struct sockaddr_in sa = {0};
  sa.sin_family = AF_INET;
  if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) return -1;
  int rc = nh_profile_ssrf_check_sockaddr((struct sockaddr *)&sa);
  int actually_ok = (rc == 0);
  if (actually_ok != expect_ok) {
    fprintf(stderr, "FAIL ssrf v4 %s expected=%d got=%d\n", ip, expect_ok, actually_ok);
    return -1;
  }
  return 0;
}

static int check_v6(const char *ip, int expect_ok) {
  struct sockaddr_in6 sa = {0};
  sa.sin6_family = AF_INET6;
  if (inet_pton(AF_INET6, ip, &sa.sin6_addr) != 1) return -1;
  int rc = nh_profile_ssrf_check_sockaddr((struct sockaddr *)&sa);
  int actually_ok = (rc == 0);
  if (actually_ok != expect_ok) {
    fprintf(stderr, "FAIL ssrf v6 %s expected=%d got=%d\n", ip, expect_ok, actually_ok);
    return -1;
  }
  return 0;
}

static int test_ssrf(void) {
  /* Public unicast — allowed */
  if (check_v4("1.1.1.1", 1) != 0) return 1;
  if (check_v4("8.8.8.8", 1) != 0) return 1;
  if (check_v4("140.82.121.4", 1) != 0) return 1;
  /* Refused v4 spaces */
  if (check_v4("0.0.0.0", 0) != 0) return 1;
  if (check_v4("127.0.0.1", 0) != 0) return 1;
  if (check_v4("10.0.0.1", 0) != 0) return 1;
  if (check_v4("10.255.255.255", 0) != 0) return 1;
  if (check_v4("172.16.0.1", 0) != 0) return 1;
  if (check_v4("172.31.255.255", 0) != 0) return 1;
  if (check_v4("172.32.0.1", 1) != 0) return 1; /* outside 172.16/12 */
  if (check_v4("192.168.1.1", 0) != 0) return 1;
  if (check_v4("169.254.169.254", 0) != 0) return 1; /* AWS IMDS */
  if (check_v4("100.64.0.1", 0) != 0) return 1;
  if (check_v4("100.127.255.255", 0) != 0) return 1;
  if (check_v4("100.128.0.1", 1) != 0) return 1; /* outside CGNAT */
  if (check_v4("224.0.0.1", 0) != 0) return 1;
  if (check_v4("255.255.255.255", 0) != 0) return 1;
  if (check_v4("192.0.2.1", 0) != 0) return 1;
  if (check_v4("198.18.0.1", 0) != 0) return 1;
  if (check_v4("198.51.100.1", 0) != 0) return 1;
  if (check_v4("203.0.113.1", 0) != 0) return 1;

  /* v6 */
  if (check_v6("::1", 0) != 0) return 1;
  if (check_v6("::", 0) != 0) return 1;
  if (check_v6("fe80::1", 0) != 0) return 1;
  if (check_v6("fc00::1", 0) != 0) return 1;
  if (check_v6("fd00::1", 0) != 0) return 1;
  if (check_v6("ff00::1", 0) != 0) return 1;
  if (check_v6("2001:db8::1", 0) != 0) return 1;
  if (check_v6("::ffff:10.0.0.1", 0) != 0) return 1; /* v4-mapped private */
  if (check_v6("::ffff:8.8.8.8", 1) != 0) return 1;  /* v4-mapped public */
  if (check_v6("2606:4700::1111", 1) != 0) return 1;
  if (check_v6("2620:11c::5000", 1) != 0) return 1;

  /* Bad family */
  struct sockaddr sa = {0};
  sa.sa_family = AF_UNIX;
  if (nh_profile_ssrf_check_sockaddr(&sa) == 0) FAIL("AF_UNIX accepted");
  if (nh_profile_ssrf_check_sockaddr(NULL) == 0) FAIL("NULL accepted");

  OK("ssrf_check_sockaddr");
  return 0;
}

int main(void) {
  if (test_sanitize_text() != 0) return 1;
  if (test_url() != 0) return 1;
  if (test_ssrf() != 0) return 1;
  puts("test_profile_sanitize: ALL PASS");
  return 0;
}
