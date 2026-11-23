/* SPDX-License-Identifier: MIT */
#include "signet/fido_ctaphid.h"
#include "signet/audit_logger.h"
#include "../src/store_passkeys_schema.h"
#include <sqlite3.h>
#include <sodium.h>

#include "test_check.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTAPHID_PING 0x81u
#define CTAPHID_INIT 0x86u
#define CTAPHID_CBOR 0x90u
#define CID 0x11223344u

static void put_cid(uint8_t *f, uint32_t cid) {
  f[0] = (uint8_t)(cid >> 24);
  f[1] = (uint8_t)(cid >> 16);
  f[2] = (uint8_t)(cid >> 8);
  f[3] = (uint8_t)cid;
}

static size_t collect_payload(const uint8_t *frames, size_t frames_len,
                              uint8_t *cmd_out, uint8_t *payload, size_t payload_cap) {
  CHECK(frames_len >= SIGNET_CTAPHID_REPORT_SIZE);
  const uint8_t *f = frames;
  *cmd_out = f[4];
  size_t total = ((size_t)f[5] << 8) | f[6];
  CHECK(total <= payload_cap);
  size_t n0 = total < 57 ? total : 57;
  memcpy(payload, f + 7, n0);
  size_t copied = n0;
  uint8_t seq = 0;
  for (size_t off = SIGNET_CTAPHID_REPORT_SIZE; copied < total; off += SIGNET_CTAPHID_REPORT_SIZE) {
    CHECK(off + SIGNET_CTAPHID_REPORT_SIZE <= frames_len);
    f = frames + off;
    CHECK(f[4] == seq++);
    size_t chunk = total - copied;
    if (chunk > 59) chunk = 59;
    memcpy(payload + copied, f + 5, chunk);
    copied += chunk;
  }
  return total;
}

static void test_init(void) {
  uint8_t frame[SIGNET_CTAPHID_REPORT_SIZE] = {0};
  uint8_t nonce[8] = {0,1,2,3,4,5,6,7};
  put_cid(frame, SIGNET_CTAPHID_BROADCAST_CID);
  frame[4] = CTAPHID_INIT;
  frame[6] = 8;
  memcpy(frame + 7, nonce, sizeof(nonce));

  uint8_t *out = NULL;
  size_t out_len = 0;
  CHECK(signet_ctaphid_test_process_frames(NULL, "agent", NULL, frame, sizeof(frame), &out, &out_len) == 0);
  CHECK(out_len == SIGNET_CTAPHID_REPORT_SIZE);
  uint8_t cmd = 0;
  uint8_t payload[64];
  size_t n = collect_payload(out, out_len, &cmd, payload, sizeof(payload));
  CHECK(cmd == CTAPHID_INIT);
  CHECK(n == 17);
  CHECK(memcmp(payload, nonce, sizeof(nonce)) == 0);
  CHECK(payload[12] == 2);      /* CTAP-HID protocol version */
  CHECK(payload[16] & 0x04);    /* CBOR capability */
  free(out);
}

static void test_fragmented_ping(void) {
  uint8_t frames[SIGNET_CTAPHID_REPORT_SIZE * 2];
  uint8_t payload[90];
  memset(frames, 0, sizeof(frames));
  for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)i;

  put_cid(frames, CID);
  frames[4] = CTAPHID_PING;
  frames[5] = 0;
  frames[6] = sizeof(payload);
  memcpy(frames + 7, payload, 57);

  put_cid(frames + SIGNET_CTAPHID_REPORT_SIZE, CID);
  frames[SIGNET_CTAPHID_REPORT_SIZE + 4] = 0;
  memcpy(frames + SIGNET_CTAPHID_REPORT_SIZE + 5, payload + 57, sizeof(payload) - 57);

  uint8_t *out = NULL;
  size_t out_len = 0;
  CHECK(signet_ctaphid_test_process_frames(NULL, "agent", NULL, frames, sizeof(frames), &out, &out_len) == 0);
  CHECK(out_len == SIGNET_CTAPHID_REPORT_SIZE * 2);
  uint8_t cmd = 0;
  uint8_t got[128];
  size_t n = collect_payload(out, out_len, &cmd, got, sizeof(got));
  CHECK(cmd == CTAPHID_PING);
  CHECK(n == sizeof(payload));
  CHECK(memcmp(got, payload, sizeof(payload)) == 0);
  free(out);
}

static void test_ctap2_get_info(void) {
  uint8_t frame[SIGNET_CTAPHID_REPORT_SIZE] = {0};
  put_cid(frame, CID);
  frame[4] = CTAPHID_CBOR;
  frame[6] = 1;
  frame[7] = 0x04; /* authenticatorGetInfo */

  uint8_t *out = NULL;
  size_t out_len = 0;
  CHECK(signet_ctaphid_test_process_frames(NULL, "agent", NULL, frame, sizeof(frame), &out, &out_len) == 0);
  uint8_t cmd = 0;
  uint8_t payload[512];
  size_t n = collect_payload(out, out_len, &cmd, payload, sizeof(payload));
  CHECK(cmd == CTAPHID_CBOR);
  CHECK(n > 1);
  CHECK(payload[0] == 0x00); /* CTAP2_OK */
  free(out);
}

struct SignetStore { sqlite3 *db; };
sqlite3 *signet_store_get_db(SignetStore *store) { return store ? store->db : NULL; }
int signet_audit_log_common(SignetAuditLogger *logger, SignetAuditEventType type,
                            const SignetAuditCommonFields *fields, const char *details) {
  (void)logger; (void)type; (void)fields; (void)details;
  return 0;
}

static size_t credential_request(uint8_t *out, bool make);
typedef struct { int calls; bool allow; bool make; } Consent;
static bool consent(void *data, const char *group, const char *agent, bool make,
                    const char *rp, const uint8_t *hash,
                    const uint8_t *request, size_t request_len) {
  Consent *state = data;
  CHECK(strcmp(group, "ctap-owners") == 0);
  CHECK(strcmp(agent, "agent") == 0);
  CHECK(strcmp(rp, "example.com") == 0);
  for (size_t i = 0; i < 32; i++) CHECK(hash[i] == 0x42);
  uint8_t expected[128];
  CHECK(credential_request(expected, make) == request_len + 1);
  CHECK(memcmp(expected + 1, request, request_len) == 0);
  state->calls++;
  state->make = make;
  return state->allow;
}

static size_t credential_request(uint8_t *out, bool make) {
  size_t n = 0;
  out[n++] = make ? 0x01 : 0x02;
  out[n++] = make ? 0xa5 : 0xa2;
  if (!make) {
    out[n++] = 0x01; out[n++] = 0x6b;
    memcpy(out + n, "example.com", 11); n += 11;
  }
  out[n++] = make ? 0x01 : 0x02;
  out[n++] = 0x58; out[n++] = 0x20;
  memset(out + n, 0x42, 32); n += 32;
  if (make) {
    static const uint8_t rest[] = {
      0x02, 0xa1, 0x62, 'i','d', 0x6b, 'e','x','a','m','p','l','e','.','c','o','m',
      0x03, 0xa1, 0x62, 'i','d', 0x41, 0x01,
      0x04, 0x81, 0xa2, 0x63, 'a','l','g', 0x26,
      0x64, 't','y','p','e', 0x6a, 'p','u','b','l','i','c','-','k','e','y',
      0x07, 0xa1, 0x62, 'r','k', 0xf5
    };
    memcpy(out + n, rest, sizeof(rest)); n += sizeof(rest);
  }
  return n;
}

static uint8_t credential_operation(SignetFidoCtapHid *dev, bool make) {
  uint8_t request[128], frames[128] = {0};
  size_t len = credential_request(request, make);
  CHECK(len <= 116);
  put_cid(frames, CID); frames[4] = CTAPHID_CBOR; frames[6] = (uint8_t)len;
  size_t first = len < 57 ? len : 57;
  memcpy(frames + 7, request, first);
  size_t frame_len = 64;
  if (len > first) {
    put_cid(frames + 64, CID);
    memcpy(frames + 69, request + first, len - first);
    frame_len = 128;
  }
  uint8_t *out = NULL; size_t out_len = 0;
  if (frame_len > 64) {
    CHECK(signet_fido_ctaphid_process_frames(dev, frames, 64, &out, &out_len) == 1);
    CHECK(out == NULL && out_len == 0);
  }
  CHECK(signet_fido_ctaphid_process_frames(dev, frames + frame_len - 64, 64,
                                           &out, &out_len) == 0);
  CHECK(out_len >= 128 && out[4] == 0xbb); /* keepalive, then CBOR response */
  uint8_t cmd, payload[2048];
  CHECK(collect_payload(out + 64, out_len - 64, &cmd, payload, sizeof(payload)) >= 1);
  CHECK(cmd == CTAPHID_CBOR);
  uint8_t status = payload[0];
  free(out);
  return status;
}

static int credential_count(sqlite3 *db) {
  sqlite3_stmt *stmt = NULL;
  CHECK(sqlite3_prepare_v2(db, "SELECT count(*) FROM passkey_credentials", -1, &stmt, NULL) == SQLITE_OK);
  CHECK(sqlite3_step(stmt) == SQLITE_ROW);
  int count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

static void test_authorization(void) {
  CHECK(sodium_init() >= 0);
  sqlite3 *db = NULL;
  CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
  CHECK(sqlite3_exec(db, SIGNET_PASSKEY_CREDENTIALS_SCHEMA_SQL, NULL, NULL, NULL) == SQLITE_OK);
  SignetStore store = {db};
  uint8_t psk[32]; memset(psk, 0x11, sizeof(psk));
  SignetFidoServiceConfig service_cfg = {.enabled = true, .store = &store,
    .fleet_psk = psk, .fleet_psk_len = sizeof(psk),
    .backend = "software-openssl", .attestation = "none"};
  SignetFidoService *service = signet_fido_service_new(&service_cfg);
  CHECK(service != NULL);
  SignetFidoCtapHidConfig cfg = {.enabled = true, .fido = service, .agent_id = "agent"};
  SignetFidoCtapHid *dev = signet_fido_ctaphid_new(&cfg);
  CHECK(dev != NULL);
  CHECK(signet_fido_ctaphid_start(dev) == -1); /* before accessing /dev/uhid */
  CHECK(credential_operation(dev, true) == 0x27);
  CHECK(credential_operation(dev, false) == 0x27);
  CHECK(credential_count(db) == 0);
  signet_fido_ctaphid_free(dev);

  cfg.owner_group = "ctap-owners";
  dev = signet_fido_ctaphid_new(&cfg); /* group alone is not authorization */
  CHECK(signet_fido_ctaphid_start(dev) == -1);
  CHECK(credential_operation(dev, true) == 0x27);
  signet_fido_ctaphid_free(dev);
  cfg.owner_group = NULL;

  Consent state = {0};
  cfg.authorize = consent; cfg.authorize_data = &state;
  dev = signet_fido_ctaphid_new(&cfg); /* hook alone is not configured ownership */
  CHECK(signet_fido_ctaphid_start(dev) == -1);
  CHECK(credential_operation(dev, true) == 0x27);
  CHECK(state.calls == 0);
  signet_fido_ctaphid_free(dev);

  cfg.owner_group = "ctap-owners";
  dev = signet_fido_ctaphid_new(&cfg);
  CHECK(credential_operation(dev, true) == 0x27);
  CHECK(state.calls == 1 && state.make);
  CHECK(credential_operation(dev, false) == 0x27);
  CHECK(state.calls == 2 && !state.make);
  CHECK(credential_count(db) == 0);
  state.allow = true;
  CHECK(credential_operation(dev, true) == 0x00);
  CHECK(state.calls == 3 && state.make);
  CHECK(credential_count(db) == 1);
  CHECK(credential_operation(dev, false) == 0x00);
  CHECK(state.calls == 4 && !state.make);
  state.allow = false; /* no cached grant across ceremonies */
  CHECK(credential_operation(dev, false) == 0x27);
  CHECK(state.calls == 5);
  signet_fido_ctaphid_free(dev);
  cfg.enabled = false;
  state.allow = true;
  dev = signet_fido_ctaphid_new(&cfg);
  CHECK(signet_fido_ctaphid_start(dev) == 0);
  CHECK(credential_operation(dev, true) == 0x27);
  CHECK(state.calls == 5);
  signet_fido_ctaphid_free(dev);
  signet_fido_service_free(service);
  sqlite3_close(db);
}

int main(void) {
  test_init();
  test_fragmented_ping();
  test_ctap2_get_info();
  test_authorization();
  puts("test_fido_ctaphid: OK");
  return 0;
}
