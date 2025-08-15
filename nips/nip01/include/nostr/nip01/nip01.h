#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include "nostr-filter.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* --- Tag builders (typed) --- */
/**
 * nostr_nip01_add_e_tag:
 * @ev: (nullable): event to modify
 * @event_id: (array fixed-size=32): binary event id
 * @relay_opt: (nullable): optional relay URL
 * @author_pk: (nullable): optional 32-byte pubkey of referenced author
 *
 * Appends an "e" tag: ["e", <id-hex>, <relay?>, <author?>].
 *
 * Returns: 0 on success, negative errno on error.
 */
int nostr_nip01_add_e_tag(NostrEvent *ev,
                          const unsigned char event_id[32],
                          const char *relay_opt,
                          const unsigned char *author_pk /* optional */);

/**
 * nostr_nip01_add_p_tag:
 * @ev: (nullable): event to modify
 * @pubkey: (array fixed-size=32): binary pubkey
 * @relay_opt: (nullable): optional relay URL
 *
 * Appends a "p" tag: ["p", <pubkey-hex>, <relay?>].
 *
 * Returns: 0 on success, negative errno on error.
 */
int nostr_nip01_add_p_tag(NostrEvent *ev,
                          const unsigned char pubkey[32],
                          const char *relay_opt);

/**
 * nostr_nip01_add_a_tag:
 * @ev: (nullable)
 * @kind: kind code for addressable event
 * @pubkey: (array fixed-size=32)
 * @d_tag_opt: (nullable): optional d tag
 * @relay_opt: (nullable): optional relay URL
 *
 * Appends an "a" tag: ["a", "kind:pubkey[:d]", <relay?>].
 *
 * Returns: 0 on success, negative errno on error.
 */
int nostr_nip01_add_a_tag(NostrEvent *ev,
                          uint32_t kind,
                          const unsigned char pubkey[32],
                          const char *d_tag_opt,
                          const char *relay_opt);

/* --- Parsing helpers --- */
/**
 * nostr_nip01_get_alt:
 * @ev: (nullable): event
 * @out_alt: (out) (transfer full): newly-allocated alt string
 *
 * Finds the first ["alt", value] tag and returns a duplicate of value.
 *
 * Returns: 0 on success, -ENOENT if absent, or -errno on error.
 */
int  nostr_nip01_get_alt(const NostrEvent *ev, char **out_alt);
/**
 * nostr_nip01_is_replaceable:
 * @kind: kind code
 *
 * Returns: %true if replaceable (k=0,3 or 10000..19999).
 */
bool nostr_nip01_is_replaceable(int kind);
/**
 * nostr_nip01_is_addressable:
 * Returns: %true if addressable (30000..39999).
 */
bool nostr_nip01_is_addressable(int kind);
/**
 * nostr_nip01_is_ephemeral:
 * Returns: %true if ephemeral (20000..29999).
 */
bool nostr_nip01_is_ephemeral(int kind);

/* --- Filters (typed builders) --- */
/**
 * NostrFilterBuilder:
 *
 * Opaque builder for accumulating fields into a NostrFilter.
 */
typedef struct {
  void *impl; /* opaque */
} NostrFilterBuilder;

/**
 * nostr_nip01_filter_builder_init:
 * @fb: (out): builder
 *
 * Initializes a builder that accumulates fields into an internal NostrFilter.
 *
 * Returns: 0 on success, -errno on failure.
 */
int  nostr_nip01_filter_builder_init(NostrFilterBuilder *fb);
/**
 * nostr_nip01_filter_builder_dispose:
 * @fb: (inout): builder
 *
 * Releases internal resources. Safe to call with NULL.
 */
void nostr_nip01_filter_builder_dispose(NostrFilterBuilder *fb);

/**
 * nostr_nip01_filter_by_ids:
 * @ids: (array length=n) (element-type guint8[32])
 */
int  nostr_nip01_filter_by_ids(NostrFilterBuilder *fb, const unsigned char (*ids)[32], size_t n);
/**
 * nostr_nip01_filter_by_authors:
 * @pubkeys: (array length=n) (element-type guint8[32])
 */
int  nostr_nip01_filter_by_authors(NostrFilterBuilder *fb, const unsigned char (*pubkeys)[32], size_t n);
int  nostr_nip01_filter_by_kinds(NostrFilterBuilder *fb, const int *kinds, size_t n);
int  nostr_nip01_filter_by_tag_e(NostrFilterBuilder *fb, const unsigned char (*ids)[32], size_t n);
int  nostr_nip01_filter_by_tag_p(NostrFilterBuilder *fb, const unsigned char (*pubkeys)[32], size_t n);
int  nostr_nip01_filter_by_tag_a(NostrFilterBuilder *fb, const char **a_refs, size_t n);
int  nostr_nip01_filter_since(NostrFilterBuilder *fb, uint32_t since);
int  nostr_nip01_filter_until(NostrFilterBuilder *fb, uint32_t until);
int  nostr_nip01_filter_limit(NostrFilterBuilder *fb, int limit);

/**
 * nostr_nip01_filter_build:
 * @out: (out) (transfer full): deep-copied filter to take ownership of
 *
 * Returns: 0 on success, -errno on failure.
 */
int  nostr_nip01_filter_build(NostrFilterBuilder *fb, NostrFilter *out);

#ifdef __cplusplus
}
#endif
