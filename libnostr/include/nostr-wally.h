#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Centralized libwally includes to avoid sprinkling headers across codebase. */
#include "nostr-config.h"

#if defined(LIBNOSTR_WITH_WALLY) && (LIBNOSTR_WITH_WALLY+0)==1
  /* Minimal surfaces we use for NIP-06 (BIP39/BIP32) and zeroization */
  #include <wally_core.h>
  #include <wally_bip39.h>
  #include <wally_bip32.h>
#else
  /* Build without wally: provide tiny stubs to allow compile of callers that
   * gate runtime usage behind feature checks. Do not provide ABI; just macros. */
  #define WALLY_NOT_AVAILABLE 1
#endif

#ifdef __cplusplus
}
#endif
