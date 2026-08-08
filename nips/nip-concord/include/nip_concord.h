#ifndef NOSTR_NIP_CONCORD_H
#define NOSTR_NIP_CONCORD_H

#include "nostr-event.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Concord kind numbers (externally allocated, documented in NIP-CAS-0008) */
#ifndef CONCORD_COMMUNITY_LIST
#define CONCORD_COMMUNITY_LIST ((uint16_t)13302)
#endif
#ifndef CONCORD_INVITE_LIST
#define CONCORD_INVITE_LIST ((uint16_t)13303)
#endif
#ifndef CONCORD_INVITE_BUNDLE
#define CONCORD_INVITE_BUNDLE ((uint16_t)33301)
#endif
#ifndef CONCORD_DIRECT_INVITE
#define CONCORD_DIRECT_INVITE ((uint16_t)3313)
#endif

/* Concord inner rumor kinds (inside encrypted wraps) */
#define CONCORD_KIND_JOIN_LEAVE 3306
#define CONCORD_KIND_KICK 3309
#define CONCORD_KIND_SNAPSHOT 3312
#define CONCORD_KIND_CONTROL_EDITION 3308

/* Stream wrap kinds */
#define CONCORD_STREAM_WRAP 1059
#define CONCORD_EPHEMERAL_STREAM_WRAP 21059
#define CONCORD_SEAL_ENCRYPTED 20013
#define CONCORD_SEAL_PLAINTEXT 20014

/* Limits */
#define CONCORD_MAX_COMMUNITIES_IN_LIST 50
#define CONCORD_MAX_CHANNELS_IN_INVITE iii
#define CONCORD_MAX_RELAYS_IN_BUNDLE 5
#define CONCORD_SNAPSHOT_CHUNK_SIZE 400

typedef enum {
    NOSTR_CONCORD_OK = 0,
    NOSTR_CONCORD_ERR_NULL,
    NOSTR_CONCORD_ERR_OOM,
    NOSTR_CONCORD_ERR_WRONG_KIND,
    NOSTR_CONCORD_ERR_BAD_PUBKEY,
    NOSTR_CONCORD_ERR_BAD_CONTENT,
    NOSTR_CONCORD_ERR_BAD_TAG,
    NOSTR_CONCORD_ERR_CARDINALITY,
    NOSTR_CONCORD_ERR_BAD_NIP44,
    NOSTR_CONCORD_ERR_BAD_SIGNATURE,
    NOSTR_CONCORD_ERR_COMMUNITY_ID,
    NOSTR_CONCORD_ERR_EPOCH,
    NOSTR_CONCORD_ERR_CHANNEL,
    NOSTR_CONCORD_ERR_INVITE_EXPIRED,
    NOSTR_CONCORD_ERR_LIST_FULL,
    NOSTR_CONCORD_ERR_SNAPSHOT_MALFORMED
} nostr_concord_status_t;

const char *nostr_concord_status_string(nostr_concord_status_t status);
bool nostr_concord_is_lower_hex_32(const char *value);

/* Community List functions */
bool nostr_concord_community_list_validate(const nostr_event_t *event);
nostr_concord_status_t nostr_concord_community_list_decrypt(
    const nostr_event_t *event,
    const uint8_t *self_seckey, /* 32-byte */
    uint8_t **decrypted_json,
    size_t *json_len
);
nostr_concord_status_t nostr_concord_community_list_encrypt(
    const uint8_t *self_seckey,
    const char *json_document,
    nostr_event_t **event_out
);

/* Invite Bundle functions */
bool nostr_concord_invite_bundle_validate(const nostr_event_t *event);
nostr_concord_status_t nostr_concord_invite_bundle_decrypt(
    const nostr_event_t *event,
    const uint8_t *token, /* 16-byte unlock token */
    uint8_t **decrypted_bundle,
    size_t *bundle_len
);

/* Direct Invite functions */
bool nostr_concord_direct_invite_validate(const nostr_event_t *event);
nostr_concord_status_t nostr_concord_direct_invite_decrypt(
    const nostr_event_t *event,
    const uint8_t *recipient_seckey,
    uint8_t **decrypted_bundle,
    size_t *bundle_len
);

/* Stream derivation (CORD-01, CORD-02) */
nostr_concord_status_t nostr_concord_derive_group_key(
    const char *label,
    const uint8_t *secret, /* 32-byte */
    const uint8_t *id,     /* 32-byte, or zeroes */
    uint64_t epoch,
    uint8_t *sk_out,       /* 32-byte */
    uint8_t *pk_out        /* 32-byte x-only */
);

nostr_concord_status_t nostr_concord_derive_community_id(
    const uint8_t *owner_xonly,
    const uint8_t *owner_salt,
    uint8_t *community_id_out
);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP_CONCORD_H */
