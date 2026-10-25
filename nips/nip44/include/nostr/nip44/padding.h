#ifndef NOSTR_NIP44_PADDING_H
#define NOSTR_NIP44_PADDING_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* NIP-44 bucket schedule, without the message-length prefix. Zero maps to
 * 32. Caller must bound input to <= SIZE_MAX/2 to avoid arithmetic overflow. */
size_t nip44_calc_padded_len(size_t unpadded_len);
#ifdef __cplusplus
}
#endif
#endif
