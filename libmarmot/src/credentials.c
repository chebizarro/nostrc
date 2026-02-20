/*
 * libmarmot - MIP-00: Credentials & KeyPackages
 *
 * Creates and parses kind:443 KeyPackage events.
 *
 * Flow:
 *   1. Generate MLS keypairs (Ed25519 signing + X25519 HPKE)
 *   2. Create MlsKeyPackage with Nostr pubkey as BasicCredential identity
 *   3. TLS-serialize the KeyPackage
 *   4. Base64-encode the serialized bytes
 *   5. Build a kind:443 NostrEvent with tags:
 *      - mls_protocol_version = "1.0"
 *      - mls_ciphersuite = "0x0001"
 *      - mls_extensions = "0xf2ee" "0x000a"
 *      - encoding = "base64"
 *      - i = hex(KeyPackageRef)
 *      - relays = relay URLs
 *      - "-" (NIP-70: only author can publish)
 *   6. Return unsigned event JSON + KeyPackageRef
 *
 * SPDX-License-Identifier: MIT
 */

#include "marmot-internal.h"
#include "mls/mls_key_package.h"
#include "mls/mls-internal.h"
#include <nostr-event.h>
#include <nostr-tag.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: ensure MLS identity is initialized
 * ──────────────────────────────────────────────────────────────────────── */

int
marmot_ensure_identity(Marmot *m)
{
    if (m->identity_ready) return 0;

    /* Generate Ed25519 signing keypair */
    if (mls_crypto_sign_keygen(m->ed25519_sk, m->ed25519_pk) != 0)
        return -1;

    /* Derive X25519 encryption keypair from Ed25519 */
    if (crypto_sign_ed25519_sk_to_curve25519(m->hpke_sk, m->ed25519_sk) != 0)
        return -1;
    if (crypto_sign_ed25519_pk_to_curve25519(m->hpke_pk, m->ed25519_pk) != 0)
        return -1;

    m->identity_ready = true;
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: base64 encode/decode via libsodium
 * ──────────────────────────────────────────────────────────────────────── */

static char *
marmot_base64_encode(const uint8_t *data, size_t len)
{
    /* +1 for libsodium's null terminator */
    size_t b64_maxlen = sodium_base64_ENCODED_LEN(len, sodium_base64_VARIANT_ORIGINAL);
    char *out = malloc(b64_maxlen);
    if (!out) return NULL;
    sodium_bin2base64(out, b64_maxlen, data, len, sodium_base64_VARIANT_ORIGINAL);
    return out;
}

static uint8_t *
marmot_base64_decode(const char *b64, size_t *out_len)
{
    if (!b64 || !out_len) return NULL;
    size_t b64_len = strlen(b64);
    /* Decoded size is at most 3/4 of base64 length */
    size_t max_bin = (b64_len / 4) * 3 + 3;
    uint8_t *out = malloc(max_bin);
    if (!out) return NULL;

    size_t bin_len = 0;
    if (sodium_base642bin(out, max_bin, b64, b64_len,
                          NULL, &bin_len, NULL,
                          sodium_base64_VARIANT_ORIGINAL) != 0) {
        free(out);
        return NULL;
    }
    *out_len = bin_len;
    return out;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: build the GroupContext extensions for KeyPackage capabilities
 * ──────────────────────────────────────────────────────────────────────── */

static int
build_kp_extensions(uint8_t **out_data, size_t *out_len)
{
    /*
     * KeyPackage extensions are the capabilities of this client.
     * For Marmot, we need to advertise support for:
     *   - 0xF2EE (marmot_group_data)
     *   - 0x000A (last_resort)
     *
     * Extensions are TLS-serialized as:
     *   opaque extensions<V> — a vector of Extension structs:
     *     uint16 extension_type
     *     opaque extension_data<V>
     */
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 64) != 0) return -1;

    /* Extension: last_resort (0x000A) — empty data */
    if (mls_tls_write_u16(&buf, 0x000A) != 0) goto fail;
    if (mls_tls_write_opaque16(&buf, NULL, 0) != 0) goto fail;

    *out_data = buf.data;
    *out_len = buf.len;
    buf.data = NULL;
    return 0;

fail:
    mls_tls_buf_free(&buf);
    return -1;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_create_key_package
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_create_key_package(Marmot *m,
                           const uint8_t nostr_pubkey[32],
                           const uint8_t nostr_sk[32],
                           const char **relay_urls, size_t relay_count,
                           MarmotKeyPackageResult *result)
{
    if (!m || !nostr_pubkey || !nostr_sk || !result)
        return MARMOT_ERR_INVALID_ARG;
    if (relay_count > 0 && !relay_urls)
        return MARMOT_ERR_INVALID_ARG;

    memset(result, 0, sizeof(*result));

    /* Ensure MLS identity is ready */
    if (marmot_ensure_identity(m) != 0)
        return MARMOT_ERR_CRYPTO;

    /* Build KeyPackage extensions (last_resort) */
    uint8_t *ext_data = NULL;
    size_t ext_len = 0;
    if (build_kp_extensions(&ext_data, &ext_len) != 0)
        return MARMOT_ERR_MEMORY;

    /* Create MLS KeyPackage */
    MlsKeyPackage kp;
    MlsKeyPackagePrivate kp_priv;
    memset(&kp, 0, sizeof(kp));
    memset(&kp_priv, 0, sizeof(kp_priv));

    int rc = mls_key_package_create(&kp, &kp_priv,
                                     nostr_pubkey, 32,
                                     ext_data, ext_len);
    free(ext_data);
    if (rc != 0) return MARMOT_ERR_MLS;

    /* Compute KeyPackageRef */
    uint8_t kp_ref[MLS_HASH_LEN];
    if (mls_key_package_ref(&kp, kp_ref) != 0) {
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_MLS;
    }
    memcpy(result->key_package_ref, kp_ref, MLS_HASH_LEN);

    /* TLS-serialize the KeyPackage */
    MlsTlsBuf tls_buf;
    if (mls_tls_buf_init(&tls_buf, 1024) != 0) {
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_MEMORY;
    }
    if (mls_key_package_serialize(&kp, &tls_buf) != 0) {
        mls_tls_buf_free(&tls_buf);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_TLS_CODEC;
    }

    /* Base64-encode */
    char *b64_content = marmot_base64_encode(tls_buf.data, tls_buf.len);
    mls_tls_buf_free(&tls_buf);
    if (!b64_content) {
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_MEMORY;
    }

    /* Build the kind:443 Nostr event */
    NostrEvent *event = nostr_event_new();
    if (!event) {
        free(b64_content);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_MEMORY;
    }

    /* Set event fields */
    nostr_event_set_kind(event, MARMOT_KIND_KEY_PACKAGE);

    /* Set pubkey from the nostr pubkey */
    char *pubkey_hex = marmot_hex_encode(nostr_pubkey, 32);
    nostr_event_set_pubkey(event, pubkey_hex);
    free(pubkey_hex);

    /* Set content to base64-encoded KeyPackage */
    nostr_event_set_content(event, b64_content);
    free(b64_content);

    /* Set created_at */
    nostr_event_set_created_at(event, marmot_now());

    /* Build tags */
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) {
        nostr_event_free(event);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_MEMORY;
    }

    /* mls_protocol_version tag */
    NostrTag *tag = nostr_tag_new("mls_protocol_version", "1.0", NULL);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* mls_ciphersuite tag */
    tag = nostr_tag_new("mls_ciphersuite", "0x0001", NULL);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* mls_extensions tag: list supported non-default extensions */
    tag = nostr_tag_new("mls_extensions", "0xf2ee", "0x000a", NULL);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* encoding tag */
    tag = nostr_tag_new("encoding", "base64", NULL);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* i tag: hex-encoded KeyPackageRef */
    char *kp_ref_hex = marmot_hex_encode(kp_ref, MLS_HASH_LEN);
    tag = nostr_tag_new("i", kp_ref_hex, NULL);
    free(kp_ref_hex);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* relays tag */
    if (relay_count > 0) {
        NostrTag *relay_tag = nostr_tag_new("relays", relay_urls[0], NULL);
        if (!relay_tag) goto tag_fail;
        for (size_t i = 1; i < relay_count; i++) {
            nostr_tag_append(relay_tag, relay_urls[i]);
        }
        nostr_tags_append(tags, relay_tag);
    }

    /* NIP-70: "-" tag prevents non-author from publishing */
    tag = nostr_tag_new("-", NULL);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    nostr_event_set_tags(event, tags);

    /* Serialize the unsigned event to JSON */
    result->event_json = nostr_event_serialize_compact(event);
    nostr_event_free(event);

    if (!result->event_json) {
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_MEMORY;
    }

    goto success;

tag_fail:
    nostr_tags_free(tags);
    nostr_event_free(event);
    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&kp_priv);
    return MARMOT_ERR_MEMORY;

success:

    /* Store the KeyPackage private material in storage for later Welcome processing */
    if (m->storage && m->storage->mls_store) {
        /* Serialize private keys for storage */
        uint8_t priv_blob[MLS_KEM_SK_LEN + MLS_KEM_SK_LEN + MLS_SIG_SK_LEN];
        memcpy(priv_blob, kp_priv.init_key_private, MLS_KEM_SK_LEN);
        memcpy(priv_blob + MLS_KEM_SK_LEN, kp_priv.encryption_key_private, MLS_KEM_SK_LEN);
        memcpy(priv_blob + MLS_KEM_SK_LEN + MLS_KEM_SK_LEN,
               kp_priv.signature_key_private, MLS_SIG_SK_LEN);

        /* Store keyed by (label="kp_priv", key=KeyPackageRef) */
        m->storage->mls_store(m->storage->ctx, "kp_priv",
                               kp_ref, MLS_HASH_LEN,
                               priv_blob, sizeof(priv_blob));
        sodium_memzero(priv_blob, sizeof(priv_blob));
    }

    /* Clean up */
    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&kp_priv);

    return MARMOT_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_create_key_package_unsigned
 *
 * Identical to marmot_create_key_package but does not require the secret
 * key.  The SK parameter was never consumed by the signed variant anyway
 * (it was only validated), so this is a thin wrapper that makes the
 * signer-only contract explicit.
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_create_key_package_unsigned(Marmot *m,
                                    const uint8_t nostr_pubkey[32],
                                    const char **relay_urls, size_t relay_count,
                                    MarmotKeyPackageResult *result)
{
    if (!m || !nostr_pubkey || !result)
        return MARMOT_ERR_INVALID_ARG;

    /*
     * The signed variant validates nostr_sk but never uses it for the
     * actual key package creation (mls_key_package_create only receives
     * the pubkey).  We pass a zeroed SK to satisfy the parameter contract
     * while keeping a single code path.
     */
    static const uint8_t zero_sk[32] = { 0 };
    return marmot_create_key_package(m, nostr_pubkey, zero_sk,
                                      relay_urls, relay_count, result);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: parse a kind:443 event JSON and extract the MlsKeyPackage
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_parse_key_package_event(const char *event_json,
                                MlsKeyPackage *kp_out,
                                uint8_t nostr_pubkey_out[32])
{
    if (!event_json || !kp_out) return -1;

    NostrEvent event;
    memset(&event, 0, sizeof(event));
    if (!nostr_event_deserialize_compact(&event, event_json, NULL))
        return MARMOT_ERR_DESERIALIZATION;

    /* Verify kind */
    if (event.kind != MARMOT_KIND_KEY_PACKAGE) {
        free(event.id); free(event.pubkey); free(event.content);
        free(event.sig); nostr_tags_free(event.tags);
        return MARMOT_ERR_INVALID_ARG;
    }

    /* Extract pubkey */
    if (nostr_pubkey_out && event.pubkey) {
        marmot_hex_decode(event.pubkey, nostr_pubkey_out, 32);
    }

    /* Get content */
    const char *content = event.content;
    if (!content || strlen(content) == 0) {
        free(event.id); free(event.pubkey); free(event.content);
        free(event.sig); nostr_tags_free(event.tags);
        return MARMOT_ERR_DESERIALIZATION;
    }

    /* Check encoding tag (default is hex for backwards compat) */
    bool is_base64 = false;
    if (event.tags) {
        for (size_t i = 0; i < nostr_tags_size(event.tags); i++) {
            NostrTag *tag = nostr_tags_get(event.tags, i);
            if (nostr_tag_size(tag) >= 2 &&
                strcmp(nostr_tag_get_key(tag), "encoding") == 0 &&
                strcmp(nostr_tag_get_value(tag), "base64") == 0) {
                is_base64 = true;
                break;
            }
        }
    }

    /* Decode content */
    uint8_t *kp_data = NULL;
    size_t kp_len = 0;

    if (is_base64) {
        kp_data = marmot_base64_decode(content, &kp_len);
    } else {
        /* Hex decode (deprecated) */
        size_t hex_len = strlen(content);
        if (hex_len % 2 != 0) {
            free(event.id); free(event.pubkey); free(event.content);
            free(event.sig); nostr_tags_free(event.tags);
            return MARMOT_ERR_DESERIALIZATION;
        }
        kp_len = hex_len / 2;
        kp_data = malloc(kp_len);
        if (kp_data && marmot_hex_decode(content, kp_data, kp_len) != 0) {
            free(kp_data);
            kp_data = NULL;
        }
    }

    /* We need to properly free the event - but nostr_event_free takes a pointer */
    /* The event was stack-allocated and populated by deserialize_compact */
    free(event.id);
    free(event.pubkey);
    free(event.content);
    free(event.sig);
    nostr_tags_free(event.tags);

    if (!kp_data) return MARMOT_ERR_DESERIALIZATION;

    /* Deserialize the MLS KeyPackage */
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, kp_data, kp_len);
    memset(kp_out, 0, sizeof(*kp_out));

    int rc = mls_key_package_deserialize(&reader, kp_out);
    free(kp_data);

    if (rc != 0) return MARMOT_ERR_MLS;

    /* Validate the KeyPackage */
    rc = mls_key_package_validate(kp_out);
    if (rc != 0) {
        mls_key_package_clear(kp_out);
        return MARMOT_ERR_VALIDATION;
    }

    return MARMOT_OK;
}
