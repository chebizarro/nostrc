/*
 * test_porthome_broker_wait.c — broker-layer integration test for the
 * PROVISION_HOME + WAIT_HOME dispatch. Runs the broker in a forked
 * child over a socketpair. Asserts:
 *
 *   1. PROVISION_HOME with no wrap-seed deposited yields NOT_SUPPORTED
 *      (the broker refuses to start a job without keying material —
 *      PAM's open_session translates this to "ordinary local home").
 *
 *   2. With a wrap-seed deposited but no manifest, PROVISION_HOME
 *      returns IN_PROGRESS and WAIT_HOME resolves to LIMITED_MODE
 *      within the bounded wait (design §5.3 no-push interlock).
 *
 *   3. WAIT_HOME with no in-flight job returns NOT_SUPPORTED so the
 *      caller can distinguish a nothing-to-do path from a real
 *      terminal outcome.
 *
 * Does NOT exercise a real relay fetch — the fetch source is the
 * test hook (nh_auth_porthome_set_test_hook). A follow-up bead wires
 * this into fake_relay + fake_porthome_blossom for the full round-trip.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "../nh_test.h"
#include "auth_broker.h"
#include "auth_client.h"
#include "auth_porthome.h"
#include "nostr_auth_protocol.h"
#include "nostr_identity.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *PUBKEY =
    "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
static const char *ACCT_ID = "00000000-0000-4000-8000-00000000abcd";

static nh_identity_ownership_result available(void *c, const char *n,
                                              uint32_t u, uint32_t g) {
  (void)c; (void)n; (void)u; (void)g;
  return NH_IDENTITY_OWNERSHIP_FREE;
}

static void set_config(nh_identity_config *config, const char *dir) {
  nh_identity_config_defaults(config);
  snprintf(config->authority_path, sizeof config->authority_path, "%s/authority.db", dir);
  snprintf(config->projection_path, sizeof config->projection_path, "%s/nss.db", dir);
  snprintf(config->home_root, sizeof config->home_root, "%s/home", dir);
}

static nh_identity_store *open_store(const char *dir, uint32_t flags) {
  static nh_identity_config config;
  set_config(&config, dir);
  nh_identity_store_options options = {0};
  options.config = &config;
  options.ownership_probe = available;
  options.flags = flags;
  nh_identity_store *store = NULL;
  NH_CHECK(nh_identity_store_open(&options, &store) == NH_IDENTITY_OK);
  return store;
}

/* Seed just enough of an account that the broker's account lookup works. */
static void seed(const char *dir) {
  nh_identity_store *store = open_store(dir, NH_IDENTITY_STORE_CREATE);
  nh_identity_enroll_request enroll = {0};
  enroll.username = "n_bob";
  enroll.pubkey_hex = PUBKEY;
  enroll.home_mode = NH_IDENTITY_HOME_CREATE;
  nh_identity_operation_state state;
  NH_CHECK(nh_identity_operation_begin_enroll(store, ACCT_ID, &enroll, &state)
           == NH_IDENTITY_OK);
  nh_identity_home_evidence staged = {11, 101}, installed = {11, 202};
  NH_CHECK(nh_identity_operation_advance_home(store, ACCT_ID,
             NH_IDENTITY_PHASE_RESERVED, NH_IDENTITY_PHASE_STAGED, &staged,
             &state) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_advance_home(store, ACCT_ID,
             NH_IDENTITY_PHASE_STAGED, NH_IDENTITY_PHASE_INSTALLED,
             &installed, &state) == NH_IDENTITY_OK);
  /* Not activating (would require a valid provider record); the
   * broker still resolves by name via lookup_by_name. Publish the
   * projection so account lookups are visible outside the open
   * transaction. */
  (void)nh_identity_store_publish_projection(store, NULL);
  nh_identity_store_close(store);
}

static void resolve_account_id(const char *dir, char out[NH_IDENTITY_UUID_CAP]) {
  nh_identity_store *store = open_store(dir, 0);
  nh_identity_account acct;
  NH_CHECK(nh_identity_store_lookup_by_name(store, "n_bob", &acct)
           == NH_IDENTITY_OK);
  strncpy(out, acct.account_id, NH_IDENTITY_UUID_CAP - 1);
  out[NH_IDENTITY_UUID_CAP - 1] = '\0';
  nh_identity_store_close(store);
}

/* Run broker child until `n_conns` connections have been drained. */
static pid_t start_broker(const char *dir, int server_fd,
                          int client_fd_to_close_in_child,
                          const uint8_t *seed_if_any, const char *acct_id) {
  pid_t pid = fork();
  NH_CHECK(pid >= 0);
  if (pid != 0) return pid;
  /* Child: drop the inherited client-side fd so recvmsg returns
   * EOF when the parent closes its own copy. */
  close(client_fd_to_close_in_child);
  nh_identity_store *store = open_store(dir, 0);
  nh_auth_broker *broker = nh_auth_broker_new(store);
  NH_CHECK(broker);
  nh_auth_porthome_registry_init();
  if (seed_if_any && acct_id)
    (void)nh_auth_broker_porthome_deposit_wrap_seed(acct_id, seed_if_any);
  /* Drain a single connection (all three ops multiplexed on it). */
  (void)nh_auth_broker_handle_connection(broker, server_fd);
  nh_auth_porthome_registry_shutdown();
  nh_auth_broker_free(broker);
  nh_identity_store_close(store);
  close(server_fd);
  _exit(0);
  return 0;
}

int main(void) {
  if (geteuid() != 0) {
    /* Broker ACL requires uid 0 on the AUTH endpoint. Skip when not
     * root — the test still passes the compile gate. */
    fprintf(stderr, "SKIP: broker AUTH endpoint requires root\n");
    return 77;
  }

  char dir[] = "/tmp/porthome-broker-XXXXXX";
  NH_CHECK(mkdtemp(dir));
  seed(dir);
  char aid[NH_IDENTITY_UUID_CAP];
  resolve_account_id(dir, aid);

  /* ─── Test 1: no wrap-seed → NOT_SUPPORTED ─── */
  {
    int sv[2];
    NH_CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    pid_t pid = start_broker(dir, sv[1], sv[0], NULL, NULL);
    close(sv[1]);
    /* Craft the PROVISION_HOME payload with account_id + tx_id. */
    nh_auth_message req;
    memset(&req, 0, sizeof req);
    req.operation = NH_AUTH_OP_PROVISION_HOME;
    memset(req.request_id, '0', NH_AUTH_REQUEST_ID_HEX_LEN);
    req.request_id[NH_AUTH_REQUEST_ID_HEX_LEN] = '\0';
    char payload[512];
    snprintf(payload, sizeof payload,
             "{\"account_id\":\"%s\",\"tx_id\":\"tx-1\"}", aid);
    req.payload_json = payload;
    NH_CHECK(nh_auth_send_message(sv[0], &req) == 0);
    unsigned char *pkt = NULL; size_t pkt_len = 0;
    NH_CHECK(nh_auth_recv_packet(sv[0], &pkt, &pkt_len) == 0);
    nh_auth_message rsp; memset(&rsp, 0, sizeof rsp);
    NH_CHECK(nh_auth_message_parse(pkt, pkt_len, &rsp) == 0);
    free(pkt);
    NH_CHECK(strstr(rsp.payload_json, "not_supported") != NULL);
    printf("PASS: no seed -> NOT_SUPPORTED\n");
    nh_auth_message_clear(&rsp);
    close(sv[0]);
    int st; waitpid(pid, &st, 0);
  }

  /* ─── Test 2: seed deposited but no manifest → IN_PROGRESS then LIMITED_MODE ─── */
  {
    uint8_t seedbuf[32]; memset(seedbuf, 0x55, 32);
    int sv[2];
    NH_CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    pid_t pid = start_broker(dir, sv[1], sv[0], seedbuf, aid);
    close(sv[1]);

    /* Fire PROVISION_HOME. */
    nh_auth_message req;
    memset(&req, 0, sizeof req);
    req.operation = NH_AUTH_OP_PROVISION_HOME;
    memset(req.request_id, '0', NH_AUTH_REQUEST_ID_HEX_LEN);
    req.request_id[NH_AUTH_REQUEST_ID_HEX_LEN] = '\0';
    char payload[512];
    snprintf(payload, sizeof payload,
             "{\"account_id\":\"%s\",\"tx_id\":\"tx-2\"}", aid);
    req.payload_json = payload;
    NH_CHECK(nh_auth_send_message(sv[0], &req) == 0);
    unsigned char *pkt = NULL; size_t pkt_len = 0;
    NH_CHECK(nh_auth_recv_packet(sv[0], &pkt, &pkt_len) == 0);
    nh_auth_message rsp; memset(&rsp, 0, sizeof rsp);
    NH_CHECK(nh_auth_message_parse(pkt, pkt_len, &rsp) == 0);
    free(pkt);
    /* Broker returned IN_PROGRESS (the job started; will fall through
     * to LIMITED because no manifest). */
    if (!strstr(rsp.payload_json, "in_progress")) {
      fprintf(stderr, "unexpected: %s\n", rsp.payload_json);
      exit(1);
    }
    printf("PASS: seed deposited -> IN_PROGRESS\n");
    nh_auth_message_clear(&rsp);

    /* Fire WAIT_HOME with a generous timeout. */
    memset(&req, 0, sizeof req);
    req.operation = NH_AUTH_OP_WAIT_HOME;
    memset(req.request_id, '0', NH_AUTH_REQUEST_ID_HEX_LEN);
    req.request_id[NH_AUTH_REQUEST_ID_HEX_LEN] = '\0';
    snprintf(payload, sizeof payload,
             "{\"account_id\":\"%s\",\"timeout_ms\":3000}", aid);
    req.payload_json = payload;
    NH_CHECK(nh_auth_send_message(sv[0], &req) == 0);
    NH_CHECK(nh_auth_recv_packet(sv[0], &pkt, &pkt_len) == 0);
    memset(&rsp, 0, sizeof rsp);
    NH_CHECK(nh_auth_message_parse(pkt, pkt_len, &rsp) == 0);
    free(pkt);
    if (!strstr(rsp.payload_json, "limited_mode")) {
      fprintf(stderr, "unexpected wait: %s\n", rsp.payload_json);
      exit(1);
    }
    printf("PASS: WAIT_HOME -> LIMITED_MODE (no-manifest interlock)\n");
    nh_auth_message_clear(&rsp);
    close(sv[0]);
    int st; waitpid(pid, &st, 0);
  }

  /* ─── Test 3: WAIT_HOME with no job → NOT_SUPPORTED ─── */
  {
    int sv[2];
    NH_CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    pid_t pid = start_broker(dir, sv[1], sv[0], NULL, NULL);
    close(sv[1]);
    nh_auth_message req;
    memset(&req, 0, sizeof req);
    req.operation = NH_AUTH_OP_WAIT_HOME;
    memset(req.request_id, '0', NH_AUTH_REQUEST_ID_HEX_LEN);
    req.request_id[NH_AUTH_REQUEST_ID_HEX_LEN] = '\0';
    char payload[512];
    snprintf(payload, sizeof payload,
             "{\"account_id\":\"%s\",\"timeout_ms\":500}", aid);
    req.payload_json = payload;
    NH_CHECK(nh_auth_send_message(sv[0], &req) == 0);
    unsigned char *pkt = NULL; size_t pkt_len = 0;
    NH_CHECK(nh_auth_recv_packet(sv[0], &pkt, &pkt_len) == 0);
    nh_auth_message rsp; memset(&rsp, 0, sizeof rsp);
    NH_CHECK(nh_auth_message_parse(pkt, pkt_len, &rsp) == 0);
    free(pkt);
    if (!strstr(rsp.payload_json, "not_supported")) {
      fprintf(stderr, "unexpected wait-no-job: %s\n", rsp.payload_json);
      exit(1);
    }
    printf("PASS: WAIT_HOME (no job) -> NOT_SUPPORTED\n");
    nh_auth_message_clear(&rsp);
    close(sv[0]);
    int st; waitpid(pid, &st, 0);
  }

  /* Cleanup. */
  char path[1024];
  const char *files[] = {"authority.db", "authority.db-wal", "authority.db-shm",
                         "authority.lock", "nss.db"};
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, files[i]);
    unlink(path);
  }
  rmdir(dir);
  printf("OK test_porthome_broker_wait\n");
  return 0;
}
