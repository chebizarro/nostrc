#ifndef NOSTR_NIP56_H
#define NOSTR_NIP56_H
#include <nostr/nip56/export.h>
#include "nostr-event.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Synchronous unsigned reporting codec. It does not infer whether an origin is
 * public: the caller declares a note report. No signing, fetching or evidence
 * collection occurs here. Return codes are negative errno. */
typedef enum {
    NOSTR_NIP56_NUDITY, NOSTR_NIP56_MALWARE, NOSTR_NIP56_PROFANITY,
    NOSTR_NIP56_ILLEGAL, NOSTR_NIP56_SPAM, NOSTR_NIP56_IMPERSONATION,
    NOSTR_NIP56_OTHER
} NostrNip56Category;
typedef enum { NOSTR_NIP56_ACCOUNT, NOSTR_NIP56_EVENT, NOSTR_NIP56_BLOB } NostrNip56TargetType;
typedef struct {
    NostrNip56TargetType type;
    const char *id; /* Borrowed pubkey, event ID or blob hash. */
    NostrNip56Category category;
} NostrNip56Target;
NOSTR_NIP56_API const char *nostr_nip56_category_name(NostrNip56Category category);
NOSTR_NIP56_API int nostr_nip56_category_parse(const char *name, NostrNip56Category *out);
/* Every p/e/x is typed and validated. Each x requires an e of the same category.
 * Unsupported extension tags remain on the input event. */
NOSTR_NIP56_API int nostr_nip56_validate(const NostrEvent *event, bool note_report);
/* No truncation: sets total count and returns -ENOSPC if capacity is too small.
 * Returned strings borrow the event until mutation/destruction. */
NOSTR_NIP56_API int nostr_nip56_parse(const NostrEvent *event, bool note_report, NostrNip56Target *targets, size_t capacity, size_t *count);
/* Returns owned unsigned event, or NULL with errno. */
NOSTR_NIP56_API NostrEvent *nostr_nip56_build_unsigned(const char *author, int64_t created_at, const NostrNip56Target *targets, size_t count, bool note_report, const char *explanation);
#ifdef __cplusplus
}
#endif
#endif
