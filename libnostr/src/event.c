#include "nostr-event.h"
#include "nostr-kinds.h"
#include "nostr-auto-internal.h"
#include "json.h"
#include "nostr-tag.h"
#include "nostr-utils.h"
#include "security_limits_runtime.h"
#include "secure_buf.h"
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "string_array.h"
#include <stdint.h>
#include "nostr-json-parse.h"

/* === NIP-01 canonical preimage serializer ===
 * Build the exact JSON array: [0, pubkey, created_at, kind, tags, content]
 * where pubkey and content are JSON strings, tags is a JSON array.
 * This excludes id and sig by definition.
 */
static char *nostr_event_serialize_nip01_array(const NostrEvent *event) {
    if (!event) return NULL;
    /* pubkey */
    const char *pk = event->pubkey ? event->pubkey : "";
    go_autofree char *pk_esc = nostr_escape_string(pk);
    if (!pk_esc) return NULL;
    /* content */
    const char *ct = event->content ? event->content : "";
    go_autofree char *ct_esc = nostr_escape_string(ct);
    if (!ct_esc) return NULL;
    /* tags */
    go_autofree char *tags_json = NULL;
    if (event->tags) {
        tags_json = nostr_tags_to_json(event->tags);
        if (!tags_json) return NULL;
    } else {
        tags_json = strdup("[]");
        if (!tags_json) return NULL;
    }
    /* size rough estimate */
    size_t cap = strlen(pk_esc) + strlen(ct_esc) + strlen(tags_json) + 64;
    go_autofree char *out = (char *)malloc(cap);
    if (!out) return NULL;
    int n = snprintf(out, cap, "[0,\"%s\",%lld,%d,%s,\"%s\"]",
                     pk_esc,
                     (long long)event->created_at,
                     event->kind,
                     tags_json,
                     ct_esc);
    if (n < 0 || (size_t)n >= cap) return NULL;
    return go_steal_pointer(&out);
}

/* Secure variant: accepts a 32-byte private key inside nostr_secure_buf. */
int nostr_event_sign_secure(NostrEvent *event, const nostr_secure_buf *sk) {
    if (!event || !sk || !sk->ptr || sk->len < 32) return -1;

    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) return -1;
    int return_val = -1;
    unsigned char seckey[32];
    memcpy(seckey, sk->ptr, 32);
    if (!secp256k1_ec_seckey_verify(ctx, seckey)) {
        goto cleanup;
    }
    secp256k1_keypair keypair;
    if (secp256k1_keypair_create(ctx, &keypair, seckey) != 1) {
        goto cleanup;
    }
    /* Derive x-only pubkey and set event->pubkey BEFORE computing hash.
     * The NIP-01 canonical serialization includes pubkey, so it must be set first. */
    {
        secp256k1_xonly_pubkey xpk;
        if (secp256k1_keypair_xonly_pub(ctx, &xpk, NULL, &keypair) != 1) {
            goto cleanup;
        }
        unsigned char x32[32];
        if (secp256k1_xonly_pubkey_serialize(ctx, x32, &xpk) != 1) {
            goto cleanup;
        }
        if (event->pubkey) { free(event->pubkey); event->pubkey = NULL; }
        event->pubkey = nostr_bin2hex(x32, 32);
        if (!event->pubkey) goto cleanup;
    }
    /* Now compute the hash with the correct pubkey set */
    unsigned char hash[32];
    char *serialized = nostr_event_serialize_nip01_array(event);
    if (!serialized) goto cleanup;
    SHA256((unsigned char *)serialized, strlen(serialized), hash);
    free(serialized);

    unsigned char auxiliary_rand[32];
    if (RAND_bytes(auxiliary_rand, sizeof(auxiliary_rand)) != 1) {
        goto cleanup;
    }
    unsigned char sig_bin[64];
    if (secp256k1_schnorrsig_sign32(ctx, sig_bin, hash, &keypair, auxiliary_rand) != 1) {
        goto cleanup;
    }
    event->sig = nostr_bin2hex(sig_bin, 64);
    if (!event->sig) goto cleanup;
    /* Set id to the same message hash used for signing */
    if (event->id) { free(event->id); event->id = NULL; }
    event->id = nostr_bin2hex(hash, 32);
    return_val = 0;
cleanup:
    /* Best-effort wipe of local secret material */
    {
        volatile unsigned char *p = seckey;
        for (size_t i = 0; i < sizeof seckey; i++) p[i] = 0;
    }
    secp256k1_context_destroy(ctx);
    return return_val;
}

NostrEvent *nostr_event_new(void) {
    NostrEvent *event = (NostrEvent *)malloc(sizeof(NostrEvent));
    if (!event)
        return NULL;

    event->id = NULL;
    event->pubkey = NULL;
    event->created_at = 0;
    event->kind = 0;
    event->tags = nostr_tags_new(0);
    event->content = NULL;
    event->sig = NULL;

    return event;
}

/* ---- Compact JSON object deserializer (simple, unescaped, no-tags fast path) ---- */

/* Match a JSON key in-place: expects p to point at '"'. If it matches
 * the given key literal, advances *pp to the character after the colon
 * (value start, possibly with whitespace) and returns 1. Otherwise 0. */
static int match_key_advance(const char **pp, const char *key) {
    if (!pp || !*pp || !key) return 0;
    const char *p = nostr_json_skip_ws(*pp);
    if (*p != '"') return 0;
    ++p;
    const char *k = key;
    while (*k && *p == *k) { ++p; ++k; }
    if (*k != '\0' || *p != '"') return 0; /* not exact match */
    ++p; /* after closing quote */
    p = nostr_json_skip_ws(p);
    if (*p != ':') return 0;
    ++p; /* after ':' */
    *pp = p;
    return 1;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static const char *find_key(const char *json, const char *key) {
    if (!json || !key) return NULL;
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strstr(p, key)) != NULL) {
        // ensure it's a JSON key: preceded by '"' and followed by '"'
        if (p > json && *(p-1) == '"' && *(p + klen) == '"') {
            const char *q = p + klen + 1; // after closing quote
            q = nostr_json_skip_ws(q);
            if (*q == ':') return q + 1; // point to value start (maybe ws)
        }
        p += klen;
    }
    return NULL;
}

typedef enum {
    EVENT_PARSE_TEMPLATE = 0,
    EVENT_PARSE_UNSIGNED,
    EVENT_PARSE_SIGNED
} EventParseMode;

static int nostr_event_deserialize_compact_impl(NostrEvent *event,
                                                const char *json,
                                                NostrJsonErrorInfo *err_out,
                                                EventParseMode mode) {
    /* nostrc-737: JFAIL sets error info and returns 0 */
#define JFAIL_EV(code_val, pos) do { \
    if (err_out) { err_out->code = (code_val); err_out->offset = (int)((pos) - json); } \
    return 0; \
} while (0)
    if (!event || !json) {
        if (err_out) { err_out->code = NOSTR_JSON_ERR_NULL_INPUT; err_out->offset = -1; }
        return 0;
    }
    const char *p = nostr_json_skip_ws(json);
    if (*p != '{') JFAIL_EV(NOSTR_JSON_ERR_EXPECTED_OBJECT, p);
    ++p; // after '{'

    int have_kind = 0;
    int have_created_at = 0;
    int have_id = 0;
    int have_pubkey = 0;
    int have_sig = 0;
    int have_tags = 0;
    int have_content = 0;

    while (1) {
        p = nostr_json_skip_ws(p);
        if (*p == '}') { ++p; break; }
        if (*p == ',') JFAIL_EV(NOSTR_JSON_ERR_BAD_SEPARATOR, p);
        // Dispatch by key (in-place match, no allocation for known keys)
        if (match_key_advance(&p, "kind")) {
            if (mode != EVENT_PARSE_TEMPLATE && have_kind)
                JFAIL_EV(NOSTR_JSON_ERR_BAD_KEY, p);
            long long v = 0;
            if (!nostr_json_parse_int64(&p, &v)) JFAIL_EV(NOSTR_JSON_ERR_BAD_NUMBER, p);
            if (v < 0 || v > 65535) JFAIL_EV(NOSTR_JSON_ERR_KIND_RANGE, p);
            event->kind = (int)v; have_kind = 1;
        } else if (match_key_advance(&p, "created_at")) {
            if (mode != EVENT_PARSE_TEMPLATE && have_created_at)
                JFAIL_EV(NOSTR_JSON_ERR_BAD_KEY, p);
            long long ts = 0;
            if (!nostr_json_parse_int64(&p, &ts)) JFAIL_EV(NOSTR_JSON_ERR_BAD_NUMBER, p);
            event->created_at = (int64_t)ts; have_created_at = 1;
        } else if (match_key_advance(&p, "pubkey")) {
            if (mode != EVENT_PARSE_TEMPLATE && have_pubkey)
                JFAIL_EV(NOSTR_JSON_ERR_BAD_KEY, p);
            char *s = nostr_json_parse_string(&p);
            if (!s) JFAIL_EV(NOSTR_JSON_ERR_BAD_STRING, p);
            if (event->pubkey) free(event->pubkey);
            event->pubkey = s;
            have_pubkey = 1;
        } else if (match_key_advance(&p, "id")) {
            if (mode != EVENT_PARSE_TEMPLATE && have_id)
                JFAIL_EV(NOSTR_JSON_ERR_BAD_KEY, p);
            char *s = nostr_json_parse_string(&p);
            if (!s) JFAIL_EV(NOSTR_JSON_ERR_BAD_STRING, p);
            if (event->id) free(event->id);
            event->id = s;
            have_id = 1;
        } else if (match_key_advance(&p, "sig")) {
            if (mode != EVENT_PARSE_TEMPLATE && have_sig)
                JFAIL_EV(NOSTR_JSON_ERR_BAD_KEY, p);
            char *s = nostr_json_parse_string(&p);
            if (!s) JFAIL_EV(NOSTR_JSON_ERR_BAD_STRING, p);
            if (event->sig) free(event->sig);
            event->sig = s;
            have_sig = 1;
        } else if (match_key_advance(&p, "content")) {
            if (mode != EVENT_PARSE_TEMPLATE && have_content)
                JFAIL_EV(NOSTR_JSON_ERR_BAD_KEY, p);
            char *s = nostr_json_parse_string(&p);
            if (!s) JFAIL_EV(NOSTR_JSON_ERR_BAD_STRING, p);
            if (event->content) free(event->content);
            event->content = s;
            have_content = 1;
        } else if (match_key_advance(&p, "tags")) {
            if (mode != EVENT_PARSE_TEMPLATE && have_tags)
                JFAIL_EV(NOSTR_JSON_ERR_BAD_KEY, p);
            const char *t = nostr_json_skip_ws(p);
            if (*t != '[') JFAIL_EV(NOSTR_JSON_ERR_EXPECTED_ARRAY, t);
            ++t; /* into outer tags array */

            NostrTags *parsed = nostr_tags_new(0);
            if (!parsed) JFAIL_EV(NOSTR_JSON_ERR_ALLOC, t);
            nostr_tags_reserve(parsed, 4);

            size_t tag_count = 0;
            t = nostr_json_skip_ws(t);
            while (*t && *t != ']') {
                if (*t != '[') {
                    nostr_tags_free(parsed);
                    JFAIL_EV(NOSTR_JSON_ERR_EXPECTED_ARRAY, t);
                }
                if (++tag_count > (size_t)nostr_limit_max_tags_per_event()) {
                    nostr_tags_free(parsed);
                    JFAIL_EV(NOSTR_JSON_ERR_TAG_LIMIT, t);
                }
                if (nostr_limit_max_tag_depth() < 2) {
                    nostr_tags_free(parsed);
                    JFAIL_EV(NOSTR_JSON_ERR_DEPTH_LIMIT, t);
                }
                ++t; /* into one tag array */

                NostrTag *tag = nostr_tag_new(NULL, NULL);
                if (!tag) {
                    nostr_tags_free(parsed);
                    JFAIL_EV(NOSTR_JSON_ERR_ALLOC, t);
                }

                t = nostr_json_skip_ws(t);
                while (*t && *t != ']') {
                    if (*t != '"') {
                        nostr_tag_free(tag);
                        nostr_tags_free(parsed);
                        JFAIL_EV(NOSTR_JSON_ERR_BAD_STRING, t);
                    }
                    char *sv = nostr_json_parse_string(&t);
                    if (!sv) {
                        nostr_tag_free(tag);
                        nostr_tags_free(parsed);
                        JFAIL_EV(NOSTR_JSON_ERR_BAD_STRING, t);
                    }
                    nostr_tag_append(tag, sv);
                    free(sv);

                    t = nostr_json_skip_ws(t);
                    if (*t == ',') {
                        ++t;
                        t = nostr_json_skip_ws(t);
                        if (*t == ']') {
                            nostr_tag_free(tag);
                            nostr_tags_free(parsed);
                            JFAIL_EV(NOSTR_JSON_ERR_BAD_SEPARATOR, t);
                        }
                        continue;
                    }
                    if (*t != ']') {
                        nostr_tag_free(tag);
                        nostr_tags_free(parsed);
                        JFAIL_EV(NOSTR_JSON_ERR_BAD_SEPARATOR, t);
                    }
                }
                if (*t != ']') {
                    nostr_tag_free(tag);
                    nostr_tags_free(parsed);
                    JFAIL_EV(NOSTR_JSON_ERR_UNCLOSED_BRACE, t);
                }
                ++t; /* after one tag array */
                nostr_tags_append(parsed, tag);

                t = nostr_json_skip_ws(t);
                if (*t == ',') {
                    ++t;
                    t = nostr_json_skip_ws(t);
                    if (*t == ']') {
                        nostr_tags_free(parsed);
                        JFAIL_EV(NOSTR_JSON_ERR_BAD_SEPARATOR, t);
                    }
                    continue;
                }
                if (*t != ']') {
                    nostr_tags_free(parsed);
                    JFAIL_EV(NOSTR_JSON_ERR_BAD_SEPARATOR, t);
                }
            }
            if (*t != ']') {
                nostr_tags_free(parsed);
                JFAIL_EV(NOSTR_JSON_ERR_UNCLOSED_BRACE, t);
            }
            ++t; /* after outer tags array */
            if (event->tags) nostr_tags_free(event->tags);
            event->tags = parsed;
            have_tags = 1;
            p = t;
        } else {
            // Unknown key: skip its value generically (string/number/object/array/true/false/null)
            const char *t = p;
            if (*t == '"') {
                char *dummy = nostr_json_parse_string(&t);
                if (!dummy) JFAIL_EV(NOSTR_JSON_ERR_SKIP_VALUE, t);
                free(dummy);
            } else if (*t == '{') {
                int depth = 1; ++t;
                while (*t && depth) {
                    if (*t == '"') {
                        char *d = nostr_json_parse_string(&t); if (!d) JFAIL_EV(NOSTR_JSON_ERR_SKIP_VALUE, t); free(d);
                    } else if (*t == '{') { ++depth; ++t; }
                    else if (*t == '}') { --depth; ++t; }
                    else { ++t; }
                }
                if (depth) JFAIL_EV(NOSTR_JSON_ERR_UNCLOSED_BRACE, t);
            } else if (*t == '[') {
                int depth = 1; ++t;
                while (*t && depth) {
                    if (*t == '"') {
                        char *d = nostr_json_parse_string(&t); if (!d) JFAIL_EV(NOSTR_JSON_ERR_SKIP_VALUE, t); free(d);
                    } else if (*t == '[') { ++depth; ++t; }
                    else if (*t == ']') { --depth; ++t; }
                    else { ++t; }
                }
                if (depth) JFAIL_EV(NOSTR_JSON_ERR_UNCLOSED_BRACE, t);
            } else { // number, true, false, null — advance until delimiter
                while (*t && *t!=',' && *t!='}' && *t!=']' && *t!='\n' && *t!='\r' && *t!='\t' && *t!=' ') ++t;
            }
            p = t;
        }
        // consume comma or closing brace between pairs
        p = nostr_json_skip_ws(p);
        if (*p == ',') {
            ++p;
            p = nostr_json_skip_ws(p);
            if (*p == '}' || *p == '\0') JFAIL_EV(NOSTR_JSON_ERR_BAD_SEPARATOR, p);
            continue;
        }
        if (*p == '}') { ++p; break; }
        // otherwise, invalid separator
        JFAIL_EV(NOSTR_JSON_ERR_BAD_SEPARATOR, p);
    }
    p = nostr_json_skip_ws(p);
    if (*p != '\0') JFAIL_EV(NOSTR_JSON_ERR_BAD_SEPARATOR, p);
    /* Require only `kind`: this rejects empty/garbage objects (e.g. "{}") while
     * still accepting UNSIGNED event templates submitted for signing, which
     * legitimately omit id/pubkey/sig — and often created_at/tags/content too
     * (e.g. {"kind":1,"content":"hi"}). All structural validation above
     * (trailing garbage, unclosed braces, bad separators, truncation) still
     * applies, so malformed JSON is rejected regardless of which fields exist.
     * NOTE: requiring the full signed-event field set here broke the NIP-46 /
     * NIP-55L signer flows, which deserialize templates before signing. */
    if (!have_kind)
        JFAIL_EV(NOSTR_JSON_ERR_MISSING_FIELD, p);
    if (mode == EVENT_PARSE_SIGNED &&
        !(have_id && have_pubkey && have_created_at && have_kind &&
          have_tags && have_content && have_sig))
        JFAIL_EV(NOSTR_JSON_ERR_MISSING_FIELD, p);
    if (mode == EVENT_PARSE_UNSIGNED &&
        (!(have_pubkey && have_created_at && have_kind && have_tags && have_content) ||
         have_sig))
        JFAIL_EV(have_sig ? NOSTR_JSON_ERR_BAD_KEY : NOSTR_JSON_ERR_MISSING_FIELD, p);
    return 1;
#undef JFAIL_EV
}

int nostr_event_deserialize_compact(NostrEvent *event, const char *json,
                                     NostrJsonErrorInfo *err_out) {
    return nostr_event_deserialize_compact_impl(
        event, json, err_out, EVENT_PARSE_TEMPLATE);
}

static void event_clear_owned_fields(NostrEvent *event) {
    if (!event) return;
    free(event->id); event->id = NULL;
    free(event->pubkey); event->pubkey = NULL;
    if (event->tags) nostr_tags_free(event->tags);
    event->tags = NULL;
    free(event->content); event->content = NULL;
    free(event->sig); event->sig = NULL;
}

static NostrEventValidationStatus event_deserialize_required(
    NostrEvent *event, const char *json, NostrJsonErrorInfo *err_out,
    EventParseMode mode) {
    if (!event || !json) {
        if (err_out) {
            err_out->code = NOSTR_JSON_ERR_NULL_INPUT;
            err_out->offset = -1;
        }
        return NOSTR_EVENT_VALIDATION_NULL;
    }

    if (strlen(json) > (size_t)nostr_limit_max_event_size()) {
        if (err_out) {
            err_out->code = NOSTR_JSON_ERR_OVERFLOW;
            err_out->offset = -1;
        }
        return NOSTR_EVENT_VALIDATION_LIMIT;
    }

    NostrEvent shadow = {0};
    NostrJsonErrorInfo local_err = {NOSTR_JSON_OK, -1};
    NostrJsonErrorInfo *parse_err = err_out ? err_out : &local_err;
    parse_err->code = NOSTR_JSON_OK;
    parse_err->offset = -1;
    if (!nostr_event_deserialize_compact_impl(&shadow, json, parse_err, mode)) {
        event_clear_owned_fields(&shadow);
        if (parse_err->code == NOSTR_JSON_ERR_MISSING_FIELD)
            return NOSTR_EVENT_VALIDATION_MISSING_FIELD;
        if (parse_err->code == NOSTR_JSON_ERR_TAG_LIMIT ||
            parse_err->code == NOSTR_JSON_ERR_DEPTH_LIMIT ||
            parse_err->code == NOSTR_JSON_ERR_OVERFLOW)
            return NOSTR_EVENT_VALIDATION_LIMIT;
        return NOSTR_EVENT_VALIDATION_SERIALIZATION_ERROR;
    }

    event_clear_owned_fields(event);
    event->id = shadow.id; shadow.id = NULL;
    event->pubkey = shadow.pubkey; shadow.pubkey = NULL;
    event->created_at = shadow.created_at;
    event->kind = shadow.kind;
    event->tags = shadow.tags; shadow.tags = NULL;
    event->content = shadow.content; shadow.content = NULL;
    event->sig = shadow.sig; shadow.sig = NULL;
    return NOSTR_EVENT_VALIDATION_OK;
}

NostrEventValidationStatus nostr_event_deserialize_signed(
    NostrEvent *event, const char *json, NostrJsonErrorInfo *err_out) {
    return event_deserialize_required(
        event, json, err_out, EVENT_PARSE_SIGNED);
}

NostrEventValidationStatus nostr_event_deserialize_unsigned(
    NostrEvent *event, const char *json, NostrJsonErrorInfo *err_out) {
    return event_deserialize_required(
        event, json, err_out, EVENT_PARSE_UNSIGNED);
}

void nostr_event_free(NostrEvent *event) {
    if (event) {
        free(event->id);
        free(event->pubkey);
        nostr_tags_free(event->tags);
        free(event->content);
        free(event->sig);
        free(event);
    }
}

/* Deep-copy helpers for Tags/Tag (used by nostr_event_copy) */
static NostrTag *tag_clone(const NostrTag *src) {
    if (!src) return NULL;
    size_t n = nostr_tag_size(src);
    NostrTag *dst = nostr_tag_new(nostr_tag_get_key(src), NULL);
    for (size_t i = 0; i < n; i++) {
        const char *s = nostr_tag_get(src, i);
        if (s) nostr_tag_append(dst, s);
    }
    return dst;
}

static NostrTags *tags_clone(const NostrTags *src) {
    if (!src) return NULL;
    /* nostrc-sub-uaf: Use nostr_tags_new(0) + reserve to avoid UB from
     * reading phantom va_args.  The old code set count=N with garbage data
     * pointers, then nostr_tags_append started at position N, leaving
     * garbage in 0..N-1 that would be freed by nostr_tags_free (heap corruption). */
    NostrTags *dst = nostr_tags_new(0);
    if (!dst) return NULL;
    size_t n = nostr_tags_size(src);
    if (n > 0) nostr_tags_reserve(dst, n);
    for (size_t i = 0; i < n; i++) {
        nostr_tags_append(dst, tag_clone(nostr_tags_get(src, i)));
    }
    return dst;
}

/* Deep copy of NostrEvent for GI boxed type support */
NostrEvent *nostr_event_copy(const NostrEvent *src) {
    if (!src) return NULL;
    NostrEvent *e = (NostrEvent *)malloc(sizeof(NostrEvent));
    if (!e) return NULL;
    e->id = src->id ? strdup(src->id) : NULL;
    e->pubkey = src->pubkey ? strdup(src->pubkey) : NULL;
    e->created_at = src->created_at;
    e->kind = src->kind;
    e->tags = tags_clone(src->tags);
    e->content = src->content ? strdup(src->content) : NULL;
    e->sig = src->sig ? strdup(src->sig) : NULL;
    return e;
}

/* Legacy array serializer removed: rely on compact/public JSON serializers */

static NostrEventValidationStatus event_canonical_hash(
    const NostrEvent *event, unsigned char hash[SHA256_DIGEST_LENGTH]) {
    if (!event || !hash)
        return NOSTR_EVENT_VALIDATION_NULL;

    go_autofree char *serialized = nostr_event_serialize_nip01_array(event);
    if (!serialized)
        return NOSTR_EVENT_VALIDATION_SERIALIZATION_ERROR;

    SHA256((const unsigned char *)serialized, strlen(serialized), hash);
    return NOSTR_EVENT_VALIDATION_OK;
}

static void hash_to_hex(const unsigned char hash[SHA256_DIGEST_LENGTH],
                        char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        out[i * 2] = hex[hash[i] >> 4];
        out[i * 2 + 1] = hex[hash[i] & 0x0f];
    }
    out[64] = '\0';
}

NostrEventValidationStatus nostr_event_compute_id(const NostrEvent *event,
                                                   char canonical_id_out[65]) {
    if (!canonical_id_out)
        return NOSTR_EVENT_VALIDATION_NULL;
    canonical_id_out[0] = '\0';

    unsigned char hash[SHA256_DIGEST_LENGTH];
    NostrEventValidationStatus status = event_canonical_hash(event, hash);
    if (status != NOSTR_EVENT_VALIDATION_OK)
        return status;

    hash_to_hex(hash, canonical_id_out);
    return NOSTR_EVENT_VALIDATION_OK;
}

static bool is_lower_hex(const char *value, size_t expected_len) {
    if (!value || strlen(value) != expected_len)
        return false;
    for (size_t i = 0; i < expected_len; i++) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f')))
            return false;
    }
    return true;
}

static NostrEventValidationStatus event_validate_internal(
    const NostrEvent *event, bool verify_signature, char canonical_id_out[65]) {
    if (canonical_id_out)
        canonical_id_out[0] = '\0';
    if (!event)
        return NOSTR_EVENT_VALIDATION_NULL;
    if (!event->id || !event->pubkey || !event->tags || !event->content ||
        (verify_signature && !event->sig))
        return NOSTR_EVENT_VALIDATION_MISSING_FIELD;

    unsigned char declared_id[32];
    if (!is_lower_hex(event->id, 64) ||
        !nostr_hex2bin(declared_id, event->id, sizeof(declared_id)))
        return NOSTR_EVENT_VALIDATION_BAD_ID;

    if (!is_lower_hex(event->pubkey, 64))
        return NOSTR_EVENT_VALIDATION_BAD_PUBKEY;

    unsigned char pubkey_bin[32];
    unsigned char sig_bin[64];
    if (verify_signature) {
        if (!nostr_hex2bin(pubkey_bin, event->pubkey, sizeof(pubkey_bin)))
            return NOSTR_EVENT_VALIDATION_BAD_PUBKEY;
        if (!is_lower_hex(event->sig, 128) ||
            !nostr_hex2bin(sig_bin, event->sig, sizeof(sig_bin)))
            return NOSTR_EVENT_VALIDATION_BAD_SIGNATURE_FORMAT;
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    NostrEventValidationStatus status = event_canonical_hash(event, hash);
    if (status != NOSTR_EVENT_VALIDATION_OK)
        return status;
    if (canonical_id_out)
        hash_to_hex(hash, canonical_id_out);

    if (memcmp(declared_id, hash, sizeof(hash)) != 0)
        return NOSTR_EVENT_VALIDATION_CANONICAL_ID_MISMATCH;
    if (!verify_signature)
        return NOSTR_EVENT_VALIDATION_OK;

    secp256k1_context *ctx =
        secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    if (!ctx)
        return NOSTR_EVENT_VALIDATION_CRYPTO_ERROR;

    secp256k1_xonly_pubkey pubkey;
    if (!secp256k1_xonly_pubkey_parse(ctx, &pubkey, pubkey_bin)) {
        secp256k1_context_destroy(ctx);
        return NOSTR_EVENT_VALIDATION_BAD_PUBKEY;
    }

    int verified =
        secp256k1_schnorrsig_verify(ctx, sig_bin, hash, sizeof(hash), &pubkey);
    secp256k1_context_destroy(ctx);
    return verified ? NOSTR_EVENT_VALIDATION_OK
                    : NOSTR_EVENT_VALIDATION_SIGNATURE_INVALID;
}

NostrEventValidationStatus nostr_event_validate_id(const NostrEvent *event,
                                                    char canonical_id_out[65]) {
    return event_validate_internal(event, false, canonical_id_out);
}

NostrEventValidationStatus nostr_event_validate(const NostrEvent *event,
                                                 char canonical_id_out[65]) {
    return event_validate_internal(event, true, canonical_id_out);
}

const char *nostr_event_validation_status_string(NostrEventValidationStatus status) {
    switch (status) {
    case NOSTR_EVENT_VALIDATION_OK: return "ok";
    case NOSTR_EVENT_VALIDATION_NULL: return "null input";
    case NOSTR_EVENT_VALIDATION_MISSING_FIELD: return "missing signed field";
    case NOSTR_EVENT_VALIDATION_BAD_ID: return "invalid id format";
    case NOSTR_EVENT_VALIDATION_BAD_PUBKEY: return "invalid pubkey";
    case NOSTR_EVENT_VALIDATION_BAD_SIGNATURE_FORMAT: return "invalid signature format";
    case NOSTR_EVENT_VALIDATION_CANONICAL_ID_MISMATCH: return "canonical id mismatch";
    case NOSTR_EVENT_VALIDATION_SIGNATURE_INVALID: return "signature invalid";
    case NOSTR_EVENT_VALIDATION_LIMIT: return "security limit exceeded";
    case NOSTR_EVENT_VALIDATION_SERIALIZATION_ERROR: return "serialization error";
    case NOSTR_EVENT_VALIDATION_CRYPTO_ERROR: return "crypto error";
    default: return "unknown validation status";
    }
}

char *nostr_event_get_id(NostrEvent *event) {
    char canonical_id[65];
    if (nostr_event_compute_id(event, canonical_id) != NOSTR_EVENT_VALIDATION_OK)
        return NULL;
    return strdup(canonical_id);
}

bool nostr_event_check_signature(NostrEvent *event) {
    return nostr_event_validate(event, NULL) == NOSTR_EVENT_VALIDATION_OK;
}

// Sign the event
int nostr_event_sign(NostrEvent *event, const char *private_key) {
    if (!event || !private_key)
        return -1;

    secp256k1_context *ctx;
    secp256k1_keypair keypair;
    unsigned char privkey_bin[32];    // 32-byte private key (256-bit)
    unsigned char sig_bin[64];        // 64-byte Schnorr signature
    unsigned char auxiliary_rand[32]; // Auxiliary randomness
    unsigned char hash[32];           // Schnorr requires a 32-byte hash
    int return_val = -1;

    // Convert the private key from hex to binary
    if (!nostr_hex2bin(privkey_bin, private_key, sizeof(privkey_bin))) {
        return -1;
    }

    // Create secp256k1 context for signing
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);

    // Verify that the private key is valid
    if (!secp256k1_ec_seckey_verify(ctx, privkey_bin)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }

    // Create a keypair from the private key
    if (!secp256k1_keypair_create(ctx, &keypair, privkey_bin)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }

    /* Derive x-only pubkey and set event->pubkey BEFORE computing hash.
     * The NIP-01 canonical serialization includes pubkey, so it must be set first. */
    {
        secp256k1_xonly_pubkey xpk;
        if (secp256k1_keypair_xonly_pub(ctx, &xpk, NULL, &keypair) != 1) {
            secp256k1_context_destroy(ctx);
            return -1;
        }
        unsigned char x32[32];
        if (secp256k1_xonly_pubkey_serialize(ctx, x32, &xpk) != 1) {
            secp256k1_context_destroy(ctx);
            return -1;
        }
        if (event->pubkey) { free(event->pubkey); event->pubkey = NULL; }
        event->pubkey = nostr_bin2hex(x32, 32);
        if (!event->pubkey) { secp256k1_context_destroy(ctx); return -1; }
    }

    /* Now compute the hash with the correct pubkey set */
    go_autofree char *serialized = nostr_event_serialize_nip01_array(event);
    if (!serialized) {
        secp256k1_context_destroy(ctx);
        return -1;
    }
    SHA256((unsigned char *)serialized, strlen(serialized), hash);

    // Generate 32 bytes of randomness for Schnorr signing
    if (RAND_bytes(auxiliary_rand, sizeof(auxiliary_rand)) != 1) {
        fprintf(stderr, "Failed to generate random bytes\n");
        secp256k1_context_destroy(ctx);
        return -1;
    }

    // Sign the hash using Schnorr signatures (BIP-340)
    if (secp256k1_schnorrsig_sign32(ctx, sig_bin, hash, &keypair, auxiliary_rand) != 1) {
        secp256k1_context_destroy(ctx);
        return -1;
    }

    // Convert the signature to a hex string and store it in the event
    event->sig = nostr_bin2hex(sig_bin, 64);
    if (!event->sig) { secp256k1_context_destroy(ctx); return -1; }

    // Set the event ID to the same message hash used for signing
    if (event->id) { free(event->id); event->id = NULL; }
    event->id = nostr_bin2hex(hash, 32);

    return_val = 0;

    /* keep label but silence unused-label warnings */
    if (0) goto cleanup;
cleanup:
    secp256k1_context_destroy(ctx);
    return return_val;
}

bool nostr_event_is_regular(NostrEvent *event) {
    return nostr_kind_is_regular(event->kind);
}

bool nostr_event_is_replaceable(NostrEvent *event) {
    return nostr_kind_is_replaceable(event->kind);
}

bool nostr_event_is_ephemeral(NostrEvent *event) {
    return nostr_kind_is_ephemeral(event->kind);
}

bool event_is_addressable(NostrEvent *event) {
    return nostr_kind_is_addressable(event->kind);
}

/* Accessors (public API via nostr-event.h) */

const char *nostr_event_get_pubkey(const NostrEvent *event) {
    return event ? event->pubkey : NULL;
}

void nostr_event_set_pubkey(NostrEvent *event, const char *pubkey) {
    if (!event) return;
    if (event->pubkey) { free(event->pubkey); event->pubkey = NULL; }
    if (pubkey) event->pubkey = strdup(pubkey);
}

int64_t nostr_event_get_created_at(const NostrEvent *event) {
    return event ? event->created_at : 0;
}

void nostr_event_set_created_at(NostrEvent *event, int64_t created_at) {
    if (!event) return;
    event->created_at = created_at;
}

int nostr_event_get_kind(const NostrEvent *event) {
    return event ? event->kind : 0;
}

void nostr_event_set_kind(NostrEvent *event, int kind) {
    if (!event) return;
    event->kind = kind;
}

void *nostr_event_get_tags(const NostrEvent *event) {
    return event ? (void *)event->tags : NULL;
}

void nostr_event_set_tags(NostrEvent *event, void *tags) {
    if (!event) return;
    if (event->tags && (void *)event->tags != tags) {
        nostr_tags_free(event->tags);
    }
    event->tags = (NostrTags *)tags; /* takes ownership */
}

const char *nostr_event_get_content(const NostrEvent *event) {
    return event ? event->content : NULL;
}

void nostr_event_set_content(NostrEvent *event, const char *content) {
    if (!event) return;
    if (event->content) { free(event->content); event->content = NULL; }
    if (content) event->content = strdup(content);
}

const char *nostr_event_get_sig(const NostrEvent *event) {
    return event ? event->sig : NULL;
}

void nostr_event_set_sig(NostrEvent *event, const char *sig) {
    if (!event) return;
    if (event->sig) { free(event->sig); event->sig = NULL; }
    if (sig) event->sig = strdup(sig);
}

/* ---- Compact, allocation-light JSON serializer (object form) ---- */

static int append_str(char **buf, size_t *cap, size_t *len, const char *s) {
    size_t sl = strlen(s);
    if (*len + sl + 1 > *cap) {
        size_t ncap = (*cap) * 2;
        if (ncap < *len + sl + 1) ncap = *len + sl + 1;
        char *tmp = (char *)realloc(*buf, ncap);
        if (!tmp) return -1;
        *buf = tmp; *cap = ncap;
    }
    memcpy(*buf + *len, s, sl);
    *len += sl;
    (*buf)[*len] = '\0';
    return 0;
}

static int append_fmt(char **buf, size_t *cap, size_t *len, const char *fmt, ...) {
    va_list ap;
    while (1) {
        va_start(ap, fmt);
        int avail = (int)(*cap - *len);
        int n = vsnprintf(*buf + *len, avail, fmt, ap);
        va_end(ap);
        if (n < 0) return -1;
        if (n < avail) { *len += (size_t)n; return 0; }
        size_t ncap = (*cap) * 2;
        if (ncap < *len + (size_t)n + 1) ncap = *len + (size_t)n + 1;
        char *tmp = (char *)realloc(*buf, ncap);
        if (!tmp) return -1;
        *buf = tmp; *cap = ncap;
    }
}

char *nostr_event_serialize_compact(const NostrEvent *event) {
    if (!event) return NULL;
    size_t cap = 256; size_t len = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';

    if (append_str(&out, &cap, &len, "{") != 0) { free(out); return NULL; }
    bool first = true;
    /* helper to add comma between fields */
    #define ADD_COMMA() do { if (!first) { if (append_str(&out, &cap, &len, ",") != 0) { free(out); return NULL; } } first = false; } while (0)

    // id
    if (event->id && *event->id) {
        char *escaped = nostr_escape_string(event->id);
        if (!escaped) { free(out); return NULL; }
        ADD_COMMA();
        if (append_fmt(&out, &cap, &len, "\"id\":\"%s\"", escaped) != 0) { free(escaped); free(out); return NULL; }
        free(escaped);
    }
    // pubkey
    if (event->pubkey && *event->pubkey) {
        char *escaped = nostr_escape_string(event->pubkey);
        if (!escaped) { free(out); return NULL; }
        ADD_COMMA();
        if (append_fmt(&out, &cap, &len, "\"pubkey\":\"%s\"", escaped) != 0) { free(escaped); free(out); return NULL; }
        free(escaped);
    }
    // created_at
    if (event->created_at > 0) {
        ADD_COMMA();
        if (append_fmt(&out, &cap, &len, "\"created_at\":%lld", (long long)event->created_at) != 0) { free(out); return NULL; }
    }
    // kind
    ADD_COMMA();
    if (append_fmt(&out, &cap, &len, "\"kind\":%d", event->kind) != 0) { free(out); return NULL; }

    // tags (always required by nostrdb)
    ADD_COMMA();
    if (event->tags) {
        char *tags_json = nostr_tags_to_json(event->tags);
        if (!tags_json) { free(out); return NULL; }
        if (append_str(&out, &cap, &len, "\"tags\":") != 0) { free(tags_json); free(out); return NULL; }
        if (append_str(&out, &cap, &len, tags_json) != 0) { free(tags_json); free(out); return NULL; }
        free(tags_json);
    } else {
        if (append_str(&out, &cap, &len, "\"tags\":[]") != 0) { free(out); return NULL; }
    }

    // content (always required by nostrdb)
    ADD_COMMA();
    if (event->content) {
        char *escaped = nostr_escape_string(event->content);
        if (!escaped) { free(out); return NULL; }
        if (append_str(&out, &cap, &len, "\"content\":\"") != 0) { free(escaped); free(out); return NULL; }
        if (append_str(&out, &cap, &len, escaped) != 0) { free(escaped); free(out); return NULL; }
        if (append_str(&out, &cap, &len, "\"") != 0) { free(escaped); free(out); return NULL; }
        free(escaped);
    } else {
        if (append_str(&out, &cap, &len, "\"content\":\"\"") != 0) { free(out); return NULL; }
    }

    // sig
    if (event->sig && *event->sig) {
        char *escaped = nostr_escape_string(event->sig);
        if (!escaped) { free(out); return NULL; }
        ADD_COMMA();
        if (append_fmt(&out, &cap, &len, "\"sig\":\"%s\"", escaped) != 0) { free(escaped); free(out); return NULL; }
        free(escaped);
    }

    if (append_str(&out, &cap, &len, "}") != 0) { free(out); return NULL; }
    #undef ADD_COMMA
    return out;
}

/* ========================================================================
 * Event Priority Classification (nostrc-7u2)
 * ======================================================================== */

NostrEventPriority nostr_event_get_priority(const NostrEvent *event, const char *user_pubkey) {
    if (!event) return NOSTR_EVENT_PRIORITY_NORMAL;

    int kind = event->kind;

    /* CRITICAL: DMs (kind 4 legacy, 1059 NIP-17 gift wrap), zaps (kind 9735) */
    if (kind == 4 || kind == 1059 || kind == 9735) {
        return NOSTR_EVENT_PRIORITY_CRITICAL;
    }

    /* LOW: Reactions (kind 7), reposts (kind 6) */
    if (kind == 7 || kind == 6) {
        return NOSTR_EVENT_PRIORITY_LOW;
    }

    /* Check for mentions of current user (CRITICAL if mentioned) */
    if (user_pubkey && *user_pubkey && event->tags) {
        size_t tag_count = nostr_tags_size(event->tags);
        for (size_t i = 0; i < tag_count; i++) {
            NostrTag *tag = nostr_tags_get(event->tags, i);
            if (!tag) continue;

            const char *tag_name = nostr_tag_get_key(tag);
            if (!tag_name) continue;

            /* Check "p" tags for pubkey mentions */
            if (tag_name[0] == 'p' && tag_name[1] == '\0') {
                const char *tagged_pubkey = nostr_tag_get_value(tag);
                if (tagged_pubkey && strcmp(tagged_pubkey, user_pubkey) == 0) {
                    return NOSTR_EVENT_PRIORITY_CRITICAL;
                }
            }
        }
    }

    /* HIGH: Text notes with "e" tags (replies) */
    if (kind == 1 && event->tags) {
        size_t tag_count = nostr_tags_size(event->tags);
        for (size_t i = 0; i < tag_count; i++) {
            NostrTag *tag = nostr_tags_get(event->tags, i);
            if (!tag) continue;

            const char *tag_name = nostr_tag_get_key(tag);
            if (tag_name && tag_name[0] == 'e' && tag_name[1] == '\0') {
                return NOSTR_EVENT_PRIORITY_HIGH;
            }
        }
    }

    /* Everything else is NORMAL */
    return NOSTR_EVENT_PRIORITY_NORMAL;
}
