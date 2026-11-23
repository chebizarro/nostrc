#ifndef NOSTR_NIP78_H
#define NOSTR_NIP78_H

#include <nostr/nip78/export.h>
#include "nostr-event.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *author;
    char *identifier;
    char *content;
    int64_t created_at;
    char *event_id;
} NostrNip78Document;

/* Pure kind-30078 codec: no clock, secret key, encryption or relay access.
 * Content is opaque, including caller-supplied NIP-44 ciphertext. Encrypt to
 * the intended recipient with the external signer BEFORE building, then sign
 * and publish through the normal relay path. Plaintext content is also allowed.
 * Author must be 64 lowercase hex characters, created_at nonnegative, and
 * identifier/content non-NULL (empty strings are allowed). Builds exactly one
 * two-field d tag. Returns an owned unsigned event or NULL with errno set. */
NOSTR_NIP78_API NostrEvent *nostr_nip78_build_event(const char *author,
    const char *identifier, const char *content, int64_t created_at);

/* Requires kind 30078, a valid author/timestamp/content and exactly one
 * two-field d tag; unrelated tags are allowed. Parses unsigned or signed
 * events, without authenticating a signature, decrypting, or enforcing app
 * schema/authorization. Callers accepting remote data must first validate the
 * event and bind author/identifier to their expected coordinate.
 * All document fields are owned copies; event_id is the computed canonical
 * event hash, not an unverified wire id. Returns 0 or negative errno and resets
 * *out to NULL on failure. Free with nostr_nip78_document_free(). */
NOSTR_NIP78_API int nostr_nip78_parse_event(const NostrEvent *event,
    NostrNip78Document **out);
NOSTR_NIP78_API void nostr_nip78_document_free(NostrNip78Document *document);

#ifdef __cplusplus
}
#endif
#endif
