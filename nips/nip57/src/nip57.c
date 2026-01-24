/**
 * NIP-57: Lightning Zaps Implementation
 *
 * Implements zap requests (kind 9734) and zap receipts (kind 9735).
 */

#include "nostr/nip57/nip57.h"
#include "nostr/nip57/nip57_types.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"
#include "nostr-keys.h"
#include "nostr-utils.h"

/* BOLT11 parsing from nostrdb */
#include "bolt11/bolt11.h"
#include "ccan/tal/tal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>

/* ============================================================================
 * Memory Management
 * ============================================================================ */

void nostr_nip57_zap_request_free(NostrZapRequest *req) {
    if (!req) return;

    if (req->relays) {
        for (size_t i = 0; i < req->relay_count; i++) {
            free(req->relays[i]);
        }
        free(req->relays);
    }
    free(req->lnurl);
    free(req->content);
    free(req->recipient_pubkey);
    free(req->event_id);
    free(req->event_coordinate);
    free(req->sender_pubkey);
    free(req);
}

void nostr_nip57_zap_receipt_free(NostrZapReceipt *receipt) {
    if (!receipt) return;

    free(receipt->bolt11);
    free(receipt->preimage);
    free(receipt->description);
    free(receipt->recipient_pubkey);
    free(receipt->sender_pubkey);
    free(receipt->event_id);
    free(receipt->event_coordinate);
    free(receipt->provider_pubkey);
    free(receipt);
}

void nostr_nip57_lnurl_pay_info_free(NostrLnurlPayInfo *info) {
    if (!info) return;

    free(info->callback);
    free(info->nostr_pubkey);
    free(info);
}

void nostr_nip57_zap_split_config_free(NostrZapSplitConfig *config) {
    if (!config) return;

    if (config->splits) {
        for (size_t i = 0; i < config->count; i++) {
            free(config->splits[i].pubkey);
            free(config->splits[i].relay);
        }
        free(config->splits);
    }
    free(config);
}

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/**
 * Get current unix timestamp
 */
static int64_t get_current_time(void) {
    return (int64_t)time(NULL);
}

/**
 * URL-encode a string for use in query parameters.
 * Caller must free the result.
 */
static char *url_encode(const char *str) {
    if (!str) return NULL;

    size_t len = strlen(str);
    /* Worst case: every char needs %XX encoding */
    char *encoded = malloc(len * 3 + 1);
    if (!encoded) return NULL;

    char *out = encoded;
    for (const char *p = str; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            *out++ = c;
        } else {
            snprintf(out, 4, "%%%02X", c);
            out += 3;
        }
    }
    *out = '\0';
    return encoded;
}

/**
 * Find a tag by key and return its value at the given index.
 * Returns NULL if not found.
 */
static const char *find_tag_value(NostrTags *tags, const char *key, size_t value_index) {
    if (!tags || !key) return NULL;

    size_t total = nostr_tags_size(tags);
    for (size_t i = 0; i < total; i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < value_index + 2) continue;

        const char *tag_key = nostr_tag_get(tag, 0);
        if (tag_key && strcmp(tag_key, key) == 0) {
            return nostr_tag_get(tag, value_index + 1);
        }
    }
    return NULL;
}

/**
 * Count tags with a given key
 */
static size_t count_tags(NostrTags *tags, const char *key) {
    if (!tags || !key) return 0;

    size_t count = 0;
    size_t total = nostr_tags_size(tags);
    for (size_t i = 0; i < total; i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 1) continue;

        const char *tag_key = nostr_tag_get(tag, 0);
        if (tag_key && strcmp(tag_key, key) == 0) {
            count++;
        }
    }
    return count;
}

/**
 * Parse relays from a relays tag.
 * Returns NULL-terminated array. Caller must free.
 */
static char **parse_relays_tag(NostrTags *tags, size_t *count_out) {
    if (!tags) {
        if (count_out) *count_out = 0;
        return NULL;
    }

    /* Find relays tag */
    size_t total = nostr_tags_size(tags);
    for (size_t i = 0; i < total; i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 2) continue;

        const char *tag_key = nostr_tag_get(tag, 0);
        if (tag_key && strcmp(tag_key, "relays") == 0) {
            /* Found relays tag, extract all values */
            size_t relay_count = nostr_tag_size(tag) - 1;
            char **relays = calloc(relay_count + 1, sizeof(char *));
            if (!relays) {
                if (count_out) *count_out = 0;
                return NULL;
            }

            size_t actual_count = 0;
            for (size_t j = 1; j <= relay_count; j++) {
                const char *relay = nostr_tag_get(tag, j);
                if (relay) {
                    relays[actual_count] = strdup(relay);
                    if (relays[actual_count]) {
                        actual_count++;
                    }
                }
            }
            relays[actual_count] = NULL;

            if (count_out) *count_out = actual_count;
            return relays;
        }
    }

    if (count_out) *count_out = 0;
    return NULL;
}

/* ============================================================================
 * Zap Request Functions
 * ============================================================================ */

NostrEvent *nostr_nip57_create_zap_request(const char *sender_sk_hex,
                                            const char *recipient_pubkey_hex,
                                            const char **relays,
                                            uint64_t amount_msats,
                                            const char *lnurl,
                                            const char *content,
                                            const char *event_id_hex,
                                            const char *event_coordinate,
                                            int event_kind) {
    if (!sender_sk_hex || !recipient_pubkey_hex || !relays || !relays[0]) {
        return NULL;
    }

    /* Get sender pubkey */
    char *sender_pubkey = nostr_key_get_public(sender_sk_hex);
    if (!sender_pubkey) return NULL;

    NostrEvent *event = nostr_event_new();
    if (!event) {
        free(sender_pubkey);
        return NULL;
    }

    nostr_event_set_kind(event, NOSTR_NIP57_KIND_ZAP_REQUEST);
    nostr_event_set_pubkey(event, sender_pubkey);
    nostr_event_set_content(event, content ? content : "");
    nostr_event_set_created_at(event, get_current_time());

    free(sender_pubkey);

    /* Count required tags */
    size_t tag_count = 2; /* relays + p */
    if (amount_msats > 0) tag_count++;
    if (lnurl) tag_count++;
    if (event_id_hex) tag_count++;
    if (event_coordinate) tag_count++;
    if (event_kind >= 0) tag_count++;

    /* Create tags */
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) {
        nostr_event_free(event);
        return NULL;
    }

    /* Relays tag - count relays first */
    size_t relay_count = 0;
    while (relays[relay_count]) relay_count++;

    NostrTag *relays_tag = nostr_tag_new("relays", NULL);
    if (!relays_tag) {
        nostr_tags_free(tags);
        nostr_event_free(event);
        return NULL;
    }
    for (size_t i = 0; i < relay_count; i++) {
        nostr_tag_add(relays_tag, relays[i]);
    }
    nostr_tags_append(tags, relays_tag);

    /* Amount tag */
    if (amount_msats > 0) {
        char amount_str[32];
        snprintf(amount_str, sizeof(amount_str), "%llu", (unsigned long long)amount_msats);
        NostrTag *amount_tag = nostr_tag_new("amount", amount_str, NULL);
        if (amount_tag) {
            nostr_tags_append(tags, amount_tag);
        }
    }

    /* LNURL tag */
    if (lnurl) {
        NostrTag *lnurl_tag = nostr_tag_new("lnurl", lnurl, NULL);
        if (lnurl_tag) {
            nostr_tags_append(tags, lnurl_tag);
        }
    }

    /* P tag (recipient) */
    NostrTag *p_tag = nostr_tag_new("p", recipient_pubkey_hex, NULL);
    if (p_tag) {
        nostr_tags_append(tags, p_tag);
    }

    /* E tag (event being zapped) */
    if (event_id_hex) {
        NostrTag *e_tag = nostr_tag_new("e", event_id_hex, NULL);
        if (e_tag) {
            nostr_tags_append(tags, e_tag);
        }
    }

    /* A tag (event coordinate) */
    if (event_coordinate) {
        NostrTag *a_tag = nostr_tag_new("a", event_coordinate, NULL);
        if (a_tag) {
            nostr_tags_append(tags, a_tag);
        }
    }

    /* K tag (kind of target event) */
    if (event_kind >= 0) {
        char kind_str[16];
        snprintf(kind_str, sizeof(kind_str), "%d", event_kind);
        NostrTag *k_tag = nostr_tag_new("k", kind_str, NULL);
        if (k_tag) {
            nostr_tags_append(tags, k_tag);
        }
    }

    nostr_event_set_tags(event, tags);

    /* Sign the event */
    if (nostr_event_sign(event, sender_sk_hex) != 0) {
        nostr_event_free(event);
        return NULL;
    }

    return event;
}

NostrZapRequest *nostr_nip57_parse_zap_request(NostrEvent *event) {
    if (!event) return NULL;

    if (nostr_event_get_kind(event) != NOSTR_NIP57_KIND_ZAP_REQUEST) {
        return NULL;
    }

    NostrTags *tags = nostr_event_get_tags(event);
    if (!tags) return NULL;

    NostrZapRequest *req = calloc(1, sizeof(NostrZapRequest));
    if (!req) return NULL;

    /* Initialize with default values */
    req->event_kind = -1;

    /* Parse relays */
    req->relays = parse_relays_tag(tags, &req->relay_count);

    /* Parse amount */
    const char *amount_str = find_tag_value(tags, "amount", 0);
    if (amount_str) {
        req->amount = strtoull(amount_str, NULL, 10);
    }

    /* Parse lnurl */
    const char *lnurl = find_tag_value(tags, "lnurl", 0);
    if (lnurl) {
        req->lnurl = strdup(lnurl);
    }

    /* Parse recipient pubkey (p tag) */
    const char *recipient = find_tag_value(tags, "p", 0);
    if (recipient) {
        req->recipient_pubkey = strdup(recipient);
    }

    /* Parse event ID (e tag) */
    const char *event_id = find_tag_value(tags, "e", 0);
    if (event_id) {
        req->event_id = strdup(event_id);
    }

    /* Parse event coordinate (a tag) */
    const char *coordinate = find_tag_value(tags, "a", 0);
    if (coordinate) {
        req->event_coordinate = strdup(coordinate);
    }

    /* Parse event kind (k tag) */
    const char *kind_str = find_tag_value(tags, "k", 0);
    if (kind_str) {
        req->event_kind = atoi(kind_str);
    }

    /* Sender pubkey from event */
    const char *sender = nostr_event_get_pubkey(event);
    if (sender) {
        req->sender_pubkey = strdup(sender);
    }

    /* Content */
    const char *content = nostr_event_get_content(event);
    if (content && strlen(content) > 0) {
        req->content = strdup(content);
    }

    /* Timestamp */
    req->created_at = nostr_event_get_created_at(event);

    return req;
}

bool nostr_nip57_validate_zap_request(NostrEvent *event) {
    if (!event) return false;

    /* Check kind */
    if (nostr_event_get_kind(event) != NOSTR_NIP57_KIND_ZAP_REQUEST) {
        return false;
    }

    /* Check signature */
    if (!nostr_event_check_signature(event)) {
        return false;
    }

    /* Check tags exist */
    NostrTags *tags = nostr_event_get_tags(event);
    if (!tags || nostr_tags_size(tags) == 0) {
        return false;
    }

    /* Must have exactly one p tag */
    if (count_tags(tags, "p") != 1) {
        return false;
    }

    /* Must have 0 or 1 e tags */
    if (count_tags(tags, "e") > 1) {
        return false;
    }

    /* Should have relays tag (recommended but not strictly required) */
    /* We just check for presence, not validity */

    return true;
}

char *nostr_nip57_zap_request_to_json(NostrEvent *event) {
    if (!event) return NULL;
    return nostr_event_serialize_compact(event);
}

char *nostr_nip57_build_callback_url(const char *callback,
                                      uint64_t amount_msats,
                                      const char *zap_request_json,
                                      const char *lnurl) {
    if (!callback || !zap_request_json || amount_msats == 0) {
        return NULL;
    }

    char *encoded_json = url_encode(zap_request_json);
    if (!encoded_json) return NULL;

    char *encoded_lnurl = NULL;
    if (lnurl) {
        encoded_lnurl = url_encode(lnurl);
    }

    /* Determine if callback already has query params */
    const char *separator = strchr(callback, '?') ? "&" : "?";

    /* Calculate URL length */
    size_t url_len = strlen(callback) + 1 + /* separator */
                     strlen("amount=") + 20 + /* amount */
                     strlen("&nostr=") + strlen(encoded_json);
    if (encoded_lnurl) {
        url_len += strlen("&lnurl=") + strlen(encoded_lnurl);
    }
    url_len += 1; /* null terminator */

    char *url = malloc(url_len);
    if (!url) {
        free(encoded_json);
        free(encoded_lnurl);
        return NULL;
    }

    if (encoded_lnurl) {
        snprintf(url, url_len, "%s%samount=%llu&nostr=%s&lnurl=%s",
                 callback, separator,
                 (unsigned long long)amount_msats,
                 encoded_json,
                 encoded_lnurl);
    } else {
        snprintf(url, url_len, "%s%samount=%llu&nostr=%s",
                 callback, separator,
                 (unsigned long long)amount_msats,
                 encoded_json);
    }

    free(encoded_json);
    free(encoded_lnurl);
    return url;
}

/* ============================================================================
 * Zap Receipt Functions
 * ============================================================================ */

NostrZapReceipt *nostr_nip57_parse_zap_receipt(NostrEvent *event) {
    if (!event) return NULL;

    if (nostr_event_get_kind(event) != NOSTR_NIP57_KIND_ZAP_RECEIPT) {
        return NULL;
    }

    NostrTags *tags = nostr_event_get_tags(event);
    if (!tags) return NULL;

    NostrZapReceipt *receipt = calloc(1, sizeof(NostrZapReceipt));
    if (!receipt) return NULL;

    /* Initialize with default values */
    receipt->event_kind = -1;

    /* Parse bolt11 */
    const char *bolt11 = find_tag_value(tags, "bolt11", 0);
    if (bolt11) {
        receipt->bolt11 = strdup(bolt11);
    }

    /* Parse preimage */
    const char *preimage = find_tag_value(tags, "preimage", 0);
    if (preimage) {
        receipt->preimage = strdup(preimage);
    }

    /* Parse description (JSON-encoded zap request) */
    const char *description = find_tag_value(tags, "description", 0);
    if (description) {
        receipt->description = strdup(description);
    }

    /* Parse recipient pubkey (p tag) */
    const char *recipient = find_tag_value(tags, "p", 0);
    if (recipient) {
        receipt->recipient_pubkey = strdup(recipient);
    }

    /* Parse sender pubkey (P tag - uppercase) */
    const char *sender = find_tag_value(tags, "P", 0);
    if (sender) {
        receipt->sender_pubkey = strdup(sender);
    }

    /* Parse event ID (e tag) */
    const char *event_id = find_tag_value(tags, "e", 0);
    if (event_id) {
        receipt->event_id = strdup(event_id);
    }

    /* Parse event coordinate (a tag) */
    const char *coordinate = find_tag_value(tags, "a", 0);
    if (coordinate) {
        receipt->event_coordinate = strdup(coordinate);
    }

    /* Parse event kind (k tag) */
    const char *kind_str = find_tag_value(tags, "k", 0);
    if (kind_str) {
        receipt->event_kind = atoi(kind_str);
    }

    /* Provider pubkey from event pubkey */
    const char *provider = nostr_event_get_pubkey(event);
    if (provider) {
        receipt->provider_pubkey = strdup(provider);
    }

    /* Timestamp */
    receipt->created_at = nostr_event_get_created_at(event);

    return receipt;
}

bool nostr_nip57_validate_zap_receipt(NostrEvent *receipt_event,
                                       const char *expected_provider_pubkey) {
    return nostr_nip57_validate_zap_receipt_full(receipt_event, expected_provider_pubkey, NULL);
}

bool nostr_nip57_validate_zap_receipt_full(NostrEvent *receipt_event,
                                            const char *expected_provider_pubkey,
                                            const char *expected_recipient_lnurl) {
    if (!receipt_event) return false;

    /* Check kind */
    if (nostr_event_get_kind(receipt_event) != NOSTR_NIP57_KIND_ZAP_RECEIPT) {
        return false;
    }

    /* Check signature */
    if (!nostr_event_check_signature(receipt_event)) {
        return false;
    }

    NostrTags *tags = nostr_event_get_tags(receipt_event);
    if (!tags) return false;

    /* Must have bolt11 tag */
    const char *bolt11 = find_tag_value(tags, "bolt11", 0);
    if (!bolt11) return false;

    /* Must have description tag */
    const char *description = find_tag_value(tags, "description", 0);
    if (!description) return false;

    /* Validate provider pubkey if specified */
    if (expected_provider_pubkey) {
        const char *provider = nostr_event_get_pubkey(receipt_event);
        if (!provider || strcmp(provider, expected_provider_pubkey) != 0) {
            return false;
        }
    }

    /* Parse embedded zap request to validate further */
    NostrEvent *zap_req = nostr_event_new();
    if (!zap_req) return false;

    if (!nostr_event_deserialize_compact(zap_req, description)) {
        nostr_event_free(zap_req);
        return false;
    }

    /* Validate embedded zap request */
    if (!nostr_nip57_validate_zap_request(zap_req)) {
        nostr_event_free(zap_req);
        return false;
    }

    /* Check invoice amount matches zap request amount (if specified in request) */
    NostrTags *req_tags = nostr_event_get_tags(zap_req);
    const char *req_amount_str = find_tag_value(req_tags, "amount", 0);
    if (req_amount_str) {
        uint64_t req_amount = strtoull(req_amount_str, NULL, 10);
        uint64_t invoice_amount = nostr_nip57_parse_bolt11_amount(bolt11);
        if (invoice_amount > 0 && req_amount != invoice_amount) {
            nostr_event_free(zap_req);
            return false;
        }
    }

    /* Check lnurl if specified */
    if (expected_recipient_lnurl) {
        const char *req_lnurl = find_tag_value(req_tags, "lnurl", 0);
        if (req_lnurl && strcmp(req_lnurl, expected_recipient_lnurl) != 0) {
            nostr_event_free(zap_req);
            return false;
        }
    }

    nostr_event_free(zap_req);
    return true;
}

uint64_t nostr_nip57_get_zap_amount(const NostrZapReceipt *receipt) {
    if (!receipt || !receipt->bolt11) return 0;
    return nostr_nip57_parse_bolt11_amount(receipt->bolt11);
}

uint64_t nostr_nip57_get_zap_amount_from_event(NostrEvent *receipt_event) {
    if (!receipt_event) return 0;

    NostrTags *tags = nostr_event_get_tags(receipt_event);
    if (!tags) return 0;

    const char *bolt11 = find_tag_value(tags, "bolt11", 0);
    if (!bolt11) return 0;

    return nostr_nip57_parse_bolt11_amount(bolt11);
}

NostrZapRequest *nostr_nip57_extract_zap_request_from_receipt(NostrEvent *receipt_event) {
    if (!receipt_event) return NULL;

    NostrTags *tags = nostr_event_get_tags(receipt_event);
    if (!tags) return NULL;

    const char *description = find_tag_value(tags, "description", 0);
    if (!description) return NULL;

    NostrEvent *zap_req = nostr_event_new();
    if (!zap_req) return NULL;

    if (!nostr_event_deserialize_compact(zap_req, description)) {
        nostr_event_free(zap_req);
        return NULL;
    }

    NostrZapRequest *req = nostr_nip57_parse_zap_request(zap_req);
    nostr_event_free(zap_req);

    return req;
}

/* ============================================================================
 * BOLT11 Invoice Parsing
 * ============================================================================ */

/**
 * BOLT11 amount multipliers
 */
static const struct {
    char suffix;
    uint64_t multiplier;
} bolt11_multipliers[] = {
    {'m', 100000000ULL},    /* milli-bitcoin = 100,000,000 msat */
    {'u', 100000ULL},       /* micro-bitcoin = 100,000 msat */
    {'n', 100ULL},          /* nano-bitcoin = 100 msat */
    {'p', 1ULL},            /* pico-bitcoin = 0.1 msat (round up) */
};

uint64_t nostr_nip57_parse_bolt11_amount(const char *bolt11) {
    if (!bolt11) return 0;

    /* BOLT11 format: ln[bc|tb|...]<amount><multiplier>1<data> */
    const char *p = bolt11;

    /* Skip "ln" prefix */
    if (strncmp(p, "ln", 2) != 0) return 0;
    p += 2;

    /* Skip network identifier (bc, tb, etc.) */
    while (*p && *p != '1' && !isdigit((unsigned char)*p)) {
        p++;
    }

    if (!*p) return 0;

    /* Parse amount */
    if (!isdigit((unsigned char)*p)) {
        /* No amount specified */
        return 0;
    }

    char *end;
    uint64_t amount = strtoull(p, &end, 10);
    if (end == p) return 0;

    /* Check for multiplier */
    char multiplier = *end;
    if (multiplier == '1') {
        /* No multiplier, amount is in whole bitcoins (unlikely for zaps) */
        return amount * 100000000000ULL; /* BTC to msat */
    }

    /* Find multiplier */
    for (size_t i = 0; i < sizeof(bolt11_multipliers) / sizeof(bolt11_multipliers[0]); i++) {
        if (bolt11_multipliers[i].suffix == multiplier) {
            return amount * bolt11_multipliers[i].multiplier;
        }
    }

    return 0;
}

int nostr_nip57_get_bolt11_description_hash(const char *bolt11, uint8_t *hash_out) {
    if (!bolt11 || !hash_out) {
        return -EINVAL;
    }

    char *fail = NULL;
    struct bolt11 *b11 = bolt11_decode_minimal(NULL, bolt11, &fail);

    if (!b11) {
        /* Decoding failed */
        if (fail) {
            tal_free(fail);
        }
        return -EINVAL;
    }

    /* Check if the invoice has a description hash (the 'h' field) */
    if (!b11->description_hash) {
        /* Invoice has a plain description (the 'd' field) instead of a hash.
         * For NIP-57 validation, we need the description hash. If the invoice
         * only has a plain description, the caller should compute SHA-256 of
         * that description themselves. Return ENOENT to indicate no hash present.
         */
        tal_free(b11);
        return -ENOENT;
    }

    /* Copy the 32-byte description hash to the output buffer */
    memcpy(hash_out, b11->description_hash->u.u8, 32);

    tal_free(b11);
    return 0;
}

/* ============================================================================
 * Zap Split Functions
 * ============================================================================ */

NostrZapSplitConfig *nostr_nip57_parse_zap_splits(NostrEvent *event) {
    if (!event) return NULL;

    NostrTags *tags = nostr_event_get_tags(event);
    if (!tags) return NULL;

    /* Count zap tags */
    size_t zap_count = count_tags(tags, "zap");
    if (zap_count == 0) return NULL;

    NostrZapSplitConfig *config = calloc(1, sizeof(NostrZapSplitConfig));
    if (!config) return NULL;

    config->splits = calloc(zap_count, sizeof(NostrZapSplit));
    if (!config->splits) {
        free(config);
        return NULL;
    }

    size_t total = nostr_tags_size(tags);
    size_t split_idx = 0;
    uint32_t total_weight = 0;

    for (size_t i = 0; i < total && split_idx < zap_count; i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 2) continue;

        const char *tag_key = nostr_tag_get(tag, 0);
        if (!tag_key || strcmp(tag_key, "zap") != 0) continue;

        /* zap tag format: ["zap", pubkey, relay, weight?] */
        const char *pubkey = nostr_tag_get(tag, 1);
        if (!pubkey) continue;

        config->splits[split_idx].pubkey = strdup(pubkey);

        if (nostr_tag_size(tag) >= 3) {
            const char *relay = nostr_tag_get(tag, 2);
            if (relay) {
                config->splits[split_idx].relay = strdup(relay);
            }
        }

        if (nostr_tag_size(tag) >= 4) {
            const char *weight_str = nostr_tag_get(tag, 3);
            if (weight_str) {
                config->splits[split_idx].weight = (uint32_t)strtoul(weight_str, NULL, 10);
                total_weight += config->splits[split_idx].weight;
            }
        }

        split_idx++;
    }

    config->count = split_idx;
    config->total_weight = total_weight;

    return config;
}

uint64_t nostr_nip57_calculate_split_amount(const NostrZapSplitConfig *config,
                                             size_t recipient_index,
                                             uint64_t total_msats) {
    if (!config || recipient_index >= config->count || total_msats == 0) {
        return 0;
    }

    /* If no weights specified, divide equally */
    if (config->total_weight == 0) {
        return total_msats / config->count;
    }

    /* Calculate based on weight */
    uint32_t weight = config->splits[recipient_index].weight;
    if (weight == 0) {
        return 0;
    }

    return (total_msats * weight) / config->total_weight;
}

/* ============================================================================
 * LNURL Helper Functions
 * ============================================================================ */

NostrLnurlPayInfo *nostr_nip57_parse_lnurl_pay_response(const char *json) {
    if (!json) return NULL;

    /* Simple JSON parsing - look for specific fields */
    /* Note: A production implementation should use a proper JSON parser */

    NostrLnurlPayInfo *info = calloc(1, sizeof(NostrLnurlPayInfo));
    if (!info) return NULL;

    /* Check for allowsNostr */
    info->allows_nostr = (strstr(json, "\"allowsNostr\":true") != NULL ||
                          strstr(json, "\"allowsNostr\": true") != NULL);

    /* Extract callback URL */
    const char *callback_start = strstr(json, "\"callback\":");
    if (callback_start) {
        callback_start = strchr(callback_start + 11, '"');
        if (callback_start) {
            callback_start++;
            const char *callback_end = strchr(callback_start, '"');
            if (callback_end) {
                size_t len = callback_end - callback_start;
                info->callback = malloc(len + 1);
                if (info->callback) {
                    memcpy(info->callback, callback_start, len);
                    info->callback[len] = '\0';
                }
            }
        }
    }

    /* Extract nostrPubkey */
    const char *pk_start = strstr(json, "\"nostrPubkey\":");
    if (pk_start) {
        pk_start = strchr(pk_start + 14, '"');
        if (pk_start) {
            pk_start++;
            const char *pk_end = strchr(pk_start, '"');
            if (pk_end) {
                size_t len = pk_end - pk_start;
                if (len == 64) { /* Valid hex pubkey length */
                    info->nostr_pubkey = malloc(len + 1);
                    if (info->nostr_pubkey) {
                        memcpy(info->nostr_pubkey, pk_start, len);
                        info->nostr_pubkey[len] = '\0';
                    }
                }
            }
        }
    }

    /* Extract minSendable */
    const char *min_start = strstr(json, "\"minSendable\":");
    if (min_start) {
        min_start += 14;
        info->min_sendable = strtoull(min_start, NULL, 10);
    }

    /* Extract maxSendable */
    const char *max_start = strstr(json, "\"maxSendable\":");
    if (max_start) {
        max_start += 14;
        info->max_sendable = strtoull(max_start, NULL, 10);
    }

    return info;
}

char *nostr_nip57_lud16_to_lnurl_url(const char *lud16) {
    if (!lud16) return NULL;

    /* Format: user@domain.com -> https://domain.com/.well-known/lnurlp/user */
    const char *at = strchr(lud16, '@');
    if (!at) return NULL;

    size_t user_len = at - lud16;
    if (user_len == 0) return NULL;

    const char *domain = at + 1;
    size_t domain_len = strlen(domain);
    if (domain_len == 0) return NULL;

    /* Build URL */
    size_t url_len = strlen("https://") + domain_len + strlen("/.well-known/lnurlp/") + user_len + 1;
    char *url = malloc(url_len);
    if (!url) return NULL;

    snprintf(url, url_len, "https://%s/.well-known/lnurlp/%.*s", domain, (int)user_len, lud16);
    return url;
}

/* Bech32 character set for lnurl encoding/decoding */
static const char *bech32_charset = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

char *nostr_nip57_encode_lnurl(const char *url) {
    if (!url) return NULL;

    /* Convert URL to lowercase for bech32 encoding */
    size_t url_len = strlen(url);
    uint8_t *data = malloc(url_len);
    if (!data) return NULL;

    for (size_t i = 0; i < url_len; i++) {
        data[i] = (uint8_t)url[i];
    }

    /* Convert 8-bit data to 5-bit groups for bech32 */
    size_t data5_len = (url_len * 8 + 4) / 5;
    uint8_t *data5 = calloc(data5_len, 1);
    if (!data5) {
        free(data);
        return NULL;
    }

    int bits = 0;
    int value = 0;
    size_t out_idx = 0;

    for (size_t i = 0; i < url_len; i++) {
        value = (value << 8) | data[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            data5[out_idx++] = (value >> bits) & 31;
        }
    }
    if (bits > 0) {
        data5[out_idx++] = (value << (5 - bits)) & 31;
    }

    free(data);

    /* Allocate result: "lnurl1" + encoded data + null */
    char *result = malloc(6 + out_idx + 1);
    if (!result) {
        free(data5);
        return NULL;
    }

    memcpy(result, "lnurl1", 6);
    for (size_t i = 0; i < out_idx; i++) {
        result[6 + i] = bech32_charset[data5[i]];
    }
    result[6 + out_idx] = '\0';

    free(data5);
    return result;
}

char *nostr_nip57_decode_lnurl(const char *lnurl) {
    if (!lnurl) return NULL;

    /* Check for lnurl prefix (case insensitive) */
    if (strncasecmp(lnurl, "lnurl1", 6) != 0) {
        return NULL;
    }

    const char *data_part = lnurl + 6;
    size_t data_len = strlen(data_part);

    /* Convert bech32 characters to 5-bit values */
    uint8_t *data5 = malloc(data_len);
    if (!data5) return NULL;

    for (size_t i = 0; i < data_len; i++) {
        char c = tolower((unsigned char)data_part[i]);
        const char *pos = strchr(bech32_charset, c);
        if (!pos) {
            free(data5);
            return NULL;
        }
        data5[i] = pos - bech32_charset;
    }

    /* Convert 5-bit groups back to 8-bit data */
    size_t data8_len = data_len * 5 / 8;
    uint8_t *data8 = calloc(data8_len + 1, 1);
    if (!data8) {
        free(data5);
        return NULL;
    }

    int bits = 0;
    int value = 0;
    size_t out_idx = 0;

    for (size_t i = 0; i < data_len; i++) {
        value = (value << 5) | data5[i];
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (out_idx < data8_len) {
                data8[out_idx++] = (value >> bits) & 255;
            }
        }
    }

    free(data5);

    /* Null terminate and return */
    data8[out_idx] = '\0';
    return (char *)data8;
}
