/*
 * libmarmot - MIP-03: Group Messages
 *
 * Creates and processes kind:445 group events. Group events contain
 * MLS-encrypted content (application messages, proposals, commits)
 * further encrypted with NIP-44 using the MLS exporter_secret.
 *
 * Encryption flow (MIP-03):
 *   1. Wrap inner event (unsigned Nostr event) as application plaintext
 *   2. NIP-44-encrypt: derive conversation_key from exporter_secret
 *      treated as a secp256k1 private key (sk = exporter_secret,
 *      pk = sk*G, convkey = NIP44_convkey(sk, pk))
 *   3. Build kind:445 event with ephemeral pubkey & NIP-44 ciphertext
 *   4. h-tag carries nostr_group_id for routing
 *
 * Decryption flow:
 *   1. Parse kind:445 event, extract "h" tag → find group
 *   2. NIP-44-decrypt using same conversation_key derivation
 *   3. Extract inner event JSON from decrypted plaintext
 *   4. Validate sender identity
 *
 * MLS PrivateMessage framing (mls_group_encrypt/decrypt) wraps the
 * plaintext before NIP-44 encryption. Raw inner JSON compatibility is
 * accepted only when MarmotConfig.allow_legacy_raw_messages is enabled.
 *
 * SPDX-License-Identifier: MIT
 */

#include "marmot-internal.h"
#include "commits.h"
#include "mls/mls_group.h"
#include "mls/mls-internal.h"
#include <nostr/nip44/nip44.h>
#include <nostr-event.h>
#include <nostr-tag.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Constants
 * ──────────────────────────────────────────────────────────────────────── */

/* Maximum number of past epochs to search when decrypting out-of-order messages */
#define MAX_EPOCH_LOOKBACK 5

/* ──────────────────────────────────────────────────────────────────────────
 * Internal base64 helpers
 * ──────────────────────────────────────────────────────────────────────── */

static char *
msg_base64_encode(const uint8_t *data, size_t len)
{
    size_t b64_maxlen = sodium_base64_ENCODED_LEN(len, sodium_base64_VARIANT_ORIGINAL);
    char *out = malloc(b64_maxlen);
    if (!out) return NULL;
    sodium_bin2base64(out, b64_maxlen, data, len, sodium_base64_VARIANT_ORIGINAL);
    return out;
}

static uint8_t *
msg_base64_decode(const char *b64, size_t *out_len)
{
    if (!b64 || !out_len) return NULL;
    size_t b64_len = strlen(b64);
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
 * Internal: derive NIP-44 conversation key from exporter_secret
 *
 * Per MIP-03: treat exporter_secret as a secp256k1 private key.
 *   sk = exporter_secret (32 bytes)
 *   pk = x_only_pubkey(sk * G)
 *   conversation_key = nostr_nip44_convkey(sk, pk)
 *
 * Both sender and receiver derive the same conversation_key because
 * they share the exporter_secret for the same epoch.
 * ──────────────────────────────────────────────────────────────────────── */

static int
derive_nip44_convkey(const uint8_t exporter_secret[32],
                     uint8_t out_convkey[32])
{
    int ret = -1;

    /* Create secp256k1 context */
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) return -1;

    /* Verify the exporter_secret is a valid secp256k1 private key */
    if (!secp256k1_ec_seckey_verify(ctx, exporter_secret)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }

    /* Create keypair from the exporter_secret */
    secp256k1_keypair keypair;
    if (!secp256k1_keypair_create(ctx, &keypair, exporter_secret)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }

    /* Extract x-only public key */
    secp256k1_xonly_pubkey xonly_pk;
    if (!secp256k1_keypair_xonly_pub(ctx, &xonly_pk, NULL, &keypair)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }

    /* Serialize x-only public key to 32 bytes */
    uint8_t pk_bytes[32];
    if (!secp256k1_xonly_pubkey_serialize(ctx, pk_bytes, &xonly_pk)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }

    secp256k1_context_destroy(ctx);

    /* Now derive the NIP-44 conversation key using ECDH(sk, pk) */
    ret = nostr_nip44_convkey(exporter_secret, pk_bytes, out_convkey);

    sodium_memzero(pk_bytes, sizeof(pk_bytes));
    return ret;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: NIP-44 encrypt/decrypt using exporter_secret-derived convkey
 * ──────────────────────────────────────────────────────────────────────── */

static int
nip44_encrypt_with_secret(const uint8_t exporter_secret[32],
                           const uint8_t *plaintext, size_t plaintext_len,
                           char **out_base64)
{
    uint8_t convkey[32];
    if (derive_nip44_convkey(exporter_secret, convkey) != 0)
        return -1;

    int rc = nostr_nip44_encrypt_v2_with_convkey(convkey,
                                                  plaintext, plaintext_len,
                                                  out_base64);
    sodium_memzero(convkey, sizeof(convkey));
    return rc;
}

int
marmot_group_event_encrypt(const uint8_t exporter_secret[32],
                           const uint8_t *plaintext, size_t plaintext_len,
                           char **out_base64)
{
    return nip44_encrypt_with_secret(exporter_secret, plaintext, plaintext_len,
                                     out_base64);
}

static int
nip44_decrypt_with_secret(const uint8_t exporter_secret[32],
                           const char *base64_payload,
                           uint8_t **out_plaintext, size_t *out_len)
{
    uint8_t convkey[32];
    if (derive_nip44_convkey(exporter_secret, convkey) != 0)
        return -1;

    int rc = nostr_nip44_decrypt_v2_with_convkey(convkey,
                                                  base64_payload,
                                                  out_plaintext, out_len);
    sodium_memzero(convkey, sizeof(convkey));
    return rc;
}

int
marmot_group_event_decrypt(const uint8_t exporter_secret[32],
                           const char *base64_payload,
                           uint8_t **out_plaintext, size_t *out_len)
{
    return nip44_decrypt_with_secret(exporter_secret, base64_payload,
                                     out_plaintext, out_len);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: Load / save MLS group state from storage
 *
 * Mirrors the helpers in groups.c but kept local to avoid exposing them.
 * ──────────────────────────────────────────────────────────────────────── */

static int
msg_load_mls_group(Marmot *m, const MarmotGroupId *gid, MlsGroup *out)
{
    if (!m->storage || !m->storage->mls_load) return -1;

    uint8_t *state_data = NULL;
    size_t state_len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, "mls_group",
                                            gid->data, gid->len,
                                            &state_data, &state_len);
    if (err != MARMOT_OK || !state_data) return -1;

    int rc = mls_group_deserialize(state_data, state_len, out);
    sodium_memzero(state_data, state_len);   /* epoch secrets */
    free(state_data);
    return rc;
}

/* A stored MLS state record as it was before this operation replaced it.  A
 * received message's ratchet step must not outlive the message: if a later
 * write of the operation fails, the record is written back (with the
 * storage transaction hooks the rollback restores it anyway), so the message
 * can be processed again instead of being lost with its consumed key
 * (nostrc-ai04). */
typedef struct {
    const char *label;   /* NULL: nothing to restore */
    uint8_t    *blob;
    size_t      len;
} StateUndo;

static void
state_undo_clear(StateUndo *u)
{
    if (u->blob) {
        sodium_memzero(u->blob, u->len);
        free(u->blob);
    }
    memset(u, 0, sizeof(*u));
}

/* Keep `label`'s record of `gid` as it is now. */
static void
state_undo_capture(Marmot *m, const MarmotGroupId *gid, const char *label, StateUndo *u)
{
    state_undo_clear(u);
    uint8_t *blob = NULL;
    size_t len = 0;
    if (m->storage->mls_load(m->storage->ctx, label, gid->data, gid->len, &blob,
                             &len) == MARMOT_OK && blob) {
        u->label = label;
        u->blob = blob;
        u->len = len;
    } else if (blob) {
        sodium_memzero(blob, len);
        free(blob);
    }
}

/* The operation failed after replacing the record: put it back. */
static void
state_undo_apply(Marmot *m, const MarmotGroupId *gid, StateUndo *u)
{
    if (u->label)
        (void)m->storage->mls_store(m->storage->ctx, u->label, gid->data, gid->len,
                                    u->blob, u->len);
    state_undo_clear(u);
}

static int
msg_save_mls_group(Marmot *m, const MlsGroup *mls)
{
    if (!m->storage || !m->storage->mls_store) return -1;

    uint8_t *state_data = NULL;
    size_t state_len = 0;
    if (mls_group_serialize(mls, &state_data, &state_len) != 0)
        return -1;

    MarmotError err = m->storage->mls_store(m->storage->ctx, "mls_group",
                                             mls->group_id, mls->group_id_len,
                                             state_data, state_len);
    sodium_memzero(state_data, state_len);
    free(state_data);
    return (err == MARMOT_OK) ? 0 : -1;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: free stack-allocated NostrEvent fields
 * ──────────────────────────────────────────────────────────────────────── */

static void
free_stack_event(NostrEvent *ev)
{
    free(ev->id);
    free(ev->pubkey);
    free(ev->content);
    free(ev->sig);
    nostr_tags_free(ev->tags);
    memset(ev, 0, sizeof(*ev));
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: parse kind:445 event and extract group routing info
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    char    *content;             /* NIP-44 encrypted content (base64/raw) */
    uint8_t  nostr_group_id[32]; /* from "h" tag */
    bool     has_group_id;
    int64_t  created_at;
    char    *event_id;           /* hex event ID (transferred) */
    char    *pubkey;             /* hex pubkey (ephemeral, transferred) */
} ParsedGroupEvent;

static void
parsed_group_event_clear(ParsedGroupEvent *ev)
{
    free(ev->content);
    free(ev->event_id);
    free(ev->pubkey);
    memset(ev, 0, sizeof(*ev));
}

/* How a kind:445 reached us, which decides how it is authenticated. */
typedef enum {
    /* From a relay (or any untrusted transport): the event id must be its
     * canonical NIP-01 hash and the Schnorr signature by its (ephemeral)
     * pubkey must verify (transports/nostr.md, nostrc-6r6s). */
    GROUP_EVENT_SIGNED,
    /* A rumor the caller took out of a NIP-59 gift wrap whose seal it
     * verified: unsigned by design.  A declared id must still be canonical;
     * a missing one is computed. */
    GROUP_EVENT_RUMOR
} GroupEventOrigin;

/* The canonical id of `event` checked as `origin` requires, into `id`
 * (65 bytes).  MARMOT_ERR_EVENT for a malformed or non-canonical id,
 * MARMOT_ERR_SIGNATURE for a missing or invalid signature. */
static MarmotError
authenticate_group_event(const NostrEvent *event, GroupEventOrigin origin,
                         char id[65])
{
    NostrEventValidationStatus st;
    if (origin == GROUP_EVENT_SIGNED)
        st = nostr_event_validate(event, id);
    else if (event->id)
        st = nostr_event_validate_id(event, id);   /* a declared id must be canonical */
    else
        st = nostr_event_compute_id(event, id);
    switch (st) {
    case NOSTR_EVENT_VALIDATION_OK:
        return MARMOT_OK;
    case NOSTR_EVENT_VALIDATION_MISSING_FIELD:
        /* validate() requires id, pubkey and sig; the others only what
         * the hash covers. */
        return origin == GROUP_EVENT_SIGNED ? MARMOT_ERR_SIGNATURE : MARMOT_ERR_EVENT;
    case NOSTR_EVENT_VALIDATION_BAD_SIGNATURE_FORMAT:
    case NOSTR_EVENT_VALIDATION_SIGNATURE_INVALID:
        return MARMOT_ERR_SIGNATURE;
    default:
        return MARMOT_ERR_EVENT;   /* bad or non-canonical id, bad pubkey, limits */
    }
}

static MarmotError
parse_group_event(const char *event_json, GroupEventOrigin origin,
                  ParsedGroupEvent *out)
{
    memset(out, 0, sizeof(*out));

    NostrEvent event;
    memset(&event, 0, sizeof(event));
    if (!nostr_event_deserialize_compact(&event, event_json, NULL))
        return MARMOT_ERR_DESERIALIZATION;

    /* Verify kind */
    if (event.kind != MARMOT_KIND_GROUP_MESSAGE) {
        free_stack_event(&event);
        return MARMOT_ERR_UNEXPECTED_EVENT;
    }

    /* Extract content */
    if (!event.content || strlen(event.content) == 0) {
        free_stack_event(&event);
        return MARMOT_ERR_DESERIALIZATION;
    }

    /* Extract "h" tag (nostr_group_id) */
    if (event.tags) {
        for (size_t i = 0; i < nostr_tags_size(event.tags); i++) {
            NostrTag *tag = nostr_tags_get(event.tags, i);
            if (nostr_tag_size(tag) >= 2 &&
                strcmp(nostr_tag_get_key(tag), "h") == 0) {
                const char *gid_hex = nostr_tag_get_value(tag);
                if (gid_hex && strlen(gid_hex) == 64) {
                    if (marmot_hex_decode(gid_hex, out->nostr_group_id, 32) == 0) {
                        out->has_group_id = true;
                    }
                    /* If decode fails, has_group_id remains false and will be caught below */
                }
                break;
            }
        }
    }
    if (!out->has_group_id) {
        free_stack_event(&event);
        memset(out, 0, sizeof(*out));
        return MARMOT_ERR_MISSING_GROUP_ID_TAG;
    }

    /* Authenticate before anything reads storage or tries a key, so a
     * rejected event changes nothing.  From here on the id is the canonical
     * one: it keys the processed markers. */
    {
        char canonical_id[65] = { 0 };
        MarmotError aerr = authenticate_group_event(&event, origin, canonical_id);
        char *id = aerr == MARMOT_OK ? strdup(canonical_id) : NULL;
        if (aerr == MARMOT_OK && !id) aerr = MARMOT_ERR_MEMORY;
        if (aerr != MARMOT_OK) {
            free_stack_event(&event);
            memset(out, 0, sizeof(*out));
            return aerr;
        }
        free(event.id);
        event.id = id;
    }

    /* Transfer ownership of fields we need */
    out->content = event.content;     event.content = NULL;
    out->event_id = event.id;         event.id = NULL;
    out->pubkey = event.pubkey;       event.pubkey = NULL;
    out->created_at = event.created_at;
    free_stack_event(&event);
    return MARMOT_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: the inner event's author is the MLS sender (nostrc-we6g)
 *
 * Marmot binds the inner (rumor) event to the member that sent it: its
 * pubkey MUST be the account identity of the MLS sender leaf's credential
 * (foundation/application-messages.md; legacy MIP-03).  The PrivateMessage
 * signature authenticates the leaf; these checks tie the event to it.
 * ──────────────────────────────────────────────────────────────────────── */

/* Sender side: `inner_json` with pubkey = `identity`.  A missing pubkey is
 * filled in (*out_json, caller frees; a declared id is recomputed); a pubkey
 * of another account is MARMOT_ERR_AUTHOR_MISMATCH; a present, matching one
 * leaves *out_json NULL (use `inner_json` as is). */
static MarmotError
bind_inner_author(const char *inner_json, const uint8_t identity[32], char **out_json)
{
    *out_json = NULL;
    NostrEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!nostr_event_deserialize_compact(&ev, inner_json, NULL))
        return MARMOT_ERR_EVENT;
    MarmotError err = MARMOT_OK;
    if (ev.pubkey && *ev.pubkey) {
        uint8_t pk[32];
        if (strlen(ev.pubkey) != 64 || marmot_hex_decode(ev.pubkey, pk, 32) != 0 ||
            memcmp(pk, identity, 32) != 0)
            err = MARMOT_ERR_AUTHOR_MISMATCH;
    } else {
        char *pk_hex = marmot_hex_encode(identity, 32);
        if (!pk_hex) {
            err = MARMOT_ERR_MEMORY;
        } else {
            free(ev.pubkey);
            ev.pubkey = pk_hex;
            char id[65];
            if (ev.id) {
                if (nostr_event_compute_id(&ev, id) != NOSTR_EVENT_VALIDATION_OK) {
                    err = MARMOT_ERR_EVENT;
                } else {
                    free(ev.id);
                    ev.id = strdup(id);
                    if (!ev.id) err = MARMOT_ERR_MEMORY;
                }
            }
            if (err == MARMOT_OK) {
                *out_json = nostr_event_serialize_compact(&ev);
                if (!*out_json) err = MARMOT_ERR_EVENT_BUILD;
            }
        }
    }
    free_stack_event(&ev);
    return err;
}

/* Receiver side: `inner_json` must be an event whose pubkey is the MLS
 * sender's `identity` (else MARMOT_ERR_AUTHOR_MISMATCH, or MARMOT_ERR_EVENT
 * when it is no event); its canonical NIP-01 id goes to `inner_id`. */
static MarmotError
check_inner_author(const char *inner_json, const uint8_t identity[32], uint8_t inner_id[32])
{
    NostrEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!nostr_event_deserialize_compact(&ev, inner_json, NULL))
        return MARMOT_ERR_EVENT;
    MarmotError err = MARMOT_ERR_AUTHOR_MISMATCH;
    uint8_t pk[32];
    char id[65];
    if (ev.pubkey && strlen(ev.pubkey) == 64 && marmot_hex_decode(ev.pubkey, pk, 32) == 0 &&
        memcmp(pk, identity, 32) == 0) {
        err = (nostr_event_compute_id(&ev, id) == NOSTR_EVENT_VALIDATION_OK &&
               marmot_hex_decode(id, inner_id, 32) == 0) ? MARMOT_OK : MARMOT_ERR_EVENT;
    }
    free_stack_event(&ev);
    return err;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API: marmot_create_message
 * ══════════════════════════════════════════════════════════════════════════ */

static MarmotError
create_message_impl(Marmot *m,
                       const MarmotGroupId *mls_group_id,
                       const char *inner_event_json,
                       MarmotOutgoingMessage *result)
{
    if (!m || !mls_group_id || !inner_event_json || !result)
        return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->find_group_by_mls_id ||
        !m->storage->get_exporter_secret || !m->storage->save_message ||
        !m->storage->save_group)
        return MARMOT_ERR_STORAGE;

    memset(result, 0, sizeof(*result));

    /* ── 1. Find the group ────────────────────────────────────────────── */
    MarmotGroup *group = NULL;
    MarmotError err = m->storage->find_group_by_mls_id(m->storage->ctx,
                                                         mls_group_id, &group);
    if (err != MARMOT_OK || !group)
        return MARMOT_ERR_GROUP_NOT_FOUND;

    if (group->state != MARMOT_GROUP_STATE_ACTIVE) {
        marmot_group_free(group);
        return MARMOT_ERR_USE_AFTER_EVICTION;
    }
    /* The record's epoch picks the exporter secret: repair it first if a
     * crash interrupted the last epoch transition. */
    err = marmot_group_reconcile(m, group);
    if (err != MARMOT_OK) {
        marmot_group_free(group);
        return err;
    }

    /* ── 2. Get exporter_secret for current epoch ─────────────────────── */
    uint8_t exporter_secret[32];
    err = m->storage->get_exporter_secret(m->storage->ctx,
                                           mls_group_id,
                                           group->epoch,
                                           exporter_secret);
    if (err != MARMOT_OK) {
        marmot_group_free(group);
        return MARMOT_ERR_GROUP_EXPORTER_SECRET;
    }

    /* ── 3. Encrypt inner event ─────────────────────────────────────────
     *
     * Per MIP-03, the inner event JSON is:
     *   1. Wrapped as MLS PrivateMessage via mls_group_encrypt()
     *   2. Then NIP-44-encrypted with the exporter_secret-derived convkey
     *
     * Missing MLS group state is an MLS error by default. Legacy raw JSON
     * NIP-44 encryption is only available through the explicit config opt-in.
     */
    const uint8_t *plaintext = (const uint8_t *)inner_event_json;
    size_t plaintext_len = strlen(inner_event_json);

    /* Try MLS PrivateMessage framing first */
    MlsGroup mls_group;
    memset(&mls_group, 0, sizeof(mls_group));
    bool mls_loaded = (msg_load_mls_group(m, mls_group_id, &mls_group) == 0);

    const uint8_t *nip44_plaintext = plaintext;
    size_t nip44_plaintext_len = plaintext_len;
    uint8_t *mls_ciphertext = NULL;
    size_t mls_ciphertext_len = 0;

    /* The inner event is authored by our account: receivers drop it
     * otherwise (MIP-03, nostrc-we6g).  A missing pubkey is filled in. */
    char *bound_json = NULL;
    if (mls_loaded) {
        uint8_t me[32];
        MarmotError berr =
            marmot_mls_sender_identity(&mls_group, mls_group.own_leaf_index, me) == 0
                ? bind_inner_author(inner_event_json, me, &bound_json)
                : MARMOT_ERR_AUTHOR_MISMATCH;
        if (berr != MARMOT_OK) {
            mls_group_free(&mls_group);
            sodium_memzero(exporter_secret, sizeof(exporter_secret));
            marmot_group_free(group);
            return berr;
        }
        if (bound_json) {
            plaintext = (const uint8_t *)bound_json;
            plaintext_len = strlen(bound_json);
        }
        if (mls_group_encrypt(&mls_group, plaintext, plaintext_len,
                              &mls_ciphertext, &mls_ciphertext_len) != 0) {
            free(bound_json);
            mls_group_free(&mls_group);
            sodium_memzero(exporter_secret, sizeof(exporter_secret));
            marmot_group_free(group);
            return MARMOT_ERR_MLS_CREATE_MESSAGE;
        }
        nip44_plaintext = mls_ciphertext;
        nip44_plaintext_len = mls_ciphertext_len;
    } else if (!m->config.allow_legacy_raw_messages) {
        sodium_memzero(exporter_secret, sizeof(exporter_secret));
        free(bound_json);
        marmot_group_free(group);
        return MARMOT_ERR_MLS;
    }

    char *nip44_ciphertext = NULL;
    if (nip44_encrypt_with_secret(exporter_secret, nip44_plaintext,
                                   nip44_plaintext_len,
                                   &nip44_ciphertext) != 0) {
        free(mls_ciphertext);
        if (mls_loaded) mls_group_free(&mls_group);
        sodium_memzero(exporter_secret, sizeof(exporter_secret));
        free(bound_json);
        marmot_group_free(group);
        return MARMOT_ERR_NIP44;
    }
    free(mls_ciphertext);
    sodium_memzero(exporter_secret, sizeof(exporter_secret));

    /* Persist updated MLS state (generation counter advances on encrypt). */
    if (mls_loaded) {
        if (msg_save_mls_group(m, &mls_group) != 0) {
            free(nip44_ciphertext);
            mls_group_free(&mls_group);
            free(bound_json);
            marmot_group_free(group);
            return MARMOT_ERR_STORAGE;
        }
        mls_group_free(&mls_group);
    }

    /* ── 4. Build kind:445 event ──────────────────────────────────────── */
    /* Per MIP-03 a completely separate, fresh ephemeral key signs every
     * kind:445 (marmot_sign_ephemeral(), below); it is never the account
     * key and never reused. */
    NostrEvent *event = nostr_event_new();
    if (!event) {
        free(nip44_ciphertext);
        free(bound_json);
        marmot_group_free(group);
        return MARMOT_ERR_MEMORY;
    }

    nostr_event_set_kind(event, MARMOT_KIND_GROUP_MESSAGE);
    nostr_event_set_content(event, nip44_ciphertext);
    nostr_event_set_created_at(event, marmot_now());
    free(nip44_ciphertext);

    /* Tags: "h" = nostr_group_id hex */
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) {
        nostr_event_free(event);
        free(bound_json);
        marmot_group_free(group);
        return MARMOT_ERR_MEMORY;
    }
    char *gid_hex = marmot_hex_encode(group->nostr_group_id, 32);
    if (gid_hex) {
        NostrTag *tag = nostr_tag_new("h", gid_hex, NULL);
        free(gid_hex);
        if (tag) {
            nostr_tags_append(tags, tag);
        }
    }
    nostr_event_set_tags(event, tags);

    if (marmot_sign_ephemeral(event) != 0) {
        nostr_event_free(event);
        free(bound_json);
        marmot_group_free(group);
        return MARMOT_ERR_EVENT_BUILD;
    }
    result->event_json = nostr_event_serialize_compact(event);
    nostr_event_free(event);

    if (!result->event_json) {
        free(bound_json);
        marmot_group_free(group);
        return MARMOT_ERR_EVENT_BUILD;
    }

    /* ── 5. Return an unsaved local message view ──────────────────────── */
    result->message = marmot_message_new();
    if (!result->message) {
        free(bound_json);
        marmot_group_free(group);
        marmot_outgoing_message_free(result);
        return MARMOT_ERR_MEMORY;
    }
    result->message->kind = MARMOT_KIND_GROUP_MESSAGE;
    result->message->created_at = marmot_now();
    result->message->processed_at = 0;
    result->message->mls_group_id = marmot_group_id_new(
        mls_group_id->data, mls_group_id->len);
    result->message->content = bound_json ? bound_json : strdup(inner_event_json);
    bound_json = NULL;
    result->message->event_json = strdup(result->event_json);
    result->message->epoch = group->epoch;
    result->message->state = MARMOT_MSG_STATE_CREATED;

    /* Do not persist here: the returned kind:445 event is intentionally
     * unsigned and has no stable Nostr event id until the caller fills the
     * ephemeral pubkey/signature. Use marmot_save_created_message() after
     * signing to persist the real outer event id/json. */
    marmot_group_free(group);
    return MARMOT_OK;
}

static MarmotError
save_created_message_impl(Marmot *m,
                             const MarmotGroupId *mls_group_id,
                             const char *signed_group_event_json,
                             const char *inner_event_json)
{
    if (!m || !mls_group_id || !signed_group_event_json || !inner_event_json)
        return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->find_group_by_mls_id ||
        !m->storage->save_message || !m->storage->save_group)
        return MARMOT_ERR_STORAGE;

    ParsedGroupEvent parsed;
    MarmotError err = parse_group_event(signed_group_event_json, GROUP_EVENT_SIGNED,
                                        &parsed);
    if (err != MARMOT_OK)
        return err;
    if (!parsed.event_id || !parsed.pubkey ||
        strlen(parsed.event_id) != 64 || strlen(parsed.pubkey) != 64) {
        parsed_group_event_clear(&parsed);
        return MARMOT_ERR_VALIDATION;
    }

    MarmotGroup *group = NULL;
    err = m->storage->find_group_by_mls_id(m->storage->ctx, mls_group_id, &group);
    if (err != MARMOT_OK || !group) {
        parsed_group_event_clear(&parsed);
        return MARMOT_ERR_GROUP_NOT_FOUND;
    }

    MarmotMessage *msg = marmot_message_new();
    if (!msg) {
        marmot_group_free(group);
        parsed_group_event_clear(&parsed);
        return MARMOT_ERR_MEMORY;
    }
    marmot_hex_decode(parsed.event_id, msg->id, 32);
    marmot_hex_decode(parsed.pubkey, msg->pubkey, 32);
    msg->kind = MARMOT_KIND_GROUP_MESSAGE;
    msg->mls_group_id = marmot_group_id_new(mls_group_id->data, mls_group_id->len);
    msg->created_at = parsed.created_at;
    msg->processed_at = marmot_now();
    msg->content = strdup(inner_event_json);
    msg->event_json = strdup(signed_group_event_json);
    memcpy(msg->wrapper_event_id, msg->id, 32); /* no kind:1059 wrapper at this API layer */
    msg->epoch = group->epoch;
    msg->state = MARMOT_MSG_STATE_CREATED;

    err = m->storage->save_message(m->storage->ctx, msg);
    if (err == MARMOT_OK) {
        group->last_message_at = parsed.created_at;
        group->last_message_processed_at = msg->processed_at;
        free(group->last_message_id);
        group->last_message_id = strdup(parsed.event_id);
        err = m->storage->save_group(m->storage->ctx, group);
    }

    marmot_message_free(msg);
    marmot_group_free(group);
    parsed_group_event_clear(&parsed);
    return err;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API: marmot_process_message
 * ══════════════════════════════════════════════════════════════════════════ */

static MarmotError
process_group_event(Marmot *m, const char *group_event_json,
                    GroupEventOrigin origin, MarmotMessageResult *result)
{
    if (!m || !group_event_json || !result)
        return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->find_group_by_nostr_id ||
        !m->storage->get_exporter_secret || !m->storage->save_message ||
        !m->storage->save_group)
        return MARMOT_ERR_STORAGE;

    memset(result, 0, sizeof(*result));

    /* ── 1. Parse and authenticate the kind:445 event ─────────────────── */
    ParsedGroupEvent parsed;
    MarmotError err = parse_group_event(group_event_json, origin, &parsed);
    if (err != MARMOT_OK)
        return err;

    /* ── 2. Find the group by nostr_group_id ──────────────────────────── */
    MarmotGroup *group = NULL;
    err = m->storage->find_group_by_nostr_id(m->storage->ctx,
                                             parsed.nostr_group_id,
                                             &group);
    if (err != MARMOT_OK || !group) {
        parsed_group_event_clear(&parsed);
        return MARMOT_ERR_GROUP_NOT_FOUND;
    }

    /* An inactive group is read no more -- except that one removed by a
     * Commit that may still lose its epoch judges that epoch's Commits
     * (nostrc-xrya, W22 review B1; see commits.c "Removal of our own leaf"). */
    bool contested_removal = false;
    if (group->state != MARMOT_GROUP_STATE_ACTIVE) {
        bool removed = false, final = true;
        contested_removal =
            marmot_get_group_removal(m, &group->mls_group_id, &removed, NULL, NULL,
                                     &final) == MARMOT_OK && removed && !final;
        if (!contested_removal) {
            marmot_group_free(group);
            parsed_group_event_clear(&parsed);
            return MARMOT_ERR_USE_AFTER_EVICTION;
        }
    }
    /* Trial decryption starts at the record's epoch: repair it first if a
     * crash interrupted the last epoch transition (review N2). */
    err = marmot_group_reconcile(m, group);
    if (err != MARMOT_OK) {
        marmot_group_free(group);
        parsed_group_event_clear(&parsed);
        return err;
    }

    /* ── 3. Idempotency: check if already processed ───────────────────── */
    if (parsed.event_id && m->storage->is_message_processed) {
        uint8_t event_id_bytes[32];
        if (marmot_hex_decode(parsed.event_id, event_id_bytes, 32) == 0) {
            bool processed = false;
            if (m->storage->is_message_processed(m->storage->ctx,
                                                  event_id_bytes,
                                                  &processed) == MARMOT_OK &&
                processed) {
                marmot_group_free(group);
                parsed_group_event_clear(&parsed);
                result->type = MARMOT_RESULT_OWN_MESSAGE;
                return MARMOT_OK;
            }
        }
    }
    if (parsed.event_id && m->storage->find_message_by_id) {
        uint8_t event_id_bytes[32];
        if (marmot_hex_decode(parsed.event_id, event_id_bytes, 32) == 0) {
            MarmotMessage *existing = NULL;
            if (m->storage->find_message_by_id(m->storage->ctx,
                                                event_id_bytes,
                                                &existing) == MARMOT_OK
                && existing) {
                marmot_message_free(existing);
                marmot_group_free(group);
                parsed_group_event_clear(&parsed);
                result->type = MARMOT_RESULT_OWN_MESSAGE;
                return MARMOT_OK;
            }
        }
    }

    /* ── 4. Try to decrypt with current epoch, then recent epochs ─────── */
    /*
     * Messages may arrive out of order relative to epoch advances.
     * Try the current epoch's exporter_secret first, then fall back to
     * recent previous epochs if decryption fails.
     */
    uint8_t exporter_secret[32];
    uint8_t *decrypted = NULL;
    size_t decrypted_len = 0;
    uint64_t used_epoch = group->epoch;
    bool decrypted_ok = false;

    /* Collect epochs to try: current, then lookback */
    uint64_t min_epoch = (group->epoch > MAX_EPOCH_LOOKBACK)
                       ? group->epoch - MAX_EPOCH_LOOKBACK : 0;

    for (uint64_t ep = group->epoch; ep >= min_epoch && ep <= group->epoch; ep--) {
        if (m->storage->get_exporter_secret(m->storage->ctx,
                                             &group->mls_group_id,
                                             ep,
                                             exporter_secret) != MARMOT_OK)
            continue;

        /* Attempt NIP-44 decrypt with this epoch's secret */
        if (nip44_decrypt_with_secret(exporter_secret, parsed.content,
                                       &decrypted, &decrypted_len) == 0) {
            used_epoch = ep;
            decrypted_ok = true;
            break;
        }
        /* Wrong epoch — try next */
    }
    sodium_memzero(exporter_secret, sizeof(exporter_secret));

    if (!decrypted_ok) {
        marmot_group_free(group);
        parsed_group_event_clear(&parsed);
        /* A removed member reads nothing of later epochs. */
        return contested_removal ? MARMOT_ERR_USE_AFTER_EVICTION : MARMOT_ERR_NIP44;
    }

    /* ── 5. Commits (nostrc-9ata) ─────────────────────────────────────────
     *
     * A handshake message is an MLSMessage PublicMessage (version 1,
     * wire_format 1); application messages are PrivateMessages (wire_format
     * 2).  Commits are applied through the validated MLS path; see
     * marmot_commit_process_inbound() for the epoch rules. */
    bool is_commit = decrypted_len >= 4 && decrypted[0] == 0x00 && decrypted[1] == 0x01 &&
                     decrypted[2] == 0x00 && decrypted[3] == MLS_WIRE_FORMAT_PUBLIC_MESSAGE;
    if (contested_removal && !is_commit) {
        free(decrypted);
        marmot_group_free(group);
        parsed_group_event_clear(&parsed);
        return MARMOT_ERR_USE_AFTER_EVICTION;
    }
    if (is_commit) {
        err = marmot_commit_process_inbound(m, group, used_epoch,
                                            decrypted, decrypted_len,
                                            parsed.event_id, result);
        free(decrypted);
        marmot_group_free(group);
        parsed_group_event_clear(&parsed);
        if (err != MARMOT_OK) marmot_message_result_free(result);
        return err;
    }

    /* ── 6. Unwrap MLS PrivateMessage ─────────────────────────────────── */
    /*
     * MIP-03 requires the NIP-44 decrypted content to be an MLS
     * PrivateMessage. Raw inner-event JSON is accepted only for explicitly
     * opted-in legacy deployments.
     */
    MlsGroup mls_group;
    memset(&mls_group, 0, sizeof(mls_group));
    bool mls_loaded = (msg_load_mls_group(m, &group->mls_group_id,
                                           &mls_group) == 0);

    uint8_t *inner_plaintext = NULL;
    size_t inner_plaintext_len = 0;
    bool used_mls = false;
    bool late = false;   /* decrypted with the retained previous-epoch state */
    StateUndo undo = { 0 };   /* the ratchet record this message replaced */
    StateUndo parent_undo = { 0 };   /* the retained parent, if a witness settled it */
    uint32_t live_sender = UINT32_MAX;   /* sender leaf of a current-epoch message */
    uint8_t sender_identity[32] = { 0 };   /* the MLS sender's account */
    bool have_identity = false;
    uint8_t inner_id[32] = { 0 };          /* canonical id of the inner event */
    bool duplicate = false;

    /* Only attempt MLS decrypt when the stored MLS epoch matches the epoch
     * whose exporter_secret successfully decrypted the NIP-44 layer. If
     * they differ (epoch lookback was used), the MLS state for the message's
     * epoch is no longer in storage and MLS decrypt would fail. */
    if (mls_loaded && mls_group.epoch == used_epoch) {
        uint32_t sender_leaf = 0;
        int mls_rc = mls_group_decrypt(&mls_group, decrypted, decrypted_len,
                                        &inner_plaintext, &inner_plaintext_len,
                                        &sender_leaf);
        if (mls_rc == 0) {
            used_mls = true;
            live_sender = sender_leaf;
            have_identity = marmot_mls_sender_identity(&mls_group, sender_leaf,
                                                       sender_identity) == 0;
        } else if (mls_rc == MARMOT_ERR_OWN_MESSAGE) {
            /* MLS identified this as our own message echoed back from the
             * relay. The plaintext was already stored locally at send time
             * (in marmot_create_message), so the application can use that. */
            free(decrypted);
            mls_group_free(&mls_group);
            marmot_group_free(group);
            parsed_group_event_clear(&parsed);
            result->type = MARMOT_RESULT_OWN_MESSAGE;
            return MARMOT_OK;
        }
    } else if (mls_loaded && used_epoch + 1 == mls_group.epoch) {
        /* A message sent in the previous epoch that arrived after we
         * applied the next Commit: read it with the retained parent state
         * (nostrc-qp24.7; one epoch, libmarmot's rewind horizon). */
        uint32_t sender_leaf = 0;
        MarmotError lerr = marmot_commit_decrypt_late(m, &group->mls_group_id, used_epoch,
                                                      decrypted, decrypted_len,
                                                      &inner_plaintext,
                                                      &inner_plaintext_len, &sender_leaf,
                                                      sender_identity,
                                                      &undo.blob, &undo.len);
        if (lerr == MARMOT_OK) {
            used_mls = true;
            late = true;
            have_identity = true;
            undo.label = MARMOT_MLS_PARENT_LABEL;
        } else if (lerr == MARMOT_ERR_OWN_MESSAGE ||
                   (lerr != MARMOT_ERR_MLS && lerr != MARMOT_ERR_STORAGE_NOT_FOUND)) {
            /* Our own echo, or a storage failure (fail closed). */
            free(decrypted);
            mls_group_free(&mls_group);
            marmot_group_free(group);
            parsed_group_event_clear(&parsed);
            if (lerr != MARMOT_ERR_OWN_MESSAGE) return lerr;
            result->type = MARMOT_RESULT_OWN_MESSAGE;
            return MARMOT_OK;
        }
    }

    if (!used_mls && !m->config.allow_legacy_raw_messages) {
        free(decrypted);
        if (mls_loaded) mls_group_free(&mls_group);
        marmot_group_free(group);
        parsed_group_event_clear(&parsed);
        return MARMOT_ERR_MLS;
    }

    char *inner_json = NULL;
    if (used_mls) {
        inner_json = malloc(inner_plaintext_len + 1);
        if (!inner_json) {
            free(inner_plaintext);
            free(decrypted);
            mls_group_free(&mls_group);
            state_undo_apply(m, &group->mls_group_id, &undo);   /* a late step */
            marmot_group_free(group);
            parsed_group_event_clear(&parsed);
            return MARMOT_ERR_MEMORY;
        }
        memcpy(inner_json, inner_plaintext, inner_plaintext_len);
        inner_json[inner_plaintext_len] = '\0';
        free(inner_plaintext);
        free(decrypted);

        /* The author is the sender (nostrc-we6g): the inner event's pubkey
         * must be the account the MLS sender leaf's credential binds (whose
         * signature the PrivateMessage carries).  Otherwise it is dropped,
         * and nothing -- not even the ratchet step -- is kept. */
        MarmotError aerr = have_identity
            ? check_inner_author(inner_json, sender_identity, inner_id)
            : MARMOT_ERR_AUTHOR_MISMATCH;
        /* The same inner event already delivered under another envelope
         * (a replay, or a sender's re-send) is a duplicate. */
        if (aerr == MARMOT_OK && m->storage->is_message_processed) {
            bool seen = false;
            if (m->storage->is_message_processed(m->storage->ctx, inner_id, &seen) ==
                    MARMOT_OK && seen)
                duplicate = true;
        }
        if (aerr != MARMOT_OK) {
            free(inner_json);
            mls_group_free(&mls_group);
            state_undo_apply(m, &group->mls_group_id, &undo);   /* a late step */
            marmot_group_free(group);
            parsed_group_event_clear(&parsed);
            return aerr;
        }
    } else {
        /* Explicit legacy mode: accept raw inner-event JSON directly from the
         * NIP-44 layer. Non-JSON payloads are not silently accepted. */
        if (decrypted_len == 0 ||
            (decrypted[0] != '{' && decrypted[0] != '[')) {
            free(decrypted);
            if (mls_loaded) mls_group_free(&mls_group);
            marmot_group_free(group);
            parsed_group_event_clear(&parsed);
            return MARMOT_ERR_CRYPTO;
        }

        inner_json = malloc(decrypted_len + 1);
        if (!inner_json) {
            free(decrypted);
            if (mls_loaded) mls_group_free(&mls_group);
            marmot_group_free(group);
            parsed_group_event_clear(&parsed);
            return MARMOT_ERR_MEMORY;
        }
        memcpy(inner_json, decrypted, decrypted_len);
        inner_json[decrypted_len] = '\0';
        free(decrypted);
    }

    /* Persist updated MLS state (generation counter advances on decrypt).
     * Fail closed: a message whose ratchet step is not stored is not
     * delivered (the transaction rolls everything back, and the event can be
     * processed again). */
    if (used_mls && !late)
        state_undo_capture(m, &group->mls_group_id, "mls_group", &undo);
    if (used_mls && !late && msg_save_mls_group(m, &mls_group) != 0) {
        state_undo_clear(&undo);
        mls_group_free(&mls_group);
        free(inner_json);
        marmot_group_free(group);
        parsed_group_event_clear(&parsed);
        return MARMOT_ERR_STORAGE;
    }
    /* nostrc-yuj2: the sender is now known to be at this epoch; the retained
     * parent may no longer need its full state (see commits.c). */
    if (used_mls && !late && !duplicate) {
        MarmotError werr = marmot_commit_note_witness(m, &mls_group, live_sender,
                                                      &parent_undo.blob, &parent_undo.len);
        if (werr != MARMOT_OK) {
            state_undo_apply(m, &group->mls_group_id, &undo);
            mls_group_free(&mls_group);
            free(inner_json);
            marmot_group_free(group);
            parsed_group_event_clear(&parsed);
            return werr;
        }
        if (parent_undo.blob) parent_undo.label = MARMOT_MLS_PARENT_LABEL;
    }
    if (mls_loaded) {
        mls_group_free(&mls_group);
    }

    if (duplicate) {
        /* Its key is used up (stored above); mark this envelope too. */
        MarmotError derr = MARMOT_OK;
        uint8_t outer_id[32];
        if (parsed.event_id && m->storage->save_processed_message &&
            marmot_hex_decode(parsed.event_id, outer_id, 32) == 0)
            derr = m->storage->save_processed_message(m->storage->ctx, outer_id, outer_id,
                                                      marmot_now(), used_epoch,
                                                      &group->mls_group_id,
                                                      MARMOT_MSG_STATE_PROCESSED, NULL);
        if (derr != MARMOT_OK)
            state_undo_apply(m, &group->mls_group_id, &undo);
        else
            state_undo_clear(&undo);
        free(inner_json);
        marmot_group_free(group);
        parsed_group_event_clear(&parsed);
        if (derr != MARMOT_OK) return derr;
        result->type = MARMOT_RESULT_OWN_MESSAGE;
        return MARMOT_OK;
    }

    /* ── 7. Populate result ───────────────────────────────────────────── */
    /*
     * MLS content_type distinguishes:
     *   1 = application message
     *   2 = proposal
     *   3 = commit
     *
     * Application messages are the common case here. Commits/proposals
     * use separate evolution events (marmot_create_group / marmot_add_members).
     */
    result->type = MARMOT_RESULT_APPLICATION_MESSAGE;
    result->app_msg.inner_event_json = inner_json;

    /* Extract sender pubkey from the inner event */
    NostrEvent inner_event;
    memset(&inner_event, 0, sizeof(inner_event));
    if (nostr_event_deserialize_compact(&inner_event, inner_json, NULL)) {
        if (inner_event.pubkey) {
            result->app_msg.sender_pubkey_hex = strdup(inner_event.pubkey);
        }
        free_stack_event(&inner_event);
    }

    /* ── 8. Store the decrypted message ───────────────────────────────── */
    MarmotMessage *msg = marmot_message_new();
    if (msg) {
        /* Event ID from the outer kind:445 event */
        if (parsed.event_id) {
            marmot_hex_decode(parsed.event_id, msg->id, 32);
        }

        /* Sender pubkey from inner event */
        if (result->app_msg.sender_pubkey_hex) {
            marmot_hex_decode(result->app_msg.sender_pubkey_hex,
                              msg->pubkey, 32);
        }

        /* Try to extract inner event kind */
        msg->kind = 9; /* default: chat (kind:9) per MIP-03 */
        /* inner_event was already freed, re-parse just for kind is wasteful.
         * We already have the inner_json — parse minimally. */
        {
            NostrEvent tmp;
            memset(&tmp, 0, sizeof(tmp));
            if (nostr_event_deserialize_compact(&tmp, inner_json, NULL)) {
                msg->kind = (uint32_t)tmp.kind;
                free_stack_event(&tmp);
            }
        }

        msg->mls_group_id = marmot_group_id_new(
            group->mls_group_id.data, group->mls_group_id.len);
        msg->created_at = parsed.created_at;
        msg->processed_at = marmot_now();
        msg->content = strdup(inner_json);
        msg->event_json = strdup(group_event_json);
        if (parsed.event_id)
            memcpy(msg->wrapper_event_id, msg->id, 32); /* outer kind:445 id at this layer */
        msg->epoch = used_epoch;
        msg->state = MARMOT_MSG_STATE_PROCESSED;

        err = m->storage->save_message(m->storage->ctx, msg);
        if (err == MARMOT_OK && parsed.event_id && m->storage->save_processed_message) {
            err = m->storage->save_processed_message(m->storage->ctx,
                                                     msg->wrapper_event_id,
                                                     msg->id,
                                                     msg->processed_at,
                                                     used_epoch,
                                                     &msg->mls_group_id,
                                                     MARMOT_MSG_STATE_PROCESSED,
                                                     NULL);
        }
        /* ... and under its inner event's id (duplicates, since 0.9.0). */
        if (err == MARMOT_OK && used_mls && m->storage->save_processed_message)
            err = m->storage->save_processed_message(m->storage->ctx, inner_id, inner_id,
                                                     msg->processed_at, used_epoch,
                                                     &msg->mls_group_id,
                                                     MARMOT_MSG_STATE_PROCESSED, NULL);
        marmot_message_free(msg);
        if (err != MARMOT_OK) {
            state_undo_apply(m, &group->mls_group_id, &parent_undo);
            state_undo_apply(m, &group->mls_group_id, &undo);
            marmot_message_result_free(result);
            marmot_group_free(group);
            parsed_group_event_clear(&parsed);
            return err;
        }
    }

    /* ── 9. Update group's last message metadata ──────────────────────── */
    group->last_message_at = parsed.created_at;
    group->last_message_processed_at = marmot_now();
    if (parsed.event_id) {
        free(group->last_message_id);
        group->last_message_id = strdup(parsed.event_id);
    }
    err = m->storage->save_group(m->storage->ctx, group);
    if (err != MARMOT_OK) {
        state_undo_apply(m, &group->mls_group_id, &parent_undo);
        state_undo_apply(m, &group->mls_group_id, &undo);
        marmot_message_result_free(result);
        marmot_group_free(group);
        parsed_group_event_clear(&parsed);
        return err;
    }

    state_undo_clear(&parent_undo);
    state_undo_clear(&undo);
    marmot_group_free(group);
    parsed_group_event_clear(&parsed);
    return MARMOT_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: one storage transaction per operation (nostrc-qp24.7)
 * ──────────────────────────────────────────────────────────────────────── */

/* A received event: its ratchet step, processed marker, message and group
 * record (or a whole epoch transition) land together or not at all. */
static MarmotError
process_group_event_txn(Marmot *m, const char *json, GroupEventOrigin origin,
                        MarmotMessageResult *result)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = process_group_event(m, json, origin, result);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK) marmot_message_result_free(result);
    return end;
}

MarmotError
marmot_process_message(Marmot *m,
                        const char *group_event_json,
                        MarmotMessageResult *result)
{
    return process_group_event_txn(m, group_event_json, GROUP_EVENT_SIGNED, result);
}

MarmotError
marmot_process_rumor_message(Marmot *m,
                              const char *rumor_json,
                              MarmotMessageResult *result)
{
    return process_group_event_txn(m, rumor_json, GROUP_EVENT_RUMOR, result);
}

/* The sender's ratchet step is stored before the event is returned: a
 * rolled-back step would reuse a key. */
MarmotError
marmot_create_message(Marmot *m,
                       const MarmotGroupId *mls_group_id,
                       const char *inner_event_json,
                       MarmotOutgoingMessage *result)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = create_message_impl(m, mls_group_id, inner_event_json, result);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK) marmot_outgoing_message_free(result);
    return end;
}

MarmotError
marmot_save_created_message(Marmot *m,
                             const MarmotGroupId *mls_group_id,
                             const char *signed_group_event_json,
                             const char *inner_event_json)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    return marmot_txn_end(m, save_created_message_impl(m, mls_group_id,
                                                       signed_group_event_json,
                                                       inner_event_json));
}
