#include "neg_session.h"
#include "neg_fingerprint.h"
#include "neg_varint.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include <string.h>

NostrNegSession *nostr_neg_session_new(const NostrNegDataSource *ds,
                                       const NostrNegOptions *opts) {
  if (!ds) return NULL;
  NostrNegSession *s = (NostrNegSession*)calloc(1, sizeof(*s));
  if (!s) return NULL;
  s->ds = *ds;
  if (opts) s->opts = *opts;
  /* sane defaults if not set */
  if (s->opts.max_ranges == 0) s->opts.max_ranges = 8;
  if (s->opts.max_idlist_items == 0) s->opts.max_idlist_items = 256;
  if (s->opts.max_round_trips == 0) s->opts.max_round_trips = 8;
  return s;
}

/* Context for NEED ID extraction callback */
typedef struct {
  NostrNegSession *session;
} extract_need_ctx_t;

/* TLV iterator: extract NEED IDs from peer's IdList.
 * Compares peer IDs against local dataset; IDs not found locally are NEED. */
static int extract_need_ids_cb(unsigned char t, const unsigned char *val, size_t vlen, void *u) {
  if (t != NEG_ELT_IDLIST || !val || vlen == 0 || !u) return 0;
  extract_need_ctx_t *ctx = (extract_need_ctx_t *)u;
  NostrNegSession *s = ctx->session;

  /* Parse varint count prefix */
  uint64_t count = 0;
  size_t used = 0;
  if (neg_varint_decode(val, vlen, &count, &used) != 0 || count == 0) return 0;

  const unsigned char *ids_start = val + used;
  size_t ids_bytes = vlen - used;
  if (ids_bytes < count * 32) return 0; /* malformed */

  s->stats.ids_recv += (uint32_t)count;

  /* Build local ID set for comparison */
  size_t local_cap = 256, local_cnt = 0;
  unsigned char *local_ids = (unsigned char *)malloc(local_cap * 32);
  if (local_ids && s->ds.begin_iter) {
    s->ds.begin_iter(s->ds.ctx);
    NostrIndexItem it;
    while (s->ds.next && s->ds.next(s->ds.ctx, &it) == 0) {
      if (local_cnt >= local_cap) {
        size_t ncap = local_cap * 2;
        unsigned char *tmp = (unsigned char *)realloc(local_ids, ncap * 32);
        if (!tmp) break;
        local_ids = tmp;
        local_cap = ncap;
      }
      memcpy(local_ids + local_cnt * 32, it.id.bytes, 32);
      local_cnt++;
    }
    if (s->ds.end_iter) s->ds.end_iter(s->ds.ctx);
  }

  /* For each peer ID, check if in local set */
  for (uint64_t i = 0; i < count; i++) {
    const unsigned char *peer_id = ids_start + i * 32;
    int found = 0;
    for (size_t j = 0; j < local_cnt; j++) {
      if (memcmp(local_ids + j * 32, peer_id, 32) == 0) {
        found = 1;
        break;
      }
    }
    if (!found) {
      /* Grow need_ids array */
      if (s->need_ids_count >= s->need_ids_cap) {
        size_t new_cap = s->need_ids_cap ? s->need_ids_cap * 2 : 64;
        unsigned char *tmp = (unsigned char *)realloc(s->need_ids, new_cap * 32);
        if (!tmp) continue;
        s->need_ids = tmp;
        s->need_ids_cap = new_cap;
      }
      memcpy(s->need_ids + s->need_ids_count * 32, peer_id, 32);
      s->need_ids_count++;
    }
  }

  free(local_ids);
  return 1; /* stop after first IdList */
}

/* TLV fingerprint capture context and callback */
struct fp_capture_ctx { unsigned char fp[16]; int have_fp; };
static int capture_fp_cb(unsigned char t, const unsigned char *val, size_t vlen, void *u) {
  struct fp_capture_ctx *ctx = (struct fp_capture_ctx*)u;
  if (t == NEG_ELT_FINGERPRINT && vlen == 16) {
    memcpy(ctx->fp, val, 16);
    ctx->have_fp = 1;
    return 1; /* stop */
  }
  return 0;
}

void nostr_neg_session_free(NostrNegSession *s) {
  if (!s) return;
  if (s->pending_msg) free(s->pending_msg);
  if (s->need_ids) free(s->need_ids);
  free(s);
}

/* local hex helpers (lowercase) */
static char tohex(unsigned v) { return (char)(v < 10 ? ('0' + v) : ('a' + (v - 10))); }
static char *bin2hex(const unsigned char *in, size_t len) {
  char *out = (char*)malloc(len * 2 + 1);
  if (!out) return NULL;
  for (size_t i = 0; i < len; ++i) {
    out[2*i] = tohex(in[i] >> 4);
    out[2*i+1] = tohex(in[i] & 0x0F);
  }
  out[len*2] = '\0';
  return out;
}
static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}
static int hex2bin(unsigned char *out, size_t outcap, const char *hex) {
  size_t n = strlen(hex);
  if ((n & 1) != 0) return -1;
  size_t bytes = n / 2;
  if (out && outcap < bytes) return -1;
  for (size_t i = 0; i < bytes; ++i) {
    int hi = hexval(hex[2*i]);
    int lo = hexval(hex[2*i+1]);
    if (hi < 0 || lo < 0) return -1;
    if (out) out[i] = (unsigned char)((hi << 4) | lo);
  }
  return (int)bytes;
}

/* prefix match helper on arbitrary bit-length (0..256) */
static int id_matches_prefix(const unsigned char id[32], const unsigned char *prefix, unsigned preflen_bits) {
  if (preflen_bits == 0) return 1;
  unsigned full_bytes = preflen_bits / 8;
  unsigned rem_bits = preflen_bits % 8;
  if (full_bytes) {
    if (memcmp(id, prefix, full_bytes) != 0) return 0;
  }
  if (rem_bits) {
    unsigned char mask = (unsigned char)(0xFF << (8 - rem_bits));
    if ((id[full_bytes] & mask) != (prefix[full_bytes] & mask)) return 0;
  }
  return 1;
}

static int compute_range_fp(NostrNegSession *s, const neg_bound_t *b, unsigned char out16[16], size_t *out_count) {
  if (!s) return -1;
  if (s->ds.begin_iter) s->ds.begin_iter(s->ds.ctx);
  size_t cap = 128, cnt = 0;
  unsigned char *ids = (unsigned char*)malloc(cap * 32);
  if (!ids) { if (s->ds.end_iter) s->ds.end_iter(s->ds.ctx); return -1; }
  NostrIndexItem it;
  while (s->ds.next && s->ds.next(s->ds.ctx, &it) == 0) {
    if (!id_matches_prefix(it.id.bytes, b->id_prefix, b->id_prefix_len)) continue;
    if (cnt == cap) {
      size_t ncap = cap * 2;
      unsigned char *tmp = (unsigned char*)realloc(ids, ncap * 32);
      if (!tmp) { free(ids); if (s->ds.end_iter) s->ds.end_iter(s->ds.ctx); return -1; }
      ids = tmp; cap = ncap;
    }
    memcpy(ids + cnt * 32, it.id.bytes, 32);
    ++cnt;
  }
  if (s->ds.end_iter) s->ds.end_iter(s->ds.ctx);
  int rc = neg_fingerprint_compute(ids, 32, cnt, out16);
  free(ids);
  if (out_count) *out_count = cnt;
  return rc;
}

static void build_response_with_ranges(NostrNegSession *s, const neg_bound_t *ranges, size_t nranges) {
  if (!s) return;
  unsigned char tmp[1 + 10 + 64 * 8];
  size_t mlen = neg_msg_encode_v1(ranges, nranges, NULL, 0, tmp, sizeof(tmp));
  free(s->pending_msg); s->pending_msg = (unsigned char*)malloc(mlen);
  if (s->pending_msg) { memcpy(s->pending_msg, tmp, mlen); s->pending_len = mlen; }
  s->stats.ranges_sent += (uint32_t)nranges;
}

static void build_response_idlist(NostrNegSession *s, const neg_bound_t *range, const unsigned char *ids, size_t count) {
  if (!s) return;
  size_t maxn = count;
  if (maxn > s->opts.max_idlist_items) maxn = s->opts.max_idlist_items;
  size_t payload_cap = 1 + 10 + 10 + maxn * 32;
  unsigned char *payload = (unsigned char*)malloc(payload_cap);
  if (!payload) return;
  size_t pln = neg_msg_payload_put_idlist(ids, 32, maxn, payload, payload_cap);
  unsigned char tmp[1 + 10 + 64 * 8 + 1 + 10 + 32 * 4];
  size_t mlen = neg_msg_encode_v1(range, 1, payload, pln, tmp, sizeof(tmp));
  free(payload);
  free(s->pending_msg); s->pending_msg = (unsigned char*)malloc(mlen);
  if (s->pending_msg) { memcpy(s->pending_msg, tmp, mlen); s->pending_len = mlen; }
  s->stats.ids_sent += (uint32_t)maxn;
}

static void split_range(const neg_bound_t *in, neg_bound_t out[2]) {
  /* Increase prefix length by 1 bit and set next bit to 0/1 */
  *out = *in; out[1] = *in;
  unsigned nb = in->id_prefix_len;
  unsigned byte = nb / 8;
  unsigned bit = nb % 8;
  unsigned char mask = (unsigned char)(1u << (7 - bit));
  out[0].id_prefix[byte] &= (unsigned char)~mask;
  out[1].id_prefix[byte] |= mask;
  out[0].id_prefix_len = (unsigned char)(nb + 1);
  out[1].id_prefix_len = (unsigned char)(nb + 1);
}

char *nostr_neg_build_initial_hex(NostrNegSession *s) {
  if (!s) return NULL;
  /* Iterate datasource to aggregate fingerprint over all items */
  if (s->ds.begin_iter) s->ds.begin_iter(s->ds.ctx);
  /* Growable array of 32-byte IDs */
  size_t cap = 256, cnt = 0;
  unsigned char *ids = (unsigned char*)malloc(cap * 32);
  if (!ids) { if (s->ds.end_iter) s->ds.end_iter(s->ds.ctx); return NULL; }
  NostrIndexItem it;
  while (s->ds.next && s->ds.next(s->ds.ctx, &it) == 0) {
    if (cnt == cap) {
      size_t ncap = cap * 2;
      unsigned char *tmp = (unsigned char*)realloc(ids, ncap * 32);
      if (!tmp) { free(ids); if (s->ds.end_iter) s->ds.end_iter(s->ds.ctx); return NULL; }
      ids = tmp; cap = ncap;
    }
    memcpy(ids + cnt * 32, it.id.bytes, 32);
    ++cnt;
  }
  if (s->ds.end_iter) s->ds.end_iter(s->ds.ctx);

  unsigned char fp[16];
  neg_fingerprint_compute(ids, 32, cnt, fp);
  free(ids);

  /* Build message: one catch-all range + fingerprint TLV */
  unsigned char payload[64];
  size_t pln = neg_msg_payload_put_fingerprint(fp, payload, sizeof(payload));
  neg_bound_t r = { .ts_delta = 0, .id_prefix_len = 0 };
  unsigned char msg[1 + 10 + 8 + 1 + 1 + 10 + 16]; /* rough cap */
  size_t mlen = neg_msg_encode_v1(&r, 1, payload, pln, msg, sizeof(msg));

  s->stats.rounds += 1;
  s->stats.bytes_sent += mlen;
  return bin2hex(msg, mlen);
}

int nostr_neg_handle_peer_hex(NostrNegSession *s, const char *hex_msg) {
  if (!s || !hex_msg) return -1;
  int blen = hex2bin(NULL, 0, hex_msg);
  if (blen <= 0) return -1;
  unsigned char *buf = (unsigned char*)malloc((size_t)blen);
  if (!buf) return -1;
  if (hex2bin(buf, (size_t)blen, hex_msg) != blen) { free(buf); return -1; }
  const unsigned char *payload = NULL; size_t payload_len = 0;
  neg_bound_t ranges[8]; size_t rn = 8;
  int rc = neg_msg_decode_v1(buf, (size_t)blen, ranges, &rn, &payload, &payload_len);
  if (rc == 0) {
    s->stats.bytes_recv += (size_t)blen;
    s->stats.ranges_recv += (uint32_t)rn;
    /* Account ids received if peer sent an IdList */
    if (payload && payload_len) {
      extract_need_ctx_t ectx = { .session = s };
      neg_msg_payload_iterate(payload, payload_len, extract_need_ids_cb, &ectx);
    }
    if (rn >= 1) {
      /* Extract peer fingerprint (first only) */
      struct fp_capture_ctx fpc = { .have_fp = 0 };
      if (payload && payload_len) { neg_msg_payload_iterate(payload, payload_len, capture_fp_cb, &fpc); }

      /* Pass 1: analyze all ranges; decide if we must split and collect children within cap */
      unsigned max_children = s->opts.max_ranges ? s->opts.max_ranges : 8;
      neg_bound_t *children = (neg_bound_t*)calloc(max_children, sizeof(neg_bound_t));
      size_t child_cnt = 0;
      size_t first_small_idx = SIZE_MAX; size_t first_match_idx = SIZE_MAX;
      unsigned char first_small_local_fp[16]; size_t first_small_count = 0;
      for (size_t i=0; i<rn; ++i) {
        unsigned char local_fp[16]; size_t count = 0;
        compute_range_fp(s, &ranges[i], local_fp, &count);
        int match = (fpc.have_fp && memcmp(local_fp, fpc.fp, 16) == 0);
        if (match) { if (first_match_idx == SIZE_MAX) first_match_idx = i; continue; }
        if (count > 0 && count <= s->opts.max_idlist_items) {
          if (first_small_idx == SIZE_MAX) { first_small_idx = i; memcpy(first_small_local_fp, local_fp, 16); first_small_count = count; }
          continue;
        }
        if (ranges[i].id_prefix_len < 32) {
          neg_bound_t tmp[2]; split_range(&ranges[i], tmp);
          size_t remaining = (child_cnt < max_children) ? (max_children - child_cnt) : 0;
          if (remaining >= 2) {
            children[child_cnt++] = tmp[0];
            children[child_cnt++] = tmp[1];
          } else {
            /* Enqueue to session for later rounds if immediate payload is full */
            if (remaining == 1) {
              children[child_cnt++] = tmp[0];
              if (s->pending_ranges_len < sizeof(s->pending_ranges)/sizeof(s->pending_ranges[0])) s->pending_ranges[s->pending_ranges_len++] = tmp[1];
            } else {
              if (s->pending_ranges_len + 2 <= sizeof(s->pending_ranges)/sizeof(s->pending_ranges[0])) {
                s->pending_ranges[s->pending_ranges_len++] = tmp[0];
                s->pending_ranges[s->pending_ranges_len++] = tmp[1];
              }
            }
          }
        }
      }

      if (child_cnt > 0) {
        build_response_with_ranges(s, children, child_cnt);
      } else if (first_match_idx != SIZE_MAX) {
        /* All matched or at least one matched and none needed split -> send Skip for the first matching range */
        unsigned char pl[1+10]; size_t pln = neg_msg_payload_put_tlv(NEG_ELT_SKIP, NULL, 0, pl, sizeof(pl));
        unsigned char msg[1+10+64*8 + sizeof pl]; size_t mlen = neg_msg_encode_v1(&ranges[first_match_idx], 1, pl, pln, msg, sizeof msg);
        free(s->pending_msg); s->pending_msg = (unsigned char*)malloc(mlen);
        if (s->pending_msg) { memcpy(s->pending_msg, msg, mlen); s->pending_len = mlen; }
        s->stats.skips_sent += 1;
      } else if (first_small_idx != SIZE_MAX) {
        /* Collect ids for first small mismatch and send IdList */
        const neg_bound_t *r = &ranges[first_small_idx];
        /* re-collect IDs */
        if (s->ds.begin_iter) s->ds.begin_iter(s->ds.ctx);
        size_t cap = first_small_count; unsigned char *ids = (unsigned char*)malloc(cap*32); size_t c=0; NostrIndexItem it;
        if (ids) {
          while (s->ds.next && s->ds.next(s->ds.ctx, &it) == 0) {
            if (!id_matches_prefix(it.id.bytes, r->id_prefix, r->id_prefix_len)) continue;
            memcpy(ids + c*32, it.id.bytes, 32); if (++c == cap) break;
          }
        }
        if (s->ds.end_iter) s->ds.end_iter(s->ds.ctx);
        if (ids) { build_response_idlist(s, r, ids, c); free(ids); s->stats.idlists_sent += 1; }
      } else {
        /* Fallback: send our fingerprint for the first range */
        unsigned char local_fp[16]; size_t count = 0; compute_range_fp(s, &ranges[0], local_fp, &count);
        unsigned char pl[1+10+16]; size_t pln = neg_msg_payload_put_fingerprint(local_fp, pl, sizeof(pl));
        unsigned char msg[1+10+64*8 + sizeof pl]; size_t mlen = neg_msg_encode_v1(&ranges[0], 1, pl, pln, msg, sizeof msg);
        free(s->pending_msg); s->pending_msg = (unsigned char*)malloc(mlen);
        if (s->pending_msg) { memcpy(s->pending_msg, msg, mlen); s->pending_len = mlen; }
      }
      free(children);
    }
  }
  free(buf);
  return rc;
}

char *nostr_neg_build_next_hex(NostrNegSession *s) {
  if (!s) return NULL;
  /* If there is no pending binary message, try to flush queued ranges (cap-limited) */
  if ((!s->pending_msg || s->pending_len == 0) && s->pending_ranges_len > 0) {
    unsigned max_ranges = s->opts.max_ranges ? s->opts.max_ranges : 8;
    size_t take = s->pending_ranges_len < max_ranges ? s->pending_ranges_len : max_ranges;
    build_response_with_ranges(s, s->pending_ranges, take);
    /* shift remaining */
    if (s->pending_ranges_len > take) {
      memmove(s->pending_ranges, s->pending_ranges + take, (s->pending_ranges_len - take) * sizeof(s->pending_ranges[0]));
    }
    s->pending_ranges_len -= take;
  }
  if (!s->pending_msg || s->pending_len == 0) {
    char *hex = (char*)malloc(1); if (hex) hex[0] = '\0'; return hex;
  }
  s->stats.rounds += 1;
  s->stats.bytes_sent += s->pending_len;
  char *hex = bin2hex(s->pending_msg, s->pending_len);
  free(s->pending_msg); s->pending_msg = NULL; s->pending_len = 0;
  return hex;
}

void  nostr_neg_get_stats(const NostrNegSession *s, NostrNegStats *out) {
  if (!out) return;
  if (s) *out = s->stats; else memset(out, 0, sizeof(*out));
}

int nostr_neg_get_need_ids(const NostrNegSession *s,
                           const unsigned char **out_ids,
                           size_t *out_count) {
  if (!s || !out_ids || !out_count) return -1;
  *out_ids = s->need_ids;
  *out_count = s->need_ids_count;
  return 0;
}
