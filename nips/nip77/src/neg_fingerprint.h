#ifndef NOSTR_NIP77_NEG_FINGERPRINT_H
#define NOSTR_NIP77_NEG_FINGERPRINT_H
#include <stddef.h>
#include <stdint.h>
#include "neg_bound.h"
#include "neg_varint.h"

/* Compute 16-byte fingerprint per spec sketch (sum IDs LE, varint count, SHA-256, first 16 bytes). */
int neg_fingerprint_compute(const unsigned char *ids, size_t id_stride, size_t count,
                            unsigned char out16[16]);

#endif
