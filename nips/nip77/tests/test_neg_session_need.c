/**
 * Test NEED ID extraction from negentropy session.
 *
 * Verifies that when a peer sends an IdList containing IDs not in our
 * local set, those IDs are accumulated in the session's need_ids array.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../include/nostr/nip77/negentropy.h"
#include "../src/neg_message.h"

/* Synthetic datasource with known IDs */
typedef struct {
  const unsigned char *ids;
  size_t count;
  size_t idx;
} ds_t;

static int ds_begin(void *ctx) { ((ds_t *)ctx)->idx = 0; return 0; }
static int ds_next(void *ctx, NostrIndexItem *out) {
  ds_t *d = (ds_t *)ctx;
  if (d->idx >= d->count) return -1;
  out->created_at = 1000 + (uint64_t)d->idx;
  memcpy(out->id.bytes, d->ids + d->idx * 32, 32);
  d->idx++;
  return 0;
}
static void ds_end(void *ctx) { (void)ctx; }

/* local hex helpers */
static char tohex(unsigned v) { return (char)(v < 10 ? ('0' + v) : ('a' + (v - 10))); }
static char *bin2hex(const unsigned char *in, size_t len) {
  char *out = (char *)malloc(len * 2 + 1);
  if (!out) return NULL;
  for (size_t i = 0; i < len; ++i) {
    out[2 * i] = tohex(in[i] >> 4);
    out[2 * i + 1] = tohex(in[i] & 0x0F);
  }
  out[len * 2] = '\0';
  return out;
}

int main(void) {
  /* Local set: 2 IDs (A=0x01, B=0x02) */
  unsigned char local_ids[2 * 32];
  memset(local_ids, 0, sizeof(local_ids));
  local_ids[0] = 0x01;   /* ID A */
  local_ids[32] = 0x02;  /* ID B */

  ds_t ds_ctx = { .ids = local_ids, .count = 2, .idx = 0 };
  NostrNegDataSource ds = {
    .ctx = &ds_ctx,
    .begin_iter = ds_begin,
    .next = ds_next,
    .end_iter = ds_end
  };

  NostrNegSession *s = nostr_neg_session_new(&ds, NULL);
  assert(s);

  /* Build a peer message containing an IdList with 3 IDs:
   * ID A (we have), ID C (we don't), ID D (we don't) */
  unsigned char peer_ids[3 * 32];
  memset(peer_ids, 0, sizeof(peer_ids));
  peer_ids[0] = 0x01;     /* ID A - we have this */
  peer_ids[32] = 0x03;    /* ID C - we DON'T have this */
  peer_ids[64] = 0x04;    /* ID D - we DON'T have this */

  /* Encode as negentropy message with IdList payload */
  neg_bound_t range = { .ts_delta = 0, .id_prefix_len = 0 };
  unsigned char payload[512];
  size_t plen = neg_msg_payload_put_idlist(peer_ids, 32, 3, payload, sizeof(payload));
  assert(plen > 0);

  unsigned char msg[512];
  size_t mlen = neg_msg_encode_v1(&range, 1, payload, plen, msg, sizeof(msg));
  assert(mlen > 0);

  /* Convert to hex */
  char *hex = bin2hex(msg, mlen);
  assert(hex);

  /* Process the peer message */
  int rc = nostr_neg_handle_peer_hex(s, hex);
  assert(rc == 0);
  free(hex);

  /* Verify NEED IDs */
  const unsigned char *need = NULL;
  size_t need_count = 0;
  rc = nostr_neg_get_need_ids(s, &need, &need_count);
  assert(rc == 0);
  assert(need_count == 2); /* ID C and ID D */

  /* Verify the specific IDs */
  unsigned char expected_c[32] = {0};
  unsigned char expected_d[32] = {0};
  expected_c[0] = 0x03;
  expected_d[0] = 0x04;
  int found_c = 0, found_d = 0;
  for (size_t i = 0; i < need_count; i++) {
    if (memcmp(need + i * 32, expected_c, 32) == 0) found_c = 1;
    if (memcmp(need + i * 32, expected_d, 32) == 0) found_d = 1;
  }
  assert(found_c && found_d);

  /* Verify stats */
  NostrNegStats st;
  nostr_neg_get_stats(s, &st);
  assert(st.ids_recv == 3); /* all 3 IDs counted */

  nostr_neg_session_free(s);
  printf("ok need_ids\n");
  return 0;
}
