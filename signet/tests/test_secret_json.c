/* SPDX-License-Identifier: MIT
 * Compile the actual management entrypoints, including ContextVM normalization. */
#include "../src/secret_json.h"
#include "../src/mgmt_protocol.c"
#include "test_check.h"
#include <nostr/nip46/nip46_msg.h>

static size_t wipe_calls, wipe_bytes;
static void observed_wipe(const void *ptr, size_t len) {
  const unsigned char *bytes = ptr;
  for (size_t i = 0; i < len; i++) CHECK(bytes[i] == 0);
  if (len) { wipe_calls++; wipe_bytes += len; }
}

int main(void) {
  signet_secret_json_set_wipe_hook(observed_wipe);
  SignetMgmtRequest req = {0};
  const char *content = "{\"agent_id\":\"agent\",\"agent_nsec\":\"secret-nsec\",\"connect_secret\":\"secret-connect\"}";
  CHECK(signet_mgmt_request_parse(SIGNET_MGMT_OP_ADOPT_EXISTING, content, &req, NULL) == 0);
  CHECK(strcmp(req.agent_nsec, "secret-nsec") == 0);
  CHECK(strcmp(req.connect_secret, "secret-connect") == 0);
  CHECK(wipe_calls >= 3 && wipe_bytes >= strlen("secret-nsecsecret-connect"));
  signet_mgmt_request_clear(&req);
  wipe_calls = wipe_bytes = 0;
  /* Semantically invalid requests still tear down their successfully parsed tree. */
  CHECK(signet_mgmt_request_parse(SIGNET_MGMT_OP_ADOPT_EXISTING, "{\"agent_nsec\":\"secret-invalid\"}", &req, NULL) != 0);
  CHECK(wipe_bytes >= strlen("secret-invalid")); signet_mgmt_request_clear(&req);
  SignetMgmtOp op; JsonNode *rpc_id = NULL;
  char *params = signet_mgmt_contextvm_params_json(
    "{\"jsonrpc\":\"2.0\",\"id\":\"request-id\",\"method\":\"agent/adopt-existing\",\"params\":{\"agent_id\":\"agent\",\"agent_nsec\":\"secret-nsec\",\"connect_secret\":\"secret-connect\"}}", &op, &rpc_id);
  CHECK(params && op == SIGNET_MGMT_OP_ADOPT_EXISTING && rpc_id);
  CHECK(strcmp(json_node_get_string(rpc_id), "request-id") == 0);
  CHECK(strstr(params, "secret-nsec") && strstr(params, "secret-connect"));
  signet_secret_gfree(params);
  const char *result = "{\"bunker_uri\":\"bunker://pub?secret=pairing\",\"nested\":[{\"payload_b64\":\"c2VjcmV0\"}]}";
  char *ack = signet_mgmt_build_ack("id", true, "ok", "done", result); CHECK(ack);
  CHECK(strstr(ack, "pairing") && strstr(ack, "c2VjcmV0"));
  char *reply = signet_mgmt_ack_to_jsonrpc(ack, rpc_id); CHECK(reply);
  CHECK(strstr(reply, "pairing") && strstr(reply, "c2VjcmV0"));
  /* Wiping a reply builder must never corrupt the borrowed caller's RPC id. */
  CHECK(strcmp(json_node_get_string(rpc_id), "request-id") == 0);
  CHECK(strstr(result, "pairing"));
  signet_secret_gfree(ack); signet_secret_gfree(reply);
  signet_secret_json_node_unref(rpc_id);
  /* Both JSON libraries coexist in the shipped server; their iterator names
   * must never be interposed across incompatible object representations. */
  NostrNip46Request n46 = {0};
  CHECK(nostr_nip46_request_parse("{\"id\":\"id\",\"method\":\"connect\",\"params\":[{\"nested\":[\"secret\"]}]}", &n46) == 0);
  CHECK(strstr(n46.params[0], "secret")); nostr_nip46_request_free(&n46);
  signet_secret_json_set_wipe_hook(NULL);
  puts("Signet management parse/normalize/reply wipe: PASS");
  return 0;
}
