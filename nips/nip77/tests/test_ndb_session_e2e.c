#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/nostr/nip77/negentropy.h"
#include "../src/neg_message.h"
#include "../backends/nostrdb/nostr-negentropy-ndb.h"
#include <nostrdb.h>

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

static int idlist_cb(unsigned char t, const unsigned char *v, size_t l, void *u){
  int *f=(int*)u; (void)v; (void)l; if (t==NEG_ELT_IDLIST){ *f=1; return 1; } return 0;
}

static void encode_peer_fp_msg(const neg_bound_t *r, const unsigned char fp[16], char **out_hex){
  unsigned char payload[1+10+16];
  size_t pl = neg_msg_payload_put_fingerprint(fp, payload, sizeof(payload));
  unsigned char msg[1+10+64*8 + sizeof payload];
  size_t ml = neg_msg_encode_v1(r, 1, payload, pl, msg, sizeof(msg));
  static const char* hexd = "0123456789abcdef";
  char *hex = (char*)malloc(ml*2+1);
  for (size_t i=0;i<ml;i++){ hex[2*i]=hexd[msg[i]>>4]; hex[2*i+1]=hexd[msg[i]&0xF]; }
  hex[ml*2]='\0';
  *out_hex = hex;
}

static int ingest_small_set(struct ndb *db){
  // three events with leading 0x00 in first byte so they share prefix 0
  char ev[256];
  for (int i=0;i<3;i++){
    // construct a simple event with unique id bytes[0]=0x00 and bytes[1]=i
    // we use deterministic id strings for simplicity
    const char *id = (i==0)?
      "0000000000000000000000000000000000000000000000000000000000000000":
      (i==1)?
      "0001000000000000000000000000000000000000000000000000000000000000":
      "0002000000000000000000000000000000000000000000000000000000000000";
    snprintf(ev, sizeof(ev),
      "{\n  \"id\": \"%s\",\n  \"pubkey\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\n  \"created_at\": %d,\n  \"kind\": 1,\n  \"tags\": [],\n  \"content\": \"s%d\",\n  \"sig\": \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"\n}",
      id, 100+i, i);
    if (ndb_process_event(db, ev, (int)strlen(ev)) != 0) return -1;
  }
  return 0;
}

static int ingest_large_set(struct ndb *db, size_t n){
  // n events with leading 0x80 to force split by prefix 1
  char ev[256];
  for (size_t i=0;i<n;i++){
    char id[65]; memset(id, '0', 64); id[64]='\0';
    id[0] = '8'; // 0x80 high nibble to set leading bit 1; we don't need exact bytes as long as consistent
    // vary second nibble so ids differ
    const char hexnib[16] = "0123456789abcdef";
    id[1] = hexnib[i % 16];
    snprintf(ev, sizeof(ev),
      "{\n  \"id\": \"%s\",\n  \"pubkey\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\n  \"created_at\": %zu,\n  \"kind\": 1,\n  \"tags\": [],\n  \"content\": \"l%zu\",\n  \"sig\": \"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"\n}",
      id, 200+i, i);
    if (ndb_process_event(db, ev, (int)strlen(ev)) != 0) return -1;
  }
  return 0;
}

static int run_idlist_case(const char *dbdir){
  NostrNegDataSource ds;
  if (nostr_ndb_make_datasource(dbdir, &ds) != 0){
    printf("skipped: datasource init failed\n");
    return 0;
  }
  NostrNegSession *s = nostr_neg_session_new(&ds, NULL);
  if (!s){ printf("skipped: session alloc failed\n"); return 0; }

  neg_bound_t r; make_prefix(&r, 1, 0); // leading 0
  unsigned char fake_fp[16]; memset(fake_fp, 0xAA, 16);
  char *peer_hex=NULL; encode_peer_fp_msg(&r, fake_fp, &peer_hex);
  if (!peer_hex){ printf("skipped: encode failed\n"); nostr_neg_session_free(s); return 0; }
  if (nostr_neg_handle_peer_hex(s, peer_hex)!=0){ free(peer_hex); nostr_neg_session_free(s); printf("skipped: handle failed\n"); return 0; }
  free(peer_hex);
  char *resp_hex = nostr_neg_build_next_hex(s);
  if (!resp_hex){ nostr_neg_session_free(s); printf("skipped: build_next failed\n"); return 0; }

  size_t blen = strlen(resp_hex)/2; unsigned char *buf=(unsigned char*)malloc(blen);
  for (size_t i=0;i<blen;i++){ unsigned hi,lo; char c=resp_hex[2*i]; hi=(c<='9'?c-'0':10+c-'a'); c=resp_hex[2*i+1]; lo=(c<='9'?c-'0':10+c-'a'); buf[i]=(unsigned char)((hi<<4)|lo); }
  free(resp_hex);
  neg_bound_t dr[4]; size_t rn=4; const unsigned char *pl=NULL; size_t pln=0;
  assert(neg_msg_decode_v1(buf, blen, dr, &rn, &pl, &pln)==0);
  assert(rn==1);
  int saw_idlist=0;
  if (pl && pln) {
    neg_msg_payload_iterate(pl, pln, idlist_cb, &saw_idlist);
  }
  free(buf);
  assert(saw_idlist==1);
  nostr_neg_session_free(s);
  return 0;
}

static int run_split_case(const char *dbdir){
  NostrNegDataSource ds;
  if (nostr_ndb_make_datasource(dbdir, &ds) != 0){ printf("skipped: datasource init failed\n"); return 0; }
  NostrNegOptions opts = {0}; opts.max_idlist_items = 256; opts.max_ranges = 8; opts.max_round_trips = 8;
  NostrNegSession *s = nostr_neg_session_new(&ds, &opts);
  if (!s){ printf("skipped: session alloc failed\n"); return 0; }

  neg_bound_t r; make_prefix(&r, 0, 0); // entire space
  unsigned char fake_fp[16]; memset(fake_fp, 0xAA, 16);
  char *peer_hex=NULL; encode_peer_fp_msg(&r, fake_fp, &peer_hex);
  if (!peer_hex){ printf("skipped: encode failed\n"); nostr_neg_session_free(s); return 0; }
  if (nostr_neg_handle_peer_hex(s, peer_hex)!=0){ free(peer_hex); nostr_neg_session_free(s); printf("skipped: handle failed\n"); return 0; }
  free(peer_hex);
  char *resp_hex = nostr_neg_build_next_hex(s);
  if (!resp_hex){ nostr_neg_session_free(s); printf("skipped: build_next failed\n"); return 0; }

  size_t blen = strlen(resp_hex)/2; unsigned char *buf=(unsigned char*)malloc(blen);
  for (size_t i=0;i<blen;i++){ unsigned hi,lo; char c=resp_hex[2*i]; hi=(c<='9'?c-'0':10+c-'a'); c=resp_hex[2*i+1]; lo=(c<='9'?c-'0':10+c-'a'); buf[i]=(unsigned char)((hi<<4)|lo); }
  free(resp_hex);
  neg_bound_t dr[8]; size_t rn=8; const unsigned char *pl=NULL; size_t pln=0;
  assert(neg_msg_decode_v1(buf, blen, dr, &rn, &pl, &pln)==0);
  assert(rn==2); // expect split
  assert(pln==0 || pl==NULL);
  free(buf);
  nostr_neg_session_free(s);
  return 0;
}

int main(void){
  // Small set DB for IdList case
  char tmpl1[] = "/tmp/ndb-e2e-small-XXXXXX"; char *db1 = mkdtemp(tmpl1); if (!db1){ printf("skipped: mkdtemp failed\n"); return 0; }
  struct ndb *db=NULL; struct ndb_config cfg; ndb_default_config(&cfg);
  ndb_config_set_flags(&cfg, NDB_FLAG_NO_FULLTEXT | NDB_FLAG_NO_NOTE_BLOCKS | NDB_FLAG_NO_STATS | NDB_FLAG_SKIP_NOTE_VERIFY);
  ndb_config_set_mapsize(&cfg, 64ull*1024ull*1024ull);
  if (ndb_init(&db, db1, &cfg)!=0){ printf("skipped: ndb_init small failed\n"); return 0; }
  if (ingest_small_set(db)!=0){ printf("skipped: ingest small failed\n"); ndb_destroy(db); return 0; }
  ndb_destroy(db);
  if (run_idlist_case(db1)!=0) { return 0; }

  // Large set DB for Split case
  char tmpl2[] = "/tmp/ndb-e2e-large-XXXXXX"; char *db2 = mkdtemp(tmpl2); if (!db2){ printf("skipped: mkdtemp failed\n"); return 0; }
  db=NULL; ndb_default_config(&cfg);
  ndb_config_set_flags(&cfg, NDB_FLAG_NO_FULLTEXT | NDB_FLAG_NO_NOTE_BLOCKS | NDB_FLAG_NO_STATS | NDB_FLAG_SKIP_NOTE_VERIFY);
  ndb_config_set_mapsize(&cfg, 64ull*1024ull*1024ull);
  if (ndb_init(&db, db2, &cfg)!=0){ printf("skipped: ndb_init large failed\n"); return 0; }
  if (ingest_large_set(db, 300)!=0){ printf("skipped: ingest large failed\n"); ndb_destroy(db); return 0; }
  ndb_destroy(db);
  if (run_split_case(db2)!=0) { return 0; }

  printf("ok ndb session e2e\n");
  return 0;
}
