#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../include/nostr/nip77/negentropy.h"
#include "../src/neg_message.h"

/* Synthetic datasource with many IDs to force split (avoid IdList path) */
typedef struct { const unsigned char *ids; size_t count; size_t idx; } caps_ds_t;
static int caps_begin(void *ctx){ ((caps_ds_t*)ctx)->idx=0; return 0; }
static int caps_next(void *ctx, NostrIndexItem *out){ caps_ds_t *s=(caps_ds_t*)ctx; if(s->idx>=s->count) return 1; out->created_at=0; memcpy(out->id.bytes, s->ids + s->idx*32, 32); s->idx++; return 0; }
static void caps_end(void *ctx){ (void)ctx; }

static void encode_peer_fp_msg_multi(const neg_bound_t *ranges, size_t rn, const unsigned char fp[16], char **out_hex){
  unsigned char payload[1+10+16];
  size_t pl = neg_msg_payload_put_fingerprint(fp, payload, sizeof(payload));
  unsigned char msg[1+10+64*8 + sizeof payload];
  size_t ml = neg_msg_encode_v1(ranges, rn, payload, pl, msg, sizeof(msg));
  static const char *hexd = "0123456789abcdef";
  char *hex = (char*)malloc(ml*2+1);
  for (size_t i=0;i<ml;i++){ hex[2*i]=hexd[msg[i]>>4]; hex[2*i+1]=hexd[msg[i]&0xF]; }
  hex[ml*2]='\0';
  *out_hex = hex;
}

static void hex_to_bin(const char *hex, unsigned char *out){ size_t n=strlen(hex)/2; for(size_t i=0;i<n;i++){ unsigned hi=(hex[2*i]<='9'?hex[2*i]-'0':10+hex[2*i]-'a'); unsigned lo=(hex[2*i+1]<='9'?hex[2*i+1]-'0':10+hex[2*i+1]-'a'); out[i]=(unsigned char)((hi<<4)|lo);} }

int main(void){
  /* Build large set of IDs to avoid IdList path */
  size_t N=900; unsigned char *ids=(unsigned char*)malloc(N*32);
  for(size_t i=0;i<N;i++){ memset(ids+i*32, 0, 32); ids[i*32+0]=(unsigned char)(i & 0xFF); ids[i*32+1]=(unsigned char)((i>>8)&0xFF); }
  caps_ds_t cds = { .ids = ids, .count = N, .idx = 0 };
  NostrNegDataSource ds = { .ctx=&cds, .begin_iter=caps_begin, .next=caps_next, .end_iter=caps_end };
  NostrNegOptions opts = {0}; opts.max_ranges=2; opts.max_idlist_items=256; opts.max_round_trips=8;
  NostrNegSession *s = nostr_neg_session_new(&ds, &opts);
  assert(s);

  /* Peer sends 3 identical ranges to force 6 children while cap=2 -> queue should hold 4 */
  neg_bound_t in[3]; for(int i=0;i<3;i++){ memset(&in[i],0,sizeof(in[i])); in[i].id_prefix_len=0; in[i].ts_delta=0; }
  unsigned char fake_fp[16]; memset(fake_fp, 0x55, sizeof(fake_fp));
  char *peer_hex=NULL; encode_peer_fp_msg_multi(in, 3, fake_fp, &peer_hex); assert(peer_hex);
  assert(nostr_neg_handle_peer_hex(s, peer_hex)==0);
  free(peer_hex);

  for(int round=0; round<3; ++round){
    char *out_hex = nostr_neg_build_next_hex(s);
    assert(out_hex && strlen(out_hex)>0);
    size_t blen=strlen(out_hex)/2; unsigned char *buf=(unsigned char*)malloc(blen);
    hex_to_bin(out_hex, buf); free(out_hex);
    neg_bound_t rs[8]; size_t rn=8; const unsigned char *pl=NULL; size_t pln=0;
    assert(neg_msg_decode_v1(buf, blen, rs, &rn, &pl, &pln)==0);
    free(buf);
    assert(rn==2); /* cap enforcement */
    assert(pln==0 || pl==NULL);
  }
  /* After three responses, queue should be empty; next build should be empty string */
  char *done = nostr_neg_build_next_hex(s);
  assert(done && strlen(done)==0);
  free(done);

  nostr_neg_session_free(s);
  free(ids);
  printf("ok caps\n");
  return 0;
}
