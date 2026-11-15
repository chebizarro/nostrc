#ifndef FIPS_TEST_REKEY_PROFILE_H
#define FIPS_TEST_REKEY_PROFILE_H
#include "host.h"
#include <fips/node/session.h>
#define RP_PACKET_MAX 1024u
#define RP_QUEUE_MAX 64u
typedef struct rp_packet { size_t len; uint8_t bytes[RP_PACKET_MAX]; } rp_packet;
typedef struct rp_node {
    fips_secp *secp;
    fips_identity *identity;
    fips_rekey_node_keys local;
    fips_peer_key peer;
    fips_node_addr me, remote;
    fips_rekey machine;
    fips_rekey_env env;
    fips_rekey_tag_source tags;
    fips_rekey_outputs out;
    uint8_t scratch[RP_PACKET_MAX], wire[RP_PACKET_MAX];
    bool indices[256];
    uint32_t next_index;
    size_t allocated, deliveries, installed, retired, order_count;
    fips_rekey_output_kind order[FIPS_REKEY_OUTPUTS_MAX];
    rp_packet queue[RP_QUEUE_MAX];
    size_t queued;
    bool fail_send, with_coords;
} rp_node;
bool rp_init(rp_node *, uint64_t key_seed, fips_entropy entropy, const uint8_t epoch[8]);
bool rp_handshake(rp_node *a, rp_node *b, const fips_rekey_profile *, fips_rekey_completed done[2]);
bool rp_pair(rp_node *a, rp_node *b, const fips_rekey_profile *);
bool rp_execute_machine(rp_node *, fips_rekey *, uint64_t, const uint8_t *, size_t);
bool rp_execute(rp_node *, uint64_t now, const uint8_t *input, size_t len);
bool rp_receive_machine(rp_node *, fips_rekey *, uint64_t, rp_packet *);
bool rp_receive(rp_node *, uint64_t now, rp_packet *);
bool rp_poll(rp_node *, uint64_t now);
bool rp_data(rp_node *, uint64_t now, rp_packet *);
void rp_close(rp_node *);
void rp_destroy(rp_node *);
#endif
