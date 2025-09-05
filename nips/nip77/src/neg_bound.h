#ifndef NOSTR_NIP77_NEG_BOUND_H
#define NOSTR_NIP77_NEG_BOUND_H
#include <stdint.h>
#include <stddef.h>

/* Bound encoding (timestamp delta; 0=infinity; idPrefix len 0..32) */
typedef struct {
  uint64_t ts_delta; /* 0 means infinity */
  unsigned char id_prefix[32];
  unsigned char id_prefix_len; /* 0..32 */
} neg_bound_t;

size_t neg_bound_encode(const neg_bound_t *b, unsigned char *out, size_t outcap);
int    neg_bound_decode(const unsigned char *in, size_t inlen, neg_bound_t *out, size_t *consumed);

#endif
