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
int nostr_nip01_add_e_tag(NostrEvent *ev,
                          const unsigned char event_id[32],
                          const char *relay_opt,
                          const unsigned char *author_pk /* optional */);

int nostr_nip01_add_p_tag(NostrEvent *ev,
                          const unsigned char pubkey[32],
                          const char *relay_opt);

int nostr_nip01_add_a_tag(NostrEvent *ev,
                          uint32_t kind,
                          const unsigned char pubkey[32],
                          const char *d_tag_opt,
                          const char *relay_opt);

/* --- Parsing helpers --- */
int  nostr_nip01_get_alt(const NostrEvent *ev, char **out_alt); /* malloc'd */
bool nostr_nip01_is_replaceable(int kind);
bool nostr_nip01_is_addressable(int kind);
bool nostr_nip01_is_ephemeral(int kind);

/* --- Filters (typed builders) --- */
typedef struct {
  void *impl; /* opaque */
} NostrFilterBuilder;

int  nostr_nip01_filter_builder_init(NostrFilterBuilder *fb);
void nostr_nip01_filter_builder_dispose(NostrFilterBuilder *fb);

int  nostr_nip01_filter_by_ids(NostrFilterBuilder *fb, const unsigned char (*ids)[32], size_t n);
int  nostr_nip01_filter_by_authors(NostrFilterBuilder *fb, const unsigned char (*pubkeys)[32], size_t n);
int  nostr_nip01_filter_by_kinds(NostrFilterBuilder *fb, const int *kinds, size_t n);
int  nostr_nip01_filter_by_tag_e(NostrFilterBuilder *fb, const unsigned char (*ids)[32], size_t n);
int  nostr_nip01_filter_by_tag_p(NostrFilterBuilder *fb, const unsigned char (*pubkeys)[32], size_t n);
int  nostr_nip01_filter_by_tag_a(NostrFilterBuilder *fb, const char **a_refs, size_t n);
int  nostr_nip01_filter_since(NostrFilterBuilder *fb, uint32_t since);
int  nostr_nip01_filter_until(NostrFilterBuilder *fb, uint32_t until);
int  nostr_nip01_filter_limit(NostrFilterBuilder *fb, int limit);

int  nostr_nip01_filter_build(NostrFilterBuilder *fb, NostrFilter *out);

#ifdef __cplusplus
}
#endif
