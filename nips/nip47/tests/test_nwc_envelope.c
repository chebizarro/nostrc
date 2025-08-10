#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr/nip47/nwc_envelope.h"
#include "nostr/nip47/nwc.h"

static void free_req(NostrNwcRequestBody *b, char *wallet_pub) {
  if (b) {
    free(b->method);
    free(b->params_json);
  }
  free(wallet_pub);
}

static void free_resp(NostrNwcResponseBody *b, char *client_pub, char *req_id) {
  if (b) {
    free(b->result_type);
    free(b->result_json);
    free(b->error_code);
    free(b->error_message);
  }
  free(client_pub);
  free(req_id);
}

int main(void) {
  /* Request build/parse roundtrip */
  const char *wallet_pub = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  NostrNwcRequestBody req = { .method = (char*)"get_balance", .params_json = (char*)"{\"unit\":\"sat\"}" };
  char *req_json = NULL;
  int rc = nostr_nwc_request_build(wallet_pub, NOSTR_NWC_ENC_NIP44_V2, &req, &req_json);
  assert(rc == 0 && req_json);

  char *out_wallet_pub = NULL; NostrNwcEncryption out_enc = 0; NostrNwcRequestBody parsed = {0};
  rc = nostr_nwc_request_parse(req_json, &out_wallet_pub, &out_enc, &parsed);
  assert(rc == 0);
  assert(strcmp(parsed.method, "get_balance") == 0);
  assert(strcmp(parsed.params_json, "{\"unit\":\"sat\"}") == 0);
  assert(out_wallet_pub && strcmp(out_wallet_pub, wallet_pub) == 0);
  assert(out_enc == NOSTR_NWC_ENC_NIP44_V2);

  free(req_json);
  free_req(&parsed, out_wallet_pub);

  /* Response build/parse roundtrip (success) */
  const char *client_pub = "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd";
  const char *req_id = "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef";
  NostrNwcResponseBody resp = { .result_type = (char*)"get_balance", .result_json = (char*)"{\"balance\":123}" };
  char *resp_json = NULL;
  rc = nostr_nwc_response_build(client_pub, req_id, NOSTR_NWC_ENC_NIP04, &resp, &resp_json);
  assert(rc == 0 && resp_json);

  char *out_client_pub = NULL; char *out_req_id = NULL; NostrNwcEncryption out_enc2 = 0; NostrNwcResponseBody parsed_resp = {0};
  rc = nostr_nwc_response_parse(resp_json, &out_client_pub, &out_req_id, &out_enc2, &parsed_resp);
  assert(rc == 0);
  assert(parsed_resp.error_code == NULL);
  assert(strcmp(parsed_resp.result_type, "get_balance") == 0);
  assert(strcmp(parsed_resp.result_json, "{\"balance\":123}") == 0);
  assert(out_client_pub && strcmp(out_client_pub, client_pub) == 0);
  assert(out_req_id && strcmp(out_req_id, req_id) == 0);
  assert(out_enc2 == NOSTR_NWC_ENC_NIP04);

  free(resp_json);
  free_resp(&parsed_resp, out_client_pub, out_req_id);

  /* Response error parse */
  const char *err_json =
    "{"
    "\"kind\":23195,"
    "\"content\":\"{\\\"error\\\":{\\\"code\\\":\\\"RATE_LIMIT\\\",\\\"message\\\":\\\"slow down\\\"}}\","
    "\"tags\":[]"
    "}";
  NostrNwcResponseBody parsed_err = {0};
  rc = nostr_nwc_response_parse(err_json, NULL, NULL, NULL, &parsed_err);
  assert(rc == 0);
  assert(parsed_err.error_code && strcmp(parsed_err.error_code, "RATE_LIMIT") == 0);
  assert(parsed_err.error_message && strcmp(parsed_err.error_message, "slow down") == 0);
  free_resp(&parsed_err, NULL, NULL);

  /* Encryption negotiation tests */
  {
    const char *client1[] = {"nip44-v2", "nip04"};
    const char *wallet1[] = {"nip04", "nip44-v2"};
    NostrNwcEncryption sel = 0;
    rc = nostr_nwc_select_encryption(client1, 2, wallet1, 2, &sel);
    assert(rc == 0 && sel == NOSTR_NWC_ENC_NIP44_V2);
  }
  {
    const char *client2[] = {"nip04"};
    const char *wallet2[] = {"nip04"};
    NostrNwcEncryption sel = 0;
    rc = nostr_nwc_select_encryption(client2, 1, wallet2, 1, &sel);
    assert(rc == 0 && sel == NOSTR_NWC_ENC_NIP04);
  }
  {
    const char *client3[] = {"nip44-v2"};
    const char *wallet3[] = {"nip04"};
    NostrNwcEncryption sel = 0;
    rc = nostr_nwc_select_encryption(client3, 1, wallet3, 1, &sel);
    assert(rc != 0);
  }

  /* Kind mismatch negatives */
  {
    /* Request parse with response kind should fail */
    const char *bad_req = "{\"kind\":23195,\"content\":\"{\\\"method\\\":\\\"get_info\\\"}\",\"tags\":[[\"p\",\"00\"],[\"encryption\",\"nip44-v2\"]]}";
    NostrNwcRequestBody out = {0};
    rc = nostr_nwc_request_parse(bad_req, NULL, NULL, &out);
    assert(rc != 0);
    free(out.method); free(out.params_json);
  }
  {
    /* Response parse with request kind should fail */
    const char *bad_resp = "{\"kind\":23194,\"content\":\"{\\\"result_type\\\":\\\"get_info\\\",\\\"result\\\":{}}\",\"tags\":[]}";
    NostrNwcResponseBody out = {0};
    rc = nostr_nwc_response_parse(bad_resp, NULL, NULL, NULL, &out);
    assert(rc != 0);
    free(out.result_type); free(out.result_json); free(out.error_code); free(out.error_message);
  }

  printf("test_nwc_envelope: OK\n");
  return 0;
}
