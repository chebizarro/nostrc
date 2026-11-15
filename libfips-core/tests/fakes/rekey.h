#ifndef FIPS_TEST_REKEY_H
#define FIPS_TEST_REKEY_H
#include "host.h"
#include <assert.h>
#include <fips/proto/rekey/mod.h>
#define RK_TEST_WIRE 64u
#define RK_TEST_FRAME 512u
typedef struct rk_test_packet {
    bool handshake;
    fips_rekey_msg_kind kind;
    size_t len;
    uint8_t bytes[RK_TEST_FRAME];
} rk_test_packet;
typedef struct rk_test_local {
    fips_peer_key key;
    uint8_t epoch[8];
    uint64_t next;
    unsigned messages;
    bool indexed, fail;
} rk_test_local;
typedef struct rk_test_node {
    fips_rekey machine;
    rk_test_local local;
    fips_rekey_profile profile;
    fips_rekey_env env;
    fips_rekey_tag_source tags;
    uint32_t next_index;
    bool indices[1024];
    size_t allocated, external_leases;
    uint16_t jitter;
    bool custom_jitter;
    uint8_t scratch[RK_TEST_FRAME];
    fips_rekey_outputs out;
    rk_test_packet wire[RK_TEST_WIRE];
    size_t wires;
} rk_test_node;
uint64_t rk_test_completions(void);
extern const fips_rekey_profile rk_test_ik, rk_test_xk, rk_test_xk_frame, rk_test_xx;
void rk_test_local_init(rk_test_local *l, uint8_t seed, unsigned messages);
fips_node_addr rk_test_addr(fips_peer_key key);
void rk_test_pair(rk_test_node *a, rk_test_node *b, const fips_rekey_profile *p,
                  fips_rekey_config config);
void rk_test_node_init(rk_test_node *n, uint8_t seed, const fips_rekey_profile *p);
void rk_test_establish(rk_test_node *n, const rk_test_local *peer, fips_rekey_params params,
                       fips_rekey_setup_facts *facts);
void rk_test_open(rk_test_node *n, const rk_test_local *peer, fips_rekey_params params,
                  uint64_t now);
void rk_test_inbound(rk_test_node *n, const rk_test_local *peer, fips_rekey_params params,
                     uint64_t now, const rk_test_packet *setup);
void rk_test_execute(rk_test_node *n, uint64_t now);
void rk_test_step(rk_test_node *n, uint64_t now, fips_rekey_input in);
void rk_test_poll(rk_test_node *n, uint64_t now);
void rk_test_start(rk_test_node *n, uint64_t now);
void rk_test_recv(rk_test_node *n, uint64_t now, const rk_test_packet *p);
void rk_test_recv_on(rk_test_node *n, uint64_t now, const rk_test_packet *p, bool on_link,
                     bool reachable, uint8_t carrier);
rk_test_packet rk_test_pop(rk_test_node *n, bool handshake, fips_rekey_msg_kind kind);
bool rk_test_has_packet(const rk_test_node *n, bool handshake, fips_rekey_msg_kind kind);
rk_test_packet rk_test_data(rk_test_node *n, uint64_t now, const uint8_t *plain, size_t len);
rk_test_packet rk_test_slot_frame(rk_test_node *n, fips_rekey_slot slot, bool k,
                                  const uint8_t *plain, size_t len);
rk_test_packet rk_test_fresh(rk_test_local *from, const rk_test_local *to, uint32_t index);
rk_test_packet rk_test_answer(rk_test_local *local, const rk_test_packet *msg1, uint32_t index);
rk_test_packet rk_test_forge3(const rk_test_packet *msg3, fips_peer_key other,
                              const uint8_t epoch[8]);
void rk_test_auth(rk_test_packet *p);
void rk_test_conserved(const rk_test_node *n);
void rk_test_close(rk_test_node *n);
size_t rk_test_count(const rk_test_node *n, fips_rekey_output_kind kind);
bool rk_test_rejected(const rk_test_node *n, fips_rekey_reason reason);
bool rk_test_abandoned(const rk_test_node *n, fips_rekey_abandon why);
bool rk_test_token(const rk_test_packet *p, fips_rekey_msg1_read *tok);
void rk_test_complete_leg(rk_test_local *local, const rk_test_packet *setup, uint32_t index,
                          fips_rekey_leg *leg);
#endif
