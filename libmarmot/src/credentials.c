/*
 * libmarmot - MIP-00: Credentials & KeyPackages
 *
 * Creates, parses and selects kind:30443 (addressable) KeyPackage events.
 *
 * Flow:
 *   1. Generate MLS keypairs (Ed25519 signing + X25519 HPKE)
 *   2. Create MlsKeyPackage with Nostr pubkey as BasicCredential identity
 *   3. TLS-serialize the KeyPackage
 *   4. Base64-encode the serialized bytes
 *   5. Build a kind:30443 NostrEvent with tags, in the MDK 0.8 order pinned
 *      by tests/vectors/mdk/protocol-vectors.json:
 *      - d = stable per-account publication slot (32 random bytes, hex)
 *      - mls_protocol_version = "1.0"
 *      - mls_ciphersuite = "0x0001"
 *      - mls_extensions = "0x000a" "0xf2ee"
 *      - mls_proposals = "0x000a"
 *      - relays = relay URLs (omitted when none are given)
 *      - i = hex(KeyPackageRef)
 *      - encoding = "base64"
 *   6. Return the event JSON (signed or unsigned) + KeyPackageRef
 *
 * Spec: marmot-protocol/marmot transports/nostr.md "KeyPackage publication"
 * and "Event identity and tag cardinality"; foundation/key-packages.md.
 * The legacy kind 443 is not produced or accepted (strict cutover, as in the
 * adopted spec and MDK master; MDK 0.8 accepted 443 only until 2026-05-31).
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
is_lower_hex_len(const char *s, size_t len)
{
    if (!s || strlen(s) != len) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

/*
 * Transport tag cardinality (marmot transports/nostr.md, "Event identity and
 * tag cardinality"): required tags appear exactly once, and a receiver must
 * not read the first match while ignoring later duplicates.
 */
static size_t
count_tags(NostrTags *tags, const char *key, NostrTag **out_first)
{
    size_t n = 0;
    if (out_first) *out_first = NULL;
    if (!tags || !key) return 0;
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        const char *tag_key = tag ? nostr_tag_get_key(tag) : NULL;
        if (!tag_key || strcmp(tag_key, key) != 0) continue;
        if (n == 0 && out_first) *out_first = tag;
        n++;
    }
    return n;
}

/* Exactly one `key` tag carrying exactly one non-empty value. */
static const char *
singleton_tag_value(NostrTags *tags, const char *key)
{
    NostrTag *tag = NULL;
    if (count_tags(tags, key, &tag) != 1 || nostr_tag_size(tag) != 2)
        return NULL;
    const char *value = nostr_tag_get(tag, 1);
    return (value && value[0] != '\0') ? value : NULL;
}

/* An id-list value is "0x" + exactly four lowercase hex digits. */
static bool
is_id_list_value(const char *v)
{
    return v && v[0] == '0' && v[1] == 'x' && is_lower_hex_len(v + 2, 4);
}

/* Exactly one `key` tag with >= 1 well-formed, pairwise-distinct ids. */
static bool
id_list_tag_valid(NostrTags *tags, const char *key)
{
    NostrTag *tag = NULL;
    if (count_tags(tags, key, &tag) != 1) return false;
    size_t n = nostr_tag_size(tag);
    if (n < 2) return false;
    for (size_t i = 1; i < n; i++) {
        const char *v = nostr_tag_get(tag, i);
        if (!is_id_list_value(v)) return false;
        for (size_t j = 1; j < i; j++) {
            if (strcmp(v, nostr_tag_get(tag, j)) == 0) return false;
        }
    }
    return true;
}

/*
 * Relay URL profile (transports/nostr.md "Relay URL profile"): absolute
 * ws/wss URL with a host, <= 512 bytes, no userinfo and no fragment.
 */
static bool
is_relay_url(const char *url)
{
    if (!url || strlen(url) > 512) return false;
    const char *host;
    if (strncmp(url, "wss://", 6) == 0) host = url + 6;
    else if (strncmp(url, "ws://", 5) == 0) host = url + 5;
    else return false;
    size_t authority_len = strcspn(host, "/?#");
    if (authority_len == 0 || host[0] == ':') return false;
    if (memchr(host, '@', authority_len)) return false;
    return strchr(url, '#') == NULL;
}

/* `relays` is optional (the adopted profile drops it); if present, exactly
 * one tag with >= 1 valid relay URL. */
static bool
relays_tag_valid(NostrTags *tags)
{
    NostrTag *tag = NULL;
    size_t n = count_tags(tags, "relays", &tag);
    if (n == 0) return true;
    if (n != 1 || nostr_tag_size(tag) < 2) return false;
    for (size_t i = 1; i < nostr_tag_size(tag); i++) {
        if (!is_relay_url(nostr_tag_get(tag, i))) return false;
    }
    return true;
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
 * Internal: stable KeyPackage publication slot (the kind:30443 `d` tag)
 *
 * The spec requires the slot id to be generated once from 32 random bytes,
 * retained locally, and reused for every routine replacement; it must not be
 * derived from identity or key material. libmarmot keeps one active
 * KeyPackage per account, so one slot per owner pubkey is persisted in the
 * backend's MLS key store (no storage schema change).
 * ──────────────────────────────────────────────────────────────────────── */

#define MARMOT_KP_SLOT_LABEL "kp_slot"
#define MARMOT_KP_SLOT_LEN   32

static MarmotError
load_or_create_key_package_slot(Marmot *m, const uint8_t owner_pubkey[32],
                                uint8_t slot_out[MARMOT_KP_SLOT_LEN])
{
    uint8_t *stored = NULL;
    size_t stored_len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, MARMOT_KP_SLOT_LABEL,
                                           owner_pubkey, 32,
                                           &stored, &stored_len);
    if (err == MARMOT_OK) {
        bool ok = stored && stored_len == MARMOT_KP_SLOT_LEN;
        if (ok) memcpy(slot_out, stored, MARMOT_KP_SLOT_LEN);
        free(stored);
        return ok ? MARMOT_OK : MARMOT_ERR_STORAGE;
    }
    free(stored);
    if (err != MARMOT_ERR_STORAGE_NOT_FOUND) return err;

    randombytes_buf(slot_out, MARMOT_KP_SLOT_LEN);
    return m->storage->mls_store(m->storage->ctx, MARMOT_KP_SLOT_LABEL,
                                 owner_pubkey, 32,
                                 slot_out, MARMOT_KP_SLOT_LEN);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: id-list tag ["name", "0x%04x", ...] from a u16 list
 * (transports/nostr.md: lowercase, zero-padded, at least one value).
 * ──────────────────────────────────────────────────────────────────────── */

static NostrTag *
id_list_tag_new(const char *name, const uint16_t *ids, size_t n)
{
    if (n == 0) return NULL;
    char v[7];
    snprintf(v, sizeof(v), "0x%04x", (unsigned)ids[0]);
    NostrTag *tag = nostr_tag_new(name, v, NULL);
    for (size_t i = 1; tag && i < n; i++) {
        snprintf(v, sizeof(v), "0x%04x", (unsigned)ids[i]);
        nostr_tag_append(tag, v);
    }
    return tag;
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

    if (!m->storage || !m->storage->mls_store || !m->storage->mls_load ||
        !m->storage->mls_delete || !m->storage->save_key_package_info ||
        !m->storage->deactivate_key_packages)
        return MARMOT_ERR_STORAGE;

    /* Ensure MLS identity is ready */
    if (marmot_ensure_identity(m) != 0)
        return MARMOT_ERR_CRYPTO;

    /* Resolve the account's publication slot before creating key material,
     * so a storage failure here leaves nothing to clean up. */
    uint8_t slot[MARMOT_KP_SLOT_LEN];
    MarmotError slot_err = load_or_create_key_package_slot(m, nostr_pubkey, slot);
    if (slot_err != MARMOT_OK)
        return slot_err;

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

    /* Build the kind:30443 Nostr event */
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

    /* d tag: stable publication slot (addressable replacement key) */
    char *slot_hex = marmot_hex_encode(slot, MARMOT_KP_SLOT_LEN);
    NostrTag *tag = slot_hex ? nostr_tag_new("d", slot_hex, NULL) : NULL;
    free(slot_hex);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* mls_protocol_version tag */
    tag = nostr_tag_new("mls_protocol_version", "1.0", NULL);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* mls_ciphersuite id-list tag */
    tag = nostr_tag_new("mls_ciphersuite", "0x0001", NULL);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* mls_extensions id-list tag, derived from the signed LeafNode
     * capabilities so the advertisement cannot drift from what receivers
     * validate (nostrc-prqu.10): 0x000a last_resort, 0xf2ee
     * marmot_group_data — the exact MDK 0.8 tag value. */
    tag = id_list_tag_new("mls_extensions", kp.leaf_node.cap_extensions,
                          kp.leaf_node.cap_extension_count);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* mls_proposals id-list tag. MDK 0.8 parsers reject a kind:30443 whose
     * mls_proposals is anything but exactly ["0x000a"] (SelfRemove), so the
     * vector-compatible profile keeps it, although the LeafNode deliberately
     * does NOT list SelfRemove (libmarmot cannot process it; nostrc-prqu.10).
     * That is safe: the tag is an advertisement/fetch filter, and MDK derives
     * a group's required proposals from the decoded leaves (intersection),
     * so a group with a libmarmot member never requires SelfRemove. */
    tag = nostr_tag_new("mls_proposals", "0x000a", NULL);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* relays tag (MDK 0.8 profile; omitted when no relays are supplied) */
    if (relay_count > 0) {
        NostrTag *relay_tag = nostr_tag_new("relays", relay_urls[0], NULL);
        if (!relay_tag) goto tag_fail;
        for (size_t i = 1; i < relay_count; i++) {
            nostr_tag_append(relay_tag, relay_urls[i]);
        }
        nostr_tags_append(tags, relay_tag);
    }

    /* i tag: hex-encoded KeyPackageRef */
    char *kp_ref_hex = marmot_hex_encode(kp_ref, MLS_HASH_LEN);
    tag = kp_ref_hex ? nostr_tag_new("i", kp_ref_hex, NULL) : NULL;
    free(kp_ref_hex);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* encoding tag (MDK 0.8 parsers require it) */
    tag = nostr_tag_new("encoding", "base64", NULL);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* No NIP-70 "-" tag: it is not part of the kind:30443 tag set, and
     * relays without NIP-42 AUTH reject protected events, which would make
     * the KeyPackage undiscoverable. */

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
 * Internal: validate an authenticated kind:30443 event and extract the
 * MlsKeyPackage.
 *
 * The caller MUST have verified the NIP-01 id and signature first (see
 * marmot_parse_key_package_event); this function only checks the event
 * shape, the KeyPackage bytes, author binding and the KeyPackageRef. It is
 * exported (not in public headers) so interop tests can check MDK vector
 * events, which ship tags and content but no signature.
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_validate_key_package_event(NostrEvent *event,
                                  MlsKeyPackage *kp_out,
                                  uint8_t nostr_pubkey_out[32])
{
    if (!event || !kp_out) return MARMOT_ERR_INVALID_ARG;

    uint8_t event_pubkey[32];
    uint8_t expected_ref[MLS_HASH_LEN];

    /* Legacy kind 443 is rejected: removed by the adopted Marmot spec. */
    if (event->kind != MARMOT_KIND_KEY_PACKAGE)
        return MARMOT_ERR_UNEXPECTED_EVENT;

    if (!is_hex_len(event->pubkey, 64) ||
        marmot_hex_decode(event->pubkey, event_pubkey, sizeof(event_pubkey)) != 0)
        return MARMOT_ERR_VALIDATION;

    /* Addressable slot: exactly one d tag, 32 bytes as lowercase hex. */
    const char *d = singleton_tag_value(event->tags, "d");
    if (!is_lower_hex_len(d, 2 * MARMOT_KP_SLOT_LEN))
        return MARMOT_ERR_VALIDATION;

    const char *version = singleton_tag_value(event->tags, "mls_protocol_version");
    if (!version || strcmp(version, "1.0") != 0)
        return MARMOT_ERR_VALIDATION;

    if (!id_list_tag_valid(event->tags, "mls_ciphersuite") ||
        !id_list_tag_valid(event->tags, "mls_extensions") ||
        !id_list_tag_valid(event->tags, "mls_proposals"))
        return MARMOT_ERR_VALIDATION;

    if (!relays_tag_valid(event->tags))
        return MARMOT_ERR_VALIDATION;

    const char *ref_hex = singleton_tag_value(event->tags, "i");
    if (!is_lower_hex_len(ref_hex, 2 * MLS_HASH_LEN))
        return MARMOT_ERR_VALIDATION;

    /* Content encoding of the MDK 0.8 profile emitted by this library. */
    const char *encoding = singleton_tag_value(event->tags, "encoding");
    if (!encoding || strcmp(encoding, "base64") != 0)
        return MARMOT_ERR_VALIDATION;

    if (!event->content || event->content[0] == '\0')
        return MARMOT_ERR_DESERIALIZATION;

    size_t kp_len = 0;
    uint8_t *kp_data = marmot_base64_decode(event->content, &kp_len);
    if (!kp_data)
        return MARMOT_ERR_DESERIALIZATION;

    MarmotError err = MARMOT_OK;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, kp_data, kp_len);
    memset(kp_out, 0, sizeof(*kp_out));
    int rc = mls_key_package_deserialize(&reader, kp_out);
    free(kp_data);
    if (rc != 0)
        return MARMOT_ERR_MLS;

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
    return MARMOT_OK;

fail_kp:
    mls_key_package_clear(kp_out);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: parse a signed kind:30443 event JSON and extract the
 * MlsKeyPackage (id + signature verified before any field is trusted).
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

    MarmotError err = MARMOT_ERR_UNEXPECTED_EVENT;
    if (event.kind == MARMOT_KIND_KEY_PACKAGE) {
        err = verify_event_id_and_signature(&event);
        if (err == MARMOT_OK)
            err = marmot_validate_key_package_event(&event, kp_out, nostr_pubkey_out);
    }
    clear_stack_event(&event);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_select_key_package_event
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    NostrEvent event;
    uint8_t     author[32];
    const char *slot;          /* borrowed from event.tags */
    bool        authenticated; /* kind 30443, id + sig verified, has slot */
    bool        valid;         /* slot winner that passed full validation */
    uint8_t     ref[MLS_HASH_LEN];
} KeyPackageCandidate;

/* Is @a newer than @b under the addressable replacement rule? */
static bool
kp_candidate_newer(const KeyPackageCandidate *a, const KeyPackageCandidate *b)
{
    if (a->event.created_at != b->event.created_at)
        return a->event.created_at > b->event.created_at;
    return strcmp(a->event.id, b->event.id) < 0;
}

MarmotError
marmot_select_key_package_event(const char **event_jsons,
                                size_t count,
                                const uint8_t owner_pubkey[32],
                                size_t *out_index)
{
    if (!out_index || (count > 0 && !event_jsons))
        return MARMOT_ERR_INVALID_ARG;
    if (count == 0)
        return MARMOT_ERR_KEY_PACKAGE;

    KeyPackageCandidate *c = calloc(count, sizeof(*c));
    if (!c) return MARMOT_ERR_MEMORY;

    /* 1. Authenticate and locate each event's slot. */
    for (size_t i = 0; i < count; i++) {
        if (!event_jsons[i] ||
            !nostr_event_deserialize_compact(&c[i].event, event_jsons[i], NULL))
            continue;
        if (c[i].event.kind != MARMOT_KIND_KEY_PACKAGE ||
            verify_event_id_and_signature(&c[i].event) != MARMOT_OK)
            continue;
        if (marmot_hex_decode(c[i].event.pubkey, c[i].author, sizeof(c[i].author)) != 0 ||
            (owner_pubkey && memcmp(c[i].author, owner_pubkey, 32) != 0))
            continue;
        c[i].slot = singleton_tag_value(c[i].event.tags, "d");
        c[i].authenticated = c[i].slot != NULL;
    }

    /* 2 + 3. Keep each (pubkey, d) slot's newest event if it validates.
     * Authors compare as decoded bytes, slot ids as exact strings. */
    for (size_t i = 0; i < count; i++) {
        if (!c[i].authenticated) continue;
        bool superseded = false;
        for (size_t j = 0; j < count && !superseded; j++) {
            if (j == i || !c[j].authenticated) continue;
            if (memcmp(c[j].author, c[i].author, 32) != 0 ||
                strcmp(c[j].slot, c[i].slot) != 0)
                continue;
            superseded = kp_candidate_newer(&c[j], &c[i]);
        }
        if (superseded) continue;

        MlsKeyPackage kp;
        memset(&kp, 0, sizeof(kp));
        if (marmot_validate_key_package_event(&c[i].event, &kp, NULL) != MARMOT_OK)
            continue;
        mls_key_package_clear(&kp);
        const char *ref_hex = singleton_tag_value(c[i].event.tags, "i");
        c[i].valid = marmot_hex_decode(ref_hex, c[i].ref, sizeof(c[i].ref)) == 0;
    }

    /* 4. Newest valid slot winner; ties broken by lower KeyPackageRef. */
    size_t best = count;
    for (size_t i = 0; i < count; i++) {
        if (!c[i].valid) continue;
        if (best == count ||
            c[i].event.created_at > c[best].event.created_at ||
            (c[i].event.created_at == c[best].event.created_at &&
             memcmp(c[i].ref, c[best].ref, MLS_HASH_LEN) < 0))
            best = i;
    }

    for (size_t i = 0; i < count; i++)
        clear_stack_event(&c[i].event);
    free(c);

    if (best == count)
        return MARMOT_ERR_KEY_PACKAGE;
    *out_index = best;
    return MARMOT_OK;
}
