#ifndef FIPSD_TEST_GATEWAY_H
#define FIPSD_TEST_GATEWAY_H
/* Shared helpers of the fips-gateway suites (nostrc-iq1n9.3). */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#define __APPLE_USE_RFC_3542 1
#endif
#include "gateway/conntrack.h"
#include "gateway/pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#define CHECKM(x, ...) do { if (!(x)) { fprintf(stderr, "%s:%d: check failed: %s: ", __FILE__, __LINE__, #x); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); abort(); } } while (0)
#define RUN(fn) do { fprintf(stderr, "  %s\n", #fn); fn(); } while (0)
#define SECS(n) ((fipsd_gw_instant)(n) * FIPSD_GW_SEC)
#define MILLIS(n) ((fipsd_gw_instant)(n) * UINT64_C(1000000))

/* A base instant far enough from zero that tests may go back in time from it. */
static inline fipsd_gw_instant gw_t0(void) { return fipsd_gw_now() + SECS(100000); }
static inline struct in6_addr gw_v6(const char *text) { struct in6_addr a; CHECK(fipsd_gw_ip6_parse(text, &a)); return a; }
static inline fips_node_addr gw_node(uint8_t byte) { fips_node_addr n; memset(&n, 0, sizeof n); n.bytes[0] = byte; return n; }
static inline struct in6_addr gw_mesh(uint8_t byte) { struct in6_addr a; memset(&a, 0, sizeof a); a.s6_addr[0] = 0xfd; a.s6_addr[15] = byte; return a; }
static inline fips_node_addr gw_node_n(uint32_t i)
{
    fips_node_addr n; memset(&n, 0, sizeof n); n.bytes[0] = 0x01;
    n.bytes[12] = (uint8_t)(i >> 24); n.bytes[13] = (uint8_t)(i >> 16); n.bytes[14] = (uint8_t)(i >> 8); n.bytes[15] = (uint8_t)i;
    return n;
}
static inline struct in6_addr gw_mesh_n(uint32_t i)
{
    struct in6_addr a; memset(&a, 0, sizeof a); a.s6_addr[0] = 0xfd; a.s6_addr[1] = 0x9a;
    a.s6_addr[12] = (uint8_t)(i >> 24); a.s6_addr[13] = (uint8_t)(i >> 16); a.s6_addr[14] = (uint8_t)(i >> 8); a.s6_addr[15] = (uint8_t)i;
    return a;
}
/* The address at offset of fd01::/120. */
static inline struct in6_addr gw_at(uint16_t offset)
{ struct in6_addr a = gw_v6("fd01::"); a.s6_addr[14] = (uint8_t)(offset >> 8); a.s6_addr[15] = (uint8_t)offset; return a; }

/* Session counts and bindings a test sets directly, as the tick's snapshot. */
typedef struct gw_sessions {
    struct { struct in6_addr addr; uint32_t count; } counts[1024]; size_t ncounts;
    struct { struct in6_addr dst, src; bool replied; } bindings[1024]; size_t nbindings;
} gw_sessions;
static inline void gw_sessions_set(gw_sessions *s, struct in6_addr addr, uint32_t count)
{
    for (size_t i = 0; i < s->ncounts; ++i) if (fipsd_gw_ip6_eq(&s->counts[i].addr, &addr)) { s->counts[i].count = count; return; }
    CHECK(s->ncounts < 1024);
    s->counts[s->ncounts].addr = addr; s->counts[s->ncounts].count = count; s->ncounts++;
}
static inline void gw_sessions_bind(gw_sessions *s, struct in6_addr dst, struct in6_addr src, bool replied)
{
    CHECK(s->nbindings < 1024);
    s->bindings[s->nbindings].dst = dst; s->bindings[s->nbindings].src = src; s->bindings[s->nbindings].replied = replied; s->nbindings++;
}
/* A read snapshot of what s holds; clear it after use. */
static inline void gw_sessions_snapshot(const gw_sessions *s, fipsd_gw_ct_snapshot *out)
{
    fipsd_gw_ct_snapshot_init_read(out);
    for (size_t i = 0; i < s->ncounts; ++i) CHECK(fipsd_gw_ct_snapshot_set_sessions(out, &s->counts[i].addr, s->counts[i].count) == 0);
    for (size_t i = 0; i < s->nbindings; ++i) CHECK(fipsd_gw_ct_snapshot_record_binding(out, &s->bindings[i].dst, &s->bindings[i].src, s->bindings[i].replied) == 0);
}

int gw_suite_pool(void);
int gw_suite_conntrack(void);
int gw_suite_state(void);
int gw_suite_nat(void);
int gw_suite_net(void);
int gw_suite_dns(void);
int gw_suite_control(void);
int gw_suite_shutdown(void);
#endif
