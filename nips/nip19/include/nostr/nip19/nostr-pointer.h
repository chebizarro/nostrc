#ifndef __NOSTR_POINTER_H__
#define __NOSTR_POINTER_H__

#include <nostr/nip19/export.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* NIP-19 pointer abstractions live in the nip19 module. */

typedef struct {
    char *public_key;      /* hex npub (32 bytes, hex-encoded) */
    char **relays;         /* array of relay URLs */
    size_t relays_count;
} NostrProfilePointer;

typedef struct {
    char *id;              /* event id hex */
    char **relays;         /* array of relay URLs */
    size_t relays_count;
    char *author;          /* pubkey hex */
    int   kind;            /* event kind */
} NostrEventPointer;

typedef struct {
    char *public_key;      /* author pubkey hex (required for naddr) */
    int   kind;            /* required for naddr */
    char *identifier;      /* d-tag identifier (required for naddr) */
    char **relays;         /* array of relay URLs */
    size_t relays_count;
} NostrEntityPointer;

NOSTR_NIP19_API NostrProfilePointer *nostr_profile_pointer_new(void);
NOSTR_NIP19_API void nostr_profile_pointer_free(NostrProfilePointer *ptr);

NOSTR_NIP19_API NostrEventPointer *nostr_event_pointer_new(void);
NOSTR_NIP19_API void nostr_event_pointer_free(NostrEventPointer *ptr);

NOSTR_NIP19_API NostrEntityPointer *nostr_entity_pointer_new(void);
NOSTR_NIP19_API void nostr_entity_pointer_free(NostrEntityPointer *ptr);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_POINTER_H__ */
