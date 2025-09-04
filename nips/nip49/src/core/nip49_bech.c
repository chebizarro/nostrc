#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "nostr/nip19/nip19.h"

#define HRP_NCRYPTSEC "ncryptsec"

int nip49_bech32_encode_ncryptsec(const uint8_t payload91[91], char **out_bech32) {
  if (!payload91 || !out_bech32) return -1;
  *out_bech32 = NULL;
  uint8_t *data5 = NULL; size_t data5_len = 0;
  if (nostr_b32_to_5bit(payload91, 91, &data5, &data5_len) != 0) return -1;
  int rc = nostr_b32_encode(HRP_NCRYPTSEC, data5, data5_len, out_bech32);
  free(data5);
  return rc;
}

int nip49_bech32_decode_ncryptsec(const char *bech32, uint8_t out_payload91[91]) {
  if (!bech32 || !out_payload91) return -1;
  char *hrp = NULL; uint8_t *data5 = NULL; size_t data5_len = 0;
  if (nostr_b32_decode(bech32, &hrp, &data5, &data5_len) != 0) return -1;
  int rc = -1;
  if (strcmp(hrp, HRP_NCRYPTSEC) == 0) {
    uint8_t *data8 = NULL; size_t data8_len = 0;
    if (nostr_b32_to_8bit(data5, data5_len, &data8, &data8_len) == 0 && data8_len == 91) {
      memcpy(out_payload91, data8, 91);
      rc = 0;
    }
    free(data8);
  }
  free(hrp); free(data5);
  return rc;
}
