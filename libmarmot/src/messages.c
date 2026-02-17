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
 * Note: Full MLS PrivateMessage framing (mls_group_encrypt/decrypt) is
 * deferred until MLS group state persistence is implemented. Currently
 * the NIP-44 layer with exporter_secret provides the encryption.
 *
 * SPDX-License-Identifier: MIT
 */

#include "marmot-internal.h"
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

static MarmotError
parse_group_event(const char *event_json, ParsedGroupEvent *out)
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

    /* Transfer ownership of fields we need */
    out->content = event.content;     event.content = NULL;
    out->event_id = event.id;         event.id = NULL;
    out->pubkey = event.pubkey;       event.pubkey = NULL;
    out->created_at = event.created_at;

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

    /* Free remaining event fields */
    free(event.sig);
    nostr_tags_free(event.tags);

    if (!out->has_group_id) {
        parsed_group_event_clear(out);
        return MARMOT_ERR_MISSING_GROUP_ID_TAG;
    }

    return MARMOT_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API: marmot_create_message
 * ══════════════════════════════════════════════════════════════════════════ */

MarmotError
marmot_create_message(Marmot *m,
                       const MarmotGroupId *mls_group_id,
                       const char *inner_event_json,
                       MarmotOutgoingMessage *result)
{
    if (!m || !mls_group_id || !inner_event_json || !result)
        return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->find_group_by_mls_id || !m->storage->get_exporter_secret)
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

    /* ── 3. Encrypt inner event with NIP-44 ───────────────────────────── */
    /*
     * Per MIP-03, the inner event JSON is:
     *   - Serialized as MLS application data (PrivateMessage)
     *   - Then NIP-44-encrypted with the exporter_secret-derived convkey
     *
     * Full MLS framing (mls_group_encrypt) requires a live MlsGroup in
     * memory, which requires MLS group state serialization/deserialization
     * (Phase 4 work). For Phase 3, we encrypt the inner event JSON
     * directly with NIP-44, which is the outer encryption layer.
     *
     * TODO: Add MLS PrivateMessage framing when group persistence lands.
     */
    const uint8_t *plaintext = (const uint8_t *)inner_event_json;
    size_t plaintext_len = strlen(inner_event_json);

    char *nip44_ciphertext = NULL;
    if (nip44_encrypt_with_secret(exporter_secret, plaintext, plaintext_len,
                                   &nip44_ciphertext) != 0) {
        sodium_memzero(exporter_secret, sizeof(exporter_secret));
        marmot_group_free(group);
        return MARMOT_ERR_NIP44;
    }
    sodium_memzero(exporter_secret, sizeof(exporter_secret));

    /* ── 4. Build kind:445 event ──────────────────────────────────────── */
    /*
     * Per MIP-03: use a completely separate ephemeral keypair for pubkey.
     * The caller must sign the event with this ephemeral key.
     * We leave pubkey unset — the caller fills it in when signing.
     */
    NostrEvent *event = nostr_event_new();
    if (!event) {
        free(nip44_ciphertext);
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

    /* Serialize the unsigned event */
    result->event_json = nostr_event_serialize_compact(event);
    nostr_event_free(event);

    if (!result->event_json) {
        marmot_group_free(group);
        return MARMOT_ERR_EVENT_BUILD;
    }

    /* ── 5. Create stored message record ──────────────────────────────── */
    result->message = marmot_message_new();
    if (result->message) {
        result->message->kind = MARMOT_KIND_GROUP_MESSAGE;
        result->message->created_at = marmot_now();
        result->message->processed_at = marmot_now();
        result->message->mls_group_id = marmot_group_id_new(
            mls_group_id->data, mls_group_id->len);
        result->message->content = strdup(inner_event_json);
        result->message->event_json = strdup(inner_event_json);
        result->message->epoch = group->epoch;
        result->message->state = MARMOT_MSG_STATE_CREATED;

        /* Generate a random message ID for tracking */
        randombytes_buf(result->message->id, 32);

        /* Persist the message */
        if (m->storage->save_message) {
            m->storage->save_message(m->storage->ctx, result->message);
        }
    }

    /* ── 6. Update group's last message metadata ──────────────────────── */
    group->last_message_at = marmot_now();
    if (m->storage->save_group) {
        m->storage->save_group(m->storage->ctx, group);
    }

    marmot_group_free(group);
    return MARMOT_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API: marmot_process_message
 * ══════════════════════════════════════════════════════════════════════════ */

MarmotError
marmot_process_message(Marmot *m,
                        const char *group_event_json,
                        MarmotMessageResult *result)
{
    if (!m || !group_event_json || !result)
        return MARMOT_ERR_INVALID_ARG;
    if (!m->storage)
        return MARMOT_ERR_STORAGE;

    memset(result, 0, sizeof(*result));

    /* ── 1. Parse the kind:445 event ──────────────────────────────────── */
    ParsedGroupEvent parsed;
    MarmotError err = parse_group_event(group_event_json, &parsed);
    if (err != MARMOT_OK)
        return err;

    /* ── 2. Find the group by nostr_group_id ──────────────────────────── */
    MarmotGroup *group = NULL;
    if (m->storage->find_group_by_nostr_id) {
        err = m->storage->find_group_by_nostr_id(m->storage->ctx,
                                                   parsed.nostr_group_id,
                                                   &group);
    }
    if (err != MARMOT_OK || !group) {
        parsed_group_event_clear(&parsed);
        return MARMOT_ERR_GROUP_NOT_FOUND;
    }

    if (group->state != MARMOT_GROUP_STATE_ACTIVE) {
        marmot_group_free(group);
        parsed_group_event_clear(&parsed);
        return MARMOT_ERR_USE_AFTER_EVICTION;
    }

    /* ── 3. Idempotency: check if already processed ───────────────────── */
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

    /* ── 4. Get exporter_secret (try current epoch, then recent ones) ── */
    uint8_t exporter_secret[32];
    bool found_secret = false;
    uint64_t used_epoch = group->epoch;

    /* Try current epoch first */
    if (m->storage->get_exporter_secret(m->storage->ctx,
                                         &group->mls_group_id,
                                         group->epoch,
                                         exporter_secret) == MARMOT_OK) {
        found_secret = true;
    }

    /* Fall back to recent epochs for out-of-order delivery */
    if (!found_secret && group->epoch > 0) {
        uint64_t min_epoch = (group->epoch > MAX_EPOCH_LOOKBACK) ? group->epoch - MAX_EPOCH_LOOKBACK : 0;
        for (uint64_t ep = group->epoch - 1; ep >= min_epoch && ep < group->epoch; ep--) {
            if (m->storage->get_exporter_secret(m->storage->ctx,
                                                 &group->mls_group_id,
                                                 ep,
                                                 exporter_secret) == MARMOT_OK) {
                found_secret = true;
                used_epoch = ep;
                break;
            }
        }
    }

    if (!found_secret) {
        marmot_group_free(group);
        parsed_group_event_clear(&parsed);
        return MARMOT_ERR_GROUP_EXPORTER_SECRET;
    }

    /* ── 5. NIP-44 decrypt the content ────────────────────────────────── */
    uint8_t *decrypted = NULL;
    size_t decrypted_len = 0;

    if (nip44_decrypt_with_secret(exporter_secret, parsed.content,
                                   &decrypted, &decrypted_len) != 0) {
        sodium_memzero(exporter_secret, sizeof(exporter_secret));
        marmot_group_free(group);
        parsed_group_event_clear(&parsed);
        return MARMOT_ERR_NIP44;
    }
    sodium_memzero(exporter_secret, sizeof(exporter_secret));

    /* ── 6. Extract inner event JSON ──────────────────────────────────── */
    /*
     * The decrypted content is the inner event JSON (unsigned Nostr event).
     * When full MLS framing is enabled, this would first go through
     * mls_group_decrypt() to unwrap the PrivateMessage.
     */
    char *inner_json = malloc(decrypted_len + 1);
    if (!inner_json) {
        free(decrypted);
        marmot_group_free(group);
        parsed_group_event_clear(&parsed);
        return MARMOT_ERR_MEMORY;
    }
    memcpy(inner_json, decrypted, decrypted_len);
    inner_json[decrypted_len] = '\0';
    free(decrypted);

    /* ── 7. Populate result ───────────────────────────────────────────── */
    /*
     * In the full implementation, the MLS content_type distinguishes:
     *   1 = application message
     *   2 = proposal
     *   3 = commit
     *
     * For Phase 3 without MLS framing, we treat everything as
     * application messages. Commits/proposals will use separate events
     * (the evolution event from marmot_create_group / marmot_add_members).
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
        msg->event_json = strdup(inner_json);
        msg->epoch = used_epoch;
        msg->state = MARMOT_MSG_STATE_PROCESSED;

        if (m->storage->save_message) {
            m->storage->save_message(m->storage->ctx, msg);
        }
        marmot_message_free(msg);
    }

    /* ── 9. Update group's last message metadata ──────────────────────── */
    group->last_message_at = parsed.created_at;
    group->last_message_processed_at = marmot_now();
    if (parsed.event_id) {
        free(group->last_message_id);
        group->last_message_id = strdup(parsed.event_id);
    }
    if (m->storage->save_group) {
        m->storage->save_group(m->storage->ctx, group);
    }

    marmot_group_free(group);
    parsed_group_event_clear(&parsed);
    return MARMOT_OK;
}
