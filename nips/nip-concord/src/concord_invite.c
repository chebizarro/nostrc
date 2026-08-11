/* nip-concord: invites (CORD-05) and stream payload layers (CORD-01). */

#include "concord_internal.h"

#include <stdlib.h>
#include <string.h>

#include <nostr/nip44/nip44.h>
#include <openssl/crypto.h>

/* CORD-05 §3, dictionary generation 4. Vector and Soapbox ship it
 * identically, so an invite minted by either client opens in the other. */
static const char *const kStockRelays[] = {
    "wss://jskitty.com/nostr",       /* 1, Vector  */
    "wss://asia.vectorapp.io/nostr", /* 2, Vector  */
    "wss://relay.ditto.pub",         /* 3, Soapbox */
    "wss://relay.dreamith.to",       /* 4, Soapbox */
};

/* The stock-set selector inside the fragment's flags byte. */
#define CONCORD_FRAGMENT_FLAG_STOCK_RELAYS 0x01
/* Escape codes in a relay entry's leading byte. */
#define CONCORD_RELAY_ENTRY_WSS_LITERAL 0x00
#define CONCORD_RELAY_ENTRY_VERBATIM 0xff

const char *nostr_concord_relay_dictionary_lookup(uint8_t id) {
    if (id < 1 || id > (uint8_t)(sizeof(kStockRelays) / sizeof(kStockRelays[0])))
        return NULL;
    return kStockRelays[id - 1];
}

const char *const *nostr_concord_stock_relays(size_t *n_relays) {
    if (n_relays) *n_relays = sizeof(kStockRelays) / sizeof(kStockRelays[0]);
    return kStockRelays;
}

nostr_concord_status_t nostr_concord_invite_key(
    const uint8_t token[CONCORD_INVITE_TOKEN_BYTES],
    uint8_t bundle_key_out[32]) {
    if (!token || !bundle_key_out) return NOSTR_CONCORD_ERR_NULL;
    /* A.6: secret = token, id = all-zeroes, no epoch. The result is the NIP-44
     * conversation key directly — this label yields a raw decrypt key, not a
     * group_key, so no scalar normalization happens here. */
    static const uint8_t zero_id[32] = { 0 };
    return nostr_concord_hkdf(CONCORD_LABEL_INVITE_KEY, token,
                              CONCORD_INVITE_TOKEN_BYTES, zero_id, false, 0,
                              bundle_key_out);
}

void nostr_concord_invite_fragment_clear(
    nostr_concord_invite_fragment_t *fragment) {
    if (!fragment) return;
    if (fragment->relays) {
        for (size_t i = 0; i < fragment->n_relays; i++)
            free(fragment->relays[i]);
        free(fragment->relays);
    }
    OPENSSL_cleanse(fragment->token, sizeof(fragment->token));
    memset(fragment, 0, sizeof(*fragment));
}

/* The shortest encoding of one relay: a dictionary id is a single byte, a
 * wss:// host drops its scheme, and anything else rides verbatim. Returns the
 * bytes written, or 0 when the URL cannot fit a length byte. */
static size_t concord_encode_relay(const char *url, uint8_t *out,
                                   size_t out_cap) {
    if (!url || !*url) return 0;

    for (uint8_t id = 1; id < 255; id++) {
        const char *known = nostr_concord_relay_dictionary_lookup(id);
        if (!known) continue;
        if (strcmp(known, url) == 0) {
            if (out_cap < 1) return 0;
            out[0] = id;
            return 1;
        }
    }

    static const char scheme[] = "wss://";
    size_t scheme_len = sizeof(scheme) - 1;
    bool wss = strncmp(url, scheme, scheme_len) == 0;
    const char *body = wss ? url + scheme_len : url;
    size_t body_len = strlen(body);
    if (body_len == 0 || body_len > 255) return 0;
    if (out_cap < 2 + body_len) return 0;

    out[0] = wss ? CONCORD_RELAY_ENTRY_WSS_LITERAL : CONCORD_RELAY_ENTRY_VERBATIM;
    out[1] = (uint8_t)body_len;
    memcpy(out + 2, body, body_len);
    return 2 + body_len;
}

nostr_concord_status_t nostr_concord_invite_fragment_encode(
    const uint8_t token[CONCORD_INVITE_TOKEN_BYTES],
    const char *const *relays, size_t n_relays, bool stock_relays,
    char **fragment_out) {
    if (!token || !fragment_out) return NOSTR_CONCORD_ERR_NULL;
    *fragment_out = NULL;

    /* The fragment only needs to *find* the bundle, which then carries the
     * Community's authoritative relay set (CORD-02 §6). */
    if (stock_relays) {
        relays = NULL;
        n_relays = 0;
    }
    if (n_relays > CONCORD_MAX_RELAYS_IN_FRAGMENT)
        return NOSTR_CONCORD_ERR_CARDINALITY;
    if (n_relays && !relays) return NOSTR_CONCORD_ERR_NULL;

    /* [version][flags][count] + at most 3 relays of 2 + 255 bytes + token. */
    uint8_t raw[3 + CONCORD_MAX_RELAYS_IN_FRAGMENT * (2 + 255) +
                CONCORD_INVITE_TOKEN_BYTES];
    size_t off = 0;
    raw[off++] = CONCORD_INVITE_FRAGMENT_VERSION;
    raw[off++] = stock_relays ? CONCORD_FRAGMENT_FLAG_STOCK_RELAYS : 0x00;

    if (!stock_relays) {
        size_t count_at = off;
        raw[off++] = 0;
        uint8_t written = 0;
        for (size_t i = 0; i < n_relays; i++) {
            size_t n = concord_encode_relay(relays[i], raw + off,
                                            sizeof(raw) - off -
                                              CONCORD_INVITE_TOKEN_BYTES);
            /* A relay too long to carry is dropped rather than failing the
             * mint: the fragment is a bootstrap hint, and the bundle holds
             * the authoritative set. */
            if (n == 0) continue;
            off += n;
            written++;
        }
        raw[count_at] = written;
    }

    memcpy(raw + off, token, CONCORD_INVITE_TOKEN_BYTES);
    off += CONCORD_INVITE_TOKEN_BYTES;

    char *encoded = nostr_concord_b64url_encode(raw, off);
    if (!encoded) return NOSTR_CONCORD_ERR_OOM;
    *fragment_out = encoded;
    return NOSTR_CONCORD_OK;
}

nostr_concord_status_t nostr_concord_invite_fragment_parse(
    const char *fragment, nostr_concord_invite_fragment_t *out) {
    if (!fragment || !out) return NOSTR_CONCORD_ERR_NULL;
    memset(out, 0, sizeof(*out));

    /* Tolerate a leading '#' so callers can hand over the URL fragment
     * verbatim, separator included. */
    if (*fragment == '#') fragment++;

    size_t raw_len = 0;
    uint8_t *raw = nostr_concord_b64url_decode(fragment, &raw_len);
    if (!raw) return NOSTR_CONCORD_ERR_BAD_FRAGMENT;

    nostr_concord_status_t status = NOSTR_CONCORD_ERR_BAD_FRAGMENT;
    char **relays = NULL;
    size_t n_relays = 0;

    /* [version][flags] … [token:16] */
    if (raw_len < 2 + CONCORD_INVITE_TOKEN_BYTES) goto done;

    size_t off = 0;
    uint8_t version = raw[off++];
    uint8_t flags = raw[off++];

    /* A client MAY reject a lower version rather than decode it against the
     * wrong dictionary generation (CORD-05 §3). We do. */
    if (version < CONCORD_INVITE_FRAGMENT_VERSION) {
        status = NOSTR_CONCORD_ERR_UNSUPPORTED_VERSION;
        goto done;
    }

    bool stock = (flags & CONCORD_FRAGMENT_FLAG_STOCK_RELAYS) != 0;
    if (!stock) {
        if (off >= raw_len) goto done;
        uint8_t count = raw[off++];
        if (count > CONCORD_MAX_RELAYS_IN_FRAGMENT) goto done;

        if (count) {
            relays = calloc(count, sizeof(*relays));
            if (!relays) { status = NOSTR_CONCORD_ERR_OOM; goto done; }
        }

        for (uint8_t i = 0; i < count; i++) {
            if (off >= raw_len) goto done;
            uint8_t lead = raw[off++];

            if (lead != CONCORD_RELAY_ENTRY_WSS_LITERAL &&
                lead != CONCORD_RELAY_ENTRY_VERBATIM) {
                const char *url = nostr_concord_relay_dictionary_lookup(lead);
                if (!url) goto done;
                relays[n_relays] = concord_strndup(url, strlen(url));
                if (!relays[n_relays]) { status = NOSTR_CONCORD_ERR_OOM; goto done; }
                n_relays++;
                continue;
            }

            if (off >= raw_len) goto done;
            uint8_t len = raw[off++];
            if (len == 0 || off + len > raw_len) goto done;

            if (lead == CONCORD_RELAY_ENTRY_WSS_LITERAL) {
                /* "wss://" is re-prepended on decode. */
                static const char scheme[] = "wss://";
                size_t scheme_len = sizeof(scheme) - 1;
                char *url = malloc(scheme_len + len + 1);
                if (!url) { status = NOSTR_CONCORD_ERR_OOM; goto done; }
                memcpy(url, scheme, scheme_len);
                memcpy(url + scheme_len, raw + off, len);
                url[scheme_len + len] = '\0';
                relays[n_relays++] = url;
            } else {
                char *url = concord_strndup((const char *)(raw + off), len);
                if (!url) { status = NOSTR_CONCORD_ERR_OOM; goto done; }
                relays[n_relays++] = url;
            }
            off += len;
        }
    }

    /* The token is the fragment's tail and is exactly 16 bytes: anything left
     * over means a malformed relay section silently swallowed bytes. */
    if (raw_len - off != CONCORD_INVITE_TOKEN_BYTES) goto done;

    memcpy(out->token, raw + off, CONCORD_INVITE_TOKEN_BYTES);
    out->relays = relays;
    out->n_relays = n_relays;
    out->stock_relays = stock;
    out->version = version;
    relays = NULL;
    status = NOSTR_CONCORD_OK;

done:
    if (relays) {
        for (size_t i = 0; i < n_relays; i++) free(relays[i]);
        free(relays);
    }
    OPENSSL_cleanse(raw, raw_len);
    free(raw);
    if (status != NOSTR_CONCORD_OK) memset(out, 0, sizeof(*out));
    return status;
}

/* ---------------- NIP-44 layers ---------------- */

static nostr_concord_status_t concord_decrypt_layer(const uint8_t conv_key[32],
                                                    const char *base64_payload,
                                                    char **plaintext_out) {
    if (!conv_key || !base64_payload || !plaintext_out)
        return NOSTR_CONCORD_ERR_NULL;
    *plaintext_out = NULL;

    uint8_t *plain = NULL;
    size_t plain_len = 0;
    if (nostr_nip44_decrypt_v2_with_convkey(conv_key, base64_payload, &plain,
                                            &plain_len) != 0) {
        return NOSTR_CONCORD_ERR_BAD_NIP44;
    }
    /* Enforce the cap ourselves: libraries are lenient, and a lenient
     * publisher mints events a strict reader cannot decrypt (Appendix B). */
    if (plain_len > CONCORD_MAX_NIP44_PLAINTEXT) {
        OPENSSL_cleanse(plain, plain_len);
        free(plain);
        return NOSTR_CONCORD_ERR_BAD_CONTENT;
    }
    /* Every Concord layer is JSON text; an embedded NUL would truncate it
     * against a C consumer, so refuse rather than hand over a short string. */
    if (memchr(plain, '\0', plain_len) != NULL) {
        OPENSSL_cleanse(plain, plain_len);
        free(plain);
        return NOSTR_CONCORD_ERR_BAD_CONTENT;
    }

    char *text = malloc(plain_len + 1);
    if (!text) {
        OPENSSL_cleanse(plain, plain_len);
        free(plain);
        return NOSTR_CONCORD_ERR_OOM;
    }
    memcpy(text, plain, plain_len);
    text[plain_len] = '\0';
    OPENSSL_cleanse(plain, plain_len);
    free(plain);

    *plaintext_out = text;
    return NOSTR_CONCORD_OK;
}

static nostr_concord_status_t concord_encrypt_layer(const uint8_t conv_key[32],
                                                    const char *plaintext,
                                                    char **base64_out) {
    if (!conv_key || !plaintext || !base64_out) return NOSTR_CONCORD_ERR_NULL;
    *base64_out = NULL;

    size_t len = strlen(plaintext);
    if (len == 0 || len > CONCORD_MAX_NIP44_PLAINTEXT)
        return NOSTR_CONCORD_ERR_BAD_CONTENT;

    char *encoded = NULL;
    if (nostr_nip44_encrypt_v2_with_convkey(conv_key,
                                            (const uint8_t *)plaintext, len,
                                            &encoded) != 0 || !encoded) {
        return NOSTR_CONCORD_ERR_BAD_NIP44;
    }
    *base64_out = encoded;
    return NOSTR_CONCORD_OK;
}

nostr_concord_status_t nostr_concord_stream_seal(const uint8_t conv_key[32],
                                                 const char *plaintext,
                                                 char **base64_out) {
    return concord_encrypt_layer(conv_key, plaintext, base64_out);
}

nostr_concord_status_t nostr_concord_stream_open(const uint8_t conv_key[32],
                                                 const char *base64_payload,
                                                 char **plaintext_out) {
    return concord_decrypt_layer(conv_key, base64_payload, plaintext_out);
}

nostr_concord_status_t nostr_concord_invite_bundle_decrypt(
    const char *content_base64, const uint8_t token[CONCORD_INVITE_TOKEN_BYTES],
    char **bundle_json_out) {
    if (!content_base64 || !token || !bundle_json_out)
        return NOSTR_CONCORD_ERR_NULL;

    uint8_t bundle_key[32];
    nostr_concord_status_t status = nostr_concord_invite_key(token, bundle_key);
    if (status != NOSTR_CONCORD_OK) return status;

    status = concord_decrypt_layer(bundle_key, content_base64, bundle_json_out);
    OPENSSL_cleanse(bundle_key, sizeof(bundle_key));
    return status;
}

nostr_concord_status_t nostr_concord_invite_bundle_encrypt(
    const char *bundle_json, const uint8_t token[CONCORD_INVITE_TOKEN_BYTES],
    char **content_base64_out) {
    if (!bundle_json || !token || !content_base64_out)
        return NOSTR_CONCORD_ERR_NULL;

    uint8_t bundle_key[32];
    nostr_concord_status_t status = nostr_concord_invite_key(token, bundle_key);
    if (status != NOSTR_CONCORD_OK) return status;

    status = concord_encrypt_layer(bundle_key, bundle_json, content_base64_out);
    OPENSSL_cleanse(bundle_key, sizeof(bundle_key));
    return status;
}
