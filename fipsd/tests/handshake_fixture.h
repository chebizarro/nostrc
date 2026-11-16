#ifndef FIPSD_HANDSHAKE_FIXTURE_H
#define FIPSD_HANDSHAKE_FIXTURE_H
#include "node/handlers/internal.h"
#include "node/transport_loopback.h"
#include <assert.h>

/* A production node/dispatcher/workers plus an in-process wire peer. The peer
 * uses production IK and AEAD, but deliberately controls possession, restart,
 * replay and msg2 release. No state is read across dispatcher ownership. */
typedef struct hf_state {
    size_t peers, indices, bindings, legs, restart_pending;
    uint64_t sent, promoted, next_link, restarts, frames_in;
    fipsd_active_peer *peer;
    fipsd_peer_info info;
    fips_noise_session keys;
} hf_state;
typedef struct handshake_fixture {
    fipsd_runtime *rt;
    fipsd_loopback_network *net;
    fipsd_node *node;
    handlers *h;
    GoSwap *configs;
    fipsd_transport *server, *client, *alternate;
    GoChannel *wire, *received, *completed, *commands_done;
    fips_secp *secp;
    fips_identity *id;
    fips_peer_identity remote, server_identity;
    fips_rekey_node_keys local;
    fips_rekey_hs hs;
    fips_rekey_completed live;
    fips_rekey_wire msg1;
    uint32_t index;
    hf_state before, after;
    struct { void (*fn)(struct handshake_fixture *, void *); void *arg; } request;
    bool (*on_event)(void *, const fipsd_transport_event *);
    bool (*on_crypto)(void *, const fipsd_crypto_job *);
    bool (*on_command)(void *, void *);
} handshake_fixture;
void hf_init(handshake_fixture *);
void hf_destroy(handshake_fixture *);
void hf_call(handshake_fixture *, void (*fn)(handshake_fixture *, void *), void *);
void hf_state_read(handshake_fixture *, hf_state *); /* dispatcher only */
void hf_snapshot(handshake_fixture *, hf_state *);
void hf_unchanged(const hf_state *, const hf_state *);
void hf_start(handshake_fixture *, uint8_t epoch); /* construct fresh msg1 */
void hf_send(handshake_fixture *, fipsd_transport *, const uint8_t *, size_t);
fipsd_transport_event *hf_wire(handshake_fixture *, uint8_t type);
void hf_finish(handshake_fixture *); /* read/authenticate msg2 */
void hf_heartbeat(handshake_fixture *); /* send real AEAD, await worker result */
void hf_policy(handshake_fixture *, bool deny, const char *path);
void hf_wait(GoChannel *);
#endif
