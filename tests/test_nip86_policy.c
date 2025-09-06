#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <openssl/sha.h>

#include "nips/nip86/include/nip86.h"
#include "libnostr/include/nostr-event.h"
#include "libnostr/include/nostr-tag.h"
#include "libnostr/include/nostr-json.h"
#include "third_party/nostrdb/deps/flatcc/include/flatcc/portable/pbase64.h"

static char *sha256_hex(const char *data) {
  unsigned char dg[SHA256_DIGEST_LENGTH];
  SHA256((const unsigned char*)data, strlen(data), dg);
  static const char *hx = "0123456789abcdef";
  char *hex = (char*)malloc(SHA256_DIGEST_LENGTH*2+1);
  for (size_t i=0;i<SHA256_DIGEST_LENGTH;i++){ hex[i*2]=hx[(dg[i]>>4)&0xF]; hex[i*2+1]=hx[dg[i]&0xF]; }
  hex[SHA256_DIGEST_LENGTH*2] = '\0';
  return hex;
}

static char *base64url_encode(const char *input) {
  size_t inlen = strlen(input);
  size_t outlen = base64_encoded_size(inlen, base64_mode_url);
  uint8_t *out = (uint8_t*)malloc(outlen+1);
  size_t src_len = inlen; size_t dst_len = outlen;
  int rc = base64_encode(out, (const uint8_t*)input, &dst_len, &src_len, base64_mode_url);
  if (rc != 0) { free(out); return NULL; }
  out[dst_len] = '\0';
  return (char*)out;
}

static char *build_auth_header(const char *url, const char *method, const char *json_body, const char *sk_hex) {
  char *payload_hex = json_body ? sha256_hex(json_body) : NULL;
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, 27235);
  nostr_event_set_created_at(ev, (int64_t)time(NULL));
  nostr_event_set_content(ev, "");
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("u", url, NULL));
  nostr_tags_append(tags, nostr_tag_new("method", method, NULL));
  if (payload_hex) nostr_tags_append(tags, nostr_tag_new("payload", payload_hex, NULL));
  nostr_event_set_tags(ev, tags);
  int sret = nostr_event_sign(ev, sk_hex);
  assert(sret == 0);
  char *ev_json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  if (payload_hex) free(payload_hex);
  assert(ev_json);
  char *b64 = base64url_encode(ev_json);
  free(ev_json);
  assert(b64);
  size_t sz = strlen("Nostr ") + strlen(b64) + 1;
  char *hdr = (char*)malloc(sz);
  snprintf(hdr, sz, "Nostr %s", b64);
  free(b64);
  return hdr;
}

static void write_file(const char *path, const char *content){
  FILE *f=fopen(path, "w"); assert(f); fputs(content, f); fclose(f);
}

int main(void) {
  const char *policy = "./test_policy.json";
  setenv("NOSTR_RELAY_POLICY", policy, 1);

  /* Seed initial policy */
  write_file(policy, "{\n  \"banned_pubkeys\":[],\n  \"allowed_pubkeys\":[\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"],\n  \"banned_events\":[],\n  \"allowed_kinds\":[1,3],\n  \"blocked_ips\":[\"203.0.113.9\"],\n  \"relay_name\":\"test\",\n  \"relay_description\":\"desc\",\n  \"relay_icon\":\"http://icon\"\n}\n");

  /* Load and verify */
  nostr_nip86_load_policy();
  assert(nostr_nip86_has_allowlist() == 1);
  assert(nostr_nip86_is_pubkey_allowed("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 1);
  assert(nostr_nip86_has_allowed_kinds() == 1);
  assert(nostr_nip86_is_kind_allowed(1) == 1);
  assert(nostr_nip86_is_kind_allowed(2) == 0);
  assert(nostr_nip86_is_ip_blocked("203.0.113.9") == 1);

  /* Call banpubkey via NIP-86 with NIP-98 Authorization */
  const char *url = "http://localhost/nip86";
  const char *sk = "e3e70682c2094cac629f6fbed82c07cd1b7e1f3a99f6d5f2d5b2137b7e4f8f4c"; /* test key */
  const char *rpc = "{\"method\":\"banpubkey\",\"params\":[\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"]}";
  char *auth = build_auth_header(url, "POST", rpc, sk);
  int http_status = 0;
  char *resp = nostr_nip86_process_request(NULL, auth, rpc, "POST", url, &http_status);
  assert(http_status == 200); assert(resp != NULL);
  free(resp); free(auth);
  /* Verify state and persistence */
  assert(nostr_nip86_is_pubkey_banned("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == 1);

  /* allowkind */
  const char *rpc2 = "{\"method\":\"allowkind\",\"params\":[7]}";
  auth = build_auth_header(url, "POST", rpc2, sk);
  resp = nostr_nip86_process_request(NULL, auth, rpc2, "POST", url, &http_status);
  assert(http_status == 200); free(resp); free(auth);
  assert(nostr_nip86_is_kind_allowed(7) == 1);

  /* blockip/unblockip */
  const char *rpc3 = "{\"method\":\"blockip\",\"params\":[\"198.51.100.2\"]}";
  auth = build_auth_header(url, "POST", rpc3, sk);
  resp = nostr_nip86_process_request(NULL, auth, rpc3, "POST", url, &http_status);
  assert(http_status == 200); free(resp); free(auth);
  assert(nostr_nip86_is_ip_blocked("198.51.100.2") == 1);

  const char *rpc4 = "{\"method\":\"unblockip\",\"params\":[\"198.51.100.2\"]}";
  auth = build_auth_header(url, "POST", rpc4, sk);
  resp = nostr_nip86_process_request(NULL, auth, rpc4, "POST", url, &http_status);
  assert(http_status == 200); free(resp); free(auth);
  assert(nostr_nip86_is_ip_blocked("198.51.100.2") == 0);

  /* Invalid auth -> 401 */
  http_status = 0;
  resp = nostr_nip86_process_request(NULL, NULL, rpc, "POST", url, &http_status);
  assert(http_status == 401);
  free(resp);

  /* Cleanup */
  remove(policy);
  printf("test_nip86_policy: OK\n");
  return 0;
}
