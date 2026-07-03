/* SPDX-License-Identifier: MIT */
#include "signet/fido_ctaphid.h"

#include <assert.h>
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
  assert(frames_len >= SIGNET_CTAPHID_REPORT_SIZE);
  const uint8_t *f = frames;
  *cmd_out = f[4];
  size_t total = ((size_t)f[5] << 8) | f[6];
  assert(total <= payload_cap);
  size_t n0 = total < 57 ? total : 57;
  memcpy(payload, f + 7, n0);
  size_t copied = n0;
  uint8_t seq = 0;
  for (size_t off = SIGNET_CTAPHID_REPORT_SIZE; copied < total; off += SIGNET_CTAPHID_REPORT_SIZE) {
    assert(off + SIGNET_CTAPHID_REPORT_SIZE <= frames_len);
    f = frames + off;
    assert(f[4] == seq++);
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
  assert(signet_ctaphid_test_process_frames(NULL, "agent", NULL, frame, sizeof(frame), &out, &out_len) == 0);
  assert(out_len == SIGNET_CTAPHID_REPORT_SIZE);
  uint8_t cmd = 0;
  uint8_t payload[64];
  size_t n = collect_payload(out, out_len, &cmd, payload, sizeof(payload));
  assert(cmd == CTAPHID_INIT);
  assert(n == 17);
  assert(memcmp(payload, nonce, sizeof(nonce)) == 0);
  assert(payload[12] == 2);      /* CTAP-HID protocol version */
  assert(payload[16] & 0x04);    /* CBOR capability */
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
  assert(signet_ctaphid_test_process_frames(NULL, "agent", NULL, frames, sizeof(frames), &out, &out_len) == 0);
  assert(out_len == SIGNET_CTAPHID_REPORT_SIZE * 2);
  uint8_t cmd = 0;
  uint8_t got[128];
  size_t n = collect_payload(out, out_len, &cmd, got, sizeof(got));
  assert(cmd == CTAPHID_PING);
  assert(n == sizeof(payload));
  assert(memcmp(got, payload, sizeof(payload)) == 0);
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
  assert(signet_ctaphid_test_process_frames(NULL, "agent", NULL, frame, sizeof(frame), &out, &out_len) == 0);
  uint8_t cmd = 0;
  uint8_t payload[512];
  size_t n = collect_payload(out, out_len, &cmd, payload, sizeof(payload));
  assert(cmd == CTAPHID_CBOR);
  assert(n > 1);
  assert(payload[0] == 0x00); /* CTAP2_OK */
  free(out);
}

int main(void) {
  test_init();
  test_fragmented_ping();
  test_ctap2_get_info();
  puts("test_fido_ctaphid: OK");
  return 0;
}
