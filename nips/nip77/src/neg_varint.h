#ifndef NOSTR_NIP77_NEG_VARINT_H
#define NOSTR_NIP77_NEG_VARINT_H
#include <stdint.h>
#include <stddef.h>

/* MSB-first base-128 Varint (big-endian bit flow) */
size_t neg_varint_encode(uint64_t value, unsigned char *out, size_t outcap);
int    neg_varint_decode(const unsigned char *in, size_t inlen, uint64_t *out, size_t *consumed);

#endif
