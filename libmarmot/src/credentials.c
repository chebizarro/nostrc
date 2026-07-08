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
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Internal helpers
 * ──────────────────────────────────────────────────────────────────────── */

static void
clear_stack_event(NostrEvent *event)
{
    if (!event) return;
    free(event->id);
    free(event->pubkey);
    free(event->content);
    free(event->sig);
    nostr_tags_free(event->tags);
    memset(event, 0, sizeof(*event));
}

static bool
is_hex_len(const char *s, size_t len)
{
    if (!s || strlen(s) != len) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

static bool
find_tag_value(NostrTags *tags, const char *key, const char **out_value)
{
    if (out_value) *out_value = NULL;
    if (!tags || !key) return false;
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (nostr_tag_size(tag) >= 2 && strcmp(nostr_tag_get_key(tag), key) == 0) {
            if (out_value) *out_value = nostr_tag_get_value(tag);
            return true;
        }
    }
    return false;
}

static MarmotError
verify_event_id_and_signature(NostrEvent *event)
{
    if (!event || !is_hex_len(event->id, 64) ||
        !is_hex_len(event->pubkey, 64) || !is_hex_len(event->sig, 128))
        return MARMOT_ERR_VALIDATION;

    char *claimed_id = event->id;
    event->id = NULL;
    char *computed_id = nostr_event_get_id(event);
    free(event->id); /* cached copy created by nostr_event_get_id() */
    event->id = claimed_id;
    if (!computed_id) return MARMOT_ERR_VALIDATION;

    bool id_ok = strcmp(computed_id, claimed_id) == 0;
    free(computed_id);
    if (!id_ok) return MARMOT_ERR_VALIDATION;

    return nostr_event_check_signature(event) ? MARMOT_OK : MARMOT_ERR_VALIDATION;
}

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

static MarmotError
create_key_package_common(Marmot *m,
                          const uint8_t nostr_pubkey[32],
                          const uint8_t nostr_sk[32],
                          bool sign_event,
                          const char **relay_urls, size_t relay_count,
                          MarmotKeyPackageResult *result)
{
    if (!m || !nostr_pubkey || !result || (sign_event && !nostr_sk))
        return MARMOT_ERR_INVALID_ARG;
    if (relay_count > 0 && !relay_urls)
        return MARMOT_ERR_INVALID_ARG;

    memset(result, 0, sizeof(*result));

    if (!m->storage || !m->storage->mls_store || !m->storage->mls_delete ||
        !m->storage->save_key_package_info || !m->storage->deactivate_key_packages)
        return MARMOT_ERR_STORAGE;

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

    if (sign_event) {
        char *sk_hex = marmot_hex_encode(nostr_sk, 32);
        if (!sk_hex) {
            nostr_event_free(event);
            mls_key_package_clear(&kp);
            mls_key_package_private_clear(&kp_priv);
            return MARMOT_ERR_MEMORY;
        }
        if (nostr_event_sign(event, sk_hex) != 0) {
            free(sk_hex);
            nostr_event_free(event);
            mls_key_package_clear(&kp);
            mls_key_package_private_clear(&kp_priv);
            return MARMOT_ERR_CRYPTO;
        }
        free(sk_hex);

        char *expected_pubkey = marmot_hex_encode(nostr_pubkey, 32);
        if (!expected_pubkey) {
            nostr_event_free(event);
            mls_key_package_clear(&kp);
            mls_key_package_private_clear(&kp_priv);
            return MARMOT_ERR_MEMORY;
        }
        bool pubkey_matches = event->pubkey && strcmp(event->pubkey, expected_pubkey) == 0;
        free(expected_pubkey);
        if (!pubkey_matches) {
            nostr_event_free(event);
            mls_key_package_clear(&kp);
            mls_key_package_private_clear(&kp_priv);
            return MARMOT_ERR_VALIDATION;
        }
    }

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
    ;

    /* Store the KeyPackage private material in storage for later Welcome
     * processing. These writes are mandatory: accepting a Welcome for this
     * KeyPackage requires both the private-key blob and full KeyPackage. */
    MarmotError err;
    uint8_t priv_blob[MLS_KEM_SK_LEN + MLS_KEM_SK_LEN + MLS_SIG_SK_LEN];
    memcpy(priv_blob, kp_priv.init_key_private, MLS_KEM_SK_LEN);
    memcpy(priv_blob + MLS_KEM_SK_LEN, kp_priv.encryption_key_private, MLS_KEM_SK_LEN);
    memcpy(priv_blob + MLS_KEM_SK_LEN + MLS_KEM_SK_LEN,
           kp_priv.signature_key_private, MLS_SIG_SK_LEN);

    err = m->storage->mls_store(m->storage->ctx, "kp_priv",
                                kp_ref, MLS_HASH_LEN,
                                priv_blob, sizeof(priv_blob));
    sodium_memzero(priv_blob, sizeof(priv_blob));
    if (err != MARMOT_OK) {
        marmot_key_package_result_free(result);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return err;
    }

    MlsTlsBuf kp_buf;
    if (mls_tls_buf_init(&kp_buf, 1024) != 0) {
        m->storage->mls_delete(m->storage->ctx, "kp_priv", kp_ref, MLS_HASH_LEN);
        marmot_key_package_result_free(result);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_MEMORY;
    }
    if (mls_key_package_serialize(&kp, &kp_buf) != 0) {
        mls_tls_buf_free(&kp_buf);
        m->storage->mls_delete(m->storage->ctx, "kp_priv", kp_ref, MLS_HASH_LEN);
        marmot_key_package_result_free(result);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_TLS_CODEC;
    }
    err = m->storage->mls_store(m->storage->ctx, "kp_full",
                                kp_ref, MLS_HASH_LEN,
                                kp_buf.data, kp_buf.len);
    mls_tls_buf_free(&kp_buf);
    if (err != MARMOT_OK) {
        m->storage->mls_delete(m->storage->ctx, "kp_priv", kp_ref, MLS_HASH_LEN);
        marmot_key_package_result_free(result);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return err;
    }

    /* Deactivate previous key packages for this pubkey (rotation). Mandatory
     * so callers do not publish a new active package while older ones remain
     * active after a failed storage write. */
    err = m->storage->deactivate_key_packages(m->storage->ctx, nostr_pubkey);
    if (err != MARMOT_OK) {
        m->storage->mls_delete(m->storage->ctx, "kp_priv", kp_ref, MLS_HASH_LEN);
        m->storage->mls_delete(m->storage->ctx, "kp_full", kp_ref, MLS_HASH_LEN);
        marmot_key_package_result_free(result);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return err;
    }

    MarmotKeyPackageInfo info;
    memset(&info, 0, sizeof(info));
    memcpy(info.ref, kp_ref, 32);
    memcpy(info.owner_pubkey, nostr_pubkey, 32);
    info.created_at = marmot_now();
    info.active = true;
    if (relay_count > 0 && relay_urls) {
        info.relay_urls = (char **)relay_urls;  /* borrowed for save */
        info.relay_count = relay_count;
    }
    err = m->storage->save_key_package_info(m->storage->ctx, &info);
    if (err != MARMOT_OK) {
        m->storage->mls_delete(m->storage->ctx, "kp_priv", kp_ref, MLS_HASH_LEN);
        m->storage->mls_delete(m->storage->ctx, "kp_full", kp_ref, MLS_HASH_LEN);
        marmot_key_package_result_free(result);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return err;
    }

    /* Clean up */
    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&kp_priv);

    return MARMOT_OK;
}

MarmotError
marmot_create_key_package(Marmot *m,
                           const uint8_t nostr_pubkey[32],
                           const uint8_t nostr_sk[32],
                           const char **relay_urls, size_t relay_count,
                           MarmotKeyPackageResult *result)
{
    return create_key_package_common(m, nostr_pubkey, nostr_sk, true,
                                     relay_urls, relay_count, result);
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

    return create_key_package_common(m, nostr_pubkey, NULL, false,
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
    if (!event_json || !kp_out) return MARMOT_ERR_INVALID_ARG;

    NostrEvent event;
    memset(&event, 0, sizeof(event));
    if (!nostr_event_deserialize_compact(&event, event_json, NULL))
        return MARMOT_ERR_DESERIALIZATION;

    MarmotError err = MARMOT_OK;
    uint8_t event_pubkey[32];
    const char *encoding = NULL;
    const char *ref_hex = NULL;
    uint8_t expected_ref[MLS_HASH_LEN];

    if (event.kind != MARMOT_KIND_KEY_PACKAGE) {
        err = MARMOT_ERR_UNEXPECTED_EVENT;
        goto fail_event;
    }

    err = verify_event_id_and_signature(&event);
    if (err != MARMOT_OK) goto fail_event;

    if (marmot_hex_decode(event.pubkey, event_pubkey, sizeof(event_pubkey)) != 0) {
        err = MARMOT_ERR_VALIDATION;
        goto fail_event;
    }

    if (!find_tag_value(event.tags, "encoding", &encoding) ||
        !encoding || strcmp(encoding, "base64") != 0) {
        err = MARMOT_ERR_VALIDATION;
        goto fail_event;
    }

    if (!find_tag_value(event.tags, "i", &ref_hex) || !is_hex_len(ref_hex, 64)) {
        err = MARMOT_ERR_VALIDATION;
        goto fail_event;
    }

    if (!event.content || event.content[0] == '\0') {
        err = MARMOT_ERR_DESERIALIZATION;
        goto fail_event;
    }

    size_t kp_len = 0;
    uint8_t *kp_data = marmot_base64_decode(event.content, &kp_len);
    if (!kp_data) {
        err = MARMOT_ERR_DESERIALIZATION;
        goto fail_event;
    }

    MlsTlsReader reader;
    mls_tls_reader_init(&reader, kp_data, kp_len);
    memset(kp_out, 0, sizeof(*kp_out));
    int rc = mls_key_package_deserialize(&reader, kp_out);
    free(kp_data);
    if (rc != 0) {
        err = MARMOT_ERR_MLS;
        goto fail_event;
    }

    rc = mls_key_package_validate(kp_out);
    if (rc != 0) {
        err = MARMOT_ERR_VALIDATION;
        goto fail_kp;
    }

    if (kp_out->leaf_node.credential_identity_len != 32 ||
        !kp_out->leaf_node.credential_identity ||
        memcmp(kp_out->leaf_node.credential_identity, event_pubkey, 32) != 0) {
        err = MARMOT_ERR_AUTHOR_MISMATCH;
        goto fail_kp;
    }

    if (mls_key_package_ref(kp_out, expected_ref) != 0) {
        err = MARMOT_ERR_MLS;
        goto fail_kp;
    }
    char *expected_ref_hex = marmot_hex_encode(expected_ref, MLS_HASH_LEN);
    if (!expected_ref_hex) {
        err = MARMOT_ERR_MEMORY;
        goto fail_kp;
    }
    bool ref_ok = strcmp(expected_ref_hex, ref_hex) == 0;
    free(expected_ref_hex);
    if (!ref_ok) {
        err = MARMOT_ERR_VALIDATION;
        goto fail_kp;
    }

    if (nostr_pubkey_out) memcpy(nostr_pubkey_out, event_pubkey, 32);
    clear_stack_event(&event);
    return MARMOT_OK;

fail_kp:
    mls_key_package_clear(kp_out);
fail_event:
    clear_stack_event(&event);
    return err;
}
