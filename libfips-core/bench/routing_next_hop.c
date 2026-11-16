/* Port of upstream benches/routing_next_hop.rs: 8/32/128/256 peers,
 * eight-entry paths, half advertise reachability plus four unrelated addrs.
 * Measures the real C selector, not a bench-local copy. Former snapshot path
 * clones only live entries (Rust Vec behavior), not C's unused coordinate
 * capacity. C's real view uses arrays, whereas Rust's external bench mirrors its
 * production HashMaps; this is not an identical-container comparison.
 * Host benchmark allocates/reads a clock; the core does neither. */
#include <fips/proto/routing/core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bench.h"
static fips_tree_coordinate coords[258];
static fips_bloom_filter filters[256];
static fips_routing_peer peers[256];
static fips_node_addr addr(uint8_t tag, uint16_t i)
{ fips_node_addr a = {{0}}; a.bytes[0]=tag; a.bytes[1]=(uint8_t)i; a.bytes[2]=(uint8_t)(i>>8); return a; }
static void coord(fips_tree_coordinate *c, fips_node_addr leaf, uint8_t tag)
{
    fips_node_addr path[8]; path[0]=leaf;
    for (uint16_t i=1; i<7; ++i) path[i]=addr(tag,i);
    path[7]=addr(9,0);
    if (fips_tree_coordinate_from_addrs(c,path,8,NULL) != FIPS_OK) abort();
}
typedef struct former_candidate {
    fips_node_addr addr;
    fips_coord_entry *entries;
    size_t len;
    double cost;
} former_candidate;
/* Retained former allocating assembly. Compact Vec-like coordinate storage is
 * only for this host baseline; production uses the public coordinate type. */
static bool former(const fips_routing_view *rv, const fips_node_addr *dest,
                   const fips_tree_coordinate *dc, const fips_tree_coordinate *mc, fips_node_addr *out)
{
    fips_node_addr *addrs=malloc(rv->peer_count*sizeof(*addrs));
    former_candidate *cs=malloc(rv->peer_count*sizeof(*cs));
    if (!addrs || !cs) abort();
    for (size_t i=0; i<rv->peer_count; ++i) addrs[i]=rv->peers[i].addr;
    size_t len=0;
    for (size_t i=0; i<rv->peer_count; ++i) {
        const fips_routing_peer *p=&rv->peers[i];
        if (!fips_bloom_contains(p->filter,dest)) continue;
        former_candidate *c=&cs[len++];
        c->addr=addrs[i]; c->cost=p->link_cost; c->len=p->coords->len;
        c->entries=malloc(c->len*sizeof(*c->entries));
        if (!c->entries) abort();
        memcpy(c->entries,p->coords->entries,c->len*sizeof(*c->entries));
    }
    size_t self=fips_tree_coordinate_distance_to(mc,dc), best_dist=SIZE_MAX;
    former_candidate *best=NULL;
    for (size_t i=0; i<len; ++i) {
        former_candidate *c=&cs[i]; size_t suffix=0;
        while (suffix<c->len && suffix<dc->len && !memcmp(c->entries[c->len-suffix-1].node_addr.bytes,dc->entries[dc->len-suffix-1].node_addr.bytes,16)) ++suffix;
        size_t dist=suffix ? c->len+dc->len-2*suffix : SIZE_MAX;
        if (dist>=self) continue;
        if (!best || c->cost<best->cost || (c->cost==best->cost && dist<best_dist) ||
            (c->cost==best->cost && dist==best_dist && memcmp(c->addr.bytes,best->addr.bytes,16)<0)) { best=c; best_dist=dist; }
    }
    bool found=best!=NULL; if (best) *out=best->addr;
    for (size_t i=0; i<len; ++i) free(cs[i].entries);
    free(cs); free(addrs); return found;
}
int main(void)
{
    const size_t counts[]={8,32,128,256}; fips_node_addr dest=addr(2,0), winner;
    coord(&coords[256],dest,4); coord(&coords[257],addr(6,0),5);
    for (size_t k=0; k<4; ++k) {
        size_t n=counts[k], survivors=0;
        for (size_t i=0; i<n; ++i) {
            fips_node_addr a=addr(1,(uint16_t)i); coord(&coords[i],a,4); fips_bloom_init(&filters[i]);
            for (uint16_t f=0; f<4; ++f) { fips_node_addr unrelated=addr(7,(uint16_t)(i*4+f)); fips_bloom_insert(&filters[i],&unrelated); }
            if (i%2==0) fips_bloom_insert(&filters[i],&dest);
            if (fips_bloom_contains(&filters[i],&dest)) ++survivors;
            peers[i]=(fips_routing_peer){a,&coords[i],&filters[i],1.0+(double)i*.01};
        }
        fips_routing_view rv={.peers=peers,.peer_count=n};
        fips_node_addr expected;
        if (!former(&rv,&dest,&coords[256],&coords[257],&expected)) abort();
        if (!fips_select_best_candidate(&rv,&dest,&coords[256],&coords[257],&winner) || memcmp(expected.bytes,winner.bytes,16)) abort();
        for (unsigned mode=0; mode<2; ++mode) {
            char name[96];
            snprintf(name, sizeof(name), "find_next_hop/%s/%zu",
                     mode ? "current_borrowed" : "former_allocating", n);
            BENCH(name, {
                bool found=mode ? fips_select_best_candidate(&rv,&dest,&coords[256],&coords[257],&winner) : former(&rv,&dest,&coords[256],&coords[257],&winner);
                REQUIRE(found && !memcmp(expected.bytes, winner.bytes, 16));
                bench_observe(&winner);
            });
            fprintf(stderr, "%s survivors=%zu\n", name, survivors);
        }
    }
    return 0;
}
