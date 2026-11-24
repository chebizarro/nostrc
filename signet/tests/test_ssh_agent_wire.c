/* SPDX-License-Identifier: MIT
 *
 * test_ssh_agent_wire.c - SSH_AGENTC_SIGN_REQUEST body parsing (nostrc-vl9xt).
 *
 * The agent socket is reachable by any local process the uid resolver maps,
 * so every length in the request body is attacker-chosen.
 */

#include "signet/ssh_agent.h"

#include "test_check.h"
#include <stdio.h>
#include <string.h>

static size_t put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
  return 4;
}

/* string blob || string data || uint32 flags */
static size_t build(uint8_t *buf, const uint8_t *blob, uint32_t blob_len,
                    const uint8_t *data, uint32_t data_len, int with_flags) {
  size_t n = 0;
  n += put_u32(buf + n, blob_len); memcpy(buf + n, blob, blob_len); n += blob_len;
  n += put_u32(buf + n, data_len); memcpy(buf + n, data, data_len); n += data_len;
  if (with_flags) n += put_u32(buf + n, 2);
  return n;
}

static void test_valid_request(void) {
  const uint8_t blob[] = {1, 2, 3};
  const uint8_t data[] = {9, 8, 7, 6, 5};
  uint8_t buf[64];
  size_t n = build(buf, blob, sizeof(blob), data, sizeof(data), 1);
  SignetSshSignRequestView v;
  CHECK(signet_ssh_agent_parse_sign_request(buf, n, &v) == 0);
  CHECK(v.key_blob == buf + 4 && v.key_blob_len == sizeof(blob));
  CHECK(v.data_len == sizeof(data) && memcmp(v.data, data, sizeof(data)) == 0);
  CHECK(v.flags == 2);
}

/* kb_len + 8 wrapped to 1 in 32-bit arithmetic and the old bound passed,
 * moving the read pointer ~4 GiB past the message. */
static void test_key_blob_length_wrap_rejected(void) {
  uint8_t buf[16] = {0};
  put_u32(buf, 0xFFFFFFF9u);
  SignetSshSignRequestView v;
  CHECK(signet_ssh_agent_parse_sign_request(buf, sizeof(buf), &v) == -1);
  put_u32(buf, 0xFFFFFFFFu);
  CHECK(signet_ssh_agent_parse_sign_request(buf, sizeof(buf), &v) == -1);
}

static void test_data_length_wrap_rejected(void) {
  uint8_t buf[16] = {0};
  put_u32(buf, 0);              /* empty key blob */
  put_u32(buf + 4, 0xFFFFFFFCu); /* (dp - data) + msg_len wrapped to 4 */
  SignetSshSignRequestView v;
  CHECK(signet_ssh_agent_parse_sign_request(buf, sizeof(buf), &v) == -1);
}

static void test_truncated_and_trailing_rejected(void) {
  const uint8_t blob[] = {1, 2, 3};
  const uint8_t data[] = {4, 5};
  uint8_t buf[64];
  SignetSshSignRequestView v;

  size_t n = build(buf, blob, sizeof(blob), data, sizeof(data), 0);
  CHECK(signet_ssh_agent_parse_sign_request(buf, n, &v) == -1); /* no flags */

  n = build(buf, blob, sizeof(blob), data, sizeof(data), 1);
  for (size_t cut = 0; cut < n; cut++)
    CHECK(signet_ssh_agent_parse_sign_request(buf, cut, &v) == -1);

  buf[n] = 0xAA;
  CHECK(signet_ssh_agent_parse_sign_request(buf, n + 1, &v) == -1);
  CHECK(signet_ssh_agent_parse_sign_request(NULL, 0, &v) == -1);
}

int main(void) {
  test_valid_request();
  test_key_blob_length_wrap_rejected();
  test_data_length_wrap_rejected();
  test_truncated_and_trailing_rejected();
  printf("test_ssh_agent_wire: all passed\n");
  return 0;
}
