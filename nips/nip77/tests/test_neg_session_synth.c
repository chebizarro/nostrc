#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../include/nostr/nip77/negentropy.h"
#include "../src/neg_message.h"

/* Synthetic datasource yielding a fixed set of IDs */
typedef struct {
  const unsigned char *ids; /* flat array of 32*count */
  size_t count;
  size_t idx;
} synth_ds_t;

static int s_begin(void *ctx){ synth_ds_t *s=(synth_ds_t*)ctx; s->idx=0; return 0; }
static int s_next(void *ctx, NostrIndexItem *out){
  synth_ds_t *s=(synth_ds_t*)ctx;
  if (s->idx >= s->count) return 1; /* end */
  out->created_at = 0;
  memcpy(out->id.bytes, s->ids + s->idx*32, 32);
  s->idx++;
  return 0;
}
static void s_end(void *ctx){ (void)ctx; }

static void make_prefix(neg_bound_t *b, unsigned preflen_bits, unsigned char leading_bit_val){
  memset(b, 0, sizeof(*b));
  b->ts_delta = 0; /* infinity */
  b->id_prefix_len = (unsigned char)preflen_bits;
  if (preflen_bits>0){
    unsigned byte = (preflen_bits-1)/8;
    unsigned bit = (preflen_bits-1)%8;
    unsigned char mask = (unsigned char)(1u << (7-bit));
    if (leading_bit_val)
      b->id_prefix[byte] |= mask;
    else
      b->id_prefix[byte] &= (unsigned char)~mask;
  }
}

static void encode_peer_fp_msg(const neg_bound_t *r, const unsigned char fp[16], char **out_hex){
  unsigned char payload[1+10+16];
  size_t pl = neg_msg_payload_put_fingerprint(fp, payload, sizeof(payload));
  unsigned char msg[1+10+64*8 + sizeof payload];
  size_t ml = neg_msg_encode_v1(r, 1, payload, pl, msg, sizeof(msg));
  /* local hex helper copy: we only need simple bin->hex */
  static const char* hexd = "0123456789abcdef";
  char *hex = (char*)malloc(ml*2+1);
  for (size_t i=0;i<ml;i++){ hex[2*i]=hexd[msg[i]>>4]; hex[2*i+1]=hexd[msg[i]&0xF]; }
  hex[ml*2]='\0';
  *out_hex = hex;
}

/* TLV iterator callback to detect IdList */
static int saw_idlist_cb(unsigned char t, const unsigned char *val, size_t vlen, void *u){
  int *flag = (int*)u; (void)val; (void)vlen; if (t==NEG_ELT_IDLIST) { *flag = 1; return 1; } return 0;
}

/* simple fingerprint utility via session initial message over subset */
static void compute_fp_for_range(NostrNegSession *s, const neg_bound_t *r, unsigned char out16[16]){
  /* Build a one-off message locally by enumerating through neg_session helpers indirectly: replicate logic */
  /* We can't call private helpers, so approximate: we send a peer msg with empty payload and then call handle? Not needed. */
  /* We'll re-encode IdList from datasource filtered by prefix and then reuse neg_fingerprint_compute via message helpers is not public. */
  /* Workaround: rebuild ids array here by iterating the datasource available in session through API: but we don't have accessor. */
  /* Instead, for this test we craft fingerprints manually: for Skip case we compute the same locally using our own simple sum+SHA256 is not exposed. */
  /* To avoid dependency on private symbol, we just trigger Skip by sending an empty payload; handler without fingerprint will treat as mismatch and split or idlist. */
  /* So we'll not compute actual peer fingerprint except for constructing mismatch (we pass zero fp). */
  (void)s; (void)r; memset(out16, 0, 16);
}

int main(void){
  /* Build three categories of ids */
  unsigned char ids_small[3*32];
  memset(ids_small, 0x00, sizeof(ids_small));
  /* ensure they share prefix 0 bit=0 */
  ids_small[0*32+0] = 0x00;
  ids_small[1*32+0] = 0x00;
  ids_small[2*32+0] = 0x00;

  /* Large set exceeding default max_idlist_items=256 -> make 300 */
  size_t large_n = 300;
  unsigned char *ids_large = (unsigned char*)malloc(large_n*32);
  for (size_t i=0;i<large_n;i++){
    memset(ids_large + i*32, 0, 32);
    ids_large[i*32+0] = 0x80; /* leading bit 1 to force prefix split */
    ids_large[i*32+1] = (unsigned char)i;
  }

  /* Case 1: Skip path — we'll cheat by making peer fingerprint equal to local by assuming zero fp and empty set; but our handler requires fingerprint match to Skip.
   * Instead, we will test IdList and Split which are most critical for orchestration. */

  /* Case 2: IdList path: small set mismatch -> expect IdList in response */
  synth_ds_t ds_small = { .ids = ids_small, .count = 3, .idx = 0 };
  NostrNegDataSource ds = { .ctx=&ds_small, .begin_iter=s_begin, .next=s_next, .end_iter=s_end };
  NostrNegSession *s = nostr_neg_session_new(&ds, NULL);
  assert(s);

  neg_bound_t r_small; make_prefix(&r_small, 1, 0); /* leading 0 */
  unsigned char fake_fp[16]; memset(fake_fp, 0xAA, 16); /* different than local */
  char *peer_hex=NULL; encode_peer_fp_msg(&r_small, fake_fp, &peer_hex);
  assert(peer_hex);
  assert(nostr_neg_handle_peer_hex(s, peer_hex)==0);
  free(peer_hex);
  char *resp_hex = nostr_neg_build_next_hex(s);
  assert(resp_hex && strlen(resp_hex)>0);
  /* decode response and check IdList TLV exists */
  size_t blen = strlen(resp_hex)/2; unsigned char *buf=(unsigned char*)malloc(blen);
  for (size_t i=0;i<blen;i++){ unsigned hi,lo; char c=resp_hex[2*i]; hi=(c<='9'?c-'0':10+c-'a'); c=resp_hex[2*i+1]; lo=(c<='9'?c-'0':10+c-'a'); buf[i]=(unsigned char)((hi<<4)|lo); }
  free(resp_hex);
  neg_bound_t dr[4]; size_t rn=4; const unsigned char *pl=NULL; size_t pln=0;
  assert(neg_msg_decode_v1(buf, blen, dr, &rn, &pl, &pln)==0);
  assert(rn==1);
  int saw_idlist=0;
  if (pl && pln) neg_msg_payload_iterate(pl, pln, saw_idlist_cb, &saw_idlist);
  free(buf);
  assert(saw_idlist==1);
  nostr_neg_session_free(s);

  /* Case 3: Split path: large set mismatch -> expect two ranges, no payload */
  synth_ds_t ds_big = { .ids = ids_large, .count = large_n, .idx = 0 };
  NostrNegDataSource ds2 = { .ctx=&ds_big, .begin_iter=s_begin, .next=s_next, .end_iter=s_end };
  NostrNegOptions opts = {0}; opts.max_idlist_items = 256; opts.max_ranges = 8; opts.max_round_trips = 8;
  NostrNegSession *s2 = nostr_neg_session_new(&ds2, &opts);
  assert(s2);
  neg_bound_t r_big; make_prefix(&r_big, 0, 0);
  char *peer_hex2=NULL; encode_peer_fp_msg(&r_big, fake_fp, &peer_hex2);
  assert(peer_hex2);
  assert(nostr_neg_handle_peer_hex(s2, peer_hex2)==0);
  free(peer_hex2);
  char *resp_hex2 = nostr_neg_build_next_hex(s2);
  assert(resp_hex2 && strlen(resp_hex2)>0);
  size_t blen2 = strlen(resp_hex2)/2; unsigned char *buf2=(unsigned char*)malloc(blen2);
  for (size_t i=0;i<blen2;i++){ unsigned hi,lo; char c=resp_hex2[2*i]; hi=(c<='9'?c-'0':10+c-'a'); c=resp_hex2[2*i+1]; lo=(c<='9'?c-'0':10+c-'a'); buf2[i]=(unsigned char)((hi<<4)|lo); }
  free(resp_hex2);
  const unsigned char *pl2=NULL; size_t pln2=0; rn=4;
  assert(neg_msg_decode_v1(buf2, blen2, dr, &rn, &pl2, &pln2)==0);
  assert(rn==2);
  assert(pln2==0 || pl2==NULL);
  free(buf2);
  nostr_neg_session_free(s2);

  free(ids_large);
  printf("ok synth\n");
  return 0;
}
