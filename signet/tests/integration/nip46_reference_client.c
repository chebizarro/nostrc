/* SPDX-License-Identifier: MIT
 * Live acceptance client. Reuses libnostr's event-driven NIP-46 transport;
 * timeouts/relay failures never count as a policy or revocation refusal.
 * Protected INI [client]: bunker_uri, secret_hex, agent_pubkey, allowed_kind,
 * (policy must allow connect/sign_event/nip44_encrypt and deny nip44_decrypt).
 * Each invocation is a fresh client process.
 */
#include <nostr/nip46/nip46_client.h>
#include <nostr/nip46/nip46_uri.h>
#include "nostr-event.h"
#include "nostr-keys.h"
#include <glib.h>
#include <sodium.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void wipe(char *s) { if (s) { sodium_memzero(s, strlen(s)); free(s); } }
int main(int argc, char **argv) {
  int rc = 1, fd = -1;
  GKeyFile *cfg = NULL;
  char data[16385] = {0};
  char *uri = NULL, *secret = NULL, *expected = NULL;
  char *ack = NULL, *event = NULL, *signed_json = NULL, *cipher = NULL, *plain = NULL;
  NostrNip46Session *session = NULL;
  NostrNip46BunkerURI parsed = {0};
  NostrEvent *signed_event = NULL;
  const char *relays = getenv("SIGNET_LIVE_RELAYS");
  if (argc != 3 || !relays || !*relays || sodium_init() < 0) {
    fprintf(stderr, "usage: SIGNET_LIVE_RELAYS=url,... nip46_reference_client protected.ini pair|sign|deny|revoked\n");
    goto out;
  }
  gboolean pair = strcmp(argv[2], "pair") == 0;
  gboolean deny = strcmp(argv[2], "deny") == 0;
  gboolean revoked = strcmp(argv[2], "revoked") == 0;
  if (!pair && !deny && !revoked && strcmp(argv[2], "sign") != 0) goto out;
  fd = open(argv[1], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 077) || st.st_size <= 0 || st.st_size > 16384) goto out;
  size_t used = 0;
  while (used < (size_t)st.st_size) {
    ssize_t n = read(fd, data + used, (size_t)st.st_size - used);
    if (n <= 0) goto out;
    used += (size_t)n;
  }
  close(fd); fd = -1;
  cfg = g_key_file_new();
  if (!g_key_file_load_from_data(cfg, data, used, G_KEY_FILE_NONE, NULL)) goto out;
  uri = g_key_file_get_string(cfg, "client", "bunker_uri", NULL);
  secret = g_key_file_get_string(cfg, "client", "secret_hex", NULL);
  expected = g_key_file_get_string(cfg, "client", "agent_pubkey", NULL);
  if (!uri || !secret || strlen(secret) != 64 || !expected || strlen(expected) != 64 ||
      nostr_nip46_uri_parse_bunker(uri, &parsed) != 0) goto out;
  gchar **wanted = g_strsplit(relays, ",", -1);
  gboolean same = g_strv_length(wanted) == parsed.n_relays;
  for (size_t i = 0; same && i < parsed.n_relays; i++) same = strcmp(wanted[i], parsed.relays[i]) == 0;
  g_strfreev(wanted);
  if (!same || parsed.n_relays == 0) goto out;
  session = nostr_nip46_client_new();
  if (!session || nostr_nip46_client_connect(session, uri, NULL) ||
      nostr_nip46_client_set_secret(session, secret)) goto out;
  nostr_nip46_client_set_timeout(session, 30000);
  if (nostr_nip46_client_start(session)) goto out;
  if (pair) {
    if (!parsed.secret || !*parsed.secret || nostr_nip46_client_connect_rpc(session, parsed.secret, NULL, &ack) ||
        !ack || (strcmp(ack, "ack") && strcmp(ack, parsed.secret))) goto out;
  }
  /* Reconnect proof intentionally NEVER calls connect_rpc again. */
  GError *error = NULL;
  gint kind = g_key_file_get_integer(cfg, "client", "allowed_kind", &error);
  if (error || kind < 0 || kind > 65535) { g_clear_error(&error); goto out; }
  char nonce[33]; uint8_t bytes[16]; randombytes_buf(bytes, sizeof(bytes));
  sodium_bin2hex(nonce, sizeof(nonce), bytes, sizeof(bytes));
  event = g_strdup_printf("{\"pubkey\":\"%s\",\"created_at\":%lld,\"kind\":%d,\"tags\":[],\"content\":\"signet-production-proof-%s\"}",
                          expected, (long long)time(NULL), kind, nonce);
  /* _ex distinguishes an authenticated denial from timeout/transport failure.
   * sign_event's legacy API does not expose that distinction. Prove encrypt is
   * allowed before revocation, then demand its authenticated denial afterwards. */
  NostrNip46RpcError failure = NOSTR_NIP46_RPC_OK;
  int encrypted = nostr_nip46_client_nip44_encrypt_rpc_ex(session, expected,
      "signet-production-proof", &cipher, &failure);
  if (revoked) {
    if (encrypted == 0 || failure != NOSTR_NIP46_RPC_ERR_DENIED) goto out;
    puts("revocation: authenticated refusal of previously allowed encryption");
  } else if (deny) {
    if (encrypted || !cipher) goto out;
    if (nostr_nip46_client_nip44_decrypt_rpc_ex(session, expected, cipher, &plain, &failure) == 0 ||
        failure != NOSTR_NIP46_RPC_ERR_DENIED) goto out;
    puts("policy: authenticated refusal");
  } else {
    if (encrypted || !cipher || nostr_nip46_client_sign_event(session, event, &signed_json) || !signed_json) goto out;
    signed_event = nostr_event_new();
    if (!signed_event || !nostr_event_deserialize_compact(signed_event, signed_json, NULL) ||
        !nostr_event_check_signature(signed_event) ||
        strcmp(nostr_event_get_pubkey(signed_event), expected) ||
        nostr_event_get_kind(signed_event) != kind ||
        !strstr(nostr_event_get_content(signed_event), nonce)) goto out;
    printf("signed-event-id=%s\n", nostr_event_get_id(signed_event));
  }
  rc = 0;
out:
  if (rc) fprintf(stderr, "NIP-46 live proof FAILED (no secrets logged); absent daemon/relay is not a denial\n");
  if (session) { nostr_nip46_client_stop(session); nostr_nip46_session_free(session); }
  if (signed_event) nostr_event_free(signed_event);
  wipe(signed_json); wipe(cipher); wipe(plain); wipe(ack); wipe(event);
  if (parsed.secret) sodium_memzero(parsed.secret, strlen(parsed.secret));
  nostr_nip46_uri_bunker_free(&parsed);
  wipe(uri); wipe(secret); wipe(expected);
  if (cfg) g_key_file_unref(cfg);
  if (fd >= 0) close(fd);
  sodium_memzero(data, sizeof(data));
  return rc;
}
