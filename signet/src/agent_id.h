/* SPDX-License-Identifier: MIT */
#ifndef SIGNET_AGENT_ID_H
#define SIGNET_AGENT_ID_H

#include <stdbool.h>
#include <stddef.h>

/* Only for identity creation/import. Stored IDs predate this alphabet. */
static inline bool signet_agent_id_is_valid(const char *id) {
  if (!id || !*id) return false;
  for (size_t i = 0; id[i]; i++) {
    unsigned char c = (unsigned char)id[i];
    if (i >= 64 || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
      return false;
  }
  return true;
}

#endif
