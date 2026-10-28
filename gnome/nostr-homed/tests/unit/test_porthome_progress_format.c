/* Login-screen progress line for portable-home provisioning (nostrc-h10m.6).
 *
 * pam_sm_open_session renders WAIT_HOME's counters through
 * nh_auth_porthome_progress_format() and sends the line to GDM as
 * PAM_TEXT_INFO. The broker does not report totals today, so the common
 * shape is a running count; a percentage appears only with a file total. */
#include "auth_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OK(x) do { if (!(x)) { fprintf(stderr, "FAIL: %s @ %d\n", #x, __LINE__); exit(1); } } while (0)

static const char *fmt(const nh_auth_porthome_progress *p) {
  static char buf[160];
  OK(nh_auth_porthome_progress_format(p, buf, sizeof buf) > 0);
  return buf;
}

int main(void) {
  nh_auth_porthome_progress p;

  /* No counters yet (or an old broker that sends zeros): generic line. */
  OK(!strcmp(fmt(NULL), "Loading your portable home\xe2\x80\xa6"));
  memset(&p, 0, sizeof p);
  OK(!strcmp(fmt(&p), "Loading your portable home\xe2\x80\xa6"));

  /* Running count, decimal units, singular/plural. */
  p.files = 1; p.bytes = 512;
  OK(!strcmp(fmt(&p), "Loading your portable home: 1 file (512 bytes) copied so far"));
  p.files = 120; p.bytes = 45200000;
  OK(!strcmp(fmt(&p), "Loading your portable home: 120 files (45.2 MB) copied so far"));
  p.bytes = 3200000000ull;
  OK(!strcmp(fmt(&p), "Loading your portable home: 120 files (3.2 GB) copied so far"));

  /* With a file total, show a percentage. */
  p.total_files = 300; p.bytes = 45200000;
  OK(!strcmp(fmt(&p), "Loading your portable home: 40% (120 of 300 files, 45.2 MB)"));

  /* A total smaller than the count is inconsistent: fall back to the count
   * rather than printing >100%. */
  p.total_files = 100;
  OK(!strcmp(fmt(&p), "Loading your portable home: 120 files (45.2 MB) copied so far"));

  /* Too-small buffer fails cleanly with an empty string. */
  char tiny[8] = "junk";
  OK(nh_auth_porthome_progress_format(&p, tiny, sizeof tiny) == -1);
  OK(tiny[0] == '\0');
  OK(nh_auth_porthome_progress_format(&p, NULL, 10) == -1);

  printf("PASS test_porthome_progress_format\n");
  return 0;
}
