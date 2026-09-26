/* GetRelays: exercises the file+normalise path with a scoped XDG_CONFIG_HOME
 * (so the real user's relays.conf is never touched) plus the direct helpers
 * for edge cases the D-Bus dispatch also uses.
 *
 * Asserts:
 *   - Absent config → NOT_FOUND (never a stub "[]" or a network fetch).
 *   - Valid config → JSON array in read order, lowercased scheme/host, bare
 *     trailing "/" dropped, duplicates removed.
 *   - Malformed config → INVALID_JSON.
 *   - relays_from_list refuses non-relay URLs and returns NOT_FOUND for an
 *     empty list.
 */
#include "nostr/nip55l/signer_ops.h"
#include "nostr/nip55l/error.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char scratch[512];

static void set_scratch(void) {
  const char *base = getenv("TMPDIR");
  if (!base || !*base) base = "/tmp";
  snprintf(scratch, sizeof scratch, "%s/nip55l_relays_XXXXXX", base);
  assert(mkdtemp(scratch));
  /* Isolate: no HOME/XDG fallback can reach the user's own config. */
  setenv("XDG_CONFIG_HOME", scratch, 1);
  unsetenv("HOME");
}

static void write_conf(const char *body) {
  char dir[600];
  snprintf(dir, sizeof dir, "%s/nostr", scratch);
  mkdir(dir, 0700);
  char path[700];
  snprintf(path, sizeof path, "%s/relays.conf", dir);
  FILE *fp = fopen(path, "wb");
  assert(fp);
  if (body) fwrite(body, 1, strlen(body), fp);
  fclose(fp);
  chmod(path, 0600);
}

static void remove_conf(void) {
  char path[700];
  snprintf(path, sizeof path, "%s/nostr/relays.conf", scratch);
  unlink(path);
}

int main(void) {
  set_scratch();

  /* No file: NOT_FOUND. Callers must fall back, never treat this as fatal. */
  char *out = NULL;
  int rc = nostr_nip55l_get_relays(&out);
  assert(rc == NOSTR_SIGNER_ERROR_NOT_FOUND);
  assert(out == NULL);

  /* Empty JSON array: still NOT_FOUND. */
  write_conf("[]");
  out = NULL;
  rc = nostr_nip55l_get_relays(&out);
  assert(rc == NOSTR_SIGNER_ERROR_NOT_FOUND);
  assert(out == NULL);

  /* Normal case: two relays, order preserved, whitespace tolerated. */
  write_conf("[ \"wss://relay.example\", \"wss://nos.lol\" ]");
  out = NULL;
  rc = nostr_nip55l_get_relays(&out);
  assert(rc == 0 && out);
  assert(strcmp(out, "[\"wss://relay.example\",\"wss://nos.lol\"]") == 0);
  free(out);

  /* Normalisation: uppercase scheme+host lowered, bare trailing "/"
   * dropped, exact duplicate removed. Order of first-appearance is kept. */
  write_conf("[\"WSS://Nos.Lol/\", \"wss://nos.lol\", \"ws://LocalHost:4848/\"]");
  out = NULL;
  rc = nostr_nip55l_get_relays(&out);
  assert(rc == 0 && out);
  assert(strcmp(out, "[\"wss://nos.lol\",\"ws://localhost:4848\"]") == 0);
  free(out);

  /* Malformed: not an array. */
  write_conf("{\"relays\":[]}");
  out = NULL;
  rc = nostr_nip55l_get_relays(&out);
  assert(rc == NOSTR_SIGNER_ERROR_INVALID_JSON);
  assert(out == NULL);

  /* Malformed: non-relay URL. */
  write_conf("[\"http://example.com\"]");
  out = NULL;
  rc = nostr_nip55l_get_relays(&out);
  assert(rc == NOSTR_SIGNER_ERROR_INVALID_JSON);
  assert(out == NULL);

  /* Malformed: JSON escape in a URL (relay URLs never need one; the
   * signer refuses them rather than decoding). */
  write_conf("[\"wss://example.com/\\u002f\"]");
  out = NULL;
  rc = nostr_nip55l_get_relays(&out);
  assert(rc == NOSTR_SIGNER_ERROR_INVALID_JSON);
  assert(out == NULL);

  remove_conf();

  /* Direct helper: an empty list returns NOT_FOUND. */
  out = NULL;
  rc = nostr_nip55l_relays_from_list(NULL, 0, &out);
  assert(rc == NOSTR_SIGNER_ERROR_NOT_FOUND);
  assert(out == NULL);

  /* Direct helper: a bad entry is refused as INVALID_ARG. */
  const char *bad[] = { "wss://ok.example", "not-a-url" };
  out = NULL;
  rc = nostr_nip55l_relays_from_list(bad, 2, &out);
  assert(rc == NOSTR_SIGNER_ERROR_INVALID_ARG);
  assert(out == NULL);

  /* Direct helper: normal case matches the file path. */
  const char *good[] = { "wss://a.example", "wss://a.example", "ws://b:4848" };
  out = NULL;
  rc = nostr_nip55l_relays_from_list(good, 3, &out);
  assert(rc == 0 && out);
  assert(strcmp(out, "[\"wss://a.example\",\"ws://b:4848\"]") == 0);
  free(out);

  /* Clean up */
  char dir[600];
  snprintf(dir, sizeof dir, "%s/nostr", scratch);
  rmdir(dir);
  rmdir(scratch);

  printf("test_nip55l_getrelays: PASS\n");
  return 0;
}
