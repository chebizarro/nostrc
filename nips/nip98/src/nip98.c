/**
 * @file nip98.c
 * @brief NIP-98: HTTP Auth implementation
 */

#include "nostr/nip98/nip98.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

/* Internal helper: base64 encode */
static int nip98_base64_encode(const uint8_t *buf, size_t len, char **out_b64) {
    if (!buf || !out_b64) return -1;

    BIO *b64 = BIO_new(BIO_f_base64());
    if (!b64) return -1;

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO *mem = BIO_new(BIO_s_mem());
    if (!mem) {
        BIO_free(b64);
        return -1;
    }

    BIO *chain = BIO_push(b64, mem);
    if (BIO_write(chain, buf, (int)len) != (int)len) {
        BIO_free_all(chain);
        return -1;
    }
    if (BIO_flush(chain) != 1) {
        BIO_free_all(chain);
        return -1;
    }

    BUF_MEM *bptr = NULL;
    BIO_get_mem_ptr(chain, &bptr);
    if (!bptr || !bptr->data) {
        BIO_free_all(chain);
        return -1;
    }

    char *out = (char *)malloc(bptr->length + 1);
    if (!out) {
        BIO_free_all(chain);
        return -1;
    }

    memcpy(out, bptr->data, bptr->length);
    out[bptr->length] = '\0';
    *out_b64 = out;
    BIO_free_all(chain);
    return 0;
}

/* Internal helper: base64 decode */
static int nip98_base64_decode(const char *b64, uint8_t **out_buf, size_t *out_len) {
    if (!b64 || !out_buf || !out_len) return -1;

    BIO *bmem = BIO_new_mem_buf((void *)b64, -1);
    if (!bmem) return -1;

    BIO *b64f = BIO_new(BIO_f_base64());
    if (!b64f) {
        BIO_free(bmem);
        return -1;
    }

    BIO_set_flags(b64f, BIO_FLAGS_BASE64_NO_NL);
    BIO *chain = BIO_push(b64f, bmem);

    size_t in_len = strlen(b64);
    size_t cap = (in_len * 3) / 4 + 3;
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) {
        BIO_free_all(chain);
        return -1;
    }

    int n = BIO_read(chain, out, (int)cap);
    if (n <= 0) {
        free(out);
        BIO_free_all(chain);
        return -1;
    }

    *out_buf = out;
    *out_len = (size_t)n;
    BIO_free_all(chain);
    return 0;
}

/* Internal helper: find tag value by key */
static const char *find_tag_value(const NostrEvent *event, const char *key) {
    if (!event || !event->tags || !key) return NULL;

    NostrTags *tags = event->tags;
    for (size_t i = 0; i < tags->count; i++) {
        NostrTag *tag = tags->data[i];
        if (!tag || tag->size < 2) continue;

        const char *tag_key = string_array_get(tag, 0);
        if (tag_key && strcmp(tag_key, key) == 0) {
            return string_array_get(tag, 1);
        }
    }
    return NULL;
}

/* Internal helper: case-insensitive string compare */
static int strcasecmp_safe(const char *a, const char *b) {
    if (!a || !b) return (a == b) ? 0 : 1;
    while (*a && *b) {
        int diff = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (diff != 0) return diff;
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

NostrEvent *nostr_nip98_create_auth_event(const char *url,
                                          const char *method,
                                          const char *payload_sha256_hex) {
    if (!url || !method) return NULL;

    NostrEvent *event = nostr_event_new();
    if (!event) return NULL;

    /* Set kind to 27235 (HTTP Auth) */
    nostr_event_set_kind(event, NOSTR_KIND_HTTP_AUTH);

    /* Set created_at to current time */
    nostr_event_set_created_at(event, (int64_t)time(NULL));

    /* Content SHOULD be empty per spec */
    nostr_event_set_content(event, "");

    /* Create tags */
    int tag_count = payload_sha256_hex ? 3 : 2;

    NostrTags *tags = (NostrTags *)malloc(sizeof(NostrTags));
    if (!tags) {
        nostr_event_free(event);
        return NULL;
    }

    tags->data = (StringArray **)malloc(tag_count * sizeof(StringArray *));
    if (!tags->data) {
        free(tags);
        nostr_event_free(event);
        return NULL;
    }
    tags->count = tag_count;
    tags->capacity = tag_count;

    /* u tag: URL */
    tags->data[0] = new_string_array(2);
    if (!tags->data[0]) {
        free(tags->data);
        free(tags);
        nostr_event_free(event);
        return NULL;
    }
    string_array_add(tags->data[0], "u");
    string_array_add(tags->data[0], url);

    /* method tag: HTTP method */
    tags->data[1] = new_string_array(2);
    if (!tags->data[1]) {
        string_array_free(tags->data[0]);
        free(tags->data);
        free(tags);
        nostr_event_free(event);
        return NULL;
    }
    string_array_add(tags->data[1], "method");
    string_array_add(tags->data[1], method);

    /* payload tag: optional SHA256 hash */
    if (payload_sha256_hex) {
        tags->data[2] = new_string_array(2);
        if (!tags->data[2]) {
            string_array_free(tags->data[0]);
            string_array_free(tags->data[1]);
            free(tags->data);
            free(tags);
            nostr_event_free(event);
            return NULL;
        }
        string_array_add(tags->data[2], "payload");
        string_array_add(tags->data[2], payload_sha256_hex);
    }

    nostr_event_set_tags(event, tags);

    return event;
}

char *nostr_nip98_create_auth_header(const NostrEvent *event) {
    if (!event) return NULL;

    /* Serialize event to JSON */
    char *json = nostr_event_serialize_compact(event);
    if (!json) return NULL;

    /* Base64 encode the JSON */
    char *b64 = NULL;
    if (nip98_base64_encode((const uint8_t *)json, strlen(json), &b64) != 0) {
        free(json);
        return NULL;
    }
    free(json);

    /* Create "Nostr <base64>" header value */
    size_t header_len = 6 + strlen(b64) + 1; /* "Nostr " + b64 + '\0' */
    char *header = (char *)malloc(header_len);
    if (!header) {
        free(b64);
        return NULL;
    }

    snprintf(header, header_len, "Nostr %s", b64);
    free(b64);

    return header;
}

NostrNip98Result nostr_nip98_parse_auth_header(const char *header,
                                                NostrEvent **out_event) {
    if (!header || !out_event) return NOSTR_NIP98_ERR_NULL_PARAM;

    *out_event = NULL;

    /* Skip leading whitespace */
    while (*header && isspace((unsigned char)*header)) header++;

    /* Check for "Nostr " prefix (case-insensitive) */
    if (strncasecmp(header, "Nostr ", 6) != 0) {
        return NOSTR_NIP98_ERR_INVALID_HEADER;
    }
    header += 6;

    /* Skip whitespace after scheme */
    while (*header && isspace((unsigned char)*header)) header++;

    if (*header == '\0') {
        return NOSTR_NIP98_ERR_INVALID_HEADER;
    }

    /* Decode base64 */
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    if (nip98_base64_decode(header, &decoded, &decoded_len) != 0) {
        return NOSTR_NIP98_ERR_DECODE;
    }

    /* Null-terminate for JSON parsing */
    char *json = (char *)malloc(decoded_len + 1);
    if (!json) {
        free(decoded);
        return NOSTR_NIP98_ERR_ALLOC;
    }
    memcpy(json, decoded, decoded_len);
    json[decoded_len] = '\0';
    free(decoded);

    /* Deserialize event */
    NostrEvent *event = nostr_event_new();
    if (!event) {
        free(json);
        return NOSTR_NIP98_ERR_ALLOC;
    }

    if (nostr_event_deserialize_compact(event, json) != 1) {
        nostr_event_free(event);
        free(json);
        return NOSTR_NIP98_ERR_DECODE;
    }
    free(json);

    *out_event = event;
    return NOSTR_NIP98_OK;
}

NostrNip98Result nostr_nip98_validate_auth_event(const NostrEvent *event,
                                                  const char *expected_url,
                                                  const char *expected_method,
                                                  const NostrNip98ValidateOptions *options) {
    if (!event || !expected_url || !expected_method) {
        return NOSTR_NIP98_ERR_NULL_PARAM;
    }

    /* 1. Check kind */
    if (nostr_event_get_kind(event) != NOSTR_KIND_HTTP_AUTH) {
        return NOSTR_NIP98_ERR_INVALID_KIND;
    }

    /* 2. Check timestamp */
    int time_window = NOSTR_NIP98_DEFAULT_TIME_WINDOW;
    if (options && options->time_window_seconds > 0) {
        time_window = options->time_window_seconds;
    }

    time_t now = time(NULL);
    int64_t created_at = nostr_event_get_created_at(event);
    if (created_at > now + time_window || created_at < now - time_window) {
        return NOSTR_NIP98_ERR_TIMESTAMP_EXPIRED;
    }

    /* 3. Check URL tag */
    const char *url = find_tag_value(event, "u");
    if (!url) {
        return NOSTR_NIP98_ERR_MISSING_TAG;
    }
    if (strcmp(url, expected_url) != 0) {
        return NOSTR_NIP98_ERR_URL_MISMATCH;
    }

    /* 4. Check method tag */
    const char *method = find_tag_value(event, "method");
    if (!method) {
        return NOSTR_NIP98_ERR_MISSING_TAG;
    }
    if (strcasecmp_safe(method, expected_method) != 0) {
        return NOSTR_NIP98_ERR_METHOD_MISMATCH;
    }

    /* 5. Check signature */
    /* Note: nostr_event_check_signature expects non-const, but doesn't modify */
    if (!nostr_event_check_signature((NostrEvent *)event)) {
        return NOSTR_NIP98_ERR_SIGNATURE_INVALID;
    }

    /* 6. Optional: Check payload hash */
    if (options && options->expected_payload_hash) {
        const char *payload = find_tag_value(event, "payload");
        if (!payload) {
            return NOSTR_NIP98_ERR_PAYLOAD_MISMATCH;
        }
        if (strcasecmp_safe(payload, options->expected_payload_hash) != 0) {
            return NOSTR_NIP98_ERR_PAYLOAD_MISMATCH;
        }
    }

    return NOSTR_NIP98_OK;
}

const char *nostr_nip98_get_url(const NostrEvent *event) {
    return find_tag_value(event, "u");
}

const char *nostr_nip98_get_method(const NostrEvent *event) {
    return find_tag_value(event, "method");
}

const char *nostr_nip98_get_payload_hash(const NostrEvent *event) {
    return find_tag_value(event, "payload");
}

const char *nostr_nip98_strerror(NostrNip98Result result) {
    switch (result) {
        case NOSTR_NIP98_OK:
            return "Success";
        case NOSTR_NIP98_ERR_NULL_PARAM:
            return "Null parameter";
        case NOSTR_NIP98_ERR_ALLOC:
            return "Memory allocation failed";
        case NOSTR_NIP98_ERR_INVALID_KIND:
            return "Invalid event kind (expected 27235)";
        case NOSTR_NIP98_ERR_TIMESTAMP_EXPIRED:
            return "Event timestamp outside valid window";
        case NOSTR_NIP98_ERR_URL_MISMATCH:
            return "URL does not match expected value";
        case NOSTR_NIP98_ERR_METHOD_MISMATCH:
            return "HTTP method does not match expected value";
        case NOSTR_NIP98_ERR_PAYLOAD_MISMATCH:
            return "Payload hash does not match expected value";
        case NOSTR_NIP98_ERR_SIGNATURE_INVALID:
            return "Event signature is invalid";
        case NOSTR_NIP98_ERR_MISSING_TAG:
            return "Required tag missing from event";
        case NOSTR_NIP98_ERR_ENCODE:
            return "Base64 encoding failed";
        case NOSTR_NIP98_ERR_DECODE:
            return "Base64 decoding or JSON parsing failed";
        case NOSTR_NIP98_ERR_INVALID_HEADER:
            return "Invalid Authorization header format";
        default:
            return "Unknown error";
    }
}
