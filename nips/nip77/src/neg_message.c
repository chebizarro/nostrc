#include "neg_message.h"
#include "neg_varint.h"
#include <string.h>

size_t neg_msg_encode_v1(const neg_bound_t *ranges, size_t nranges,
                         const unsigned char *payload, size_t payload_len,
                         unsigned char *out, size_t outcap) {
  size_t n = 0;
  if (out && outcap > 0) out[n] = NEG_MSG_V1; n += 1;
  /* number of ranges */
  unsigned char tmp[10];
  size_t rlen = neg_varint_encode((uint64_t)nranges, tmp, sizeof(tmp));
  if (out && outcap >= n + rlen) memcpy(out + n, tmp, rlen); n += rlen;
  for (size_t i = 0; i < nranges; ++i) {
    n += neg_bound_encode(&ranges[i], out ? out + n : NULL, out ? outcap - n : 0);
  }
  if (payload && payload_len > 0) {
    if (out && outcap >= n + payload_len) memcpy(out + n, payload, payload_len);
    n += payload_len;
  }
  return n;
}

int neg_msg_decode_v1(const unsigned char *in, size_t inlen,
                      neg_bound_t *ranges, size_t *inout_nranges,
                      const unsigned char **payload, size_t *payload_len) {
  if (!in || inlen == 0) return -1;
  size_t off = 0;
  if (in[off++] != NEG_MSG_V1) return -1;
  uint64_t nr = 0; size_t used = 0;
  int r = neg_varint_decode(in + off, inlen - off, &nr, &used);
  if (r != 0) return r;
  off += used;
  size_t maxr = inout_nranges ? *inout_nranges : 0;
  for (uint64_t i = 0; i < nr; ++i) {
    size_t c = 0;
    if (ranges && i < maxr) {
      r = neg_bound_decode(in + off, inlen - off, &ranges[i], &c);
    } else {
      neg_bound_t tmp; r = neg_bound_decode(in + off, inlen - off, &tmp, &c);
    }
    if (r != 0) return r;
    off += c;
  }
  if (inout_nranges) *inout_nranges = (size_t)nr;
  if (payload) *payload = in + off;
  if (payload_len) *payload_len = inlen - off;
  return 0;
}

size_t neg_msg_payload_put_tlv(unsigned char type, const unsigned char *val, size_t vlen,
                               unsigned char *out, size_t outcap) {
  size_t n = 0;
  if (out && outcap > 0) out[n] = type; n += 1;
  unsigned char tmp[10];
  size_t l = neg_varint_encode((uint64_t)vlen, tmp, sizeof(tmp));
  if (l == 0) return 0;
  if (out && outcap >= n + l) memcpy(out + n, tmp, l); n += l;
  if (vlen) {
    if (out && outcap >= n + vlen) memcpy(out + n, val, vlen);
    n += vlen;
  }
  return n;
}

size_t neg_msg_payload_put_fingerprint(const unsigned char fp16[16],
                                       unsigned char *out, size_t outcap) {
  if (!fp16) return 0;
  return neg_msg_payload_put_tlv(NEG_ELT_FINGERPRINT, fp16, 16, out, outcap);
}

size_t neg_msg_payload_put_idlist(const unsigned char *ids, size_t id_stride, size_t count,
                                  unsigned char *out, size_t outcap) {
  if (!ids || count == 0) return 0;
  if (id_stride == 0) id_stride = 32;
  /* idlist value layout: varint(count) + ids (packed 32B each or strided) */
  unsigned char tmp[10];
  size_t l = neg_varint_encode((uint64_t)count, tmp, sizeof(tmp));
  if (l == 0) return 0;

  size_t vlen = l + count * 32;
  if (!out) {
    /* size only */
    return 1 + l + vlen; /* type + len(varint(vlen)) isn't accounted yet */
  }
  /* Build into a staging buffer if needed: write TLV directly */
  size_t n = 0;
  if (outcap == 0) return 0;
  out[n++] = NEG_ELT_IDLIST;
  unsigned char lbuf[10];
  size_t llen = neg_varint_encode((uint64_t)vlen, lbuf, sizeof(lbuf));
  if (llen == 0 || outcap < 1 + llen + vlen) return 0;
  memcpy(out + n, lbuf, llen); n += llen;
  memcpy(out + n, tmp, l); n += l;
  /* copy ids packed 32B each from strided input */
  for (size_t i = 0; i < count; ++i) {
    memcpy(out + n, ids + i * id_stride, 32);
    n += 32;
  }
  return n;
}

int neg_msg_payload_iterate(const unsigned char *payload, size_t payload_len,
                            neg_msg_tlv_iter_fn cb, void *user) {
  if (!payload || !cb) return -1;
  size_t off = 0;
  while (off < payload_len) {
    unsigned char type = payload[off++];
    uint64_t vlen = 0; size_t used = 0;
    if (off >= payload_len) return -1;
    int r = neg_varint_decode(payload + off, payload_len - off, &vlen, &used);
    if (r != 0) return r;
    off += used;
    if (off + vlen > payload_len) return -1;
    int stop = cb(type, payload + off, (size_t)vlen, user);
    if (stop) return stop;
    off += (size_t)vlen;
  }
  return 0;
}
