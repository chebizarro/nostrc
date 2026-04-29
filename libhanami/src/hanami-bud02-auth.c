/*
 * hanami-bud02-auth.c - BUD-02 Blossom authorization (kind 24242)
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-bud02-auth.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

/* =========================================================================
 * Action string table
 * ========================================================================= */

static const char *ACTION_STRINGS[] = {
    [HANAMI_BUD02_ACTION_UPLOAD] = "upload",
    [HANAMI_BUD02_ACTION_DELETE] = "delete",
    [HANAMI_BUD02_ACTION_GET]    = "get",
    [HANAMI_BUD02_ACTION_LIST]   = "list",
    [HANAMI_BUD02_ACTION_MIRROR] = "mirror",
};

#define ACTION_COUNT (sizeof(ACTION_STRINGS) / sizeof(ACTION_STRINGS[0]))

const char *hanami_bud02_action_str(hanami_bud02_action_t action)
{
    if ((size_t)action < ACTION_COUNT)
        return ACTION_STRINGS[action];
    return NULL;
}

hanami_bud02_result_t hanami_bud02_action_from_str(const char *str,
                                                    hanami_bud02_action_t *out)
{
    if (!str || !out)
        return HANAMI_BUD02_ERR_NULL_PARAM;

    for (size_t i = 0; i < ACTION_COUNT; i++) {
        if (strcmp(str, ACTION_STRINGS[i]) == 0) {
            *out = (hanami_bud02_action_t)i;
            return HANAMI_BUD02_OK;
        }
    }
    return HANAMI_BUD02_ERR_INVALID_ACTION;
}

/* =========================================================================
 * Internal base64 helpers (mirrors nip98 pattern)
 * ========================================================================= */

static int bud02_base64_encode(const uint8_t *buf, size_t len, char **out_b64)
{
    if (!buf || !out_b64)
        return -1;

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

    char *out = malloc(bptr->length + 1);
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

static int bud02_base64_decode(const char *b64, uint8_t **out_buf,
                               size_t *out_len)
{
    if (!b64 || !out_buf || !out_len)
        return -1;

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
    uint8_t *out = malloc(cap);
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

/* =========================================================================
 * Internal tag helpers
 * ========================================================================= */

static const char *find_tag_value(const NostrEvent *event, const char *key)
{
    if (!event || !event->tags || !key)
        return NULL;

    NostrTags *tags = event->tags;
    for (size_t i = 0; i < tags->count; i++) {
        NostrTag *tag = tags->data[i];
        if (!tag || tag->size < 2)
            continue;
        const char *tag_key = string_array_get(tag, 0);
        if (tag_key && strcmp(tag_key, key) == 0)
            return string_array_get(tag, 1);
    }
    return NULL;
}

/* Helper to create a StringArray tag with 2 elements */
static StringArray *make_tag2(const char *key, const char *value)
{
    StringArray *tag = new_string_array(2);
    if (!tag) return NULL;
    string_array_add(tag, key);
    string_array_add(tag, value);
    return tag;
}

/* =========================================================================
 * Event creation
 * ========================================================================= */

NostrEvent *hanami_bud02_create_auth_event(hanami_bud02_action_t action,
                                           const char *sha256_hex,
                                           int64_t expiration,
                                           const char *server_url)
{
    const char *action_str = hanami_bud02_action_str(action);
    if (!action_str)
        return NULL;

    NostrEvent *event = nostr_event_new();
    if (!event)
        return NULL;

    int64_t now = (int64_t)time(NULL);
    nostr_event_set_kind(event, HANAMI_BUD02_KIND);
    nostr_event_set_created_at(event, now);
    nostr_event_set_content(event, "Authorize " /* human-friendly */ );

    /* Default expiration: now + 5 minutes */
    if (expiration <= 0)
        expiration = now + HANAMI_BUD02_DEFAULT_EXPIRATION;

    /* Count tags: t + expiration + optional x + optional server */
    int tag_count = 2; /* t + expiration always present */
    if (sha256_hex)  tag_count++;
    if (server_url)  tag_count++;

    NostrTags *tags = malloc(sizeof(NostrTags));
    if (!tags) {
        nostr_event_free(event);
        return NULL;
    }
    tags->data = malloc((size_t)tag_count * sizeof(StringArray *));
    if (!tags->data) {
        free(tags);
        nostr_event_free(event);
        return NULL;
    }
    tags->count = 0;
    tags->capacity = (size_t)tag_count;

    /* [t, action] tag */
    StringArray *t_tag = make_tag2("t", action_str);
    if (!t_tag) goto fail;
    tags->data[tags->count++] = t_tag;

    /* [x, sha256_hex] tag — optional */
    if (sha256_hex) {
        StringArray *x_tag = make_tag2("x", sha256_hex);
        if (!x_tag) goto fail;
        tags->data[tags->count++] = x_tag;
    }

    /* [expiration, unix_ts] tag */
    {
        char exp_str[32];
        snprintf(exp_str, sizeof(exp_str), "%" PRId64, expiration);
        StringArray *exp_tag = make_tag2("expiration", exp_str);
        if (!exp_tag) goto fail;
        tags->data[tags->count++] = exp_tag;
    }

    /* [server, url] tag — optional */
    if (server_url) {
        StringArray *srv_tag = make_tag2("server", server_url);
        if (!srv_tag) goto fail;
        tags->data[tags->count++] = srv_tag;
    }

    nostr_event_set_tags(event, tags);
    return event;

fail:
    /* Clean up partially created tags */
    for (size_t i = 0; i < tags->count; i++)
        string_array_free(tags->data[i]);
    free(tags->data);
    free(tags);
    nostr_event_free(event);
    return NULL;
}

/* =========================================================================
 * Header creation/parsing
 * ========================================================================= */

char *hanami_bud02_create_auth_header(const NostrEvent *event)
{
    if (!event)
        return NULL;

    char *json = nostr_event_serialize_compact(event);
    if (!json)
        return NULL;

    char *b64 = NULL;
    if (bud02_base64_encode((const uint8_t *)json, strlen(json), &b64) != 0) {
        free(json);
        return NULL;
    }
    free(json);

    size_t header_len = 6 + strlen(b64) + 1; /* "Nostr " + b64 + '\0' */
    char *header = malloc(header_len);
    if (!header) {
        free(b64);
        return NULL;
    }
    snprintf(header, header_len, "Nostr %s", b64);
    free(b64);
    return header;
}

hanami_bud02_result_t hanami_bud02_parse_auth_header(const char *header,
                                                     NostrEvent **out_event)
{
    if (!header || !out_event)
        return HANAMI_BUD02_ERR_NULL_PARAM;

    *out_event = NULL;

    /* Skip leading whitespace */
    while (*header && isspace((unsigned char)*header))
        header++;

    /* Check "Nostr " prefix (case-insensitive) */
    if (strncasecmp(header, "Nostr ", 6) != 0)
        return HANAMI_BUD02_ERR_INVALID_HEADER;
    header += 6;

    while (*header && isspace((unsigned char)*header))
        header++;

    if (*header == '\0')
        return HANAMI_BUD02_ERR_INVALID_HEADER;

    /* Decode base64 */
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    if (bud02_base64_decode(header, &decoded, &decoded_len) != 0)
        return HANAMI_BUD02_ERR_DECODE;

    /* Null-terminate for JSON parsing */
    char *json = malloc(decoded_len + 1);
    if (!json) {
        free(decoded);
        return HANAMI_BUD02_ERR_ALLOC;
    }
    memcpy(json, decoded, decoded_len);
    json[decoded_len] = '\0';
    free(decoded);

    /* Deserialize event */
    NostrEvent *event = nostr_event_new();
    if (!event) {
        free(json);
        return HANAMI_BUD02_ERR_ALLOC;
    }

    if (nostr_event_deserialize_compact(event, json, NULL) != 1) {
        nostr_event_free(event);
        free(json);
        return HANAMI_BUD02_ERR_DECODE;
    }
    free(json);

    *out_event = event;
    return HANAMI_BUD02_OK;
}

/* =========================================================================
 * Validation
 * ========================================================================= */

hanami_bud02_result_t hanami_bud02_validate_auth_event(
    const NostrEvent *event,
    hanami_bud02_action_t expected_action,
    const hanami_bud02_validate_options_t *options)
{
    if (!event)
        return HANAMI_BUD02_ERR_NULL_PARAM;

    /* 1. Check kind */
    if (nostr_event_get_kind(event) != HANAMI_BUD02_KIND)
        return HANAMI_BUD02_ERR_INVALID_KIND;

    /* 2. Check "t" tag matches expected action */
    const char *t_val = find_tag_value(event, "t");
    if (!t_val)
        return HANAMI_BUD02_ERR_MISSING_TAG;

    const char *expected_str = hanami_bud02_action_str(expected_action);
    if (!expected_str)
        return HANAMI_BUD02_ERR_INVALID_ACTION;
    if (strcmp(t_val, expected_str) != 0)
        return HANAMI_BUD02_ERR_ACTION_MISMATCH;

    /* 3. Check expiration (mandatory per BUD-02 spec) */
    const char *exp_val = find_tag_value(event, "expiration");
    if (!exp_val)
        return HANAMI_BUD02_ERR_MISSING_EXPIRATION;
    
    int64_t exp_ts = strtoll(exp_val, NULL, 10);
    int64_t now = (options && options->now_override > 0)
                      ? options->now_override
                      : (int64_t)time(NULL);
    if (exp_ts <= now)
        return HANAMI_BUD02_ERR_EXPIRED;

    /* 4. Optional: check "x" hash tag */
    if (options && options->expected_sha256) {
        const char *x_val = find_tag_value(event, "x");
        if (!x_val)
            return HANAMI_BUD02_ERR_MISSING_TAG;
        if (strcmp(x_val, options->expected_sha256) != 0)
            return HANAMI_BUD02_ERR_HASH_MISMATCH;
    }

    /* 5. Check signature */
    if (!nostr_event_check_signature((NostrEvent *)event))
        return HANAMI_BUD02_ERR_SIGNATURE;

    /* 6. Optional: check pubkey */
    if (options && options->expected_pubkey) {
        const char *event_pubkey = nostr_event_get_pubkey(event);
        if (!event_pubkey || strcmp(event_pubkey, options->expected_pubkey) != 0)
            return HANAMI_BUD02_ERR_PUBKEY_MISMATCH;
    }

    return HANAMI_BUD02_OK;
}

/* =========================================================================
 * Accessors
 * ========================================================================= */

const char *hanami_bud02_get_action(const NostrEvent *event)
{
    return find_tag_value(event, "t");
}

const char *hanami_bud02_get_hash(const NostrEvent *event)
{
    return find_tag_value(event, "x");
}

int64_t hanami_bud02_get_expiration(const NostrEvent *event)
{
    const char *val = find_tag_value(event, "expiration");
    if (!val) return 0;
    return strtoll(val, NULL, 10);
}

const char *hanami_bud02_get_server(const NostrEvent *event)
{
    return find_tag_value(event, "server");
}

/* =========================================================================
 * Error strings
 * ========================================================================= */

const char *hanami_bud02_strerror(hanami_bud02_result_t result)
{
    switch (result) {
    case HANAMI_BUD02_OK:               return "Success";
    case HANAMI_BUD02_ERR_NULL_PARAM:   return "Null parameter";
    case HANAMI_BUD02_ERR_ALLOC:        return "Memory allocation failed";
    case HANAMI_BUD02_ERR_INVALID_KIND: return "Invalid event kind (expected 24242)";
    case HANAMI_BUD02_ERR_EXPIRED:      return "Authorization event has expired";
    case HANAMI_BUD02_ERR_ACTION_MISMATCH:
        return "Action tag does not match expected action";
    case HANAMI_BUD02_ERR_HASH_MISMATCH:
        return "Blob SHA-256 does not match expected hash";
    case HANAMI_BUD02_ERR_SIGNATURE:    return "Event signature is invalid";
    case HANAMI_BUD02_ERR_MISSING_TAG:  return "Required tag missing from event";
    case HANAMI_BUD02_ERR_ENCODE:       return "Base64 encoding failed";
    case HANAMI_BUD02_ERR_DECODE:       return "Base64 decoding or JSON parsing failed";
    case HANAMI_BUD02_ERR_INVALID_HEADER:
        return "Invalid Authorization header format";
    case HANAMI_BUD02_ERR_INVALID_ACTION:
        return "Invalid BUD-02 action string";
    case HANAMI_BUD02_ERR_PUBKEY_MISMATCH:
        return "Event pubkey does not match expected pubkey";
    case HANAMI_BUD02_ERR_MISSING_EXPIRATION:
        return "Expiration tag is missing (required by BUD-02)";
    default:                            return "Unknown BUD-02 error";
    }
}
