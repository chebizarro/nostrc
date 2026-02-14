#ifndef NIPS_NIP02_NOSTR_NIP02_NIP02_H
#define NIPS_NIP02_NOSTR_NIP02_NIP02_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
  unsigned char pubkey[32];
  char *relay;    /* optional (NULL/empty ok) */
  char *petname;  /* optional (NULL/empty ok) */
} NostrFollowEntry;

typedef struct {
  NostrFollowEntry *entries;
  size_t count;
} NostrFollowList;

/**
 * nostr_nip02_build_follow_list:
 * @ev: (nullable): event to populate (caller owns)
 * @author_pk: (array fixed-size=32): author pubkey for event.pubkey
 * @list: (nullable): list of follows; NULL or empty produces empty list
 * @created_at: unix timestamp
 *
 * Builds a canonical kind:3 contact list: sets kind, pubkey, created_at,
 * and replaces tags with canonical "p" entries (and optional relay/petname).
 *
 * Returns: 0 on success, -errno on error.
 */
int nostr_nip02_build_follow_list(NostrEvent *ev,
                                  const unsigned char author_pk[32],
                                  const NostrFollowList *list,
                                  uint32_t created_at);

/**
 * nostr_nip02_parse_follow_list:
 * @ev: (nullable): event to parse
 * @out: (out) (transfer full): follow list; free with nostr_nip02_free_follow_list()
 *
 * Returns: 0 on success, -ENOENT if kind!=3, or -errno on error.
 */
int  nostr_nip02_parse_follow_list(const NostrEvent *ev, NostrFollowList *out);
/**
 * nostr_nip02_free_follow_list:
 * @list: (inout) (transfer full): list to free; safe on NULL/empty
 */
void nostr_nip02_free_follow_list(NostrFollowList *list);

/**
 * nostr_nip02_append:
 * @ev: (nullable): existing kind:3 event; will be updated in-place
 * @add: (array length=add_n) (nullable): entries to append
 * @add_n: number of entries
 *
 * Appends unique follows to the existing list by pubkey. Creates kind:3 if
 * necessary and preserves created_at unless it is 0.
 *
 * Returns: >=0 number of new entries appended; -errno on error.
 */
int nostr_nip02_append(NostrEvent *ev, const NostrFollowEntry *add, size_t add_n);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP02_NOSTR_NIP02_NIP02_H */
