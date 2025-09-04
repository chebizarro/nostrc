#ifndef NOSTR_NIP19_H
#define NOSTR_NIP19_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/*
 * NIP-19 public API
 * This header declares canonical nostr_* encode/decode functions for bech32 and TLV pointer types.
 * It intentionally contains no transitional wrappers.
 */
#include "nostr-pointer.h"

/* Spec: docs/nips/19.md - "Bare keys and ids" (lines 13–25)
 * Use bech32 (not m) for npub/nsec/note. */

/**
 * NostrBech32Type:
 * @NOSTR_B32_NPUB: bech32-encoded public key (32 bytes)
 * @NOSTR_B32_NSEC: bech32-encoded secret key (32 bytes)
 * @NOSTR_B32_NOTE: bech32-encoded event id (32 bytes)
 * @NOSTR_B32_NPROFILE: TLV-encoded profile pointer (pubkey + optional relays)
 * @NOSTR_B32_NEVENT: TLV-encoded event pointer (id + optional relays/author/kind)
 * @NOSTR_B32_NADDR: TLV-encoded addressable entity pointer (identifier + author + kind + optional relays)
 * @NOSTR_B32_NRELAY: TLV-encoded relay pointer (one or more relays)
 *
 * Inspect-only classification of a bech32-encoded NIP-19 entity.
 */
typedef enum {
    NOSTR_B32_UNKNOWN = 0,
    NOSTR_B32_NPUB,
    NOSTR_B32_NSEC,
    NOSTR_B32_NOTE,
    NOSTR_B32_NPROFILE,
    NOSTR_B32_NEVENT,
    NOSTR_B32_NADDR,
    NOSTR_B32_NRELAY
} NostrBech32Type;

/* Low-level Bech32 helpers (bech32, not bech32m) */
int nostr_b32_encode(const char *hrp, const uint8_t *data5, size_t data5_len, char **out_bech);
int nostr_b32_decode(const char *bech, char **out_hrp, uint8_t **out_data5, size_t *out_data5_len);
int nostr_b32_to_5bit(const uint8_t *in8, size_t in8_len, uint8_t **out5, size_t *out5_len);
int nostr_b32_to_8bit(const uint8_t *in5, size_t in5_len, uint8_t **out8, size_t *out8_len);

/* Typed helpers for 32-byte payloads */
int nostr_nip19_encode_npub(const uint8_t pubkey[32], char **out_bech);
int nostr_nip19_decode_npub(const char *npub, uint8_t out_pubkey[32]);
int nostr_nip19_encode_nsec(const uint8_t seckey[32], char **out_bech);
int nostr_nip19_decode_nsec(const char *nsec, uint8_t out_seckey[32]);
int nostr_nip19_encode_note(const uint8_t event_id[32], char **out_bech);
int nostr_nip19_decode_note(const char *note, uint8_t out_event_id[32]);

/* HRP inspect (no full decode) */
/**
 * nostr_nip19_inspect:
 * @bech: candidate NIP-19 bech32 string
 * @out_type: returns detected type on success
 *
 * Returns: 0 on success, -1 on parse/checksum failure.
 */
int nostr_nip19_inspect(const char *bech, NostrBech32Type *out_type);

/* TLV helpers (Shareable identifiers with extra metadata; see docs/nips/19.md)
 * TLV types: 0(special), 1(relay ASCII), 2(author 32-byte), 3(kind uint32 BE) */

/* Generic TLV bech32 encode/decode */
/**
 * nostr_nip19_encode_tlv: encode an arbitrary TLV buffer under @hrp into bech32.
 * nostr_nip19_decode_tlv: decode a bech32 TLV into hrp and raw TLV bytes.
 */
int nostr_nip19_encode_tlv(const char *hrp, const uint8_t *tlv, size_t tlv_len, char **out_bech);
int nostr_nip19_decode_tlv(const char *bech, char **out_hrp, uint8_t **out_tlv, size_t *out_tlv_len);

/* Typed TLV encoders/decoders */
/** nprofile: TLVs: 0(pubkey 32), 1(relay ascii, repeatable) */
int nostr_nip19_encode_nprofile(const NostrProfilePointer *p, char **out_bech);
int nostr_nip19_decode_nprofile(const char *bech, NostrProfilePointer **out_p);

/** nevent: TLVs: 0(id 32), 1(relay), 2(author 32), 3(kind u32 BE) */
int nostr_nip19_encode_nevent(const NostrEventPointer *e, char **out_bech);
int nostr_nip19_decode_nevent(const char *bech, NostrEventPointer **out_e);

/** naddr: TLVs: 0(identifier), 1(relay), 2(author 32), 3(kind u32 BE) */
int nostr_nip19_encode_naddr(const NostrEntityPointer *a, char **out_bech);
int nostr_nip19_decode_naddr(const char *bech, NostrEntityPointer **out_a);

/** nrelay: TLVs: 1(relay ascii, repeatable) */
int nostr_nip19_encode_nrelay(const char *relay_url, char **out_bech);
/** Multi-relay encoder (preferred). Encodes all relays as repeated T=1 items. */
int nostr_nip19_encode_nrelay_multi(const char *const *relays, size_t relay_count, char **out_bech);
int nostr_nip19_decode_nrelay(const char *bech, char ***out_relays, size_t *out_count);

/*
 * Unified pointer API
 * -------------------
 * These helpers integrate NIP-19 functions with pointer types for easier usage.
 */

/** NostrPointerKind: discriminates the pointer union. */
typedef enum {
    NOSTR_PTR_NONE = 0,
    NOSTR_PTR_NPROFILE,
    NOSTR_PTR_NEVENT,
    NOSTR_PTR_NADDR,
    NOSTR_PTR_NRELAY,
} NostrPointerKind;

/** NostrPointer: tagged union holding one of the pointer types. */
typedef struct NostrPointer {
    NostrPointerKind kind;
    union {
        NostrProfilePointer *nprofile;
        NostrEventPointer   *nevent;
        NostrEntityPointer  *naddr;
        struct { char **relays; size_t count; } nrelay;
    } u;
} NostrPointer;

/** Config builders (inputs are borrowed; function copies as needed). */
typedef struct {
    const char *public_key;                 /* hex */
    const char *const *relays; size_t relays_count;
} NostrNProfileConfig;

typedef struct {
    const char *id;                         /* hex */
    const char *author;                     /* hex, optional */
    int kind;                               /* optional, <=0 to omit */
    const char *const *relays; size_t relays_count;
} NostrNEventConfig;

typedef struct {
    const char *identifier;                 /* string */
    const char *public_key;                 /* hex */
    int kind;                               /* required (>0) */
    const char *const *relays; size_t relays_count;
} NostrNAddrConfig;

typedef struct {
    const char *const *relays; size_t relays_count; /* at least 1 */
} NostrNRelayConfig;

/* Parse any bech32 into a tagged pointer. Caller owns result and must free with nostr_pointer_free(). */
int nostr_pointer_parse(const char *bech, NostrPointer **out_ptr);

/* Construct pointers from config. Caller owns result and must free with nostr_pointer_free(). */
int nostr_pointer_from_nprofile_config(const NostrNProfileConfig *cfg, NostrPointer **out_ptr);
int nostr_pointer_from_nevent_config(const NostrNEventConfig *cfg, NostrPointer **out_ptr);
int nostr_pointer_from_naddr_config(const NostrNAddrConfig *cfg, NostrPointer **out_ptr);
int nostr_pointer_from_nrelay_config(const NostrNRelayConfig *cfg, NostrPointer **out_ptr);

/* Encode a tagged pointer to the appropriate bech32. Returns heap string; caller frees. */
int nostr_pointer_to_bech32(const NostrPointer *ptr, char **out_bech);

/* Free a tagged pointer (and its inner allocations). Safe on NULL. */
void nostr_pointer_free(NostrPointer *ptr);

#endif // NOSTR_NIP19_H
