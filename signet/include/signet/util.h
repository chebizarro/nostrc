/* SPDX-License-Identifier: MIT */

#ifndef SIGNET_UTIL_H
#define SIGNET_UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static inline int64_t signet_now_unix(void) {
  return (int64_t)time(NULL);
}

static inline bool signet_hex_to_bytes32(const char *hex, uint8_t out[32]) {
  if (!hex || !out || strlen(hex) != 64) return false;
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(hex + (i * 2), "%2x", &byte) != 1) return false;
    out[i] = (uint8_t)byte;
  }
  return true;
}

#endif /* SIGNET_UTIL_H */
