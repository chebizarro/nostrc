#include <fips/proto/bloom/mod.h>
#include <fips/proto/math.h>
#include <math.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bloom_pins.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #x); exit(1); } } while (0)
#define OK(x) CHECK((x) == FIPS_OK)
#define EQ(a,b) CHECK(fips_bloom_equal(&(a), &(b)))
#define HAS(f,n) CHECK(contains(&(f), (n)))
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
static fips_node_addr addr(uint8_t n) { fips_node_addr a = {{0}}; a.bytes[0] = n; return a; }
static void insert(fips_bloom_filter *f, uint8_t n) { fips_node_addr a = addr(n); fips_bloom_insert(f, &a); }
static bool contains(const fips_bloom_filter *f, uint8_t n) { fips_node_addr a = addr(n); return fips_bloom_contains(f, &a); }
static fips_bloom_filter tagged(uint8_t n) { fips_bloom_filter f; fips_bloom_init(&f); insert(&f, n); return f; }
static fips_bloom_filter entries(uint8_t tag, uint16_t from, uint16_t count)
{
    fips_bloom_filter f; fips_bloom_init(&f);
    for (uint16_t i = from; i < from + count; ++i) {
        fips_node_addr a; memset(a.bytes, tag, 16); a.bytes[1] = (uint8_t)i; a.bytes[2] = (uint8_t)(i >> 8);
        fips_bloom_insert(&f, &a);
    }
    return f;
}
static fips_bloom_filter filled(uint8_t tag, uint8_t count)
{
    fips_bloom_filter f; fips_bloom_init(&f);
    for (unsigned i = 0; i < count; ++i) { fips_node_addr a = addr((uint8_t)i); a.bytes[1] = tag; fips_bloom_insert(&f, &a); }
    return f;
}
static void test_bloom_filter_new(void)
{
    fips_bloom_filter f; fips_bloom_init(&f);
    CHECK(f.num_bits == 8192 && f.hash_count == 5 && fips_bloom_count_ones(&f) == 0 && fips_bloom_is_empty(&f));
}
static void test_bloom_filter_insert_contains(void)
{
    fips_bloom_filter f; fips_bloom_init(&f); CHECK(!contains(&f, 1) && !contains(&f, 2));
    insert(&f, 1); HAS(f,1); CHECK(!fips_bloom_is_empty(&f));
}
static void test_bloom_filter_multiple_inserts(void)
{
    fips_bloom_filter f; fips_bloom_init(&f);
    for (unsigned i = 0; i < 100; ++i) insert(&f, (uint8_t)i);
    for (unsigned i = 0; i < 100; ++i) HAS(f,(uint8_t)i);
    CHECK(fips_bloom_fill_ratio(&f) > 0 && fips_bloom_fill_ratio(&f) < 0.5);
}
static void test_bloom_filter_merge(void) { fips_bloom_filter a = tagged(1), b = tagged(2); OK(fips_bloom_merge(&a,&b,NULL)); HAS(a,1); HAS(a,2); }
static void test_bloom_filter_union(void)
{
    fips_bloom_filter a = tagged(1), b = tagged(2), u;
    OK(fips_bloom_union(&a,&b,&u,NULL)); HAS(u,1); HAS(u,2); CHECK(!contains(&a,2) && !contains(&b,1));
}
static void test_bloom_filter_clear(void) { fips_bloom_filter f = tagged(1); fips_bloom_clear(&f); CHECK(fips_bloom_is_empty(&f) && !contains(&f,1)); }
static void test_bloom_filter_merge_size_mismatch(void)
{
    fips_bloom_filter a,b; fips_proto_error e;
    OK(fips_bloom_with_params(&a,1024,7,NULL)); OK(fips_bloom_with_params(&b,2048,7,NULL));
    CHECK(fips_bloom_merge(&a,&b,&e) == FIPS_ERR_RANGE);
    CHECK(e.variant == FIPS_PROTO_BadBloom && e.as.bad_bloom.variant == FIPS_BLOOM_InvalidSize);
    CHECK(e.as.bad_bloom.as.invalid_size.expected == 1024 && e.as.bad_bloom.as.invalid_size.got == 2048);
}
static void test_bloom_filter_custom_params(void) { fips_bloom_filter f; OK(fips_bloom_with_params(&f,1024,5,NULL)); CHECK(f.num_bits == 1024 && f.hash_count == 5); }
static void test_bloom_filter_invalid_params(void)
{
    fips_bloom_filter f = tagged(42), before = f; fips_proto_error e;
    CHECK(fips_bloom_with_params(&f,1001,7,&e) == FIPS_ERR_RANGE);
    CHECK(e.as.bad_bloom.variant == FIPS_BLOOM_SizeNotByteAligned && e.as.bad_bloom.as.size_not_byte_aligned == 1001); EQ(f,before);
    CHECK(fips_bloom_with_params(&f,0,7,&e) == FIPS_ERR_RANGE && e.as.bad_bloom.as.size_not_byte_aligned == 0); EQ(f,before);
    CHECK(fips_bloom_with_params(&f,1024,0,&e) == FIPS_ERR_RANGE && e.as.bad_bloom.variant == FIPS_BLOOM_ZeroHashCount); EQ(f,before);
}
static void test_bloom_filter_from_bytes(void)
{
    fips_bloom_filter f,g; fips_bloom_init(&f); OK(fips_bloom_from_bytes(&g,f.bits,f.num_bits/8,f.hash_count,NULL)); EQ(f,g);
}
static void test_bloom_filter_estimated_count(void)
{
    fips_bloom_filter f; fips_bloom_init(&f); double n;
    CHECK(fips_bloom_estimated_count(&f,INFINITY,&n) && n == 0);
    for (unsigned i=0;i<50;++i) insert(&f,(uint8_t)i);
    CHECK(fips_bloom_estimated_count(&f,INFINITY,&n) && n>30 && n<100);
}
static void test_bloom_filter_equality(void) { fips_bloom_filter a,b; fips_bloom_init(&a); fips_bloom_init(&b); EQ(a,b); insert(&a,1); CHECK(!fips_bloom_equal(&a,&b)); insert(&b,1); EQ(a,b); }
static void test_bloom_filter_from_bytes_empty(void) { fips_bloom_filter f; fips_proto_error e; CHECK(fips_bloom_from_bytes(&f,NULL,0,5,&e)==FIPS_ERR_RANGE && e.as.bad_bloom.variant==FIPS_BLOOM_SizeNotByteAligned); }
static void test_bloom_filter_from_bytes_zero_hash_count(void) { fips_bloom_filter f; uint8_t b[128]={0}; fips_proto_error e; CHECK(fips_bloom_from_bytes(&f,b,sizeof(b),0,&e)==FIPS_ERR_RANGE && e.as.bad_bloom.variant==FIPS_BLOOM_ZeroHashCount); }
static void test_bloom_filter_from_slice(void) { fips_bloom_filter f=tagged(42),g; OK(fips_bloom_from_bytes(&g,f.bits,f.num_bits/8,5,NULL)); EQ(f,g); }
static void test_bloom_filter_insert_bytes_contains_bytes(void)
{
    fips_bloom_filter f; fips_bloom_init(&f);
    const uint8_t a[]="hello world",b[]="goodbye";
    CHECK(!fips_bloom_contains_bytes(&f,a,sizeof(a)-1)); fips_bloom_insert_bytes(&f,a,sizeof(a)-1);
    CHECK(fips_bloom_contains_bytes(&f,a,sizeof(a)-1) && !fips_bloom_contains_bytes(&f,b,sizeof(b)-1));
    fips_bloom_insert_bytes(&f,b,sizeof(b)-1); CHECK(fips_bloom_contains_bytes(&f,a,sizeof(a)-1) && fips_bloom_contains_bytes(&f,b,sizeof(b)-1));
}
static void test_bloom_filter_estimated_count_saturated(void)
{
    fips_bloom_filter f; uint8_t b[8]; memset(b,255,sizeof(b)); OK(fips_bloom_from_bytes(&f,b,sizeof(b),3,NULL));
    double n=42; CHECK(!fips_bloom_estimated_count(&f,INFINITY,&n) && n==42); CHECK(!fips_bloom_estimated_count(&f,.05,&n) && n==42);
}
static void test_bloom_filter_estimated_count_fpr_cap_boundary(void)
{
    fips_bloom_filter f; double n; fips_bloom_init(&f); memset(f.bits,255,560);
    CHECK(fips_bloom_estimated_count(&f,.05,&n)); memset(f.bits,255,564);
    CHECK(!fips_bloom_estimated_count(&f,.05,&n)); CHECK(fips_bloom_estimated_count(&f,.10,&n));
    /* Policy uses >, not >=; NaN behaves like Rust's comparison. */
    CHECK(fips_bloom_estimated_count(&f,fips_bloom_fpr(&f),&n)); CHECK(fips_bloom_estimated_count(&f,NAN,&n));
}
static void test_bloom_filter_default(void) { fips_bloom_filter a,b; fips_bloom_init(&a); OK(fips_bloom_with_params(&b,8192,5,NULL)); EQ(a,b); }
/* Rust's Debug trait has no C object/trait equivalent; its information is
 * checked through the public diagnostic values, without a logging/IO API. */
static void test_bloom_filter_debug_format(void)
{
    fips_bloom_filter f; fips_bloom_init(&f); double n;
    CHECK(f.num_bits==8192 && f.hash_count==5 && fips_bloom_fill_ratio(&f)==0 && fips_bloom_estimated_count(&f,INFINITY,&n) && n==0);
    for(unsigned i=0;i<10;++i) insert(&f,(uint8_t)i);
    CHECK(fips_bloom_fill_ratio(&f)>0 && fips_bloom_estimated_count(&f,INFINITY,&n) && n>0);
}
static uint64_t le64(const uint8_t *p) { uint64_t x=0; for(unsigned i=0;i<8;++i) x|=(uint64_t)p[i]<<(8*i); return x; }
static void indices_check(const uint8_t *data,size_t len,size_t m,uint8_t k)
{
    fips_bloom_filter f; uint8_t digest[32], expected[FIPS_BLOOM_MAX_BYTES]={0};
    SHA256(data,len,digest); uint64_t h1=le64(digest),h2=le64(digest+8);
    for(unsigned i=0;i<k;++i) { size_t bit=(size_t)(h1+(uint64_t)i*h2)%m; expected[bit/8]|=(uint8_t)(1u<<(bit%8)); }
    OK(fips_bloom_with_params(&f,m,k,NULL)); fips_bloom_insert_bytes(&f,data,len);
    CHECK(!memcmp(f.bits,expected,m/8) && fips_bloom_contains_bytes(&f,data,len));
}
static void test_bloom_filter_bit_indices_match_double_hashing_formula(void)
{
    const uint8_t a[]={0,1,2,3,255};
    const struct {const uint8_t *p;size_t n;} inputs[]={{NULL,0},{(const uint8_t *)"alpha",5},{(const uint8_t *)"the quick brown fox",19},{a,sizeof(a)}};
    for(size_t i=0;i<COUNT(inputs);++i) { indices_check(inputs[i].p,inputs[i].n,1024,5); indices_check(inputs[i].p,inputs[i].n,8192,7); }
    fips_node_addr node=addr(7); indices_check(node.bytes,16,1024,5);
    fips_bloom_filter f; OK(fips_bloom_with_params(&f,1024,5,NULL)); fips_bloom_insert(&f,&node); CHECK(!contains(&f,200));
    for(size_t i=0;i<COUNT(index_pins);++i) {
        fips_bloom_init(&f); fips_bloom_insert_bytes(&f,index_pins[i].addr,16);
        CHECK(fips_bloom_count_ones(&f)==5);
        for(unsigned j=0;j<5;++j) { size_t bit=index_pins[i].indices[j]; CHECK(f.bits[bit/8]&(1u<<(bit%8))); }
    }
}
static void overlap_is_one_against_itself_and_against_any_subset_of_it(void)
{
    fips_bloom_filter a=filled(1,10),b=a,c=filled(2,30); double n;
    OK(fips_bloom_merge(&b,&c,NULL)); CHECK(fips_bloom_overlap(&a,&a,&n) && n==1); CHECK(fips_bloom_overlap(&b,&a,&n) && n==1);
    CHECK(fips_bloom_overlap(&a,&b,&n) && n==(double)fips_bloom_count_ones(&a)/(double)fips_bloom_count_ones(&b) && n<1);
}
static void overlap_is_zero_for_filters_with_no_bit_in_common(void)
{
    fips_bloom_filter a,b; fips_bloom_init(&a); fips_bloom_init(&b); fips_bloom_insert_bytes(&a,(const uint8_t *)"x",1);
    for(unsigned i=0;i<512;++i) { uint8_t bytes[2]={(uint8_t)i,(uint8_t)(i>>8)}; fips_bloom_insert_bytes(&b,bytes,2); }
    for(size_t i=0;i<1024;++i) b.bits[i]&=(uint8_t)~a.bits[i];
    double n; CHECK(fips_bloom_count_ones(&b)>0 && fips_bloom_overlap(&b,&a,&n) && n==0);
}
static void overlap_is_undefined_against_an_empty_filter_or_across_sizes(void)
{
    fips_bloom_filter a=filled(1,10),b; double n=42; fips_bloom_init(&b); CHECK(!fips_bloom_overlap(&a,&b,&n) && n==42);
    OK(fips_bloom_with_params(&b,1024,5,NULL)); fips_bloom_insert_bytes(&b,(const uint8_t *)"x",1);
    CHECK(!fips_bloom_overlap(&a,&b,&n) && !fips_bloom_overlap(&b,&a,&n));
}
static void a_filter_returning_what_was_sent_plus_one_entry_echoes_it(void) { fips_bloom_filter sent=entries(0xa1,0,1000),got=sent; insert(&got,7); CHECK(fips_bloom_echoes(&got,&sent,.8)); }
static void an_unrelated_200_entry_filter_does_not_echo_a_1000_entry_one(void) { fips_bloom_filter sent=entries(0xa1,0,1000),got=entries(0xb2,0,200); CHECK(!fips_bloom_echoes(&got,&sent,.8) && fips_bloom_echoes(&got,&sent,-1)); }
static void a_subset_holding_half_of_what_was_sent_echoes_only_at_a_low_threshold(void) { fips_bloom_filter sent=entries(0xa1,0,1000),half=entries(0xa1,0,500); CHECK(!fips_bloom_echoes(&half,&sent,.8) && fips_bloom_echoes(&half,&sent,.3)); }
static void an_exact_return_of_a_filter_below_the_bit_floor_does_not_echo(void)
{
    fips_bloom_filter small=entries(0xa1,0,20),large=entries(0xa1,0,30);
    CHECK(fips_bloom_count_ones(&small)<128 && !fips_bloom_echoes(&small,&small,.8)); CHECK(fips_bloom_count_ones(&large)>=128 && fips_bloom_echoes(&large,&large,.8));
}
static void a_threshold_of_one_or_nan_never_echoes_even_an_exact_return(void)
{
    fips_bloom_filter f=entries(0xa1,0,1000);
    CHECK(!fips_bloom_echoes(&f,&f,1) && !fips_bloom_echoes(&f,&f,1.5) && !fips_bloom_echoes(&f,&f,NAN) && fips_bloom_echoes(&f,&f,1-1e-12));
}
static void echo_is_false_across_sizes_against_an_empty_filter_and_for_a_saturated_one(void)
{
    fips_bloom_filter f=entries(0xa1,0,1000),b;
    OK(fips_bloom_with_params(&b,1024,5,NULL)); for(unsigned i=0;i<100;++i) { uint8_t bytes[2]={(uint8_t)i,0}; fips_bloom_insert_bytes(&b,bytes,2); }
    CHECK(fips_bloom_count_ones(&b)>=128 && !fips_bloom_echoes(&b,&f,.8) && !fips_bloom_echoes(&f,&b,.8));
    fips_bloom_init(&b); CHECK(!fips_bloom_echoes(&f,&b,.8)); memset(b.bits,255,1024); CHECK(!fips_bloom_echoes(&b,&f,.8) && !fips_bloom_echoes(&b,&f,-1));
}
static void pinned_estimates(void)
{
    for(size_t i=0;i<COUNT(estimate_pins);++i) {
        fips_bloom_filter f; OK(fips_bloom_with_params(&f,estimate_pins[i].m,estimate_pins[i].k,NULL));
        for(size_t j=0;j<estimate_pins[i].x;++j) f.bits[j/8]|=(uint8_t)(1u<<(j%8));
        double n; CHECK(fips_bloom_estimated_count(&f,INFINITY,&n)); uint64_t bits; memcpy(&bits,&n,8);
        if(bits!=estimate_pins[i].count) fprintf(stderr,"pin m=%zu k=%u x=%zu: %016llx != %016llx\n",f.num_bits,f.hash_count,estimate_pins[i].x,(unsigned long long)bits,(unsigned long long)estimate_pins[i].count);
        CHECK(bits==estimate_pins[i].count); double p=fips_bloom_fpr(&f); memcpy(&bits,&p,8); CHECK(bits==estimate_pins[i].fpr);
        double independent; memcpy(&independent,&estimate_pins[i].math_log_count,8);
        CHECK(fabs(n-independent)<=fabs(n)*1e-15); /* independent math.log, not the protocol's bit oracle */
    }
}

static fips_bloom_state s;
static fips_bloom_peer storage[16];
static fips_node_addr leaves[16];
static fips_bloom_action action_storage[16];
static fips_bloom_actions actions;
static void reset(void) { fips_bloom_state_init(&s,addr(0),false,storage,COUNT(storage),leaves,COUNT(leaves)); actions=(fips_bloom_actions){action_storage,0,COUNT(action_storage)}; }
static fips_bloom_peer *peer(uint8_t n)
{
    fips_node_addr a=addr(n); OK(fips_bloom_state_add_peer(&s,a));
    fips_bloom_peer *p=fips_bloom_state_peer(&s,&a); CHECK(p); fips_bloom_state_peer_role(&s,p,true,false,true); return p;
}
static void leaf(uint8_t n) { OK(fips_bloom_state_add_leaf(&s,addr(n))); }
static fips_bloom_filter outgoing(uint8_t n) { fips_bloom_filter f; fips_node_addr a=addr(n); fips_bloom_state_outgoing(&s,&a,&f); return f; }
static fips_bloom_filter base(void) { fips_bloom_filter f; fips_bloom_state_base_filter(&s,&f); return f; }
static void inbound(fips_bloom_peer *p, fips_bloom_filter f) { p->inbound=f; p->has_inbound=true; }
static void test_bloom_state_new(void) { reset(); CHECK(!memcmp(s.own_addr.bytes,addr(0).bytes,16) && !s.is_leaf_only && s.sequence==0 && s.leaf_count==0); }
static void test_bloom_state_leaf_only(void) { reset(); fips_bloom_state_init(&s,addr(0),true,storage,16,leaves,16); CHECK(s.is_leaf_only); }
static void test_bloom_state_leaf_dependents(void)
{
    reset(); leaf(1); leaf(2); CHECK(s.leaf_count==2); fips_node_addr a=addr(1);
    CHECK(fips_bloom_state_remove_leaf(&s,&a) && s.leaf_count==1 && !fips_bloom_state_remove_leaf(&s,&a));
}
static void test_bloom_state_debounce(void)
{
    reset(); fips_bloom_peer *p=peer(1); CHECK(fips_bloom_peer_should_send(&s,p,1000));
    fips_bloom_peer_record_update(p,1000); p->pending=true;
    CHECK(!fips_bloom_peer_should_send(&s,p,1200) && fips_bloom_peer_should_send(&s,p,1600));
}
static void should_send_update_after_backward_clock_step_waits_at_most_the_debounce(void)
{
    reset(); fips_bloom_peer *p=peer(1); fips_bloom_peer_record_update(p,100000); p->pending=true;
    CHECK(fips_bloom_peer_should_send(&s,p,40000)); fips_bloom_peer_record_update(p,40000); p->pending=true;
    CHECK(!fips_bloom_peer_should_send(&s,p,40200) && fips_bloom_peer_should_send(&s,p,40500));
    s.update_debounce_ms=UINT64_MAX; CHECK(!fips_bloom_peer_should_send(&s,p,UINT64_MAX));
}
static void test_bloom_state_sequence(void)
{
    reset(); fips_bloom_peer *a=peer(1),*b=peer(2);
    CHECK(fips_bloom_peer_next_sequence(&s,a,10)==10 && fips_bloom_peer_next_sequence(&s,b,10)==10);
    CHECK(fips_bloom_peer_next_sequence(&s,a,10)==11 && s.sequence==3);
}
static void test_bloom_state_next_sequence_for_a_new_peer_starts_at_the_floor_whatever_was_sent_to_others(void)
{
    reset(); fips_bloom_peer *a=peer(1),*b=peer(2);
    for(unsigned i=0;i<5;++i) CHECK(fips_bloom_peer_next_sequence(&s,a,1000)==1000+i);
    CHECK(fips_bloom_peer_next_sequence(&s,b,1000)==1000);
}
static void test_bloom_state_sequence_to_a_peer_that_returns_exceeds_every_value_sent_before_it_left(void)
{
    reset(); fips_bloom_peer *a=peer(1); for(unsigned i=0;i<3;++i) CHECK(fips_bloom_peer_next_sequence(&s,a,1000)==1000+i);
    fips_node_addr id=addr(1); CHECK(fips_bloom_state_remove_peer(&s,&id)); a=peer(1); CHECK(fips_bloom_peer_next_sequence(&s,a,1256)==1256);
    a->sequence=UINT64_MAX; CHECK(fips_bloom_peer_next_sequence(&s,a,0)==UINT64_MAX);
}
static void test_bloom_state_pending_updates(void)
{
    reset(); fips_bloom_peer *a=peer(1),*b=peer(2); fips_bloom_state_clear_pending(&s); CHECK(!a->pending && !b->pending);
    a->pending=true; CHECK(a->pending && !b->pending); fips_bloom_state_mark_all(&s); CHECK(a->pending && b->pending);
    fips_bloom_state_clear_pending(&s); CHECK(!a->pending && !b->pending);
}
static void test_bloom_state_base_filter(void) { reset(); leaf(1); fips_bloom_filter f=base(); HAS(f,0); HAS(f,1); CHECK(!contains(&f,99)); }
static void test_bloom_state_compute_outgoing_filter(void)
{
    reset(); leaf(1); fips_bloom_peer *a=peer(10),*b=peer(20); fips_bloom_filter f=tagged(100); insert(&f,101); inbound(a,f); inbound(b,tagged(200));
    fips_bloom_filter x=outgoing(10),y=outgoing(20); HAS(x,0); HAS(x,1); HAS(x,200); HAS(y,0); HAS(y,1); HAS(y,100); HAS(y,101);
}
static void test_bloom_state_leaf_dependents_accessor(void)
{
    reset(); leaf(1); leaf(2); CHECK(s.leaf_count==2 && s.leaves[0].bytes[0]==1 && s.leaves[1].bytes[0]==2);
}
static void test_bloom_state_record_sent_filter(void)
{
    reset(); fips_bloom_peer *p=peer(1); fips_bloom_filter f=tagged(42); fips_bloom_peer_record_filter(p,&f);
    fips_bloom_state_clear_pending(&s); fips_node_addr exclude=addr(99); fips_bloom_state_mark_changed(&s,&exclude); CHECK(p->pending);
}
static void test_bloom_state_remove_peer_state(void)
{
    reset(); fips_bloom_peer *p=peer(1); fips_bloom_filter f; fips_bloom_init(&f); fips_bloom_peer_record_update(p,1000);
    p->pending=true; fips_bloom_peer_record_filter(p,&f); fips_node_addr id=addr(1); CHECK(fips_bloom_state_remove_peer(&s,&id));
    CHECK(!fips_bloom_state_peer(&s,&id)); p=peer(1); CHECK(fips_bloom_peer_should_send(&s,p,1200));
    fips_bloom_state_clear_pending(&s); fips_bloom_state_mark_changed(&s,NULL); CHECK(p->pending && p->sent_len==0);
}
static void test_bloom_state_mark_changed_peers_never_sent(void)
{
    reset(); fips_bloom_peer *a=peer(1),*b=peer(2); fips_bloom_state_clear_pending(&s); fips_bloom_state_mark_changed(&s,NULL); CHECK(a->pending && b->pending);
}
static void record_base(fips_bloom_peer *p) { fips_bloom_filter f=base(); fips_bloom_peer_record_filter(p,&f); }
static void test_bloom_state_mark_changed_peers_unchanged(void)
{
    reset(); fips_bloom_peer *a=peer(1),*b=peer(2); record_base(a); record_base(b); fips_bloom_state_clear_pending(&s);
    fips_bloom_state_mark_changed(&s,NULL); CHECK(!a->pending && !b->pending);
}
static void test_bloom_state_mark_changed_peers_one_changed(void)
{
    reset(); fips_bloom_peer *a=peer(1),*b=peer(2); record_base(a); record_base(b); fips_bloom_state_clear_pending(&s);
    inbound(a,tagged(100)); fips_bloom_state_mark_changed(&s,&a->addr); CHECK(!a->pending && b->pending);
}
static void test_bloom_state_mark_changed_peers_excludes_source(void)
{
    reset(); fips_bloom_peer *a=peer(1); fips_bloom_state_clear_pending(&s); fips_bloom_state_mark_changed(&s,&a->addr); CHECK(!a->pending);
}
static void test_compute_outgoing_filters_matches_per_peer(void)
{
    reset(); leaf(200); leaf(201);
    for(unsigned i=1;i<=6;++i) { fips_bloom_peer *p=peer((uint8_t)i); fips_bloom_filter f; fips_bloom_init(&f); for(unsigned j=0;j<5;++j) insert(&f,(uint8_t)(i*7+j)); inbound(p,f); }
    fips_bloom_peer *odd=peer(7); fips_bloom_filter f; OK(fips_bloom_with_params(&f,4096,5,NULL)); insert(&f,99); inbound(odd,f);
    (void)peer(120); (void)peer(121); OK(fips_bloom_state_refresh(&s,0,1,&actions)); CHECK(actions.len==9);
    for(size_t i=0;i<actions.len;++i) { fips_bloom_filter expected=outgoing(actions.items[i].peer.bytes[0]); EQ(expected,actions.items[i].announce.filter); }
    fips_bloom_filter a=outgoing(3),b=outgoing(1); CHECK(!contains(&a,21) && contains(&b,21));
}
static void test_compute_outgoing_filters_empty_inputs(void)
{
    reset(); OK(fips_bloom_state_refresh(&s,0,1,&actions)); CHECK(actions.len==0); (void)peer(1);
    OK(fips_bloom_state_refresh(&s,0,1,&actions)); fips_bloom_filter expected=base(); CHECK(actions.len==1); EQ(expected,actions.items[0].announce.filter);
}
static void history_is(fips_bloom_peer *p,const uint8_t *tags,size_t n)
{
    CHECK(p->sent_len==n);
    for(size_t i=0;i<n;++i) { fips_bloom_filter f=tagged(tags[i]); EQ(f,p->sent[i]); }
}
static void record_tag(fips_bloom_peer *p,uint8_t n) { fips_bloom_filter f=tagged(n); fips_bloom_peer_record_filter(p,&f); }
static void the_sent_history_keeps_the_last_four_distinct_filters_newest_first(void)
{
    reset(); fips_bloom_peer *p=peer(1); CHECK(p->sent_len==0 && FIPS_BLOOM_SENT_HISTORY==4);
    for(unsigned n=10;n<16;++n) record_tag(p,(uint8_t)n);
    const uint8_t tags[]={15,14,13,12}; history_is(p,tags,4);
}
static void resending_a_kept_filter_moves_it_to_the_newest_place_without_dropping_another(void)
{
    reset(); fips_bloom_peer *p=peer(1); for(unsigned n=10;n<=13;++n) record_tag(p,(uint8_t)n);
    record_tag(p,13); const uint8_t a[]={13,12,11,10}; history_is(p,a,4);
    record_tag(p,11); const uint8_t b[]={11,13,12,10}; history_is(p,b,4);
}
static void the_sent_history_is_kept_per_peer(void)
{
    reset(); fips_bloom_peer *a=peer(1),*b=peer(2); record_tag(a,10); record_tag(b,20); record_tag(a,11);
    const uint8_t x[]={11,10},y[]={20}; history_is(a,x,2); history_is(b,y,1);
}
static void forgetting_a_restarted_peer_clears_its_whole_sent_history(void)
{
    reset(); fips_bloom_peer *p=peer(1),*b=peer(2); for(unsigned n=10;n<=12;++n) record_tag(p,(uint8_t)n); record_tag(b,20);
    fips_bloom_peer_forget_sent(p); CHECK(p->sent_len==0); const uint8_t x[]={20}; history_is(b,x,1);
    record_tag(p,13); const uint8_t y[]={13}; history_is(p,y,1);
}
static void removing_a_peer_clears_its_whole_sent_history(void)
{
    reset(); fips_bloom_peer *p=peer(1),*b=peer(2); for(unsigned n=10;n<=12;++n) record_tag(p,(uint8_t)n); record_tag(b,20);
    fips_node_addr a=addr(1); CHECK(fips_bloom_state_remove_peer(&s,&a)); b=fips_bloom_state_peer(&s,&(fips_node_addr){{2}});
    const uint8_t x[]={20}; history_is(b,x,1); p=peer(1); CHECK(p->sent_len==0);
}
static void change_detection_compares_with_the_newest_sent_filter_only(void)
{
    reset(); fips_bloom_peer *p=peer(1); record_base(p); record_tag(p,10); fips_bloom_state_clear_pending(&s);
    fips_bloom_state_mark_changed(&s,NULL); CHECK(p->pending); fips_bloom_state_clear_pending(&s);
    record_base(p); fips_bloom_state_mark_changed(&s,NULL); CHECK(!p->pending);
}
static void only_the_first_echo_refusal_from_a_peer_in_each_interval_is_a_warning(void)
{
    reset(); fips_bloom_peer *p=peer(1); CHECK(fips_bloom_peer_warn_echo(p,1000)); CHECK(!fips_bloom_peer_warn_echo(p,1000));
    CHECK(!fips_bloom_peer_warn_echo(p,1000+FIPS_BLOOM_ECHO_WARN_MS-1)); CHECK(fips_bloom_peer_warn_echo(p,1000+FIPS_BLOOM_ECHO_WARN_MS)); CHECK(!fips_bloom_peer_warn_echo(p,1001+FIPS_BLOOM_ECHO_WARN_MS));
}
static void the_first_echo_refusal_from_a_node_in_its_first_millisecond_is_a_warning(void) { reset(); fips_bloom_peer *p=peer(1); CHECK(fips_bloom_peer_warn_echo(p,0) && !fips_bloom_peer_warn_echo(p,1)); }
static void echo_refusal_warnings_are_spaced_per_peer(void) { reset(); fips_bloom_peer *a=peer(1),*b=peer(2); CHECK(fips_bloom_peer_warn_echo(a,1000) && fips_bloom_peer_warn_echo(b,1001) && !fips_bloom_peer_warn_echo(a,1002)); }
static void an_echo_refusal_read_before_the_last_warning_warns_and_restamps(void) { reset(); fips_bloom_peer *p=peer(1); CHECK(fips_bloom_peer_warn_echo(p,10000) && fips_bloom_peer_warn_echo(p,5000) && !fips_bloom_peer_warn_echo(p,5001)); }
static void removing_a_peer_clears_its_echo_warning_stamp(void)
{
    reset(); fips_bloom_peer *p=peer(1); CHECK(fips_bloom_peer_warn_echo(p,1000)); fips_node_addr a=addr(1); CHECK(fips_bloom_state_remove_peer(&s,&a)); p=peer(1); CHECK(fips_bloom_peer_warn_echo(p,1001));
}
static void the_mtu_warning_is_given_once_per_peering_and_again_after_the_peer_is_removed(void)
{
    reset(); fips_bloom_peer *p=peer(1),*b=peer(2); CHECK(fips_bloom_peer_warn_mtu(p) && !fips_bloom_peer_warn_mtu(p) && fips_bloom_peer_warn_mtu(b));
    fips_node_addr a=addr(1); CHECK(fips_bloom_state_remove_peer(&s,&a)); p=peer(1); b=fips_bloom_state_peer(&s,&(fips_node_addr){{2}});
    CHECK(fips_bloom_peer_warn_mtu(p) && !fips_bloom_peer_warn_mtu(b));
}

/* Upstream state.rs Track/hold model: synthetic milliseconds, never sleeps. */
#define E1 UINT64_C(0x0e01)
#define E2 UINT64_C(0x0e02)
#define E3 UINT64_C(0x0e03)
#define NONE FIPS_BLOOM_RESEND_NONE
#define LOSS FIPS_BLOOM_RESEND_LOSS
#define TIMEOUT FIPS_BLOOM_RESEND_TIMEOUT
#define UNVERIFIED FIPS_BLOOM_RESEND_UNVERIFIED
#define SESSION_CHANGED FIPS_BLOOM_RESEND_SESSION_CHANGED
static fips_bloom_link_evidence link_no(uint64_t epoch,uint64_t next) { return (fips_bloom_link_evidence){.epoch=epoch,.next_counter=next}; }
static fips_bloom_link_evidence link_rr(uint64_t epoch,uint64_t next,uint64_t h,uint64_t r,uint32_t o)
{
    return (fips_bloom_link_evidence){epoch,next,true,{h,r,o}};
}
typedef struct track { fips_bloom_peer *p; uint8_t content; uint64_t counter; } track;
static track track_new(void) { reset(); fips_bloom_peer *p=peer(1); p->pending=false; return (track){p,1,0}; }
static void send(track *t,uint64_t counter,fips_bloom_link_evidence ev,uint64_t now)
{
    fips_bloom_filter f=tagged((uint8_t)(100+t->content));
    fips_bloom_action proposal = {.kind=FIPS_BLOOM_SEND, .peer=t->p->addr};
    fips_filter_announce_init(&proposal.announce,&f,0);
    OK(fips_bloom_state_sent(&s,t->p,&proposal,(fips_bloom_send_outcome){FIPS_BLOOM_SEND_ACCEPTED,1071,1400},counter,&ev,now,&actions));
    t->counter=counter;
}
static void send_new(track *t,uint64_t counter,fips_bloom_link_evidence ev,uint64_t now) { ++t->content; send(t,counter,ev,now); }
static fips_bloom_resend check(track *t,fips_bloom_link_evidence ev,uint64_t now) { return fips_bloom_peer_check_announce(&s,t->p,&ev,now); }
typedef fips_bloom_link_evidence (*model)(uint64_t,uint64_t,bool);
typedef struct resend_list { size_t len; uint64_t times[128]; fips_bloom_resend reasons[128]; } resend_list;
static resend_list hold(track *t,uint64_t from,uint64_t to,bool renew,model m)
{
    resend_list list={0};
    for(uint64_t now=from;now<=to;now+=1000) {
        fips_bloom_resend r=check(t,m(now,t->counter,false),now);
        if(r!=NONE) { CHECK(list.len<COUNT(list.times)); list.times[list.len]=now; list.reasons[list.len++]=r;
            fips_bloom_link_evidence ev=m(now,t->counter,true); if(renew) ++t->content; send(t,ev.next_counter-1,ev,now); }
    }
    return list;
}
static void one(resend_list a,uint64_t time,fips_bloom_resend why) { CHECK(a.len==1 && a.times[0]==time && a.reasons[0]==why); }
static fips_bloom_link_evidence lossy(uint64_t now,uint64_t c,bool recording)
{
    (void)now;
    if(recording) { uint64_t n=c+1; return link_rr(E1,n+1,n-1,n,0); }
    return link_rr(E1,c+2,c+1,c+1,0);
}
static fips_bloom_link_evidence late_hole(uint64_t now,uint64_t c,bool recording) { (void)now; return link_rr(E1,c+(recording?2:3),14,14,1); }
static fips_bloom_link_evidence quiet(uint64_t now,uint64_t c,bool recording) { (void)now; return link_no(E1,c+(recording?2:1)); }
static fips_bloom_link_evidence later(uint64_t now,uint64_t c,bool recording) { (void)now; return link_no(E2,c+(recording?2:1)); }
static fips_bloom_link_evidence old_report(uint64_t now,uint64_t c,bool recording) { (void)now; return link_rr(E1,c+(recording?2:1),20,30,0); }
static fips_bloom_link_evidence regressed(uint64_t now,uint64_t c,bool recording) { (void)now; return link_rr(E1,c+(recording?2:3),14,8,0); }
static fips_bloom_link_evidence surplus(uint64_t now,uint64_t c,bool recording) { (void)now; return link_rr(E1,c+(recording?2:3),14,20,0); }
static fips_bloom_link_evidence ambiguous(uint64_t now,uint64_t c,bool recording) { (void)now; return link_rr(E2,c+(recording?2:3),14,515,1); }
static fips_bloom_link_evidence later_hole(uint64_t now,uint64_t c,bool recording) { (void)now; return link_rr(E2,c+(recording?2:3),14,15,1); }
static fips_bloom_link_evidence frozen(uint64_t now,uint64_t c,bool recording)
{
    (void)c; uint64_t n=1+now/10+(recording?1u:0u);
    return now<62000 ? link_rr(E2,n,5000,90000,40) : link_rr(E2,n,6100,96101,45);
}
static fips_bloom_link_evidence third(uint64_t now,uint64_t c,bool recording)
{
    uint64_t n=10+(now-121000)/1000+(recording?1u:0u); if(n<c+1) n=c+1;
    return link_rr(E3,n,5,6,0);
}
static fips_bloom_link_evidence polluted(uint64_t now,uint64_t c,bool recording)
{
    uint64_t secs=now/1000,n=1+10*secs; if(n<c+1) n=c+1; n+=recording?1u:0u;
    return link_rr(E2,n,9000,100+5*secs,(uint32_t)(5*secs));
}
static void test_bloom_ack_covering_report_with_a_missing_frame_resends_on_loss(void)
{
    track t=track_new(); send(&t,12,link_rr(E1,13,9,10,0),0); CHECK(check(&t,link_rr(E1,15,14,14,0),1000)==LOSS && t.p->pending);
}
static void test_bloom_ack_covering_report_with_every_frame_confirms(void)
{
    track t=track_new(); send(&t,12,link_rr(E1,13,9,10,0),0); CHECK(check(&t,link_rr(E1,15,14,15,0),1000)==NONE && !t.p->ack.outstanding && !t.p->pending);
}
static void test_bloom_ack_reordered_frame_does_not_mask_a_lost_announce(void)
{
    track t=track_new(); send(&t,12,link_rr(E1,13,9,9,0),0); CHECK(check(&t,late_hole(1000,12,false),1000)==NONE && t.p->ack.outstanding);
    one(hold(&t,2000,120000,false,late_hole),30000,TIMEOUT);
}
static void test_bloom_ack_report_below_the_announce_waits_for_a_covering_one(void)
{
    track t=track_new(); send(&t,12,link_rr(E1,13,9,10,0),0); CHECK(check(&t,link_rr(E1,13,11,12,0),1000)==NONE && t.p->ack.outstanding);
    CHECK(check(&t,link_rr(E1,15,14,15,0),2000)==NONE && !t.p->ack.outstanding);
}
static void test_bloom_ack_announce_from_an_earlier_session_is_resent_once(void)
{
    track t=track_new(); send(&t,12,link_rr(E1,13,9,10,0),0); CHECK(check(&t,link_no(E2,1),1000)==NONE);
    CHECK(check(&t,link_rr(E2,5,3,4,0),2000)==SESSION_CHANGED); send(&t,5,link_rr(E2,6,3,4,0),2000);
    CHECK(check(&t,link_rr(E2,7,6,7,0),3000)==NONE && !t.p->ack.outstanding);
    t=track_new(); send(&t,12,link_rr(E1,13,9,10,0),0); CHECK(check(&t,link_no(E2,1),1000)==NONE);
    one(hold(&t,2000,121000,false,later),31000,TIMEOUT);
}
static void test_bloom_ack_first_session_measures_from_counter_zero(void)
{
    track t=track_new(); send(&t,12,link_no(E1,13),0); CHECK(check(&t,link_rr(E1,13,12,13,0),1000)==NONE && !t.p->ack.outstanding);
    t=track_new(); send(&t,12,link_no(E1,13),0); CHECK(check(&t,link_rr(E1,13,12,12,0),1000)==LOSS);
}
static void test_bloom_ack_later_session_without_a_report_has_no_base(void)
{
    track t=track_new(); send(&t,3,link_no(E1,4),0); send(&t,12,link_no(E2,13),1000);
    CHECK(check(&t,link_rr(E2,13,8,509,0),2000)==NONE && t.p->ack.outstanding);
    CHECK(check(&t,link_rr(E2,15,14,515,0),3000)==NONE && !t.p->ack.outstanding);
    t=track_new(); send(&t,3,link_no(E1,4),0); send(&t,12,link_no(E2,13),1000);
    CHECK(check(&t,link_rr(E2,15,14,515,0),2000)==UNVERIFIED);
}
static void test_bloom_ack_report_from_another_session_at_send_gets_one_timeout(void)
{
    track t=track_new(); send(&t,12,link_rr(E1,13,20,30,0),0); one(hold(&t,1000,120000,false,old_report),30000,TIMEOUT);
}
static void test_bloom_ack_no_report_ever_resends_once_per_session(void)
{
    track t=track_new(); send(&t,12,link_no(E1,13),0); CHECK(check(&t,link_no(E1,13),29999)==NONE);
    one(hold(&t,30000,120000,false,quiet),30000,TIMEOUT); CHECK(check(&t,link_no(E2,1),121000)==NONE);
    one(hold(&t,122000,240000,false,later),151000,TIMEOUT);
}
static void test_bloom_ack_sustained_trigger_backs_off_to_one_resend_a_minute(void)
{
    track t=track_new(); send(&t,12,lossy(0,11,true),0); resend_list list=hold(&t,0,600000,true,lossy);
    const uint64_t first[]={0,1000,3000,7000,15000,31000,63000}; CHECK(list.len==15);
    for(size_t i=0;i<list.len;++i) { CHECK(list.times[i]==(i<COUNT(first)?first[i]:123000+(i-COUNT(first))*60000)); CHECK(list.reasons[i]==LOSS);
        size_t n=0; for(size_t j=i;j<list.len && list.times[j]<list.times[i]+60000;++j) ++n; CHECK(n<=6); }
}
static void test_bloom_ack_backoff_resets_only_after_the_quiet_period(void)
{
    for(unsigned quiet_case=0;quiet_case<2;++quiet_case) {
        track t=track_new(); send(&t,12,lossy(0,11,true),0); resend_list first=hold(&t,0,31000,true,lossy);
        const uint64_t expected[]={0,1000,3000,7000,15000,31000}; CHECK(first.len==6); CHECK(!memcmp(first.times,expected,sizeof(expected)));
        uint64_t next=quiet_case?150000:151000; resend_list r=hold(&t,next,next+70000,true,lossy);
        CHECK(r.len>=2 && r.times[0]==next && r.times[1]==(quiet_case?210000:152000));
    }
}
static void test_bloom_ack_regressed_report_is_no_evidence(void)
{
    track t=track_new(); send(&t,12,link_rr(E1,13,9,10,0),0); CHECK(check(&t,regressed(1000,12,false),1000)==NONE && t.p->ack.outstanding);
    one(hold(&t,2000,30000,false,regressed),30000,TIMEOUT);
}
static void test_bloom_ack_removed_peer_starts_fresh(void)
{
    track t=track_new(); send(&t,12,link_rr(E1,13,9,10,0),0); fips_node_addr a=addr(1); CHECK(fips_bloom_state_remove_peer(&s,&a));
    t.p=peer(1); t.p->pending=false; CHECK(check(&t,link_rr(E1,15,14,14,0),1000)==NONE && !t.p->ack.outstanding);
    send(&t,3,link_no(E2,4),2000); CHECK(check(&t,link_rr(E2,4,3,4,0),3000)==NONE && !t.p->ack.outstanding);
}
static void test_bloom_ack_confirmation_keeps_the_backoff(void)
{
    track t=track_new(); send(&t,12,lossy(0,11,true),0); CHECK(check(&t,lossy(0,12,false),0)==LOSS); send(&t,13,lossy(0,12,true),0);
    CHECK(check(&t,link_rr(E1,15,14,15,0),100)==NONE && !t.p->ack.outstanding); send_new(&t,15,lossy(200,14,true),200);
    CHECK(check(&t,lossy(500,15,false),500)==NONE && check(&t,lossy(1000,15,false),1000)==LOSS);
}
static void test_bloom_ack_frozen_report_after_a_rekey_spends_only_the_unchecked_budget(void)
{
    track t=track_new(); send(&t,3,link_no(E1,4),0); send(&t,3,link_rr(E2,4,5000,90000,40),0);
    one(hold(&t,1000,59000,false,frozen),30000,TIMEOUT); send_new(&t,6000,link_rr(E2,6001,5000,90000,40),60000);
    one(hold(&t,61000,120000,false,frozen),90000,TIMEOUT); one(hold(&t,121000,140000,false,third),121000,SESSION_CHANGED);
}
static void test_bloom_ack_polluted_report_after_a_rekey_spends_only_the_unchecked_budget(void)
{
    track t=track_new(); send(&t,3,link_no(E1,4),0); send(&t,5,polluted(0,4,true),0); one(hold(&t,1000,120000,false,polluted),30000,TIMEOUT);
}
static void test_bloom_ack_surplus_receipts_are_no_evidence(void)
{
    track t=track_new(); send(&t,12,link_rr(E1,13,9,10,0),0); CHECK(check(&t,surplus(1000,12,false),1000)==NONE && t.p->ack.outstanding);
    one(hold(&t,2000,30000,false,surplus),30000,TIMEOUT);
}
static track spend(void)
{
    track t=track_new(); send(&t,12,lossy(0,11,true),0); resend_list r=hold(&t,0,120000,false,lossy);
    const uint64_t times[]={0,1000,3000,33000}; const fips_bloom_resend reasons[]={LOSS,LOSS,LOSS,TIMEOUT};
    CHECK(r.len==4 && !memcmp(r.times,times,sizeof(times)) && !memcmp(r.reasons,reasons,sizeof(reasons)));
    CHECK(FIPS_BLOOM_LOSS_BUDGET==3 && FIPS_BLOOM_UNVERIFIED_BUDGET==1); return t;
}
static void test_bloom_ack_budgets_bound_resends_per_lineage_per_session(void)
{
    track t=spend(); uint64_t c=t.counter; send_new(&t,c+1,lossy(121000,c,true),121000);
    CHECK(check(&t,lossy(122000,t.counter,false),122000)==LOSS);
    t=spend(); c=t.counter; fips_bloom_link_evidence ev=lossy(121000,c,true); ev.epoch=E2; send(&t,c+1,ev,121000);
    ev=lossy(122000,t.counter,false); ev.epoch=E2; CHECK(check(&t,ev,122000)==LOSS);
}
static void test_bloom_ack_resend_of_the_same_content_is_not_a_new_lineage(void)
{
    track t=track_new(); send(&t,12,lossy(0,11,true),0); resend_list r=hold(&t,0,7000,false,lossy); const uint64_t times[]={0,1000,3000};
    CHECK(r.len==3 && !memcmp(r.times,times,sizeof(times))); CHECK(check(&t,lossy(8000,t.counter,false),8000)==NONE);
}
static void test_bloom_ack_report_polluted_after_the_base_is_not_a_loss(void)
{
    track t=track_new(); send(&t,12,link_rr(E1,13,9,10,0),0); CHECK(check(&t,link_rr(E1,20,9000,16,0),1000)==NONE && t.p->ack.outstanding);
}
static void test_bloom_ack_in_window_reorder_on_a_complete_base_confirms(void)
{
    track t=track_new(); send(&t,12,link_rr(E1,13,9,10,0),0); CHECK(check(&t,link_rr(E1,15,14,15,1),1000)==NONE && !t.p->ack.outstanding && !t.p->pending);
}
static void test_bloom_ack_in_window_reorder_on_the_zero_base_confirms(void)
{
    track t=track_new(); send(&t,4,link_no(E1,5),0); CHECK(check(&t,link_rr(E1,5,4,5,2),1000)==NONE && !t.p->ack.outstanding && !t.p->pending);
}
static void test_bloom_ack_ambiguous_pair_in_a_later_session_waits_for_the_fallback(void)
{
    track t=track_new(); send(&t,3,link_no(E1,4),0); send(&t,12,link_rr(E2,13,9,510,0),0);
    CHECK(check(&t,ambiguous(1000,12,false),1000)==NONE && t.p->ack.outstanding); one(hold(&t,2000,120000,false,ambiguous),30000,TIMEOUT);
}
static void test_bloom_ack_later_session_base_is_never_complete(void)
{
    track t=track_new(); send(&t,3,link_no(E1,4),0); send(&t,12,link_rr(E2,13,9,10,0),0);
    CHECK(check(&t,later_hole(1000,12,false),1000)==NONE && t.p->ack.outstanding); one(hold(&t,2000,120000,false,later_hole),30000,TIMEOUT);
}

static void test_filter_announce_size_class(void)
{
    fips_bloom_filter f; fips_bloom_init(&f); fips_filter_announce a; fips_filter_announce_init(&a,&f,100);
    CHECK(a.size_class==1 && a.hash_count==5 && fips_filter_announce_is_valid(&a) && fips_filter_announce_is_v1_compliant(&a));
}
static void test_filter_announce_with_size_class(void)
{
    fips_bloom_filter f; OK(fips_bloom_with_params(&f,16384,7,NULL)); fips_filter_announce a; fips_filter_announce_init(&a,&f,100); a.size_class=2;
    CHECK(fips_filter_announce_is_valid(&a) && !fips_filter_announce_is_v1_compliant(&a) && a.filter.num_bits/8==2048);
}
static void test_filter_announce_encode_decode_roundtrip(void)
{
    fips_bloom_filter f=tagged(42); insert(&f,99); fips_filter_announce a,b; fips_filter_announce_init(&a,&f,500);
    uint8_t bytes[1035]; fips_writer w; fips_reader r; fips_writer_init(&w,bytes,sizeof(bytes)); OK(fips_filter_announce_encode(&a,&w,NULL));
    CHECK(w.pos==1035 && bytes[0]==0x20); fips_reader_init(&r,bytes+1,sizeof(bytes)-1); OK(fips_filter_announce_decode(&r,&b,NULL));
    CHECK(b.sequence==500 && b.hash_count==5 && b.size_class==1 && fips_filter_announce_is_valid(&b) && fips_filter_announce_is_v1_compliant(&b));
    HAS(b.filter,42); HAS(b.filter,99); CHECK(!contains(&b.filter,1)); EQ(f,b.filter);
}
static void test_filter_announce_decode_rejects_bad_size_class(void)
{
    uint8_t bytes[1034]={0}; bytes[8]=5; bytes[9]=5; fips_reader r; fips_filter_announce a; fips_proto_error e;
    fips_reader_init(&r,bytes,sizeof(bytes)); CHECK(fips_filter_announce_decode(&r,&a,&e)==FIPS_ERR_RANGE && e.variant==FIPS_PROTO_BadSizeClass && e.as.bad_size_class.got==5 && e.as.bad_size_class.max==3);
}
static void test_filter_announce_decode_rejects_non_v1_size_class(void)
{
    fips_bloom_filter f; OK(fips_bloom_with_params(&f,4096,5,NULL)); fips_filter_announce a,b; fips_filter_announce_init(&a,&f,100); a.size_class=0;
    uint8_t bytes[523]; fips_writer w; fips_reader r; fips_proto_error e; fips_writer_init(&w,bytes,sizeof(bytes)); OK(fips_filter_announce_encode(&a,&w,NULL));
    fips_reader_init(&r,bytes+1,sizeof(bytes)-1); CHECK(fips_filter_announce_decode(&r,&b,&e)==FIPS_ERR_RANGE && e.variant==FIPS_PROTO_BadSizeClass && e.as.bad_size_class.got==0 && e.as.bad_size_class.max==1);
}
static void test_filter_announce_decode_rejects_truncated(void)
{
    uint8_t bytes[5]={0}; fips_reader r; fips_filter_announce a; fips_reader_init(&r,bytes,5); CHECK(fips_filter_announce_decode(&r,&a,NULL)==FIPS_ERR_RANGE);
}
static void test_filter_announce_with_hash_count_other_than_five_is_not_v1_compliant(void)
{
    const uint8_t ks[]={1,4,6,255,5};
    for(size_t i=0;i<COUNT(ks);++i) { fips_bloom_filter f; OK(fips_bloom_with_params(&f,8192,ks[i],NULL)); fips_filter_announce a; fips_filter_announce_init(&a,&f,1);
        CHECK(fips_filter_announce_is_valid(&a) && fips_filter_announce_is_v1_compliant(&a)==(ks[i]==5)); }
}
static void codec_boundaries(void)
{
    uint8_t bytes[1044]; memset(bytes,0,sizeof(bytes)); bytes[8]=5; bytes[9]=1;
    fips_filter_announce sentinel; fips_bloom_filter f=tagged(7); fips_filter_announce_init(&sentinel,&f,42);
    fips_proto_error e;
    for(size_t n=0;n<sizeof(bytes);++n) {
        fips_reader r; fips_filter_announce a=sentinel; fips_reader_init(&r,bytes,n);
        if(n==1034) { OK(fips_filter_announce_decode(&r,&a,NULL)); CHECK(r.pos==n); continue; }
        CHECK(fips_filter_announce_decode(&r,&a,&e)==FIPS_ERR_RANGE && e.variant==FIPS_PROTO_MessageTooShort);
        CHECK(e.as.message_too_short.expected==(n<10?10:1034) && e.as.message_too_short.got==n && r.pos==0 && !memcmp(&a,&sentinel,sizeof(a)));
    }
    for(unsigned sc=0;sc<256;++sc) {
        bytes[9]=(uint8_t)sc; fips_reader r; fips_filter_announce a=sentinel; fips_reader_init(&r,bytes,1034);
        if(sc==1) { OK(fips_filter_announce_decode(&r,&a,NULL)); continue; }
        CHECK(fips_filter_announce_decode(&r,&a,&e)==FIPS_ERR_RANGE && e.variant==FIPS_PROTO_BadSizeClass && e.as.bad_size_class.got==sc && e.as.bad_size_class.max==(sc>3?3:1));
    }
    bytes[9]=1; bytes[8]=0; fips_reader r; fips_filter_announce a=sentinel; fips_reader_init(&r,bytes,1034);
    CHECK(fips_filter_announce_decode(&r,&a,&e)==FIPS_ERR_RANGE && e.variant==FIPS_PROTO_BadBloom && e.as.bad_bloom.variant==FIPS_BLOOM_ZeroHashCount && r.pos==0); bytes[8]=5;
    /* Shifted reader uses cumulative expected/got, including overlength. */
    memmove(bytes+3,bytes,1034); fips_reader_init(&r,bytes,1036); OK(fips_reader_advance(&r,3,NULL));
    CHECK(fips_filter_announce_decode(&r,&a,&e)==FIPS_ERR_RANGE && e.as.message_too_short.expected==1037 && e.as.message_too_short.got==1036 && r.pos==3);
    for(size_t cap=0;cap<1035;++cap) {
        fips_writer w; fips_writer_init(&w,bytes,cap); bytes[0]=0xcc;
        CHECK(fips_filter_announce_encode(&sentinel,&w,&e)==FIPS_ERR_RANGE && e.variant==FIPS_PROTO_MessageTooLong && e.as.message_too_long.max==cap && e.as.message_too_long.got==1035 && w.pos==0 && bytes[0]==0xcc);
    }
    fips_writer w; fips_writer_init(&w,bytes,1036); OK(fips_writer_write_u8(&w,1,NULL)); OK(fips_writer_write_u8(&w,2,NULL));
    CHECK(fips_filter_announce_encode(&sentinel,&w,&e)==FIPS_ERR_RANGE && w.pos==2 && e.as.message_too_long.got==1037);
    a=sentinel; a.hash_count=7; CHECK(fips_filter_announce_encode(&a,&w,&e)==FIPS_ERR_RANGE && e.variant==FIPS_PROTO_Malformed && w.pos==2);
    a=sentinel; a.size_class=255; CHECK(!fips_filter_announce_is_valid(&a));
    fips_bloom_filter before=f; CHECK(fips_bloom_with_params(&f,32776,5,&e)==FIPS_ERR_RANGE && e.variant==FIPS_PROTO_MessageTooLong); EQ(f,before);
    CHECK(fips_bloom_from_bytes(&f,bytes,SIZE_MAX,5,&e)==FIPS_ERR_RANGE && e.as.message_too_long.got==SIZE_MAX); EQ(f,before);
    fips_reader_init(&r,bytes,0); CHECK(fips_bloom_decode(&r,0,&f,&e)==FIPS_ERR_RANGE && e.as.bad_bloom.variant==FIPS_BLOOM_ZeroHashCount && r.pos==0); EQ(f,before);
    /* Every supported raw size, including the largest bounded filter. */
    uint8_t raw[FIPS_BLOOM_MAX_BYTES]; memset(raw,0xa5,sizeof(raw));
    OK(fips_bloom_from_bytes(&f,raw,sizeof(raw),255,NULL)); fips_writer_init(&w,raw,sizeof(raw)); OK(fips_bloom_encode(&f,&w,NULL));
    fips_reader_init(&r,raw,sizeof(raw)); OK(fips_bloom_decode(&r,255,&before,NULL)); EQ(f,before); CHECK(r.pos==sizeof(raw));
}
static void state_actions_and_refusals(void)
{
    reset(); fips_bloom_peer *a=peer(1),*b=peer(2),*mesh=peer(3);
    fips_bloom_state_peer_role(&s,a,true,true,true); fips_bloom_state_peer_role(&s,mesh,false,false,true);
    inbound(a,tagged(100)); inbound(b,tagged(200)); inbound(mesh,tagged(150));
    OK(fips_bloom_state_refresh(&s,0,100,&actions)); CHECK(actions.len==3 && s.inbound_recomputes==1);
    HAS(actions.items[0].announce.filter,200); CHECK(!contains(&actions.items[0].announce.filter,100));
    for(size_t i=0;i<3;++i) CHECK(!contains(&actions.items[i].announce.filter,150));
    fips_bloom_link_evidence ev=link_no(E1,1); fips_bloom_action send_action=actions.items[0]; actions.len=0;
    OK(fips_bloom_state_sent(&s,a,&send_action,(fips_bloom_send_outcome){FIPS_BLOOM_SEND_TOO_LARGE,1071,1000},0,&ev,0,&actions));
    CHECK(actions.len==1 && actions.items[0].refusal==FIPS_BLOOM_REFUSE_MTU && actions.items[0].warn && actions.items[0].packet_size==1071 && actions.items[0].mtu==1000 && a->pending && !a->ack.outstanding && !a->has_last_sent && a->sent_len==0);
    OK(fips_bloom_state_sent(&s,a,&send_action,(fips_bloom_send_outcome){FIPS_BLOOM_SEND_TOO_LARGE,1071,1000},0,&ev,1,&actions)); CHECK(!actions.items[1].warn);
    OK(fips_bloom_state_sent(&s,a,&send_action,(fips_bloom_send_outcome){FIPS_BLOOM_SEND_ACCEPTED,1071,1000},0,&ev,2,&actions)); CHECK(a->ack.outstanding && a->sent_len==1 && !a->pending && a->last_sent_ms==2);
    fips_bloom_filter f=tagged(99); fips_filter_announce ann; fips_filter_announce_init(&ann,&f,1);
    actions.len=0; OK(fips_bloom_state_receive(&s,a,&ann,100,&actions)); CHECK(a->inbound_sequence==1 && s.inbound_dirty);
    fips_bloom_filter kept=a->inbound; memset(ann.filter.bits,255,1024); ann.sequence=2;
    OK(fips_bloom_state_receive(&s,a,&ann,101,&actions)); CHECK(actions.len==1 && actions.items[0].refusal==FIPS_BLOOM_REFUSE_FPR && a->inbound_sequence==1); EQ(kept,a->inbound);
    ann.filter=f; ann.hash_count=7; OK(fips_bloom_state_receive(&s,a,&ann,102,&actions)); CHECK(actions.items[1].refusal==FIPS_BLOOM_REFUSE_PARAMETERS && a->inbound_sequence==1);
    /* Valid codec with non-v1 k is a receive policy refusal, not decode error. */
    OK(fips_bloom_with_params(&f,8192,7,NULL)); fips_filter_announce_init(&ann,&f,2);
    uint8_t encoded[1035]; fips_writer w; fips_reader r; fips_writer_init(&w,encoded,sizeof(encoded)); OK(fips_filter_announce_encode(&ann,&w,NULL));
    fips_reader_init(&r,encoded+1,1034); OK(fips_filter_announce_decode(&r,&ann,NULL)); OK(fips_bloom_state_receive(&s,a,&ann,102,&actions)); CHECK(actions.items[2].refusal==FIPS_BLOOM_REFUSE_PARAMETERS);
    fips_bloom_filter old=entries(0xa1,0,1000),newer=entries(0xb2,0,1000); fips_bloom_peer_record_filter(a,&old); fips_bloom_peer_record_filter(a,&newer);
    fips_filter_announce_init(&ann,&old,2); OK(fips_bloom_state_receive(&s,a,&ann,1000,&actions));
    CHECK(actions.items[3].refusal==FIPS_BLOOM_REFUSE_CHILD_ECHO && actions.items[3].warn && actions.items[3].overlap==1 && actions.items[3].bound<1);
    uint8_t digest[32]; SHA256(old.bits,1024,digest); CHECK(!memcmp(digest,actions.items[3].received_digest,32) && !memcmp(digest,actions.items[3].sent_digest,32));
    OK(fips_bloom_state_receive(&s,a,&ann,1001,&actions)); CHECK(!actions.items[4].warn && a->inbound_sequence==1); EQ(kept,a->inbound);
    /* Refusals count/report every time, but full action storage is atomic. */
    size_t saved=actions.capacity; actions.capacity=actions.len; uint64_t stamp=a->echo_warned_ms;
    CHECK(fips_bloom_state_receive(&s,a,&ann,301000,&actions)==FIPS_ERR_RANGE && a->echo_warned_ms==stamp && a->inbound_sequence==1);
    actions.capacity=saved;
    /* The first child filter is not echo-guarded; parent/mesh aren't either. */
    a->has_inbound=false; OK(fips_bloom_state_receive(&s,a,&ann,1002,&actions)); CHECK(a->has_inbound && a->inbound_sequence==2);
    ann.sequence=1; ann.filter=tagged(40); OK(fips_bloom_state_receive(&s,a,&ann,1003,&actions)); EQ(old,a->inbound); CHECK(a->inbound_sequence==2);
    fips_bloom_state_peer_role(&s,a,true,false,true); ann.sequence=3; ann.filter=old; OK(fips_bloom_state_receive(&s,a,&ann,1004,&actions)); CHECK(a->inbound_sequence==3);
    /* Restart forgets history but not link delivery, debounce, or MTU stamp. */
    fips_bloom_state_peer_restarted(&s,a); CHECK(a->sent_len==0 && a->ack.outstanding && a->has_last_sent && a->mtu_warned && a->pending);
    actions.len=0; OK(fips_bloom_state_refresh(&s,1005,1000,&actions)); CHECK(actions.len==3);
    uint64_t recomputes=s.inbound_recomputes; fips_bloom_state_note_inbound_change(&s); fips_bloom_state_note_inbound_change(&s);
    actions.len=0; OK(fips_bloom_state_refresh(&s,1005,1000,&actions)); CHECK(s.inbound_recomputes==recomputes+1);
    CHECK(!fips_bloom_state_take_inbound_change(&s));
}
static void state_bounds_and_refresh_triggers(void)
{
    reset(); fips_bloom_peer *p=peer(1); record_base(p); fips_bloom_peer_record_update(p,0);
    s.inbound_dirty=false; p->pending=true; actions.capacity=0;
    CHECK(fips_bloom_state_refresh(&s,1000,1,&actions)==FIPS_ERR_RANGE && !s.inbound_dirty && s.sequence==0);
    actions.capacity=16; fips_bloom_state_peer_role(&s,p,true,false,false); p->pending=true;
    OK(fips_bloom_state_refresh(&s,1000,1,&actions)); CHECK(actions.len==0); fips_bloom_state_peer_role(&s,p,true,false,true);
    OK(fips_bloom_state_refresh(&s,1000,1,&actions)); CHECK(actions.len==1); actions.len=0; p->pending=false;
    leaf(42); OK(fips_bloom_state_refresh(&s,1000,1,&actions)); CHECK(actions.len==1); HAS(actions.items[0].announce.filter,42);
    fips_node_addr id=addr(42); CHECK(fips_bloom_state_remove_leaf(&s,&id)); actions.len=0;
    fips_bloom_filter f=tagged(42); fips_bloom_peer_record_filter(p,&f); p->pending=false;
    OK(fips_bloom_state_refresh(&s,2000,1,&actions)); CHECK(actions.len==1 && !contains(&actions.items[0].announce.filter,42));
    reset(); s.peer_capacity=1; p=peer(1); fips_node_addr copy=p->addr; CHECK(fips_bloom_state_add_peer(&s,addr(2))==FIPS_ERR_RANGE && s.peer_count==1 && !memcmp(p->addr.bytes,copy.bytes,16));
    OK(fips_bloom_state_add_peer(&s,addr(1))); CHECK(s.peer_count==1); s.leaf_capacity=1; leaf(10); leaf(10); CHECK(s.leaf_count==1 && fips_bloom_state_add_leaf(&s,addr(11))==FIPS_ERR_RANGE);
    /* No timer-based refresh: after successful unchanged sends, no SEND actions. */
    actions.len=0; OK(fips_bloom_state_refresh(&s,0,1,&actions)); CHECK(actions.len==1); fips_bloom_link_evidence ev=link_no(E1,1);
    OK(fips_bloom_state_sent(&s,p,&actions.items[0],(fips_bloom_send_outcome){FIPS_BLOOM_SEND_ACCEPTED,1071,1000},0,&ev,0,&actions)); actions.len=0;
    OK(fips_bloom_state_refresh(&s,10000000,2,&actions)); CHECK(actions.len==0);
}

static void retry_and_action_capacity_coalescing(void)
{
    track t=track_new(); s.update_debounce_ms=10000;
    send(&t,12,link_rr(E1,13,9,10,0),0);
    fips_bloom_peer_record_update(t.p,0);
    CHECK(check(&t,link_rr(E1,15,14,14,0),100)==LOSS);
    CHECK(t.p->ack.loss_left==2);
    for(uint64_t now=1100;now<=9100;now+=1000) CHECK(check(&t,link_rr(E1,15,14,14,0),now)==NONE);
    CHECK(t.p->ack.loss_left==2 && t.p->ack.level==1);
    actions.len=0; actions.capacity=0;
    s.inbound_dirty=false;
    OK(fips_bloom_state_refresh(&s,9999,1,&actions)); CHECK(actions.len==0);
    actions.capacity=1;
    (void)peer(2); fips_bloom_peer *other=&s.peers[1];
    record_base(other); fips_bloom_peer_record_update(other,10000);
    s.inbound_dirty=false;
    OK(fips_bloom_state_refresh(&s,10000,1,&actions));
    CHECK(actions.len==1 && actions.items[0].resend==LOSS && actions.items[0].peer.bytes[0]==1);
    /* Confirmation is still processed while a retry is pending. */
    CHECK(check(&t,link_rr(E1,15,14,15,0),10001)==NONE && !t.p->ack.outstanding);
    reset(); fips_bloom_peer *a=peer(1); record_base(a); fips_bloom_peer_record_update(a,0);
    leaf(42); actions.capacity=0;
    CHECK(fips_bloom_state_refresh(&s,1000,1,&actions)==FIPS_ERR_RANGE && s.inbound_dirty && !a->pending && s.inbound_recomputes==0);
    actions.capacity=1; OK(fips_bloom_state_refresh(&s,1000,1,&actions));
    CHECK(actions.len==1 && !s.inbound_dirty && a->pending && s.inbound_recomputes==1);
}

int main(void)
{
    test_bloom_filter_new();
    test_bloom_filter_insert_contains();
    test_bloom_filter_multiple_inserts();
    test_bloom_filter_merge();
    test_bloom_filter_union();
    test_bloom_filter_clear();
    test_bloom_filter_merge_size_mismatch();
    test_bloom_filter_custom_params();
    test_bloom_filter_invalid_params();
    test_bloom_filter_from_bytes();
    test_bloom_filter_estimated_count();
    test_bloom_filter_equality();
    test_bloom_filter_from_bytes_empty();
    test_bloom_filter_from_bytes_zero_hash_count();
    test_bloom_filter_from_slice();
    test_bloom_filter_insert_bytes_contains_bytes();
    test_bloom_filter_estimated_count_saturated();
    test_bloom_filter_estimated_count_fpr_cap_boundary();
    test_bloom_filter_default();
    test_bloom_filter_debug_format();
    test_bloom_filter_bit_indices_match_double_hashing_formula();
    test_compute_outgoing_filters_matches_per_peer();
    test_compute_outgoing_filters_empty_inputs();
    overlap_is_one_against_itself_and_against_any_subset_of_it();
    overlap_is_zero_for_filters_with_no_bit_in_common();
    overlap_is_undefined_against_an_empty_filter_or_across_sizes();
    a_filter_returning_what_was_sent_plus_one_entry_echoes_it();
    an_unrelated_200_entry_filter_does_not_echo_a_1000_entry_one();
    a_subset_holding_half_of_what_was_sent_echoes_only_at_a_low_threshold();
    an_exact_return_of_a_filter_below_the_bit_floor_does_not_echo();
    a_threshold_of_one_or_nan_never_echoes_even_an_exact_return();
    echo_is_false_across_sizes_against_an_empty_filter_and_for_a_saturated_one();
    test_bloom_state_new();
    test_bloom_state_leaf_only();
    test_bloom_state_leaf_dependents();
    test_bloom_state_debounce();
    should_send_update_after_backward_clock_step_waits_at_most_the_debounce();
    test_bloom_state_sequence();
    test_bloom_state_next_sequence_for_a_new_peer_starts_at_the_floor_whatever_was_sent_to_others();
    test_bloom_state_sequence_to_a_peer_that_returns_exceeds_every_value_sent_before_it_left();
    test_bloom_state_pending_updates();
    test_bloom_state_base_filter();
    test_bloom_state_compute_outgoing_filter();
    test_bloom_state_leaf_dependents_accessor();
    test_bloom_state_record_sent_filter();
    test_bloom_state_remove_peer_state();
    test_bloom_state_mark_changed_peers_never_sent();
    test_bloom_state_mark_changed_peers_unchanged();
    test_bloom_state_mark_changed_peers_one_changed();
    test_bloom_state_mark_changed_peers_excludes_source();
    test_bloom_ack_covering_report_with_a_missing_frame_resends_on_loss();
    test_bloom_ack_covering_report_with_every_frame_confirms();
    test_bloom_ack_reordered_frame_does_not_mask_a_lost_announce();
    test_bloom_ack_report_below_the_announce_waits_for_a_covering_one();
    test_bloom_ack_announce_from_an_earlier_session_is_resent_once();
    test_bloom_ack_first_session_measures_from_counter_zero();
    test_bloom_ack_later_session_without_a_report_has_no_base();
    test_bloom_ack_report_from_another_session_at_send_gets_one_timeout();
    test_bloom_ack_no_report_ever_resends_once_per_session();
    test_bloom_ack_sustained_trigger_backs_off_to_one_resend_a_minute();
    test_bloom_ack_backoff_resets_only_after_the_quiet_period();
    test_bloom_ack_regressed_report_is_no_evidence();
    test_bloom_ack_removed_peer_starts_fresh();
    test_bloom_ack_confirmation_keeps_the_backoff();
    test_bloom_ack_frozen_report_after_a_rekey_spends_only_the_unchecked_budget();
    test_bloom_ack_polluted_report_after_a_rekey_spends_only_the_unchecked_budget();
    test_bloom_ack_surplus_receipts_are_no_evidence();
    test_bloom_ack_budgets_bound_resends_per_lineage_per_session();
    test_bloom_ack_resend_of_the_same_content_is_not_a_new_lineage();
    test_bloom_ack_report_polluted_after_the_base_is_not_a_loss();
    test_bloom_ack_in_window_reorder_on_a_complete_base_confirms();
    test_bloom_ack_in_window_reorder_on_the_zero_base_confirms();
    test_bloom_ack_ambiguous_pair_in_a_later_session_waits_for_the_fallback();
    test_bloom_ack_later_session_base_is_never_complete();
    the_sent_history_keeps_the_last_four_distinct_filters_newest_first();
    resending_a_kept_filter_moves_it_to_the_newest_place_without_dropping_another();
    the_sent_history_is_kept_per_peer();
    forgetting_a_restarted_peer_clears_its_whole_sent_history();
    removing_a_peer_clears_its_whole_sent_history();
    change_detection_compares_with_the_newest_sent_filter_only();
    only_the_first_echo_refusal_from_a_peer_in_each_interval_is_a_warning();
    the_first_echo_refusal_from_a_node_in_its_first_millisecond_is_a_warning();
    echo_refusal_warnings_are_spaced_per_peer();
    an_echo_refusal_read_before_the_last_warning_warns_and_restamps();
    removing_a_peer_clears_its_echo_warning_stamp();
    the_mtu_warning_is_given_once_per_peering_and_again_after_the_peer_is_removed();
    test_filter_announce_size_class();
    test_filter_announce_with_size_class();
    test_filter_announce_encode_decode_roundtrip();
    test_filter_announce_decode_rejects_bad_size_class();
    test_filter_announce_decode_rejects_non_v1_size_class();
    test_filter_announce_decode_rejects_truncated();
    test_filter_announce_with_hash_count_other_than_five_is_not_v1_compliant();
    pinned_estimates();
    codec_boundaries();
    state_actions_and_refusals();
    state_bounds_and_refresh_triggers();
    retry_and_action_capacity_coalescing();
    puts("bloom: 93 upstream behavioral ports + 5 C boundary/action/pin groups passed");
    return 0;
}
