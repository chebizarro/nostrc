#include "nostr-event.h"
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

static inline int hexval_char(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
    if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
    return -1;
}

/* === NIP-01 canonical preimage serializer ===
 * Build the exact JSON array: [0, pubkey, created_at, kind, tags, content]
 * where pubkey and content are JSON strings, tags is a JSON array.
 * This excludes id and sig by definition.
 */
static char *nostr_event_serialize_nip01_array(const NostrEvent *event) {
    if (!event) return NULL;
    /* pubkey */
    const char *pk = event->pubkey ? event->pubkey : "";
    char *pk_esc = nostr_escape_string(pk);
    if (!pk_esc) return NULL;
    /* content */
    const char *ct = event->content ? event->content : "";
    char *ct_esc = nostr_escape_string(ct);
    if (!ct_esc) { free(pk_esc); return NULL; }
    /* tags */
    char *tags_json = NULL;
    if (event->tags) {
        tags_json = nostr_tags_to_json(event->tags);
        if (!tags_json) { free(pk_esc); free(ct_esc); return NULL; }
    } else {
        tags_json = strdup("[]");
        if (!tags_json) { free(pk_esc); free(ct_esc); return NULL; }
    }
    /* size rough estimate */
    size_t cap = strlen(pk_esc) + strlen(ct_esc) + strlen(tags_json) + 64;
    char *out = (char *)malloc(cap);
    if (!out) { free(pk_esc); free(ct_esc); free(tags_json); return NULL; }
    int n = snprintf(out, cap, "[0,\"%s\",%lld,%d,%s,\"%s\"]",
                     pk_esc,
                     (long long)event->created_at,
                     event->kind,
                     tags_json,
                     ct_esc);
    free(pk_esc);
    free(ct_esc);
    free(tags_json);
    if (n < 0 || (size_t)n >= cap) { free(out); return NULL; }
    return out;
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

static const char *skip_ws_local(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
    return p;
}

/* Match a JSON key in-place: expects p to point at '"'. If it matches
 * the given key literal, advances *pp to the character after the colon
 * (value start, possibly with whitespace) and returns 1. Otherwise 0. */
static int match_key_advance(const char **pp, const char *key) {
    const char *p = skip_ws_local(*pp);
    if (*p != '"') return 0;
    ++p;
    const char *k = key;
    while (*k && *p == *k) { ++p; ++k; }
    if (*k != '\0' || *p != '"') return 0; /* not exact match */
    ++p; /* after closing quote */
    p = skip_ws_local(p);
    if (*p != ':') return 0;
    ++p; /* after ':' */
    *pp = p;
    return 1;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static const char *find_key(const char *json, const char *key) {
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strstr(p, key)) != NULL) {
        // ensure it's a JSON key: preceded by '"' and followed by '"'
        if (p > json && *(p-1) == '"' && *(p + klen) == '"') {
            const char *q = p + klen + 1; // after closing quote
            q = skip_ws_local(q);
            if (*q == ':') return q + 1; // point to value start (maybe ws)
        }
        p += klen;
    }
    return NULL;
}

static int parse_json_string_fast(const char **pp, char **out) {
    const char *p = skip_ws_local(*pp);
    if (*p != '"') return 0;
    ++p;
    const char *start = p;
    // First scan to see if there are any escapes; if none, copy directly
    const char *q = p;
    int has_escape = 0;
    while (*q && *q != '"') {
        if (*q == '\\') { has_escape = 1; break; }
        ++q;
    }
    if (!*q) return 0; // missing closing quote
    if (!has_escape) {
        size_t len = (size_t)(q - start);
        char *s = (char *)malloc(len + 1);
        if (!s) return 0;
        memcpy(s, start, len);
        s[len] = '\0';
        *out = s;
        *pp = q + 1;
        return 1;
    }
    // Has escapes: decode common ones; bail on unicode (\u)
    // Allocate buffer equal to remaining length as upper bound
    size_t cap = (size_t)(strlen(p) + 1);
    char *buf = (char *)malloc(cap);
    if (!buf) return 0;
    size_t len = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            char e = *p++;
            switch (e) {
                case '"': buf[len++] = '"'; break;
                case '\\': buf[len++] = '\\'; break;
                case '/': buf[len++] = '/'; break;
                case 'b': buf[len++] = '\b'; break;
                case 'f': buf[len++] = '\f'; break;
                case 'n': buf[len++] = '\n'; break;
                case 'r': buf[len++] = '\r'; break;
                case 't': buf[len++] = '\t'; break;
                case 'u': {
                    // Decode \uXXXX (with surrogate pair support) into UTF-8
                    int h0 = hexval_char(*p); if (h0 < 0) { free(buf); return 0; } ++p;
                    int h1 = hexval_char(*p); if (h1 < 0) { free(buf); return 0; } ++p;
                    int h2 = hexval_char(*p); if (h2 < 0) { free(buf); return 0; } ++p;
                    int h3 = hexval_char(*p); if (h3 < 0) { free(buf); return 0; } ++p;
                    uint32_t cp = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                    // Handle surrogate pairs
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // Expect next sequence: \uDC00-\uDFFF
                        if (*p != '\\' || *(p+1) != 'u') { free(buf); return 0; }
                        p += 2;
                        int g0 = hexval_char(*p); if (g0 < 0) { free(buf); return 0; } ++p;
                        int g1 = hexval_char(*p); if (g1 < 0) { free(buf); return 0; } ++p;
                        int g2 = hexval_char(*p); if (g2 < 0) { free(buf); return 0; } ++p;
                        int g3 = hexval_char(*p); if (g3 < 0) { free(buf); return 0; } ++p;
                        uint32_t low = (uint32_t)((g0 << 12) | (g1 << 8) | (g2 << 4) | g3);
                        if (low < 0xDC00 || low > 0xDFFF) { free(buf); return 0; }
                        cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        // Lone low surrogate is invalid
                        free(buf); return 0;
                    }
                    // UTF-8 encode
                    if (cp <= 0x7F) {
                        if (len + 1 + 1 > cap) { size_t ncap = cap * 2; while (len + 2 > ncap) ncap *= 2; char *tmp = realloc(buf, ncap); if (!tmp) { free(buf); return 0; } buf = tmp; cap = ncap; }
                        buf[len++] = (char)cp;
                    } else if (cp <= 0x7FF) {
                        if (len + 2 + 1 > cap) { size_t ncap = cap * 2; while (len + 3 > ncap) ncap *= 2; char *tmp = realloc(buf, ncap); if (!tmp) { free(buf); return 0; } buf = tmp; cap = ncap; }
                        buf[len++] = (char)(0xC0 | (cp >> 6));
                        buf[len++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp <= 0xFFFF) {
                        if (len + 3 + 1 > cap) { size_t ncap = cap * 2; while (len + 4 > ncap) ncap *= 2; char *tmp = realloc(buf, ncap); if (!tmp) { free(buf); return 0; } buf = tmp; cap = ncap; }
                        buf[len++] = (char)(0xE0 | (cp >> 12));
                        buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[len++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        if (len + 4 + 1 > cap) { size_t ncap = cap * 2; while (len + 5 > ncap) ncap *= 2; char *tmp = realloc(buf, ncap); if (!tmp) { free(buf); return 0; } buf = tmp; cap = ncap; }
                        buf[len++] = (char)(0xF0 | (cp >> 18));
                        buf[len++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[len++] = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default:
                    // invalid escape
                    free(buf);
                    return 0;
            }
        } else {
            buf[len++] = (char)c;
        }
    }
    if (*p != '"') { free(buf); return 0; }
    buf[len] = '\0';
    *out = buf;
    *pp = p + 1;
    return 1;
}

static int parse_json_int64_simple(const char **pp, long long *out) {
    const char *p = skip_ws_local(*pp);
    int neg = 0; long long v = 0; int any = 0;
    if (*p == '-') { neg = 1; ++p; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; any = 1; }
    if (!any) return 0;
    *out = neg ? -v : v;
    *pp = p;
    return 1;
}

int nostr_event_deserialize_compact(NostrEvent *event, const char *json) {
    if (!event || !json) return 0;
    const char *p = skip_ws_local(json);
    if (*p != '{') return 0;
    ++p; // after '{'

    int have_kind = 0;
    int have_created_at = 0;

    while (1) {
        p = skip_ws_local(p);
        /* Allow and skip commas between members */
        if (*p == ',') { ++p; continue; }
        if (*p == '}') { ++p; break; }
        // Dispatch by key (in-place match, no allocation for known keys)
        if (match_key_advance(&p, "kind")) {
            long long v = 0;
            if (!parse_json_int64_simple(&p, &v)) { return 0; }
            event->kind = (int)v; have_kind = 1;
        } else if (match_key_advance(&p, "created_at")) {
            long long ts = 0;
            if (!parse_json_int64_simple(&p, &ts)) { return 0; }
            event->created_at = (int64_t)ts; have_created_at = 1;
        } else if (match_key_advance(&p, "pubkey")) {
            char *s = NULL;
            if (!parse_json_string_fast(&p, &s)) { return 0; }
            if (event->pubkey) free(event->pubkey);
            event->pubkey = s;
        } else if (match_key_advance(&p, "id")) {
            char *s = NULL;
            if (!parse_json_string_fast(&p, &s)) { return 0; }
            if (event->id) free(event->id);
            event->id = s;
        } else if (match_key_advance(&p, "sig")) {
            char *s = NULL;
            if (!parse_json_string_fast(&p, &s)) { return 0; }
            if (event->sig) free(event->sig);
            event->sig = s;
        } else if (match_key_advance(&p, "content")) {
            char *s = NULL;
            if (!parse_json_string_fast(&p, &s)) { return 0; }
            if (event->content) free(event->content);
            event->content = s;
        } else if (match_key_advance(&p, "tags")) {
            const char *t = skip_ws_local(p);
            if (*t != '[') { return 0; }
            ++t; // into tags array
            /* Pre-count number of tags at top-level to reserve capacity */
            const char *scan = t; int depth = 1; int max_depth = 1; size_t tag_count = 0;
            while (*scan && depth) {
                scan = skip_ws_local(scan);
                if (*scan == '[' && depth == 1) { ++tag_count; ++scan; }
                else if (*scan == '"') { char *d=NULL; if (!parse_json_string_fast(&scan, &d)) return 0; free(d); }
                else if (*scan == '[') { ++depth; if (depth > max_depth) max_depth = depth; ++scan; }
                else if (*scan == ']') { --depth; ++scan; }
                else { ++scan; }
            }
            if (depth) { return 0; }
            /* Enforce security limits */
            if (tag_count > (size_t)nostr_limit_max_tags_per_event()) { return 0; }
            if (max_depth > (int)nostr_limit_max_tag_depth()) { return 0; }
            NostrTags *parsed = nostr_tags_new(tag_count);
            if (!parsed) { return 0; }
            /* If tags_new pre-filled data slots with garbage, reset count to 0 but keep capacity */
            parsed->count = 0;
            nostr_tags_reserve(parsed, tag_count > 0 ? tag_count : 4);
            t = skip_ws_local(t);
            while (*t && *t != ']') {
                if (*t != '[') { nostr_tags_free(parsed); return 0; }
                ++t; // into a tag array
                NostrTag *tag = nostr_tag_new(NULL, NULL);
                if (!tag) { nostr_tags_free(parsed); return 0; }
                /* Reserve elements for this tag by scanning until closing ']' */
                const char *t2 = t; size_t elem_count = 0;
                while (*t2 && *t2 != ']') {
                    t2 = skip_ws_local(t2);
                    if (*t2 == '"') { char *tmp=NULL; if (!parse_json_string_fast(&t2, &tmp)) { nostr_tag_free(tag); nostr_tags_free(parsed); return 0; } free(tmp); ++elem_count; t2 = skip_ws_local(t2); if (*t2 == ',') { ++t2; } }
                    else { break; }
                }
                if (elem_count > 0) nostr_tag_reserve(tag, elem_count);
                t = skip_ws_local(t);
                while (*t && *t != ']') {
                    char *sv = NULL;
                    if (!parse_json_string_fast(&t, &sv)) { nostr_tag_free(tag); nostr_tags_free(parsed); return 0; }
                    nostr_tag_append(tag, sv);
                    free(sv);
                    t = skip_ws_local(t);
                    if (*t == ',') { ++t; t = skip_ws_local(t); }
                }
                if (*t != ']') { nostr_tag_free(tag); nostr_tags_free(parsed); return 0; }
                ++t; // after closing tag array
                nostr_tags_append(parsed, tag);
                t = skip_ws_local(t);
                if (*t == ',') { ++t; t = skip_ws_local(t); }
            }
            if (*t != ']') { nostr_tags_free(parsed); return 0; }
            ++t; // after closing tags
            if (event->tags) nostr_tags_free(event->tags);
            event->tags = parsed;
            p = t; // advance parser position
        } else {
            // Unknown key: skip its value generically (string/number/object/array/true/false/null)
            // Minimal skipper for compact inputs
            const char *t = p;
            if (*t == '"') {
                char *dummy = NULL;
                if (!parse_json_string_fast(&t, &dummy)) { return 0; }
                free(dummy);
            } else if (*t == '{') {
                int depth = 1; ++t;
                while (*t && depth) {
                    if (*t == '"') {
                        char *d = NULL; if (!parse_json_string_fast(&t, &d)) { return 0; } free(d);
                    } else if (*t == '{') { ++depth; ++t; }
                    else if (*t == '}') { --depth; ++t; }
                    else { ++t; }
                }
                if (depth) { return 0; }
            } else if (*t == '[') {
                int depth = 1; ++t;
                while (*t && depth) {
                    if (*t == '"') {
                        char *d = NULL; if (!parse_json_string_fast(&t, &d)) { return 0; } free(d);
                    } else if (*t == '[') { ++depth; ++t; }
                    else if (*t == ']') { --depth; ++t; }
                    else { ++t; }
                }
                if (depth) { return 0; }
            } else { // number, true, false, null — advance until delimiter
                while (*t && *t!=',' && *t!='}' && *t!=']' && *t!='\n' && *t!='\r' && *t!='\t' && *t!=' ') ++t;
            }
            p = t;
        }
        // consume comma or closing brace between pairs
        p = skip_ws_local(p);
        if (*p == ',') { ++p; continue; }
        if (*p == '}') { ++p; break; }
        // otherwise, invalid separator
        return 0;
    }
    (void)have_kind;
    (void)have_created_at;
    return 1;
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
    NostrTags *dst = nostr_tags_new(nostr_tags_size(src));
    if (!dst) return NULL;
    for (size_t i = 0; i < nostr_tags_size(src); i++) {
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

char *nostr_event_get_id(NostrEvent *event) {
    if (!event)
        return NULL;

    // OPTIMIZATION (nostrc-o56): Return cached ID if available.
    // Events received from relays already have their ID set during deserialization.
    // Returning the cached value avoids expensive serialization+hashing on every call,
    // and more importantly, prevents use-after-free races where another thread frees
    // the event while we're serializing it.
    if (event->id && *event->id) {
        return strdup(event->id);  // Return copy for transfer-full semantics
    }

    // ID not cached - compute it from the canonical NIP-01 array preimage
    char *serialized = nostr_event_serialize_nip01_array(event);
    if (!serialized)
        return NULL;

    // Hash the serialized event using SHA-256
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char *)serialized, strlen(serialized), hash);
    free(serialized);

    // Convert the binary hash to a hex string
    char *id = nostr_bin2hex(hash, SHA256_DIGEST_LENGTH);

    // Cache the computed ID for future calls
    if (id && !event->id) {
        event->id = strdup(id);
    }

    return id;
}

bool nostr_event_check_signature(NostrEvent *event) {
    if (!event) {
        fprintf(stderr, "Event is null\n");
        return false;
    }

    // Decode public key from hex
    unsigned char pubkey_bin[32]; // 32 bytes for schnorr pubkey
    if (!nostr_hex2bin(pubkey_bin, event->pubkey, sizeof(pubkey_bin))) {
        fprintf(stderr, "Invalid public key hex\n");
        return false;
    }

    // Decode signature from hex
    unsigned char sig_bin[64]; // 64 bytes for schnorr signature
    if (!nostr_hex2bin(sig_bin, event->sig, sizeof(sig_bin))) {
        fprintf(stderr, "Invalid signature hex\n");
        return false;
    }

    // Create secp256k1 context
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    if (!ctx) {
        fprintf(stderr, "Failed to create secp256k1 context\n");
        return false;
    }

    // Parse the public key using secp256k1_xonly_pubkey_parse (for Schnorr signatures)
    secp256k1_xonly_pubkey pubkey;
    if (!secp256k1_xonly_pubkey_parse(ctx, &pubkey, pubkey_bin)) {
        fprintf(stderr, "Failed to parse public key\n");
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Always recompute the 32-byte message hash from NIP-01 canonical array.
    unsigned char hash[32];
    char *serialized = nostr_event_serialize_nip01_array(event);
    if (!serialized) {
        fprintf(stderr, "Failed to serialize canonical preimage\n");
        secp256k1_context_destroy(ctx);
        return false;
    }
    SHA256((unsigned char *)serialized, strlen(serialized), hash);
    free(serialized);

    /*** Verification ***/

    // Verify the signature using secp256k1_schnorrsig_verify
    int verified = secp256k1_schnorrsig_verify(ctx, sig_bin, hash, 32, &pubkey);

    // Clean up
    secp256k1_context_destroy(ctx);

    if (verified) {
        return true;
    } else {
        fprintf(stderr, "Signature verification failed for event id=%s pubkey=%s\n", 
                event->id ? event->id : "(null)", 
                event->pubkey ? event->pubkey : "(null)");
        return false;
    }
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
    char *serialized = nostr_event_serialize_nip01_array(event);
    if (!serialized) {
        secp256k1_context_destroy(ctx);
        return -1;
    }
    SHA256((unsigned char *)serialized, strlen(serialized), hash);
    free(serialized);

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
    return event->kind < 1000 && event->kind != 0 && event->kind != 3;
}

bool nostr_event_is_replaceable(NostrEvent *event) {
    return event->kind == 0 || event->kind == 3 || (10000 <= event->kind && event->kind < 20000);
}

bool nostr_event_is_ephemeral(NostrEvent *event) {
    return 20000 <= event->kind && event->kind < 30000;
}

bool event_is_addressable(NostrEvent *event) {
    return 30000 <= event->kind && event->kind < 40000;
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
        ADD_COMMA();
        if (append_fmt(&out, &cap, &len, "\"id\":\"%s\"", event->id) != 0) { free(out); return NULL; }
    }
    // pubkey
    if (event->pubkey && *event->pubkey) {
        ADD_COMMA();
        if (append_fmt(&out, &cap, &len, "\"pubkey\":\"%s\"", event->pubkey) != 0) { free(out); return NULL; }
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
        ADD_COMMA();
        if (append_fmt(&out, &cap, &len, "\"sig\":\"%s\"", event->sig) != 0) { free(out); return NULL; }
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
