/**
 * NIP-17: Private Direct Messages
 *
 * Three-layer encryption: Rumor -> Seal -> Gift Wrap
 */

#include "nostr/nip17/nip17.h"
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

/* Max randomization window for gift wrap timestamp (2 days in seconds) */
#define GW_TIME_WINDOW (2 * 24 * 60 * 60)

/**
 * Get current unix timestamp
 */
static int64_t get_current_time(void) {
    return (int64_t)time(NULL);
}

/**
 * Get randomized timestamp within window for metadata protection
 */
static int64_t get_randomized_time(void) {
    int64_t now = get_current_time();
    /* Generate ephemeral key and use first 4 bytes as random source */
    char *rand_key = nostr_key_generate_private();
    if (!rand_key) return now;

    unsigned char rand_bytes[4];
    nostr_hex2bin(rand_bytes, rand_key, 4);
    free(rand_key);

    uint32_t rand_val = (rand_bytes[0] << 24) | (rand_bytes[1] << 16) |
                        (rand_bytes[2] << 8) | rand_bytes[3];
    int64_t offset = rand_val % GW_TIME_WINDOW;

    return now - offset;
}

NostrEvent *nostr_nip17_create_rumor(const char *sender_pubkey_hex,
                                      const char *recipient_pubkey_hex,
                                      const char *content,
                                      int64_t created_at) {
    if (!sender_pubkey_hex || !recipient_pubkey_hex || !content) {
        return NULL;
    }

    NostrEvent *rumor = nostr_event_new();
    if (!rumor) return NULL;

    nostr_event_set_kind(rumor, NOSTR_KIND_DIRECT_MESSAGE);
    nostr_event_set_pubkey(rumor, sender_pubkey_hex);
    nostr_event_set_content(rumor, content);
    nostr_event_set_created_at(rumor, created_at ? created_at : get_current_time());

    /* Add p-tag for recipient */
    NostrTag *ptag = nostr_tag_new("p", recipient_pubkey_hex, NULL);
    if (!ptag) {
        nostr_event_free(rumor);
        return NULL;
    }

    NostrTags *tags = nostr_tags_new(1, ptag);
    if (!tags) {
        nostr_tag_free(ptag);
        nostr_event_free(rumor);
        return NULL;
    }

    nostr_event_set_tags(rumor, tags);

    /* Rumor is NOT signed - sig remains NULL */
    return rumor;
}

NostrEvent *nostr_nip17_create_seal(NostrEvent *rumor,
                                     const char *sender_sk_hex,
                                     const char *recipient_pubkey_hex) {
    if (!rumor || !sender_sk_hex || !recipient_pubkey_hex) {
        return NULL;
    }

    /* Serialize rumor to JSON */
    char *rumor_json = nostr_event_serialize_compact(rumor);
    if (!rumor_json) return NULL;

    /* Convert keys to binary for NIP-44 */
    unsigned char sender_sk[32];
    unsigned char recipient_pk[32];

    if (!nostr_hex2bin(sender_sk, sender_sk_hex, 32)) {
        free(rumor_json);
        return NULL;
    }

    if (!nostr_hex2bin(recipient_pk, recipient_pubkey_hex, 32)) {
        memset(sender_sk, 0, 32);
        free(rumor_json);
        return NULL;
    }

    /* Encrypt rumor JSON with NIP-44 */
    char *encrypted = NULL;
    int rc = nostr_nip44_encrypt_v2(sender_sk, recipient_pk,
                                     (const uint8_t *)rumor_json,
                                     strlen(rumor_json),
                                     &encrypted);

    memset(sender_sk, 0, 32);
    free(rumor_json);

    if (rc != 0 || !encrypted) {
        return NULL;
    }

    /* Create seal event */
    NostrEvent *seal = nostr_event_new();
    if (!seal) {
        free(encrypted);
        return NULL;
    }

    /* Get sender pubkey from secret key */
    char *sender_pubkey = nostr_key_get_public(sender_sk_hex);
    if (!sender_pubkey) {
        free(encrypted);
        nostr_event_free(seal);
        return NULL;
    }

    nostr_event_set_kind(seal, NOSTR_KIND_SEAL);
    nostr_event_set_pubkey(seal, sender_pubkey);
    nostr_event_set_content(seal, encrypted);
    nostr_event_set_created_at(seal, get_current_time());

    free(sender_pubkey);
    free(encrypted);

    /* Sign the seal with sender's key */
    if (nostr_event_sign(seal, sender_sk_hex) != 0) {
        nostr_event_free(seal);
        return NULL;
    }

    return seal;
}

NostrEvent *nostr_nip17_create_gift_wrap(NostrEvent *seal,
                                          const char *recipient_pubkey_hex) {
    if (!seal || !recipient_pubkey_hex) {
        return NULL;
    }

    /* Generate ephemeral keypair */
    char *ephemeral_sk = nostr_key_generate_private();
    if (!ephemeral_sk) return NULL;

    char *ephemeral_pk = nostr_key_get_public(ephemeral_sk);
    if (!ephemeral_pk) {
        free(ephemeral_sk);
        return NULL;
    }

    /* Serialize seal to JSON */
    char *seal_json = nostr_event_serialize_compact(seal);
    if (!seal_json) {
        free(ephemeral_sk);
        free(ephemeral_pk);
        return NULL;
    }

    /* Convert keys to binary for NIP-44 */
    unsigned char eph_sk[32];
    unsigned char recipient_pk[32];

    if (!nostr_hex2bin(eph_sk, ephemeral_sk, 32)) {
        free(ephemeral_sk);
        free(ephemeral_pk);
        free(seal_json);
        return NULL;
    }

    if (!nostr_hex2bin(recipient_pk, recipient_pubkey_hex, 32)) {
        memset(eph_sk, 0, 32);
        free(ephemeral_sk);
        free(ephemeral_pk);
        free(seal_json);
        return NULL;
    }

    /* Encrypt seal JSON with NIP-44 using ephemeral key */
    char *encrypted = NULL;
    int rc = nostr_nip44_encrypt_v2(eph_sk, recipient_pk,
                                     (const uint8_t *)seal_json,
                                     strlen(seal_json),
                                     &encrypted);

    memset(eph_sk, 0, 32);
    free(seal_json);

    if (rc != 0 || !encrypted) {
        free(ephemeral_sk);
        free(ephemeral_pk);
        return NULL;
    }

    /* Create gift wrap event */
    NostrEvent *gift_wrap = nostr_event_new();
    if (!gift_wrap) {
        free(ephemeral_sk);
        free(ephemeral_pk);
        free(encrypted);
        return NULL;
    }

    nostr_event_set_kind(gift_wrap, NOSTR_KIND_GIFT_WRAP);
    nostr_event_set_pubkey(gift_wrap, ephemeral_pk);
    nostr_event_set_content(gift_wrap, encrypted);
    nostr_event_set_created_at(gift_wrap, get_randomized_time());

    free(encrypted);

    /* Add p-tag for recipient */
    NostrTag *ptag = nostr_tag_new("p", recipient_pubkey_hex, NULL);
    if (!ptag) {
        free(ephemeral_sk);
        free(ephemeral_pk);
        nostr_event_free(gift_wrap);
        return NULL;
    }

    NostrTags *tags = nostr_tags_new(1, ptag);
    if (!tags) {
        nostr_tag_free(ptag);
        free(ephemeral_sk);
        free(ephemeral_pk);
        nostr_event_free(gift_wrap);
        return NULL;
    }

    nostr_event_set_tags(gift_wrap, tags);

    /* Sign with ephemeral key */
    if (nostr_event_sign(gift_wrap, ephemeral_sk) != 0) {
        free(ephemeral_sk);
        free(ephemeral_pk);
        nostr_event_free(gift_wrap);
        return NULL;
    }

    free(ephemeral_sk);
    free(ephemeral_pk);

    return gift_wrap;
}

NostrEvent *nostr_nip17_wrap_dm(const char *sender_sk_hex,
                                 const char *recipient_pubkey_hex,
                                 const char *content) {
    if (!sender_sk_hex || !recipient_pubkey_hex || !content) {
        return NULL;
    }

    /* Get sender pubkey */
    char *sender_pubkey = nostr_key_get_public(sender_sk_hex);
    if (!sender_pubkey) return NULL;

    /* Create rumor */
    NostrEvent *rumor = nostr_nip17_create_rumor(sender_pubkey, recipient_pubkey_hex,
                                                  content, 0);
    free(sender_pubkey);
    if (!rumor) return NULL;

    /* Create seal */
    NostrEvent *seal = nostr_nip17_create_seal(rumor, sender_sk_hex, recipient_pubkey_hex);
    nostr_event_free(rumor);
    if (!seal) return NULL;

    /* Create gift wrap */
    NostrEvent *gift_wrap = nostr_nip17_create_gift_wrap(seal, recipient_pubkey_hex);
    nostr_event_free(seal);

    return gift_wrap;
}

NostrEvent *nostr_nip17_unwrap_gift_wrap(NostrEvent *gift_wrap,
                                          const char *recipient_sk_hex) {
    if (!gift_wrap || !recipient_sk_hex) {
        return NULL;
    }

    if (nostr_event_get_kind(gift_wrap) != NOSTR_KIND_GIFT_WRAP) {
        return NULL;
    }

    const char *encrypted = nostr_event_get_content(gift_wrap);
    const char *sender_pk_hex = nostr_event_get_pubkey(gift_wrap);

    if (!encrypted || !sender_pk_hex) {
        return NULL;
    }

    /* Convert keys to binary */
    unsigned char recipient_sk[32];
    unsigned char sender_pk[32];

    if (!nostr_hex2bin(recipient_sk, recipient_sk_hex, 32)) {
        return NULL;
    }

    if (!nostr_hex2bin(sender_pk, sender_pk_hex, 32)) {
        memset(recipient_sk, 0, 32);
        return NULL;
    }

    /* Decrypt with NIP-44 */
    uint8_t *decrypted = NULL;
    size_t decrypted_len = 0;

    int rc = nostr_nip44_decrypt_v2(recipient_sk, sender_pk,
                                     encrypted, &decrypted, &decrypted_len);

    memset(recipient_sk, 0, 32);

    if (rc != 0 || !decrypted) {
        return NULL;
    }

    /* Parse seal event from JSON */
    NostrEvent *seal = nostr_event_new();
    if (!seal) {
        free(decrypted);
        return NULL;
    }

    /* Null-terminate the decrypted content */
    char *json = malloc(decrypted_len + 1);
    if (!json) {
        free(decrypted);
        nostr_event_free(seal);
        return NULL;
    }
    memcpy(json, decrypted, decrypted_len);
    json[decrypted_len] = '\0';
    free(decrypted);

    if (!nostr_event_deserialize_compact(seal, json)) {
        free(json);
        nostr_event_free(seal);
        return NULL;
    }

    free(json);
    return seal;
}

NostrEvent *nostr_nip17_unwrap_seal(NostrEvent *seal,
                                     const char *recipient_sk_hex) {
    if (!seal || !recipient_sk_hex) {
        return NULL;
    }

    if (nostr_event_get_kind(seal) != NOSTR_KIND_SEAL) {
        return NULL;
    }

    const char *encrypted = nostr_event_get_content(seal);
    const char *sender_pk_hex = nostr_event_get_pubkey(seal);

    if (!encrypted || !sender_pk_hex) {
        return NULL;
    }

    /* Convert keys to binary */
    unsigned char recipient_sk[32];
    unsigned char sender_pk[32];

    if (!nostr_hex2bin(recipient_sk, recipient_sk_hex, 32)) {
        return NULL;
    }

    if (!nostr_hex2bin(sender_pk, sender_pk_hex, 32)) {
        memset(recipient_sk, 0, 32);
        return NULL;
    }

    /* Decrypt with NIP-44 */
    uint8_t *decrypted = NULL;
    size_t decrypted_len = 0;

    int rc = nostr_nip44_decrypt_v2(recipient_sk, sender_pk,
                                     encrypted, &decrypted, &decrypted_len);

    memset(recipient_sk, 0, 32);

    if (rc != 0 || !decrypted) {
        return NULL;
    }

    /* Parse rumor event from JSON */
    NostrEvent *rumor = nostr_event_new();
    if (!rumor) {
        free(decrypted);
        return NULL;
    }

    /* Null-terminate the decrypted content */
    char *json = malloc(decrypted_len + 1);
    if (!json) {
        free(decrypted);
        nostr_event_free(rumor);
        return NULL;
    }
    memcpy(json, decrypted, decrypted_len);
    json[decrypted_len] = '\0';
    free(decrypted);

    if (!nostr_event_deserialize_compact(rumor, json)) {
        free(json);
        nostr_event_free(rumor);
        return NULL;
    }

    free(json);
    return rumor;
}

int nostr_nip17_decrypt_dm(NostrEvent *gift_wrap,
                            const char *recipient_sk_hex,
                            char **content_out,
                            char **sender_pubkey_out) {
    if (!gift_wrap || !recipient_sk_hex || !content_out) {
        return -EINVAL;
    }

    *content_out = NULL;
    if (sender_pubkey_out) *sender_pubkey_out = NULL;

    /* Unwrap gift wrap to get seal */
    NostrEvent *seal = nostr_nip17_unwrap_gift_wrap(gift_wrap, recipient_sk_hex);
    if (!seal) {
        return -EINVAL;
    }

    /* Validate seal signature */
    if (!nostr_event_check_signature(seal)) {
        nostr_event_free(seal);
        return -EINVAL;
    }

    /* Unwrap seal to get rumor */
    NostrEvent *rumor = nostr_nip17_unwrap_seal(seal, recipient_sk_hex);
    if (!rumor) {
        nostr_event_free(seal);
        return -EINVAL;
    }

    /* Validate: seal pubkey must match rumor pubkey (prevents sender spoofing) */
    const char *seal_pk = nostr_event_get_pubkey(seal);
    const char *rumor_pk = nostr_event_get_pubkey(rumor);

    if (!seal_pk || !rumor_pk || strcmp(seal_pk, rumor_pk) != 0) {
        nostr_event_free(seal);
        nostr_event_free(rumor);
        return -EINVAL;
    }

    /* Extract content */
    const char *content = nostr_event_get_content(rumor);
    if (!content) {
        nostr_event_free(seal);
        nostr_event_free(rumor);
        return -EINVAL;
    }

    *content_out = strdup(content);
    if (!*content_out) {
        nostr_event_free(seal);
        nostr_event_free(rumor);
        return -ENOMEM;
    }

    if (sender_pubkey_out) {
        *sender_pubkey_out = strdup(rumor_pk);
    }

    nostr_event_free(seal);
    nostr_event_free(rumor);

    return 0;
}

bool nostr_nip17_validate_gift_wrap(NostrEvent *gift_wrap) {
    if (!gift_wrap) return false;

    /* Check kind */
    if (nostr_event_get_kind(gift_wrap) != NOSTR_KIND_GIFT_WRAP) {
        return false;
    }

    /* Check signature */
    if (!nostr_event_check_signature(gift_wrap)) {
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

    return true;
}

bool nostr_nip17_validate_seal(NostrEvent *seal, NostrEvent *rumor) {
    if (!seal) return false;

    /* Check kind */
    if (nostr_event_get_kind(seal) != NOSTR_KIND_SEAL) {
        return false;
    }

    /* Check signature */
    if (!nostr_event_check_signature(seal)) {
        return false;
    }

    /* If rumor provided, validate pubkey consistency */
    if (rumor) {
        const char *seal_pk = nostr_event_get_pubkey(seal);
        const char *rumor_pk = nostr_event_get_pubkey(rumor);

        if (!seal_pk || !rumor_pk || strcmp(seal_pk, rumor_pk) != 0) {
            return false;
        }
    }

    return true;
}

/* ---- DM Relay Preferences (Kind 10050) ---- */

NostrEvent *nostr_nip17_create_dm_relay_list(const char **relays,
                                              const char *sk_hex) {
    if (!relays || !sk_hex) {
        return NULL;
    }

    /* Get public key from secret key */
    char *pubkey = nostr_key_get_public(sk_hex);
    if (!pubkey) return NULL;

    NostrEvent *event = nostr_event_new();
    if (!event) {
        free(pubkey);
        return NULL;
    }

    nostr_event_set_kind(event, NOSTR_KIND_DM_RELAY_LIST);
    nostr_event_set_pubkey(event, pubkey);
    nostr_event_set_content(event, "");
    nostr_event_set_created_at(event, get_current_time());

    free(pubkey);

    /* Count relays and create tags */
    size_t count = 0;
    while (relays[count]) count++;

    if (count == 0) {
        nostr_event_free(event);
        return NULL;
    }

    /* Create tags array */
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) {
        nostr_event_free(event);
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        NostrTag *tag = nostr_tag_new("relay", relays[i], NULL);
        if (tag) {
            nostr_tags_append(tags, tag);
        }
    }

    nostr_event_set_tags(event, tags);

    /* Sign the event */
    if (nostr_event_sign(event, sk_hex) != 0) {
        nostr_event_free(event);
        return NULL;
    }

    return event;
}

NostrDmRelayList *nostr_nip17_parse_dm_relay_list(NostrEvent *event) {
    if (!event) return NULL;

    /* Verify kind */
    if (nostr_event_get_kind(event) != NOSTR_KIND_DM_RELAY_LIST) {
        return NULL;
    }

    NostrTags *tags = nostr_event_get_tags(event);
    if (!tags) return NULL;

    /* Count relay tags */
    size_t count = 0;
    size_t total_tags = nostr_tags_size(tags);

    for (size_t i = 0; i < total_tags; i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (tag && nostr_tag_size(tag) >= 2) {
            const char *key = nostr_tag_get(tag, 0);
            if (key && strcmp(key, "relay") == 0) {
                count++;
            }
        }
    }

    if (count == 0) return NULL;

    /* Allocate result */
    NostrDmRelayList *list = malloc(sizeof(NostrDmRelayList));
    if (!list) return NULL;

    list->relays = calloc(count + 1, sizeof(char *));
    if (!list->relays) {
        free(list);
        return NULL;
    }

    list->count = 0;

    /* Extract relay URLs */
    for (size_t i = 0; i < total_tags; i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (tag && nostr_tag_size(tag) >= 2) {
            const char *key = nostr_tag_get(tag, 0);
            if (key && strcmp(key, "relay") == 0) {
                const char *url = nostr_tag_get(tag, 1);
                if (url) {
                    list->relays[list->count] = strdup(url);
                    if (list->relays[list->count]) {
                        list->count++;
                    }
                }
            }
        }
    }

    list->relays[list->count] = NULL;  /* NULL terminate */

    return list;
}

void nostr_nip17_free_dm_relay_list(NostrDmRelayList *list) {
    if (!list) return;

    if (list->relays) {
        for (size_t i = 0; i < list->count; i++) {
            free(list->relays[i]);
        }
        free(list->relays);
    }

    free(list);
}

NostrDmRelayList *nostr_nip17_get_dm_relays_from_event(NostrEvent *event,
                                                        const char **default_relays) {
    /* Try to parse from event first */
    if (event) {
        NostrDmRelayList *list = nostr_nip17_parse_dm_relay_list(event);
        if (list && list->count > 0) {
            return list;
        }
        nostr_nip17_free_dm_relay_list(list);
    }

    /* Fall back to defaults */
    if (!default_relays || !default_relays[0]) {
        return NULL;
    }

    /* Count defaults */
    size_t count = 0;
    while (default_relays[count]) count++;

    NostrDmRelayList *list = malloc(sizeof(NostrDmRelayList));
    if (!list) return NULL;

    list->relays = calloc(count + 1, sizeof(char *));
    if (!list->relays) {
        free(list);
        return NULL;
    }

    list->count = 0;
    for (size_t i = 0; i < count; i++) {
        list->relays[i] = strdup(default_relays[i]);
        if (list->relays[i]) {
            list->count++;
        }
    }
    list->relays[list->count] = NULL;

    return list;
}
