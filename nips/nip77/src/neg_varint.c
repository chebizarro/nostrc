#include "neg_varint.h"

size_t neg_varint_encode(uint64_t value, unsigned char *out, size_t outcap) {
  /* Simple LEB128-like but MSB marks continuation; emit big-endian groups */
  unsigned char buf[10];
  size_t n = 0;
  do {
    buf[9 - n] = (unsigned char)(value & 0x7F);
    value >>= 7;
    n++;
  } while (value != 0 && n < 10);
  for (size_t i = 10 - n; i < 10; ++i) {
    unsigned char byte = buf[i];
    if (i != 9) byte |= 0x80; /* set continuation on all but last */
    if (out && (10 - i - 1) < outcap) {
      out[(10 - i - 1)] = byte; /* compact to front */
    }
  }
  if (out && outcap >= n) {
    /* shift to start */
    for (size_t i = 0; i < n; ++i) out[i] = out[n - 1 - i];
  }
  return n;
}

int neg_varint_decode(const unsigned char *in, size_t inlen, uint64_t *out, size_t *consumed) {
  if (!in || inlen == 0) return -1;
  uint64_t v = 0;
  size_t i = 0;
  for (; i < inlen; ++i) {
    unsigned char byte = in[i];
    v = (v << 7) | (uint64_t)(byte & 0x7F);
    if ((byte & 0x80) == 0) { /* last */
      if (out) *out = v;
      if (consumed) *consumed = i + 1;
      return 0;
    }
  }
  return 1; /* need more bytes */
}
