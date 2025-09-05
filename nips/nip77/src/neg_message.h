#ifndef NOSTR_NIP77_NEG_MESSAGE_H
#define NOSTR_NIP77_NEG_MESSAGE_H
#include <stddef.h>
#include <stdint.h>
#include "neg_bound.h"

#define NEG_MSG_V1 0x61

typedef enum {
  NEG_ELT_SKIP = 0x00,
  NEG_ELT_FINGERPRINT = 0x01,
  NEG_ELT_IDLIST = 0x02,
} neg_elt_type_t;

/* Encode/decode V1 messages (ranges, elements) */
size_t neg_msg_encode_v1(const neg_bound_t *ranges, size_t nranges,
                         const unsigned char *payload, size_t payload_len,
                         unsigned char *out, size_t outcap);

int neg_msg_decode_v1(const unsigned char *in, size_t inlen,
                      neg_bound_t *ranges, size_t *inout_nranges,
                      const unsigned char **payload, size_t *payload_len);

/* TLV-style payload helpers */
/* Format: [type:1][len:varint][value:len] ... */

/* Returns bytes written (0 on overflow) */
size_t neg_msg_payload_put_tlv(unsigned char type, const unsigned char *val, size_t vlen,
                               unsigned char *out, size_t outcap);

/* Convenience helpers */
size_t neg_msg_payload_put_fingerprint(const unsigned char fp16[16],
                                       unsigned char *out, size_t outcap);
/* ids points to contiguous 32-byte IDs, count of IDs */
size_t neg_msg_payload_put_idlist(const unsigned char *ids, size_t id_stride, size_t count,
                                  unsigned char *out, size_t outcap);

/* Iterator callback invoked per element; return non-zero to stop iteration. */
typedef int (*neg_msg_tlv_iter_fn)(unsigned char type,
                                   const unsigned char *val, size_t vlen,
                                   void *user);

/* Iterate over TLVs in [payload,payload_len]. Returns 0 on full consume, or first non-zero from cb. */
int neg_msg_payload_iterate(const unsigned char *payload, size_t payload_len,
                            neg_msg_tlv_iter_fn cb, void *user);

#endif
