#ifndef NOSTR_NIP77_NEGENTROPY_H
#define NOSTR_NIP77_NEGENTROPY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Public API for NIP-77 (Negentropy) — datasource-agnostic. */

typedef struct NostrNegSession NostrNegSession; /* Opaque */

/* 32-byte Event ID */
typedef struct {
  unsigned char bytes[32];
} NostrEventId;

/* Index item (timestamp + id) */
typedef struct {
  uint64_t created_at; /* seconds since epoch */
  NostrEventId id;     /* 32 bytes */
} NostrIndexItem;

/* Datasource interface */
typedef struct {
  void *ctx;
  int (*begin_iter)(void *ctx);
  int (*next)(void *ctx, NostrIndexItem *out);
  void (*end_iter)(void *ctx);
} NostrNegDataSource;

/* Options */
typedef struct {
  uint32_t max_ranges;        /* maximum concurrent ranges */
  uint32_t max_idlist_items;  /* maximum IDs in IdList messages */
  uint32_t max_round_trips;   /* negotiation budget */
} NostrNegOptions;

/* Stats (counters, bytes, rounds) */
typedef struct {
  uint64_t bytes_sent;
  uint64_t bytes_recv;
  uint32_t rounds;
  uint32_t ranges_sent;
  uint32_t ids_sent;
  /* Additional visibility counters */
  uint32_t ranges_recv;
  uint32_t ids_recv;
  uint32_t skips_sent;
  uint32_t idlists_sent;
} NostrNegStats;

/* Construction */
NostrNegSession *nostr_neg_session_new(const NostrNegDataSource *ds,
                                       const NostrNegOptions *opts);
void nostr_neg_session_free(NostrNegSession *s);

/* Protocol round helpers
 * Returns newly-allocated hex buffers caller must free with free().
 */
char *nostr_neg_build_initial_hex(NostrNegSession *s);
int   nostr_neg_handle_peer_hex(NostrNegSession *s, const char *hex_msg);
char *nostr_neg_build_next_hex(NostrNegSession *s);

/* Stats */
void  nostr_neg_get_stats(const NostrNegSession *s, NostrNegStats *out);

/* NEED IDs — event IDs the peer has that we don't.
 * Valid after protocol completes. Pointer owned by session.
 * Returns 0 on success, -1 on invalid arguments. */
int   nostr_neg_get_need_ids(const NostrNegSession *s,
                              const unsigned char **out_ids,
                              size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP77_NEGENTROPY_H */
