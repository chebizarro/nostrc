/*
 * libmarmot - adopted-spec KeyPackage profile building blocks (nostrc-prqu.9)
 *
 * MLS extensions draft codecs (app_data_dictionary, ComponentsList), the
 * RFC 9420 MLSMessage framing for mls_key_package, and
 * marmot.member.account-identity-proof.v2 (marmot-protocol/marmot @26fa6a6,
 * app-components/account-identity-proof-v2.md). The profile's event shape
 * and validation live in credentials.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "marmot-internal.h"
#include "kp_profile.h"
#include "mls/mls_key_package.h"
#include "mls/mls-internal.h"
#include <nostr-event.h>
#include <nostr-tag.h>
#include <sodium.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Vector helpers: every <V> vector is a QUIC varint length + bytes.
 * ──────────────────────────────────────────────────────────────────────── */

static int
read_vec(MlsTlsReader *r, const uint8_t **out, size_t *out_len)
{
    size_t n = 0;
    if (mls_tls_read_vli(r, &n) != 0) return -1;
    if (mls_tls_reader_remaining(r) < n) return -1;
    *out = r->data + r->pos;
    *out_len = n;
    r->pos += n;
    return 0;
}

int
marmot_components_list_encode(const uint16_t *ids, size_t n, MlsTlsBuf *out)
{
    if (mls_tls_write_vli(out, n * 2) != 0) return -1;
    for (size_t i = 0; i < n; i++)
        if (mls_tls_write_u16(out, ids[i]) != 0) return -1;
    return 0;
}

int
marmot_components_list_decode(const uint8_t *data, size_t len,
                              uint16_t **ids_out, size_t *n_out)
{
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    const uint8_t *v = NULL;
    size_t vlen = 0;
    if (read_vec(&r, &v, &vlen) != 0 || !mls_tls_reader_done(&r) || (vlen % 2) != 0)
        return -1;
    size_t n = vlen / 2;
    uint16_t *ids = n ? malloc(n * sizeof(uint16_t)) : NULL;
    if (n && !ids) return -1;
    for (size_t i = 0; i < n; i++)
        ids[i] = (uint16_t)((v[2 * i] << 8) | v[2 * i + 1]);
    *ids_out = ids;
    *n_out = n;
    return 0;
}

bool
marmot_u16_list_contains(const uint16_t *ids, size_t n, uint16_t id)
{
    for (size_t i = 0; i < n; i++)
        if (ids[i] == id) return true;
    return false;
}

int
marmot_app_data_dict_encode(const MarmotComponentData *entries, size_t n, MlsTlsBuf *out)
{
    MlsTlsBuf inner;
    if (mls_tls_buf_init(&inner, 128) != 0) return -1;
    for (size_t i = 0; i < n; i++) {
        /* Entries MUST be in strictly increasing component_id order. */
        if (i > 0 && entries[i].component_id <= entries[i - 1].component_id) goto fail;
        if (mls_tls_write_u16(&inner, entries[i].component_id) != 0 ||
            mls_tls_write_opaque32(&inner, entries[i].data, entries[i].len) != 0)
            goto fail;
    }
    int rc = mls_tls_write_opaque32(out, inner.data, inner.len);
    mls_tls_buf_free(&inner);
    return rc;
fail:
    mls_tls_buf_free(&inner);
    return -1;
}

int
marmot_app_data_dict_parse(const uint8_t *data, size_t len,
                           MarmotComponentData **entries_out, size_t *n_out)
{
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    const uint8_t *v = NULL;
    size_t vlen = 0;
    if (read_vec(&r, &v, &vlen) != 0 || !mls_tls_reader_done(&r)) return -1;

    MlsTlsReader e;
    mls_tls_reader_init(&e, v, vlen);
    size_t cap = 4, n = 0;
    MarmotComponentData *out = malloc(cap * sizeof(*out));
    if (!out) return -1;
    while (!mls_tls_reader_done(&e)) {
        uint16_t id = 0;
        const uint8_t *d = NULL;
        size_t dlen = 0;
        if (mls_tls_read_u16(&e, &id) != 0 || read_vec(&e, &d, &dlen) != 0) goto fail;
        if (n > 0 && id <= out[n - 1].component_id) goto fail; /* sorted, unique */
        if (n == cap) {
            cap *= 2;
            MarmotComponentData *grown = realloc(out, cap * sizeof(*out));
            if (!grown) goto fail;
            out = grown;
        }
        out[n].component_id = id;
        out[n].data = d; /* borrowed from @data */
        out[n].len = dlen;
        n++;
    }
    *entries_out = out;
    *n_out = n;
    return 0;
fail:
    free(out);
    return -1;
}

int
marmot_extensions_find(const uint8_t *exts, size_t len, uint16_t type,
                       const uint8_t **data_out, size_t *dlen_out, size_t *count_out)
{
    MlsTlsReader r;
    mls_tls_reader_init(&r, exts, len);
    size_t count = 0;
    *data_out = NULL;
    *dlen_out = 0;
    while (!mls_tls_reader_done(&r)) {
        uint16_t t = 0;
        const uint8_t *d = NULL;
        size_t dlen = 0;
        if (mls_tls_read_u16(&r, &t) != 0 || read_vec(&r, &d, &dlen) != 0) return -1;
        if (t == type) {
            if (count == 0) {
                *data_out = d;
                *dlen_out = dlen;
            }
            count++;
        }
    }
    if (count_out) *count_out = count;
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * MLSMessage framing (RFC 9420 §6): version mls10, wire_format
 * mls_key_package, then the KeyPackage.
 * ──────────────────────────────────────────────────────────────────────── */

int
marmot_mls_message_frame_key_package(const MlsKeyPackage *kp, MlsTlsBuf *out)
{
    if (mls_tls_write_u16(out, MARMOT_MLS_VERSION_10) != 0 ||
        mls_tls_write_u16(out, MARMOT_MLS_WIRE_FORMAT_KEY_PACKAGE) != 0)
        return -1;
    return mls_key_package_serialize(kp, out);
}

int
marmot_mls_message_unframe_key_package(const uint8_t *data, size_t len, MlsKeyPackage *kp_out)
{
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    uint16_t version = 0, wire = 0;
    if (mls_tls_read_u16(&r, &version) != 0 || mls_tls_read_u16(&r, &wire) != 0 ||
        version != MARMOT_MLS_VERSION_10 || wire != MARMOT_MLS_WIRE_FORMAT_KEY_PACKAGE)
        return -1;
    memset(kp_out, 0, sizeof(*kp_out));
    if (mls_key_package_deserialize(&r, kp_out) != 0) return -1;
    if (!mls_tls_reader_done(&r)) {
        mls_key_package_clear(kp_out);
        return -1;
    }
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * marmot.member.account-identity-proof.v2
 * ──────────────────────────────────────────────────────────────────────── */

/* Only the ciphersuite and Ed25519 leaf-key encoding implemented by this
 * MLS engine can be used to authorize a leaf. The timestamp bounds are the
 * common MarmotAuthorizationProof envelope bounds, not an expiry rule. */
static bool
proof_context_supported(uint64_t created_at, uint16_t ciphersuite,
                        uint16_t signature_scheme, size_t sig_key_len)
{
    return created_at >= 1 && created_at <= 9007199254740991ULL &&
           ciphersuite == MARMOT_CIPHERSUITE &&
           signature_scheme == MARMOT_SIGNATURE_SCHEME_ED25519 &&
           sig_key_len == MLS_SIG_PK_LEN;
}

static NostrEvent *
proof_template(const uint8_t account_pk[32], uint64_t created_at, uint16_t ciphersuite,
               uint16_t signature_scheme, const uint8_t *sig_key, size_t sig_key_len)
{
    if (!account_pk || !sig_key ||
        !proof_context_supported(created_at, ciphersuite, signature_scheme, sig_key_len))
        return NULL;
    char *pk_hex = marmot_hex_encode(account_pk, 32);
    char *key_hex = marmot_hex_encode(sig_key, sig_key_len);
    char cs[7], scheme[7];
    snprintf(cs, sizeof(cs), "0x%04x", (unsigned)ciphersuite);
    snprintf(scheme, sizeof(scheme), "0x%04x", (unsigned)signature_scheme);
    NostrEvent *ev = pk_hex && key_hex ? nostr_event_new() : NULL;
    NostrTags *tags = ev ? nostr_tags_new(0) : NULL;
    if (!tags) goto fail;
    NostrTag *t[5] = {
        nostr_tag_new("d", "marmot.account-identity-proof.v2", NULL),
        nostr_tag_new("component", "0x8009", NULL),
        nostr_tag_new("ciphersuite", cs, NULL),
        nostr_tag_new("signature_scheme", scheme, NULL),
        nostr_tag_new("mls_signature_key", key_hex, NULL),
    };
    for (int i = 0; i < 5; i++) {
        if (!t[i]) {
            for (int j = i; j < 5; j++) if (t[j]) nostr_tag_free(t[j]);
            nostr_tags_free(tags);
            goto fail;
        }
        nostr_tags_append(tags, t[i]);
    }
    nostr_event_set_pubkey(ev, pk_hex);
    nostr_event_set_created_at(ev, (int64_t)created_at);
    nostr_event_set_kind(ev, MARMOT_KIND_ACCOUNT_PROOF);
    nostr_event_set_tags(ev, tags);
    nostr_event_set_content(ev, MARMOT_ACCOUNT_PROOF_CONTENT);
    free(pk_hex);
    free(key_hex);
    return ev;
fail:
    if (ev) nostr_event_free(ev);
    free(pk_hex);
    free(key_hex);
    return NULL;
}

char *
marmot_account_proof_template_id(const uint8_t account_pk[32], uint64_t created_at,
                                 uint16_t ciphersuite, uint16_t signature_scheme,
                                 const uint8_t *sig_key, size_t sig_key_len)
{
    NostrEvent *ev = proof_template(account_pk, created_at, ciphersuite, signature_scheme,
                                    sig_key, sig_key_len);
    if (!ev) return NULL;
    char *id = nostr_event_get_id(ev);
    nostr_event_free(ev);
    return id;
}

char *
marmot_account_proof_template_json(const uint8_t account_pk[32], uint64_t created_at,
                                   const uint8_t *sig_key, size_t sig_key_len)
{
    NostrEvent *ev = proof_template(account_pk, created_at, MARMOT_CIPHERSUITE,
                                    MARMOT_SIGNATURE_SCHEME_ED25519, sig_key, sig_key_len);
    if (!ev) return NULL;
    char *json = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    return json;
}

static void
write_proof(uint8_t out[MARMOT_ACCOUNT_PROOF_LEN], const uint8_t pk[32], uint64_t at,
            const uint8_t sig[64])
{
    memcpy(out, pk, 32);
    for (int i = 0; i < 8; i++) out[32 + i] = (uint8_t)(at >> (56 - 8 * i));
    memcpy(out + 40, sig, 64);
}

MarmotError
marmot_account_proof_create(const uint8_t account_pk[32], const uint8_t *account_sk,
                            MarmotAccountSignFunc sign_fn, void *sign_data,
                            uint16_t ciphersuite, uint16_t signature_scheme,
                            const uint8_t *sig_key, size_t sig_key_len, uint64_t created_at,
                            uint8_t out[MARMOT_ACCOUNT_PROOF_LEN])
{
    if (!account_pk || !sig_key || !out || (!account_sk && !sign_fn))
        return MARMOT_ERR_INVALID_ARG;
    if (!proof_context_supported(created_at, ciphersuite, signature_scheme, sig_key_len))
        return MARMOT_ERR_VALIDATION;
    NostrEvent *tmpl = proof_template(account_pk, created_at, ciphersuite, signature_scheme,
                                      sig_key, sig_key_len);
    if (!tmpl) return MARMOT_ERR_MEMORY;
    char *want_id = nostr_event_get_id(tmpl);
    char *pk_hex = marmot_hex_encode(account_pk, 32);
    MarmotError err = MARMOT_ERR_CRYPTO;
    uint8_t sig[64];
    NostrEvent *signed_ev = NULL;
    char *signed_json = NULL;
    if (!want_id || !pk_hex) {
        err = MARMOT_ERR_MEMORY;
        goto out;
    }

    if (account_sk) {
        char *sk_hex = marmot_hex_encode(account_sk, 32);
        if (!sk_hex) {
            err = MARMOT_ERR_MEMORY;
            goto out;
        }
        int rc = nostr_event_sign(tmpl, sk_hex);
        sodium_memzero(sk_hex, strlen(sk_hex));
        free(sk_hex);
        if (rc != 0) goto out;
        signed_ev = tmpl;
        tmpl = NULL;
    } else {
        char *unsigned_json = nostr_event_serialize_compact(tmpl);
        if (!unsigned_json) {
            err = MARMOT_ERR_MEMORY;
            goto out;
        }
        int rc = sign_fn(sign_data, unsigned_json, &signed_json);
        free(unsigned_json);
        if (rc != 0 || !signed_json) goto out;
        signed_ev = nostr_event_new();
        if (!signed_ev || !nostr_event_deserialize_compact(signed_ev, signed_json, NULL)) {
            err = MARMOT_ERR_VALIDATION;
            goto out;
        }
    }

    /* The signed event must be exactly the template (its recomputed id
     * commits to pubkey, created_at, kind, tags and content), signed by the
     * account key. */
    err = MARMOT_ERR_VALIDATION;
    if (!signed_ev->pubkey || strcmp(signed_ev->pubkey, pk_hex) != 0 || !signed_ev->sig ||
        strlen(signed_ev->sig) != 128)
        goto out;
    char *claimed = signed_ev->id;
    signed_ev->id = NULL;
    char *got_id = nostr_event_get_id(signed_ev);
    free(signed_ev->id);
    signed_ev->id = claimed;
    bool same = got_id && claimed && strcmp(got_id, want_id) == 0 && strcmp(claimed, want_id) == 0;
    free(got_id);
    if (!same || !nostr_event_check_signature(signed_ev)) goto out;
    if (marmot_hex_decode(signed_ev->sig, sig, sizeof(sig)) != 0) goto out;
    write_proof(out, account_pk, created_at, sig);
    err = MARMOT_OK;

out:
    if (tmpl) nostr_event_free(tmpl);
    if (signed_ev) nostr_event_free(signed_ev);
    free(signed_json);
    free(want_id);
    free(pk_hex);
    return err;
}

MarmotError
marmot_account_proof_from_signed(const uint8_t account_pk[32],
                                 const uint8_t *sig_key, size_t sig_key_len,
                                 const char *signed_json,
                                 uint8_t out[MARMOT_ACCOUNT_PROOF_LEN])
{
    if (!account_pk || !sig_key || !signed_json || !out) return MARMOT_ERR_INVALID_ARG;
    NostrEvent *ev = nostr_event_new();
    if (!ev) return MARMOT_ERR_MEMORY;
    MarmotError err = MARMOT_ERR_VALIDATION;
    char *pk_hex = marmot_hex_encode(account_pk, 32);
    char *want_id = NULL, *got_id = NULL;
    uint8_t sig[64];
    if (!pk_hex) {
        err = MARMOT_ERR_MEMORY;
        goto out;
    }
    if (!nostr_event_deserialize_compact(ev, signed_json, NULL) || ev->created_at < 1 ||
        !ev->pubkey || strcmp(ev->pubkey, pk_hex) != 0 || !ev->id || !ev->sig ||
        strlen(ev->sig) != 128)
        goto out;
    /* Exactly the template for this key, at the event's own created_at: the
     * id commits to pubkey, created_at, kind, tags and content. */
    want_id = marmot_account_proof_template_id(account_pk, (uint64_t)ev->created_at,
                                               MARMOT_CIPHERSUITE,
                                               MARMOT_SIGNATURE_SCHEME_ED25519, sig_key,
                                               sig_key_len);
    char *claimed = ev->id;
    ev->id = NULL;
    got_id = nostr_event_get_id(ev);
    free(ev->id);
    ev->id = claimed;
    if (!want_id || !got_id || strcmp(got_id, want_id) != 0 || strcmp(claimed, want_id) != 0 ||
        !nostr_event_check_signature(ev) || marmot_hex_decode(ev->sig, sig, sizeof(sig)) != 0)
        goto out;
    write_proof(out, account_pk, (uint64_t)ev->created_at, sig);
    err = MARMOT_OK;
out:
    free(want_id);
    free(got_id);
    free(pk_hex);
    nostr_event_free(ev);
    return err;
}

MarmotError
marmot_account_proof_verify(const uint8_t *proof, size_t proof_len, const uint8_t identity[32],
                            uint16_t ciphersuite, uint16_t signature_scheme,
                            const uint8_t *sig_key, size_t sig_key_len)
{
    if (!proof || proof_len != MARMOT_ACCOUNT_PROOF_LEN || !identity || !sig_key)
        return MARMOT_ERR_VALIDATION;
    if (memcmp(proof, identity, 32) != 0) return MARMOT_ERR_VALIDATION;
    uint64_t at = 0;
    for (int i = 0; i < 8; i++) at = (at << 8) | proof[32 + i];
    if (!proof_context_supported(at, ciphersuite, signature_scheme, sig_key_len))
        return MARMOT_ERR_VALIDATION;
    NostrEvent *ev = proof_template(identity, at, ciphersuite, signature_scheme, sig_key,
                                    sig_key_len);
    if (!ev) return MARMOT_ERR_MEMORY;
    MarmotError err = MARMOT_ERR_VALIDATION;
    char *id = nostr_event_get_id(ev); /* computed only; not stored in ev */
    char *sig_hex = marmot_hex_encode(proof + 40, 64);
    if (id && sig_hex) {
        free(ev->id);
        ev->id = id; /* ownership moves to the event */
        id = NULL;
        nostr_event_set_sig(ev, sig_hex);
        if (nostr_event_check_signature(ev)) err = MARMOT_OK;
    }
    free(id);
    free(sig_hex);
    nostr_event_free(ev);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * The proof on a member LeafNode (nostrc-7vyi)
 * ──────────────────────────────────────────────────────────────────────── */

MarmotLeafProofStatus
marmot_leaf_proof_status(const MlsLeafNode *leaf, uint16_t ciphersuite)
{
    if (!leaf) return MARMOT_LEAF_PROOF_INVALID;
    const uint8_t *dict = NULL;
    size_t dlen = 0, count = 0;
    if (marmot_extensions_find(leaf->extensions_data, leaf->extensions_len,
                               MARMOT_EXT_APP_DATA_DICTIONARY, &dict, &dlen, &count) != 0)
        return MARMOT_LEAF_PROOF_INVALID;
    if (count == 0) return MARMOT_LEAF_PROOF_ABSENT;
    if (count != 1) return MARMOT_LEAF_PROOF_INVALID;

    MarmotComponentData *entries = NULL;
    size_t ne = 0;
    if (marmot_app_data_dict_parse(dict, dlen, &entries, &ne) != 0)
        return MARMOT_LEAF_PROOF_INVALID;
    const MarmotComponentData *app_components = NULL, *proof = NULL;
    for (size_t i = 0; i < ne; i++) {
        if (entries[i].component_id == MARMOT_COMPONENT_APP_COMPONENTS)
            app_components = &entries[i];
        else if (entries[i].component_id == MARMOT_COMPONENT_ACCOUNT_PROOF_V2)
            proof = &entries[i];   /* keys are unique: the parse rejects repeats */
    }
    MarmotLeafProofStatus st = MARMOT_LEAF_PROOF_INVALID;
    uint16_t *ids = NULL;
    size_t n_ids = 0;
    if (app_components && proof &&
        marmot_components_list_decode(app_components->data, app_components->len,
                                      &ids, &n_ids) == 0 &&
        marmot_u16_list_contains(ids, n_ids, MARMOT_COMPONENT_ACCOUNT_PROOF_V2) &&
        leaf->credential_identity_len == 32 && leaf->credential_identity &&
        marmot_account_proof_verify(proof->data, proof->len, leaf->credential_identity,
                                    ciphersuite, MARMOT_SIGNATURE_SCHEME_ED25519,
                                    leaf->signature_key, MLS_SIG_PK_LEN) == MARMOT_OK)
        st = MARMOT_LEAF_PROOF_VALID;
    free(ids);
    free(entries);
    return st;
}

static MarmotError
leaf_dictionary_extensions(const uint16_t *supported, size_t n_supported,
                           const uint8_t proof[MARMOT_ACCOUNT_PROOF_LEN],
                           uint8_t **out, size_t *out_len);

MarmotError
marmot_leaf_proof_extensions(const uint8_t proof[MARMOT_ACCOUNT_PROOF_LEN],
                             uint8_t **out, size_t *out_len)
{
    static const uint16_t supported[] = {MARMOT_COMPONENT_APP_COMPONENTS,
                                         MARMOT_COMPONENT_ACCOUNT_PROOF_V2};
    return leaf_dictionary_extensions(supported, 2, proof, out, out_len);
}

MarmotError
marmot_leaf_adopted_extensions(const uint8_t proof[MARMOT_ACCOUNT_PROOF_LEN],
                               uint8_t **out, size_t *out_len)
{
    /* app_components itself, then every adopted component libmarmot can be
     * required to support (ascending; includes 0x8009). */
    uint16_t supported[1 + MLS_ADOPTED_SUPPORTED_COMPONENT_COUNT];
    supported[0] = MARMOT_COMPONENT_APP_COMPONENTS;
    memcpy(supported + 1, MLS_ADOPTED_SUPPORTED_COMPONENTS,
           sizeof(MLS_ADOPTED_SUPPORTED_COMPONENTS));
    return leaf_dictionary_extensions(supported, 1 + MLS_ADOPTED_SUPPORTED_COMPONENT_COUNT,
                                      proof, out, out_len);
}

MarmotError
marmot_leaf_set_adopted_proof(MlsLeafNode *leaf, const uint8_t proof[MARMOT_ACCOUNT_PROOF_LEN])
{
    if (!leaf || !proof) return MARMOT_ERR_INVALID_ARG;
    uint8_t *exts = NULL;
    size_t len = 0;
    MarmotError err = marmot_leaf_adopted_extensions(proof, &exts, &len);
    if (err != MARMOT_OK) return err;
    free(leaf->extensions_data);
    leaf->extensions_data = exts;
    leaf->extensions_len = len;
    return MARMOT_OK;
}

static MarmotError
leaf_dictionary_extensions(const uint16_t *supported, size_t n_supported,
                           const uint8_t proof[MARMOT_ACCOUNT_PROOF_LEN],
                           uint8_t **out, size_t *out_len)
{
    if (!proof || !out || !out_len) return MARMOT_ERR_INVALID_ARG;
    MlsTlsBuf app_components, safe_aad, dict, exts;
    mls_tls_buf_init(&app_components, 16);
    mls_tls_buf_init(&safe_aad, 4);
    mls_tls_buf_init(&dict, 160);
    mls_tls_buf_init(&exts, 176);
    MarmotError err = MARMOT_ERR_MEMORY;
    if (!app_components.data || !safe_aad.data || !dict.data || !exts.data) goto out;
    if (marmot_components_list_encode(supported, n_supported, &app_components) != 0 ||
        marmot_components_list_encode(NULL, 0, &safe_aad) != 0)
        goto out;
    MarmotComponentData entries[3] = {
        {MARMOT_COMPONENT_APP_COMPONENTS, app_components.data, app_components.len},
        {MARMOT_COMPONENT_SAFE_AAD, safe_aad.data, safe_aad.len},
        {MARMOT_COMPONENT_ACCOUNT_PROOF_V2, proof, MARMOT_ACCOUNT_PROOF_LEN},
    };
    if (marmot_app_data_dict_encode(entries, 3, &dict) != 0 ||
        mls_tls_write_u16(&exts, MARMOT_EXT_APP_DATA_DICTIONARY) != 0 ||
        mls_tls_write_opaque32(&exts, dict.data, dict.len) != 0)
        goto out;
    *out = exts.data;
    *out_len = exts.len;
    exts.data = NULL;
    err = MARMOT_OK;
out:
    mls_tls_buf_free(&app_components);
    mls_tls_buf_free(&safe_aad);
    mls_tls_buf_free(&dict);
    if (exts.data) mls_tls_buf_free(&exts);
    return err;
}

MarmotError
marmot_leaf_set_proof(MlsLeafNode *leaf, const uint8_t proof[MARMOT_ACCOUNT_PROOF_LEN])
{
    if (!leaf || !proof) return MARMOT_ERR_INVALID_ARG;
    uint8_t *exts = NULL;
    size_t len = 0;
    MarmotError err = marmot_leaf_proof_extensions(proof, &exts, &len);
    if (err != MARMOT_OK) return err;
    free(leaf->extensions_data);
    leaf->extensions_data = exts;
    leaf->extensions_len = len;
    return MARMOT_OK;
}
