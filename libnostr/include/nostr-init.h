#pragma once

/*
 * libnostr global initialization and cleanup
 *
 * If built with libwally (LIBNOSTR_WITH_WALLY=1), nostr_global_init() will
 * call wally_init(0) and nostr_global_cleanup() will call wally_cleanup(0).
 *
 * By default, constructor/destructor attributes will auto-initialize on load
 * unless NOSTR_DISABLE_AUTO_INIT is defined.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * nostr_global_init:
 * @returns: 0 on success, non-zero on failure
 *
 * Initializes global library state. Safe to call multiple times; increments
 * a refcount internally. Must be balanced with nostr_global_cleanup().
 */
int nostr_global_init(void);

/**
 * nostr_global_cleanup:
 *
 * Decrements the init refcount and performs cleanup when it reaches zero.
 * Safe to call multiple times after matching init calls.
 */
void nostr_global_cleanup(void);

#ifdef __cplusplus
}
#endif
