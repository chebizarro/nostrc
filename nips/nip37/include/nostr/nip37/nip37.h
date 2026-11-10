#ifndef NOSTR_NIP37_H
#define NOSTR_NIP37_H
#include <nostr/nip37/export.h>
#include "nostr-event.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Pure synchronous codecs: no encryption, signing, clock or relay access.
 * All returned events/strings are owned; parsed strings borrow the source.
 * int results use negative errno; NULL results set errno. */
typedef struct {
    const char *identifier;
    int draft_kind;
    bool deleted;
    bool has_expiration;
    int64_t expiration;
} NostrNip37Envelope;
typedef struct {
    char author[65];
    const char *identifier;
} NostrNip37Checkpoint;
NOSTR_NIP37_API int nostr_nip37_parse_envelope(const NostrEvent *event, NostrNip37Envelope *out);
NOSTR_NIP37_API NostrEvent *nostr_nip37_build_envelope(const char *author, int64_t created_at, const char *identifier, int draft_kind, const char *encrypted_content, const int64_t *expiration);
/* Signed inner drafts (including an explicit sig field) are rejected. */
NOSTR_NIP37_API NostrEvent *nostr_nip37_parse_draft(const char *plaintext, size_t length);
NOSTR_NIP37_API char *nostr_nip37_serialize_draft(const NostrEvent *draft);
NOSTR_NIP37_API int nostr_nip37_parse_checkpoint(const NostrEvent *event, NostrNip37Checkpoint *out);
NOSTR_NIP37_API NostrEvent *nostr_nip37_build_checkpoint(const char *author, int64_t created_at, const char *parent_author, const char *identifier, const char *encrypted_content);
#ifdef __cplusplus
}
#endif
#endif
