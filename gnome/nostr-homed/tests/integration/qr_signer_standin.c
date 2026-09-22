/* qr_signer_standin — headless "phone signer" for the NIP-46 QR pairing.
 *
 * Consumes a live `nostrconnect://` URI (typically decoded from the QR the
 * greeter is displaying), stands up a NIP-46 bunker over the URI's relays
 * using the supplied signing key, and answers `get_public_key` +
 * `sign_event` requests from the client until it is asked to exit or the
 * hard deadline elapses.
 *
 * The library primitive `nostr_nip46_bunker_connect_to_client` does the
 * work: parse URI, bring up the pool, grant client ACL, publish the
 * signer-initiated `connect` request. The default bunker RPC handler
 * signs with the session's secret key — no callback needed.
 *
 * Usage:
 *   qr_signer_standin --uri "<nostrconnect://…>" \
 *                     --nsec-file <64-hex file> \
 *                     [--wait-sec 120]
 *
 * The --nsec-file must contain a single 64-char lowercase-hex secp256k1
 * private key (trailing whitespace tolerated). Passing a DIFFERENT key
 * than the account's produces the "wrong-key" negative — the provider
 * MUST deny at get_public_key without ever asking sign_event.
 *
 * Design context: docs/designs/nip46-qr-login-greeter.md §7.1
 * (headless signer stand-in). Tracks beads nostrc-z1fb / nostrc-zcll.7.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_types.h"
#include "nostr/nip46/nip46_uri.h"
#include "json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int is_lc_hex64(const char *s) {
  if (!s || strlen(s) != 64) return 0;
  for (size_t i = 0; i < 64; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
  }
  return 1;
}

static char *slurp_trim(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long len = ftell(f);
  if (len < 0 || len > 64 * 1024) { fclose(f); return NULL; }
  rewind(f);
  char *buf = malloc((size_t)len + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t n = fread(buf, 1, (size_t)len, f);
  fclose(f);
  buf[n] = '\0';
  while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
               buf[n - 1] == ' ' || buf[n - 1] == '\t'))
    buf[--n] = '\0';
  return buf;
}

static void print_redacted_uri(const char *uri) {
  const char *needle = "secret=";
  const char *hit = strstr(uri ? uri : "", needle);
  if (!hit) { printf("uri=%s\n", uri ? uri : "(null)"); return; }
  size_t prefix = (size_t)(hit - uri);
  fwrite("uri=", 1, 4, stdout);
  fwrite(uri, 1, prefix + strlen(needle), stdout);
  fputs("<REDACTED>", stdout);
  const char *tail = uri + prefix + strlen(needle);
  while (*tail && *tail != '&') tail++;
  fputs(tail, stdout);
  fputc('\n', stdout);
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s --uri <nostrconnect://...> --nsec-file <path> "
          "[--wait-sec N]\n",
          argv0);
}

int main(int argc, char **argv) {
  const char *uri = NULL, *nsec_path = NULL;
  int wait_sec = 120;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--uri") && i + 1 < argc) uri = argv[++i];
    else if (!strncmp(a, "--uri=", 6)) uri = a + 6;
    else if (!strcmp(a, "--nsec-file") && i + 1 < argc) nsec_path = argv[++i];
    else if (!strncmp(a, "--nsec-file=", 12)) nsec_path = a + 12;
    else if (!strcmp(a, "--wait-sec") && i + 1 < argc)
      wait_sec = atoi(argv[++i]);
    else if (!strncmp(a, "--wait-sec=", 11)) wait_sec = atoi(a + 11);
    else { usage(argv[0]); return 2; }
  }
  if (!uri || !nsec_path) { usage(argv[0]); return 2; }
  if (strncmp(uri, "nostrconnect://", 15) != 0) {
    fprintf(stderr, "standin: --uri must be nostrconnect://…\n");
    return 2;
  }
  if (wait_sec <= 0 || wait_sec > 600) wait_sec = 120;

  char *sk_hex = slurp_trim(nsec_path);
  if (!sk_hex || !is_lc_hex64(sk_hex)) {
    fprintf(stderr, "standin: nsec-file must contain 64 lc-hex chars: %s\n",
            nsec_path);
    free(sk_hex);
    return 2;
  }

  /* Sanity: parse the URI so we can print its structure. */
  NostrNip46ConnectURI parsed = {0};
  if (nostr_nip46_uri_parse_connect(uri, &parsed) != 0) {
    fprintf(stderr, "standin: URI parse failed\n");
    memset(sk_hex, 0, 64); free(sk_hex);
    return 2;
  }
  print_redacted_uri(uri);
  printf("client_pubkey=%s\n", parsed.client_pubkey_hex);
  printf("relays=%zu\n", parsed.n_relays);
  for (size_t i = 0; i < parsed.n_relays; i++)
    printf("  relay[%zu]=%s\n", i, parsed.relays[i]);
  if (parsed.perms_csv) printf("perms=%s\n", parsed.perms_csv);
  if (parsed.name) printf("name=%s\n", parsed.name);
  fflush(stdout);
  nostr_nip46_uri_connect_free(&parsed);

  nostr_json_init();

  NostrNip46Session *bunker = nostr_nip46_bunker_new(NULL);
  if (!bunker) {
    fprintf(stderr, "standin: bunker_new failed\n");
    memset(sk_hex, 0, 64); free(sk_hex);
    nostr_json_cleanup();
    return 1;
  }

  /* Set the signing key. This becomes the bunker's identity: get_public_key
   * returns its pubkey, and sign_event signs with it. */
  if (nostr_nip46_client_set_secret(bunker, sk_hex) != 0) {
    fprintf(stderr, "standin: set_secret failed\n");
    memset(sk_hex, 0, 64); free(sk_hex);
    nostr_nip46_session_free(bunker);
    nostr_json_cleanup();
    return 1;
  }
  memset(sk_hex, 0, 64); free(sk_hex);

  /* Dial the URI's relays, subscribe, and publish the signer-initiated
   * connect request carrying the URI's secret. bunker_connect_to_client
   * starts the pool and publishes in one shot, but publish is fire-and-
   * forget over connected relays only — if the WebSocket handshake hasn't
   * finished yet, the connect event is dropped and the broker never
   * receives it. So: attempt up to 4 times with a short delay so the pool
   * has time to establish. */
  int connect_ok = 0;
  for (int attempt = 0; attempt < 4; attempt++) {
    if (attempt > 0) sleep(2);
    if (nostr_nip46_bunker_connect_to_client(bunker, uri) == 0) {
      connect_ok = 1;
      printf("standin: connect_to_client attempt %d ok\n", attempt + 1);
      fflush(stdout);
      /* Republish once after another delay to cover the case where the
       * publish succeeded to zero connected relays. */
      if (attempt == 0) {
        sleep(3);
        if (nostr_nip46_bunker_connect_to_client(bunker, uri) == 0)
          printf("standin: connect_to_client republish ok\n");
        fflush(stdout);
      }
      break;
    }
    fprintf(stderr, "standin: connect_to_client attempt %d failed\n",
            attempt + 1);
  }
  if (!connect_ok) {
    fprintf(stderr, "standin: bunker_connect_to_client failed\n");
    nostr_nip46_session_free(bunker);
    nostr_json_cleanup();
    return 1;
  }
  printf("standin: connect published; listening for %d s\n", wait_sec);
  fflush(stdout);

  /* Keep the bunker session alive so it processes subsequent
   * get_public_key + sign_event over the same subscription. */
  time_t start = time(NULL);
  while (time(NULL) - start < wait_sec) sleep(1);

  printf("standin: wait window elapsed, tearing down\n");
  nostr_nip46_session_free(bunker);
  nostr_json_cleanup();
  return 0;
}
