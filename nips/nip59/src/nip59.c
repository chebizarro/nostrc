/**
 * NIP-59: Gift Wrap
 *
 * General-purpose event wrapping for private transmission.
 * Uses NIP-44 encryption with ephemeral sender keys.
 */

#include "nostr/nip59/nip59.h"
#include "nostr/nip44/nip44.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"
#include "nostr-keys.h"
#include "nostr-utils.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Default randomization window: 2 days in seconds */
#define NIP59_DEFAULT_TIME_WINDOW (2 * 24 * 60 * 60)

/**
 * Get current unix timestamp
 */
static int64_t get_current_time(void) {
    return (int64_t)time(NULL);
}

/**
 * Generate random bytes using key generation as entropy source
 */
static int get_random_bytes(uint8_t *out, size_t len) {
    /* Use key generation as source of randomness */
    while (len > 0) {
        char *rand_key = nostr_key_generate_private();
        if (!rand_key) return -1;

        size_t copy_len = (len >= 32) ? 32 : len;
        if (!nostr_hex2bin(out, rand_key, copy_len)) {
            free(rand_key);
            return -1;
        }
        free(rand_key);

        out += copy_len;
        len -= copy_len;
    }
    return 0;
}

int nostr_nip59_create_ephemeral_key(char **sk_hex_out, char **pk_hex_out) {
    if (!sk_hex_out || !pk_hex_out) {
        return NIP59_ERR_INVALID_ARG;
    }

    *sk_hex_out = NULL;
    *pk_hex_out = NULL;

    /* Generate ephemeral secret key */
    char *sk = nostr_key_generate_private();
    if (!sk) {
        return NIP59_ERR_KEY_GENERATION;
    }

    /* Derive public key */
    char *pk = nostr_key_get_public(sk);
    if (!pk) {
        free(sk);
        return NIP59_ERR_KEY_GENERATION;
    }

    *sk_hex_out = sk;
    *pk_hex_out = pk;
    return NIP59_OK;
}

int64_t nostr_nip59_randomize_timestamp(int64_t base_time, uint32_t window_seconds) {
    /* Use current time if base_time is 0 */
    if (base_time == 0) {
        base_time = get_current_time();
    }

    /* Use default window if not specified */
    if (window_seconds == 0) {
        window_seconds = NIP59_DEFAULT_TIME_WINDOW;
    }

    /* Generate random offset */
    uint8_t rand_bytes[4];
    if (get_random_bytes(rand_bytes, 4) != 0) {
        /* Fallback: just return base time if randomness fails */
        return base_time;
    }

    uint32_t rand_val = ((uint32_t)rand_bytes[0] << 24) |
                        ((uint32_t)rand_bytes[1] << 16) |
                        ((uint32_t)rand_bytes[2] << 8) |
                        (uint32_t)rand_bytes[3];

    /* Offset is in the past (subtract from base) */
    int64_t offset = rand_val % window_seconds;

    return base_time - offset;
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

    /* Serialize inner event to JSON */
    char *inner_json = nostr_event_serialize_compact(inner_event);
    if (!inner_json) {
        return NULL;
    }

    /* Encrypt with NIP-44 */
    char *encrypted = NULL;
    int rc = nostr_nip44_encrypt_v2(ephemeral_sk_bin, recipient_pk_bin,
                                     (const uint8_t *)inner_json,
                                     strlen(inner_json),
                                     &encrypted);
    free(inner_json);

    if (rc != 0 || !encrypted) {
        return NULL;
    }

    /* Get ephemeral pubkey */
    char *eph_sk_hex = nostr_bin2hex(ephemeral_sk_bin, 32);
    if (!eph_sk_hex) {
        free(encrypted);
        return NULL;
    }

    char *eph_pk_hex = nostr_key_get_public(eph_sk_hex);
    if (!eph_pk_hex) {
        free(eph_sk_hex);
        free(encrypted);
        return NULL;
    }

    /* Create gift wrap event */
    NostrEvent *gift_wrap = nostr_event_new();
    if (!gift_wrap) {
        free(eph_sk_hex);
        free(eph_pk_hex);
        free(encrypted);
        return NULL;
    }

    nostr_event_set_kind(gift_wrap, NOSTR_KIND_GIFT_WRAP);
    nostr_event_set_pubkey(gift_wrap, eph_pk_hex);
    nostr_event_set_content(gift_wrap, encrypted);
    nostr_event_set_created_at(gift_wrap, nostr_nip59_randomize_timestamp(0, 0));

    free(encrypted);

    /* Add p-tag for recipient */
    NostrTag *ptag = nostr_tag_new("p", recipient_pubkey_hex, NULL);
    if (!ptag) {
        free(eph_sk_hex);
        free(eph_pk_hex);
        nostr_event_free(gift_wrap);
        return NULL;
    }

    NostrTags *tags = nostr_tags_new(1, ptag);
    if (!tags) {
        nostr_tag_free(ptag);
        free(eph_sk_hex);
        free(eph_pk_hex);
        nostr_event_free(gift_wrap);
        return NULL;
    }

    nostr_event_set_tags(gift_wrap, tags);

    /* Sign with ephemeral key */
    if (nostr_event_sign(gift_wrap, eph_sk_hex) != 0) {
        free(eph_sk_hex);
        free(eph_pk_hex);
        nostr_event_free(gift_wrap);
        return NULL;
    }

    /* Clear ephemeral secret key */
    memset(eph_sk_hex, 0, strlen(eph_sk_hex));
    free(eph_sk_hex);
    free(eph_pk_hex);

    return gift_wrap;
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
            return NULL;
        }
    } else {
        /* Generate new ephemeral key */
        generated_sk = nostr_key_generate_private();
        if (!generated_sk) {
            return NULL;
        }
        if (!nostr_hex2bin(eph_sk_bin, generated_sk, 32)) {
            memset(generated_sk, 0, strlen(generated_sk));
            free(generated_sk);
            return NULL;
        }
        generated_key = true;
    }

    NostrEvent *result = nostr_nip59_wrap_with_key(inner_event, recipient_pubkey_hex, eph_sk_bin);

    /* Clear sensitive data */
    memset(eph_sk_bin, 0, 32);
    if (generated_key && generated_sk) {
        memset(generated_sk, 0, strlen(generated_sk));
        free(generated_sk);
    }

    return result;
}

NostrEvent *nostr_nip59_unwrap_with_key(NostrEvent *gift_wrap,
                                         const uint8_t recipient_sk_bin[32]) {
    if (!gift_wrap || !recipient_sk_bin) {
        return NULL;
    }

    /* Verify kind */
    if (nostr_event_get_kind(gift_wrap) != NOSTR_KIND_GIFT_WRAP) {
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
    uint8_t *decrypted = NULL;
    size_t decrypted_len = 0;

    int rc = nostr_nip44_decrypt_v2(recipient_sk_bin, sender_pk_bin,
                                     encrypted, &decrypted, &decrypted_len);

    if (rc != 0 || !decrypted) {
        return NULL;
    }

    /* Parse inner event from JSON */
    NostrEvent *inner_event = nostr_event_new();
    if (!inner_event) {
        free(decrypted);
        return NULL;
    }

    /* Null-terminate the decrypted content */
    char *json = malloc(decrypted_len + 1);
    if (!json) {
        free(decrypted);
        nostr_event_free(inner_event);
        return NULL;
    }
    memcpy(json, decrypted, decrypted_len);
    json[decrypted_len] = '\0';
    free(decrypted);

    if (!nostr_event_deserialize_compact(inner_event, json)) {
        free(json);
        nostr_event_free(inner_event);
        return NULL;
    }

    free(json);
    return inner_event;
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
    memset(recipient_sk_bin, 0, 32);

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

    /* Check signature */
    if (!nostr_event_check_signature(gift_wrap)) {
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
