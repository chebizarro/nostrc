#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"

/*
 * NIP-31: Alternative human-readable content ("alt")
 *
 * Canonical helpers to set/get the NIP-31 "alt" tag on an event.
 * These are GI-friendly with ownership/transfer and nullability documented.
 */

/**
 * nostr_nip31_set_alt:
 * @ev: (inout) (transfer none) (not nullable): event to modify
 * @alt: (in) (not nullable): UTF-8 text to set as the alt description
 *
 * Replaces any existing 'alt' tag with a single tag of the form ["alt", alt].
 *
 * Returns: 0 on success, negative errno-style value on failure.
 */
int  nostr_nip31_set_alt(NostrEvent *ev, const char *alt);

/**
 * nostr_nip31_get_alt:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 * @out_alt: (out) (transfer full) (nullable): malloc'd UTF-8 string if found
 *
 * Looks up the first 'alt' tag and returns a heap-allocated copy of its value
 * in @out_alt. Caller must free() the returned string.
 *
 * Returns: 0 on success; -ENOENT if not present; other negative on error.
 */
int  nostr_nip31_get_alt(const NostrEvent *ev, char **out_alt); /* malloc'd */

#ifdef __cplusplus
}
#endif
