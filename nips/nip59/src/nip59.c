/**
 * NIP-59: Gift Wrap
 *
 * General-purpose event wrapping for private transmission.
 * Uses NIP-44 encryption with ephemeral sender keys.
 */

#include "nostr/nip59/nip59.h"
#include "nostr/nip44/nip44.h"
#include "nostr-event.h"
#include "nostr-auto-internal.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"
#include "nostr-keys.h"
#include "nostr-utils.h"
#include "secure_buf.h"

#include <openssl/rand.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Default randomization window: 2 days in seconds */
#define NIP59_DEFAULT_TIME_WINDOW (2 * 24 * 60 * 60)

/* Timestamps are drawn from OpenSSL's CSPRNG, the generator behind
 * nostr_key_generate_private() and every NIP-44 nonce. The failure-path
 * test compiles this file with -DNIP59_RAND_BYTES=<stub>; production
 * builds have no seam. */
#ifndef NIP59_RAND_BYTES
#define NIP59_RAND_BYTES RAND_bytes
#else
int NIP59_RAND_BYTES(unsigned char *buf, int num);
#endif

/**
 * Get current unix timestamp
 */
static int64_t get_current_time(void) {
    return (int64_t)time(NULL);
}

/* free() for a string that held secret material. */
static void free_wiped(char *s) {
    if (!s) return;
    secure_wipe(s, strlen(s));
    free(s);
}

/* A uniform draw from [1, n], n >= 1. Rejection sampling instead of a bare
 * modulo keeps it unbiased. */
static int random_offset(uint32_t n, uint32_t *out) {
    /* Draws at or above the largest multiple of n below 2^32 are rejected;
     * that is under half of them, so 64 rejections in a row mean the
     * generator is broken, not unlucky. */
    const uint64_t span = UINT64_C(1) << 32;
    const uint64_t limit = span - (span % n);
    for (int attempt = 0; attempt < 64; attempt++) {
        unsigned char b[4];
        if (NIP59_RAND_BYTES(b, (int)sizeof(b)) != 1)
            return -1;
        uint32_t r = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                     ((uint32_t)b[2] << 8) | (uint32_t)b[3];
        if ((uint64_t)r < limit) {
            *out = 1 + r % n;
            return 0;
        }
    }
    return -1;
}

int nostr_nip59_create_ephemeral_key(char **sk_hex_out, char **pk_hex_out) {
    if (!sk_hex_out || !pk_hex_out) {
        return NIP59_ERR_INVALID_ARG;
    }

    *sk_hex_out = NULL;
    *pk_hex_out = NULL;

    /* Generate ephemeral secret key */
    char *sk = nostr_key_generate_private();
    if (!sk) return NIP59_ERR_KEY_GENERATION;

    /* Derive public key */
    char *pk = nostr_key_get_public(sk);
    if (!pk) {
        free_wiped(sk);
        return NIP59_ERR_KEY_GENERATION;
    }

    *sk_hex_out = sk;
    *pk_hex_out = pk;
    return NIP59_OK;
}

int nostr_nip59_randomize_timestamp(int64_t base_time, uint32_t window_seconds,
                                    int64_t *out_time) {
    if (!out_time) return NIP59_ERR_INVALID_ARG;
    *out_time = 0;

    if (base_time == 0) base_time = get_current_time();
    if (window_seconds == 0) window_seconds = NIP59_DEFAULT_TIME_WINDOW;

    /* Keeps the result non-negative; also catches time() failing (-1). */
    if (base_time < (int64_t)window_seconds) return NIP59_ERR_INVALID_ARG;

    /* No fallback to base_time: the real time is what is being hidden. */
    uint32_t offset = 0;
    if (random_offset(window_seconds, &offset) != 0) return NIP59_ERR_RANDOMNESS;

    *out_time = base_time - (int64_t)offset;
    return NIP59_OK;
}

NostrEvent *nostr_nip59_wrap_with_key(NostrEvent *inner_event,
                                       const char *recipient_pubkey_hex,
                                       const uint8_t ephemeral_sk_bin[32]) {
    if (!inner_event || !recipient_pubkey_hex || !ephemeral_sk_bin) {
        return NULL;
    }

    /* Convert recipient pubkey to binary */
    uint8_t recipient_pk_bin[32];
    if (!nostr_hex2bin(recipient_pk_bin, recipient_pubkey_hex, 32)) {
        return NULL;
    }

    /* Without randomness there is nothing to hide the send time behind:
     * refuse to wrap rather than stamp the real time. */
    int64_t created_at = 0;
    if (nostr_nip59_randomize_timestamp(0, 0, &created_at) != NIP59_OK) {
        return NULL;
    }

    NostrEvent *result = NULL;
    NostrEvent *gift_wrap = NULL;
    char *inner_json = NULL;
    char *encrypted = NULL;
    char *eph_sk_hex = NULL;
    char *eph_pk_hex = NULL;

    /* Serialize inner event to JSON */
    inner_json = nostr_event_serialize_compact(inner_event);
    if (!inner_json) goto out;

    /* Encrypt with NIP-44 */
    int rc = nostr_nip44_encrypt_v2(ephemeral_sk_bin, recipient_pk_bin,
                                     (const uint8_t *)inner_json,
                                     strlen(inner_json),
                                     &encrypted);
    if (rc != 0 || !encrypted) goto out;

    /* Get ephemeral pubkey */
    eph_sk_hex = nostr_bin2hex(ephemeral_sk_bin, 32);
    if (!eph_sk_hex) goto out;

    eph_pk_hex = nostr_key_get_public(eph_sk_hex);
    if (!eph_pk_hex) goto out;

    /* Create gift wrap event */
    gift_wrap = nostr_event_new();
    if (!gift_wrap) goto out;

    nostr_event_set_kind(gift_wrap, NOSTR_KIND_GIFT_WRAP);
    nostr_event_set_pubkey(gift_wrap, eph_pk_hex);
    nostr_event_set_content(gift_wrap, encrypted);
    nostr_event_set_created_at(gift_wrap, created_at);

    /* Add p-tag for recipient */
    NostrTag *ptag = nostr_tag_new("p", recipient_pubkey_hex, NULL);
    if (!ptag) goto out;

    NostrTags *tags = nostr_tags_new(1, ptag);
    if (!tags) {
        nostr_tag_free(ptag);
        goto out;
    }

    nostr_event_set_tags(gift_wrap, tags);

    /* Sign with ephemeral key */
    if (nostr_event_sign(gift_wrap, eph_sk_hex) != 0) goto out;

    result = gift_wrap;
    gift_wrap = NULL;

out:
    /* The ephemeral key and the plaintext it hid, on every path. */
    free_wiped(eph_sk_hex);
    free_wiped(inner_json);
    free(eph_pk_hex);
    free(encrypted);
    if (gift_wrap) nostr_event_free(gift_wrap);
    return result;
}

NostrEvent *nostr_nip59_wrap(NostrEvent *inner_event,
                              const char *recipient_pubkey_hex,
                              const char *ephemeral_sk_hex) {
    if (!inner_event || !recipient_pubkey_hex) {
        return NULL;
    }

    uint8_t eph_sk_bin[32];
    bool generated_key = false;
    char *generated_sk = NULL;

    if (ephemeral_sk_hex) {
        /* Use provided ephemeral key */
        if (!nostr_hex2bin(eph_sk_bin, ephemeral_sk_hex, 32)) {
            secure_wipe(eph_sk_bin, sizeof(eph_sk_bin));
            return NULL;
        }
    } else {
        /* Generate new ephemeral key */
        generated_sk = nostr_key_generate_private();
        if (!generated_sk) {
            return NULL;
        }
        if (!nostr_hex2bin(eph_sk_bin, generated_sk, 32)) {
            secure_wipe(eph_sk_bin, sizeof(eph_sk_bin));
            free_wiped(generated_sk);
            return NULL;
        }
        generated_key = true;
    }

    NostrEvent *result = nostr_nip59_wrap_with_key(inner_event, recipient_pubkey_hex, eph_sk_bin);

    /* Clear sensitive data */
    secure_wipe(eph_sk_bin, sizeof(eph_sk_bin));
    if (generated_key) {
        free_wiped(generated_sk);
    }

    return result;
}

NostrEvent *nostr_nip59_unwrap_with_key(NostrEvent *gift_wrap,
                                         const uint8_t recipient_sk_bin[32]) {
    if (!gift_wrap || !recipient_sk_bin) {
        return NULL;
    }

    /* Never decrypt an unauthenticated or id-mismatched outer event. */
    if (!nostr_nip59_validate_gift_wrap(gift_wrap)) {
        return NULL;
    }

    /* Get encrypted content and sender (ephemeral) pubkey */
    const char *encrypted = nostr_event_get_content(gift_wrap);
    const char *sender_pk_hex = nostr_event_get_pubkey(gift_wrap);

    if (!encrypted || !sender_pk_hex || strlen(encrypted) == 0) {
        return NULL;
    }

    /* Convert sender pubkey to binary */
    uint8_t sender_pk_bin[32];
    if (!nostr_hex2bin(sender_pk_bin, sender_pk_hex, 32)) {
        return NULL;
    }

    /* Decrypt with NIP-44 */
    go_autofree uint8_t *decrypted = NULL;
    size_t decrypted_len = 0;

    int rc = nostr_nip44_decrypt_v2(recipient_sk_bin, sender_pk_bin,
                                     encrypted, &decrypted, &decrypted_len);
    if (rc != 0 || !decrypted) return NULL;

    /* Parse inner event from JSON */
    go_autoptr(NostrEvent) inner_event = nostr_event_new();
    if (!inner_event) return NULL;

    /* Null-terminate the decrypted content */
    go_autofree char *json = malloc(decrypted_len + 1);
    if (!json) return NULL;
    memcpy(json, decrypted, decrypted_len);
    json[decrypted_len] = '\0';

    /* NIP-59 can wrap either a signed event or an unsigned rumor. Try the two
     * strict structural contracts separately so attacker-controlled decrypted
     * JSON never falls back to the permissive signing-template parser. */
    char canonical_id[65];
    NostrEventValidationStatus status =
        nostr_event_deserialize_signed(inner_event, json, NULL);
    bool signed_inner = status == NOSTR_EVENT_VALIDATION_OK;
    if (!signed_inner) {
        status = nostr_event_deserialize_unsigned(inner_event, json, NULL);
        if (status != NOSTR_EVENT_VALIDATION_OK)
            return NULL;
    }

    if (signed_inner) {
        status = nostr_event_validate(inner_event, canonical_id);
    } else if (inner_event->id) {
        status = nostr_event_validate_id(inner_event, canonical_id);
    } else {
        status = nostr_event_compute_id(inner_event, canonical_id);
    }
    if (status != NOSTR_EVENT_VALIDATION_OK)
        return NULL;

    return go_steal_pointer(&inner_event);
}

NostrEvent *nostr_nip59_unwrap(NostrEvent *gift_wrap,
                                const char *recipient_sk_hex) {
    if (!gift_wrap || !recipient_sk_hex) {
        return NULL;
    }

    uint8_t recipient_sk_bin[32];
    if (!nostr_hex2bin(recipient_sk_bin, recipient_sk_hex, 32)) {
        return NULL;
    }

    NostrEvent *result = nostr_nip59_unwrap_with_key(gift_wrap, recipient_sk_bin);

    /* Clear sensitive data */
    secure_wipe(recipient_sk_bin, sizeof(recipient_sk_bin));

    return result;
}

bool nostr_nip59_validate_gift_wrap(NostrEvent *gift_wrap) {
    if (!gift_wrap) {
        return false;
    }

    /* Check kind */
    if (nostr_event_get_kind(gift_wrap) != NOSTR_KIND_GIFT_WRAP) {
        return false;
    }

    /* Bind the declared id and verify the signature in one pass. */
    if (nostr_event_validate(gift_wrap, NULL) != NOSTR_EVENT_VALIDATION_OK) {
        return false;
    }

    /* Check for non-empty content */
    const char *content = nostr_event_get_content(gift_wrap);
    if (!content || strlen(content) == 0) {
        return false;
    }

    /* Check for p-tag */
    NostrTags *tags = nostr_event_get_tags(gift_wrap);
    if (!tags || nostr_tags_size(tags) == 0) {
        return false;
    }

    NostrTag *prefix = nostr_tag_new("p", NULL);
    NostrTag *ptag = nostr_tags_get_first(tags, prefix);
    nostr_tag_free(prefix);

    if (!ptag) {
        return false;
    }

    /* Verify p-tag has a value (recipient pubkey) */
    if (nostr_tag_size(ptag) < 2) {
        return false;
    }

    const char *recipient = nostr_tag_get(ptag, 1);
    if (!recipient || strlen(recipient) != 64) {
        return false;
    }

    return true;
}

char *nostr_nip59_get_recipient(NostrEvent *gift_wrap) {
    if (!gift_wrap) {
        return NULL;
    }

    NostrTags *tags = nostr_event_get_tags(gift_wrap);
    if (!tags) {
        return NULL;
    }

    NostrTag *prefix = nostr_tag_new("p", NULL);
    NostrTag *ptag = nostr_tags_get_first(tags, prefix);
    nostr_tag_free(prefix);

    if (!ptag || nostr_tag_size(ptag) < 2) {
        return NULL;
    }

    const char *recipient = nostr_tag_get(ptag, 1);
    if (!recipient) {
        return NULL;
    }

    return strdup(recipient);
}

bool nostr_nip59_is_gift_wrap(NostrEvent *event) {
    if (!event) {
        return false;
    }
    return nostr_event_get_kind(event) == NOSTR_KIND_GIFT_WRAP;
}
