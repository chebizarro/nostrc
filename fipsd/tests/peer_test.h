#ifndef FIPSD_PEER_TEST_H
#define FIPSD_PEER_TEST_H
#include "peer/active.h"
#include "fakes/host.h"
#include <stdio.h>
#include <string.h>
#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); return 1; } } while (0)
static inline fips_peer_identity peer_id(uint8_t n)
{
    fips_peer_identity id = {0};
    id.node_addr.bytes[14] = n; id.node_addr.bytes[15] = n;
    id.pubkey.bytes[0] = n;
    id.pubkey_full[0] = 2; id.pubkey_full[1] = n;
    return id;
}
static inline fips_transport_addr peer_addr(uint8_t n)
{
    fips_transport_addr a = {0}; a.bytes[0] = n; a.len = 1; return a;
}
static inline fipsd_active_peer *new_peer(uint8_t n, uint32_t transport, uint8_t addr)
{
    fips_peer_identity id = peer_id(n);
    fips_transport_addr a = peer_addr(addr);
    return fipsd_active_peer_create(&id, (fips_transport_id){transport}, &a,
        (fips_link_id){n}, FIPS_LINK_OUTBOUND, 1234, fips_test_instant(0), fipsd_peer_timing_default());
}
int peer_machine_tests(void);
int peer_budget_tests(void);
int peer_data_table_tests(void);
int peer_link_tests(void);
int peer_dispatcher_tests(void);
#endif
