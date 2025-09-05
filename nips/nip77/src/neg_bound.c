#include "neg_bound.h"
#include "neg_varint.h"
#include <string.h>

size_t neg_bound_encode(const neg_bound_t *b, unsigned char *out, size_t outcap) {
  if (!b) return 0;
  size_t n = 0;
  /* ts_delta varint */
  unsigned char tmp[10];
  size_t tlen = neg_varint_encode(b->ts_delta, tmp, sizeof(tmp));
  if (out && outcap >= tlen) memcpy(out + n, tmp, tlen);
  n += tlen;
  /* id prefix length (single byte 0..32) */
  if (out && outcap > n) out[n] = b->id_prefix_len;
  n += 1;
  /* id prefix bytes */
  if (b->id_prefix_len > 0) {
    size_t plen = b->id_prefix_len;
    if (out && outcap >= n + plen) memcpy(out + n, b->id_prefix, plen);
    n += plen;
  }
  return n;
}

int neg_bound_decode(const unsigned char *in, size_t inlen, neg_bound_t *out, size_t *consumed) {
  if (!in || inlen == 0 || !out) return -1;
  size_t used = 0; uint64_t ts = 0; size_t clen = 0;
  int r = neg_varint_decode(in, inlen, &ts, &clen);
  if (r != 0) return r;
  used += clen;
  if (inlen < used + 1) return 1;
  unsigned char plen = in[used++];
  if (plen > 32) return -1;
  out->ts_delta = ts;
  out->id_prefix_len = plen;
  if (plen > 0) {
    if (inlen < used + plen) return 1;
    memcpy(out->id_prefix, in + used, plen);
    used += plen;
  }
  if (consumed) *consumed = used;
  return 0;
}
