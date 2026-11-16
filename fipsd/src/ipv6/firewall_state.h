#ifndef FIPSD_IPV6_FIREWALL_STATE_H
#define FIPSD_IPV6_FIREWALL_STATE_H
/* Read-side classifier for the `inet fips` baseline filter (upstream
 * firewall_state.rs). For the "Listening on fips0" panel it tells the operator
 * whether a (proto, port) listener is reachable on the TUN or silently dropped
 * by the shipped nftables baseline, by walking `nft -j list table inet fips`'s
 * inbound chain. It reads the kernel's firewall; it filters nothing itself:
 * connection state (`ct state established,related`) and its timeouts are
 * conntrack's, and those rules are not classified. Linux-only; other hosts get
 * the no-firewall classifier.
 *
 * - NO_FIREWALL: the table does not exist (fips-firewall.service not active).
 * - ACCEPT: a canonical-shape rule accepts the pair with no other restriction.
 * - DROP: no rule matches; the chain falls through to its trailing drop.
 * - UNKNOWN: a rule references the pair with matchers not decomposed here
 *   (saddr/daddr filters, undecodable sets, jumps): inspect with nft.
 *
 * The chain is scoped to one interface by its opening `iifname != "<name>"
 * return` rule. A loaded table protects the node only when that name is the
 * live TUN's; a scope that cannot be read is treated as not protecting it. */
#include "ipv6/listening.h"
#include <jansson.h>
#include <stdbool.h>
typedef enum fipsd_ipv6_filter_state {
    FIPSD_IPV6_FILTER_NO_FIREWALL, FIPSD_IPV6_FILTER_ACCEPT, FIPSD_IPV6_FILTER_DROP, FIPSD_IPV6_FILTER_UNKNOWN
} fipsd_ipv6_filter_state;
const char *fipsd_ipv6_filter_state_str(fipsd_ipv6_filter_state state);
typedef enum fipsd_ipv6_scope_kind {
    FIPSD_IPV6_SCOPE_NONE,       /* no table loaded */
    FIPSD_IPV6_SCOPE_UNSCOPED,   /* no rule returns or matches on the input interface */
    FIPSD_IPV6_SCOPE_IFACE,      /* exactly `iifname != "<name>" return` with a literal name */
    FIPSD_IPV6_SCOPE_UNREADABLE  /* any other shape: a set, wildcard, `==`, iif/iifgroup, a vmap, another verdict */
} fipsd_ipv6_scope_kind;
#define FIPSD_IPV6_IFACE_NAME 64u
typedef struct fipsd_ipv6_scope { fipsd_ipv6_scope_kind kind; char name[FIPSD_IPV6_IFACE_NAME]; } fipsd_ipv6_scope;
typedef struct fipsd_ipv6_filter_classifier fipsd_ipv6_filter_classifier;
fipsd_ipv6_filter_classifier *fipsd_ipv6_filter_no_firewall(void);
/* From captured `nft -j list table inet fips` output, as query would from the kernel. */
fipsd_ipv6_filter_classifier *fipsd_ipv6_filter_from_nft_json(const json_t *json);
/* Linux: the running kernel's chain (no_firewall when the table is absent or
 * nft is missing); elsewhere no_firewall. Blocks on the nft process. */
fipsd_ipv6_filter_classifier *fipsd_ipv6_filter_query(void);
void fipsd_ipv6_filter_destroy(fipsd_ipv6_filter_classifier *classifier);
bool fipsd_ipv6_filter_is_active(const fipsd_ipv6_filter_classifier *classifier);
fipsd_ipv6_scope fipsd_ipv6_filter_scope(const fipsd_ipv6_filter_classifier *classifier);
/* True only when the table is loaded and polices tun: unscoped, or scoped to exactly tun. */
bool fipsd_ipv6_filter_polices(const fipsd_ipv6_filter_classifier *classifier, const char *tun);
/* The single interface the chain is scoped to when that is another than tun. */
bool fipsd_ipv6_filter_foreign_scope(const fipsd_ipv6_filter_classifier *classifier, const char *tun,
                                     char *out, size_t cap);
fipsd_ipv6_filter_state fipsd_ipv6_filter_classify(const fipsd_ipv6_filter_classifier *classifier,
                                                   fipsd_ipv6_proto proto, uint16_t port);
/* Warn once when the table polices another interface than tun. */
void fipsd_ipv6_filter_warn_scope(const fipsd_ipv6_filter_classifier *classifier, const char *tun);
/* The nft JSON of the loaded table, or NULL (Linux only). */
json_t *fipsd_ipv6_filter_run_nft(void);
#endif
