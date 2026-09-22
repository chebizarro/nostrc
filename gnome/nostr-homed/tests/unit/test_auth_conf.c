/* auth.conf parser test (design D12).
 *
 * The parser is deliberately non-fatal: missing files, malformed lines and
 * unknown keys are all silently ignored so a typo in a config key cannot
 * lock a user out. These tests exercise the recognised keys, the trimming
 * and comment rules, and the tolerance rules. */
#include "auth_broker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_file(const char *path, const char *body) {
  FILE *f = fopen(path, "w");
  if (!f) { perror(path); exit(1); }
  fputs(body, f);
  fclose(f);
}

#define OK(x) do { if (!(x)) { fprintf(stderr, "FAIL: %s @ %d\n", #x, __LINE__); exit(1); } } while (0)

int main(void) {
  char tmp[] = "/tmp/nh-auth-conf-XXXXXX";
  int fd = mkstemp(tmp);
  OK(fd >= 0);
  close(fd);

  /* Missing file → non-fatal, all zero. */
  nh_auth_conf c;
  OK(nh_auth_conf_load("/no/such/path/nh-nonexistent.conf", &c) == 0);
  OK(c.nip46_qr_relays_count == 0);
  OK(c.nip46_qr_wait_ms == 0);
  OK(c.nip46_qr_render[0] == '\0');
  OK(c.nip46_qr_max_concurrent == 0);

  /* Recognised keys, mixed comments + whitespace. */
  write_file(tmp,
             "# nostr-authd broker sample\n"
             "; another comment form\n"
             "   \n"
             "nip46_qr_relays = wss://bunker.sharegap.net , wss://relay2.example ,not-a-url, ftp://nope, wss://relay3.example\n"
             "  nip46_qr_wait_ms   =  60000\n"
             "nip46_qr_render=uri\n"
             "nip46_qr_max_concurrent=8\n"
             "malformed line without equals\n"
             "unknown_key=whatever\n");
  OK(nh_auth_conf_load(tmp, &c) == 0);
  OK(c.nip46_qr_relays_count == 3);
  OK(strcmp(c.nip46_qr_relays[0], "wss://bunker.sharegap.net") == 0);
  OK(strcmp(c.nip46_qr_relays[1], "wss://relay2.example") == 0);
  OK(strcmp(c.nip46_qr_relays[2], "wss://relay3.example") == 0);
  OK(c.nip46_qr_wait_ms == 60000);
  OK(strcmp(c.nip46_qr_render, "uri") == 0);
  OK(c.nip46_qr_max_concurrent == 8);

  /* Empty file → zero. */
  write_file(tmp, "");
  OK(nh_auth_conf_load(tmp, &c) == 0);
  OK(c.nip46_qr_relays_count == 0);
  OK(c.nip46_qr_wait_ms == 0);

  /* Relay cap of 4 enforced. */
  write_file(tmp,
             "nip46_qr_relays=wss://a,wss://b,wss://c,wss://d,wss://e,wss://f\n");
  OK(nh_auth_conf_load(tmp, &c) == 0);
  OK(c.nip46_qr_relays_count == 4);

  /* Junk value on numeric key is dropped, not fatal. */
  write_file(tmp, "nip46_qr_wait_ms=not-a-number\n"
                  "nip46_qr_render=qr\n");
  OK(nh_auth_conf_load(tmp, &c) == 0);
  OK(c.nip46_qr_wait_ms == 0);
  OK(strcmp(c.nip46_qr_render, "qr") == 0);

  unlink(tmp);
  puts("RESULT: PASS");
  return 0;
}
