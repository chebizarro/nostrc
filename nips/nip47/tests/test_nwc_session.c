#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr/nip47/nwc_client.h"
#include "nostr/nip47/nwc_wallet.h"

int main(void) {
  const char *client_supported[] = {"nip44-v2", "nip04"};
  const char *wallet_supported[] = {"nip04", "nip44-v2"};
  const char *wallet_pub = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const char *client_pub = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

  NostrNwcClientSession cs = {0};
  NostrNwcWalletSession ws = {0};

  int rc = nostr_nwc_client_session_init(&cs, wallet_pub,
                                         client_supported, 2,
                                         wallet_supported, 2);
  assert(rc == 0);

  rc = nostr_nwc_wallet_session_init(&ws, client_pub,
                                     wallet_supported, 2,
                                     client_supported, 2);
  assert(rc == 0);

  /* Both should agree on nip44-v2 */
  assert(cs.enc == ws.enc);

  /* Build a request from client */
  NostrNwcRequestBody req = { .method = (char*)"get_balance", .params_json = (char*)"{\"unit\":\"msat\"}" };
  char *req_json = NULL;
  rc = nostr_nwc_client_build_request(&cs, &req, &req_json);
  assert(rc == 0 && req_json);

  /* Wallet parses request */
  char *parsed_wallet_pub = NULL; NostrNwcEncryption parsed_enc = 0; NostrNwcRequestBody parsed_req = {0};
  rc = nostr_nwc_request_parse(req_json, &parsed_wallet_pub, &parsed_enc, &parsed_req);
  assert(rc == 0);
  assert(parsed_wallet_pub && strcmp(parsed_wallet_pub, wallet_pub) == 0);
  assert(parsed_enc == cs.enc);
  assert(strcmp(parsed_req.method, "get_balance") == 0);

  /* Wallet builds a response */
  NostrNwcResponseBody resp = { .result_type = (char*)"get_balance", .result_json = (char*)"{\"balance\":42}" };
  const char *dummy_req_id = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
  char *resp_json = NULL;
  rc = nostr_nwc_wallet_build_response(&ws, dummy_req_id, &resp, &resp_json);
  assert(rc == 0 && resp_json);

  /* Client parses response */
  char *out_client_pub = NULL; char *out_req_id = NULL; NostrNwcEncryption out_enc = 0; NostrNwcResponseBody parsed_resp = {0};
  rc = nostr_nwc_response_parse(resp_json, &out_client_pub, &out_req_id, &out_enc, &parsed_resp);
  assert(rc == 0);
  assert(out_client_pub && strcmp(out_client_pub, client_pub) == 0);
  assert(out_req_id && strcmp(out_req_id, dummy_req_id) == 0);
  assert(out_enc == cs.enc);
  assert(strcmp(parsed_resp.result_type, "get_balance") == 0);

  /* Cleanup */
  free(req_json);
  free(parsed_wallet_pub);
  free(parsed_req.method); free(parsed_req.params_json);
  free(resp_json);
  free(out_client_pub); free(out_req_id);
  free(parsed_resp.result_type); free(parsed_resp.result_json);
  nostr_nwc_client_session_clear(&cs);
  nostr_nwc_wallet_session_clear(&ws);

  printf("test_nwc_session: OK\n");
  return 0;
}
