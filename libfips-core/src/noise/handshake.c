#include <fips/noise/handshake.h>
#include <fips/noise/ecdh.h>
#include <fips/proto/framing.h>
#include "../identity/internal.h"

_Static_assert(FIPS_NOISE_TAG_SIZE == FIPS_FRAMING_NOISE_TAG_SIZE, "Noise tag framing");
_Static_assert(FIPS_NOISE_PUBKEY_SIZE == FIPS_FRAMING_NOISE_PUBKEY_SIZE, "Noise key framing");
_Static_assert(FIPS_NOISE_EPOCH_SIZE == FIPS_FRAMING_NOISE_EPOCH_SIZE, "Noise epoch framing");
_Static_assert(FIPS_NOISE_EPOCH_ENCRYPTED_SIZE == FIPS_FRAMING_NOISE_EPOCH_ENCRYPTED_SIZE, "Noise encrypted epoch framing");
_Static_assert(FIPS_NOISE_HANDSHAKE_MSG1_SIZE == FIPS_FRAMING_NOISE_HANDSHAKE_MSG1_SIZE, "FMP msg1 framing");
_Static_assert(FIPS_NOISE_HANDSHAKE_MSG2_SIZE == FIPS_FRAMING_NOISE_HANDSHAKE_MSG2_SIZE, "FMP msg2 framing");
_Static_assert(FIPS_NOISE_XK_HANDSHAKE_MSG1_SIZE == FIPS_FRAMING_NOISE_PUBKEY_SIZE, "XK msg1 framing");
_Static_assert(FIPS_NOISE_XK_HANDSHAKE_MSG2_SIZE == FIPS_FRAMING_NOISE_HANDSHAKE_MSG2_SIZE, "XK msg2 framing");
_Static_assert(FIPS_NOISE_XK_HANDSHAKE_MSG3_SIZE == FIPS_FRAMING_NOISE_PUBKEY_SIZE + FIPS_FRAMING_NOISE_TAG_SIZE + FIPS_FRAMING_NOISE_EPOCH_ENCRYPTED_SIZE, "XK msg3 framing");

static bool valid_public(const fips_secp *ctx, const uint8_t bytes[33])
{
    secp256k1_pubkey parsed;
    return secp256k1_ec_pubkey_parse(ctx->ctx, &parsed, bytes, 33) != 0;
}

static fips_noise_status construct(fips_handshake_state *out, const fips_secp *ctx,
    const fips_identity *key, const uint8_t *remote, const uint8_t epoch[8],
    fips_entropy entropy, fips_noise_pattern pattern, fips_handshake_role role)
{
    if (key->secp != ctx) return FIPS_NOISE_WrongState;
    if (remote && !valid_public(ctx, remote)) return FIPS_NOISE_InvalidPublicKey;
    fips_handshake_state s;
    memset(&s, 0, sizeof(s));
    s.pattern = pattern;
    s.role = role;
    s.progress = FIPS_HANDSHAKE_INITIAL;
    s.secp = ctx;
    s.static_keypair = key;
    s.entropy = entropy;
    memcpy(s.local_epoch, epoch, 8);
    uint8_t premessage[33];
    if (remote) {
        memcpy(s.remote_static, remote, 33);
        s.has_remote_static = true;
        memcpy(premessage, remote, 33);
    } else {
        fips_identity_pubkey_full(key, premessage);
    }
    const char *name = pattern == FIPS_NOISE_IK ? FIPS_NOISE_PROTOCOL_NAME_IK : FIPS_NOISE_PROTOCOL_NAME_XK;
    fips_symmetric_init(&s.symmetric, (const uint8_t *)name, FIPS_NOISE_PROTOCOL_NAME_SIZE);
    premessage[0] = 0x02; /* Nostr x-only <- s; only the pre-message is normalized. */
    fips_symmetric_mix_hash(&s.symmetric, premessage, sizeof(premessage));
    memcpy(out, &s, sizeof(s));
    fips_handshake_destroy(&s);
    return FIPS_NOISE_OK;
}
fips_noise_status fips_handshake_new_initiator(fips_handshake_state *out,
    const fips_secp *ctx, const fips_identity *key, const uint8_t remote[33],
    const uint8_t epoch[8], fips_entropy entropy)
{ return construct(out, ctx, key, remote, epoch, entropy, FIPS_NOISE_IK, FIPS_HANDSHAKE_INITIATOR); }
fips_noise_status fips_handshake_new_responder(fips_handshake_state *out,
    const fips_secp *ctx, const fips_identity *key, const uint8_t epoch[8], fips_entropy entropy)
{ return construct(out, ctx, key, NULL, epoch, entropy, FIPS_NOISE_IK, FIPS_HANDSHAKE_RESPONDER); }
fips_noise_status fips_handshake_new_xk_initiator(fips_handshake_state *out,
    const fips_secp *ctx, const fips_identity *key, const uint8_t remote[33],
    const uint8_t epoch[8], fips_entropy entropy)
{ return construct(out, ctx, key, remote, epoch, entropy, FIPS_NOISE_XK, FIPS_HANDSHAKE_INITIATOR); }
fips_noise_status fips_handshake_new_xk_responder(fips_handshake_state *out,
    const fips_secp *ctx, const fips_identity *key, const uint8_t epoch[8], fips_entropy entropy)
{ return construct(out, ctx, key, NULL, epoch, entropy, FIPS_NOISE_XK, FIPS_HANDSHAKE_RESPONDER); }

static fips_noise_status symmetric_status(fips_symmetric_status status, bool decrypt)
{
    switch (status) {
    case FIPS_SYMMETRIC_OK: return FIPS_NOISE_OK;
    case FIPS_SYMMETRIC_ERR_AUTH: return FIPS_NOISE_DecryptionFailed;
    case FIPS_SYMMETRIC_ERR_NONCE_OVERFLOW: return FIPS_NOISE_NonceOverflow;
    case FIPS_SYMMETRIC_ERR_RANGE: return decrypt ? FIPS_NOISE_MessageTooShort : FIPS_NOISE_MessageTooLarge;
    case FIPS_SYMMETRIC_ERR_CRYPTO: return decrypt ? FIPS_NOISE_DecryptionFailed : FIPS_NOISE_EncryptionFailed;
    }
    return FIPS_NOISE_EncryptionFailed;
}
static fips_noise_status ready(const fips_handshake_state *s, fips_noise_pattern pattern,
    fips_handshake_role role, fips_handshake_progress progress)
{
    return s->secp && s->static_keypair && s->pattern == pattern && s->role == role &&
        s->progress == progress ? FIPS_NOISE_OK : FIPS_NOISE_WrongState;
}
static fips_noise_status generate_ephemeral(fips_handshake_state *s)
{
    secp256k1_pubkey pub;
    size_t len = 33;
    s->entropy.fill(s->entropy.ctx, s->ephemeral_secret, 32);
    if (!secp256k1_ec_pubkey_create(s->secp->ctx, &pub, s->ephemeral_secret)) {
        sodium_memzero(s->ephemeral_secret, 32);
        return FIPS_NOISE_Secp256k1;
    }
    secp256k1_ec_pubkey_serialize(s->secp->ctx, s->ephemeral_public, &len, &pub, SECP256K1_EC_COMPRESSED);
    s->has_ephemeral = true;
    return FIPS_NOISE_OK;
}
/* Export a static scalar only for the duration of one DH. The other path
 * borrows the owned ephemeral scalar. Wipe the DH output even if MixKey fails. */
static fips_noise_status mix_dh(fips_handshake_state *s, bool ephemeral,
                              const uint8_t remote[33], bool cache_es)
{
    uint8_t secret[32] = {0}, dh[32] = {0};
    fips_noise_status rc = FIPS_NOISE_Secp256k1;
    if (!ephemeral) fips_identity_export_secret(s->static_keypair, secret);
    if (fips_noise_ecdh(s->secp, ephemeral ? s->ephemeral_secret : secret, remote, dh) == FIPS_IDENTITY_OK) {
        rc = symmetric_status(fips_symmetric_mix_key(&s->symmetric, dh, 32), false);
        if (rc == FIPS_NOISE_OK && cache_es) {
            memcpy(s->es, dh, 32);
            s->has_es = true;
        }
    }
    sodium_memzero(secret, sizeof(secret));
    sodium_memzero(dh, sizeof(dh));
    return rc;
}
static fips_noise_status read_ephemeral(fips_handshake_state *s, const uint8_t *input)
{
    if (!valid_public(s->secp, input)) return FIPS_NOISE_InvalidPublicKey;
    memcpy(s->remote_ephemeral, input, 33);
    s->has_remote_ephemeral = true;
    fips_symmetric_mix_hash(&s->symmetric, input, 33);
    return FIPS_NOISE_OK;
}
static fips_noise_status write_static(fips_handshake_state *s, uint8_t *out)
{
    uint8_t pub[33];
    size_t len;
    fips_identity_pubkey_full(s->static_keypair, pub);
    fips_noise_status rc = symmetric_status(fips_symmetric_encrypt_and_hash(
        &s->symmetric, pub, sizeof(pub), out, 49, &len), false);
    sodium_memzero(pub, sizeof(pub));
    return rc;
}
static fips_noise_status read_static(fips_handshake_state *s, const uint8_t *input)
{
    uint8_t pub[33] = {0};
    size_t len;
    fips_noise_status rc = symmetric_status(fips_symmetric_decrypt_and_hash(
        &s->symmetric, input, 49, pub, sizeof(pub), &len), true);
    if (rc == FIPS_NOISE_OK) {
        if (!valid_public(s->secp, pub)) rc = FIPS_NOISE_InvalidPublicKey;
        else {
            memcpy(s->remote_static, pub, 33);
            s->has_remote_static = true;
        }
    }
    sodium_memzero(pub, sizeof(pub));
    return rc;
}
static fips_noise_status write_epoch(fips_handshake_state *s, uint8_t *out)
{
    size_t len;
    return symmetric_status(fips_symmetric_encrypt_and_hash(&s->symmetric,
        s->local_epoch, 8, out, 24, &len), false);
}
static fips_noise_status read_epoch(fips_handshake_state *s, const uint8_t *input)
{
    uint8_t epoch[8] = {0};
    size_t len;
    fips_noise_status rc = symmetric_status(fips_symmetric_decrypt_and_hash(
        &s->symmetric, input, 24, epoch, sizeof(epoch), &len), true);
    if (rc == FIPS_NOISE_OK) {
        memcpy(s->remote_epoch, epoch, 8);
        s->has_remote_epoch = true;
    }
    sodium_memzero(epoch, sizeof(epoch));
    return rc;
}

#define TRY(call) do { rc = (call); if (rc != FIPS_NOISE_OK) goto done; } while (0)
static fips_noise_status write_1(fips_handshake_state *s, fips_noise_pattern pattern,
                               uint8_t *out, size_t cap, size_t *len)
{
    fips_noise_status rc = ready(s, pattern, FIPS_HANDSHAKE_INITIATOR, FIPS_HANDSHAKE_INITIAL);
    if (rc != FIPS_NOISE_OK || !s->has_remote_static) return FIPS_NOISE_WrongState;
    size_t n = pattern == FIPS_NOISE_IK ? FIPS_NOISE_HANDSHAKE_MSG1_SIZE : FIPS_NOISE_XK_HANDSHAKE_MSG1_SIZE;
    if (cap < n) return FIPS_NOISE_MessageTooLarge;
    uint8_t message[FIPS_NOISE_HANDSHAKE_MSG1_SIZE] = {0};
    TRY(generate_ephemeral(s));
    memcpy(message, s->ephemeral_public, 33);
    fips_symmetric_mix_hash(&s->symmetric, message, 33);
    TRY(mix_dh(s, true, s->remote_static, pattern == FIPS_NOISE_IK));
    if (pattern == FIPS_NOISE_IK) {
        TRY(write_static(s, message + 33));
        TRY(mix_dh(s, false, s->remote_static, false));
        TRY(write_epoch(s, message + 82));
    }
    s->progress = FIPS_HANDSHAKE_MESSAGE1_DONE;
    memcpy(out, message, n);
    *len = n;
done:
    sodium_memzero(message, sizeof(message));
    return rc;
}
static fips_noise_status read_1(fips_handshake_state *s, fips_noise_pattern pattern,
                              const uint8_t *input, size_t len)
{
    fips_noise_status rc = ready(s, pattern, FIPS_HANDSHAKE_RESPONDER, FIPS_HANDSHAKE_INITIAL);
    if (rc != FIPS_NOISE_OK) return rc;
    size_t n = pattern == FIPS_NOISE_IK ? FIPS_NOISE_HANDSHAKE_MSG1_SIZE : FIPS_NOISE_XK_HANDSHAKE_MSG1_SIZE;
    if (len != n) return FIPS_NOISE_MessageTooShort;
    TRY(read_ephemeral(s, input));
    TRY(mix_dh(s, false, s->remote_ephemeral, false));
    if (pattern == FIPS_NOISE_IK) {
        TRY(read_static(s, input + 33));
        TRY(mix_dh(s, false, s->remote_static, false));
        TRY(read_epoch(s, input + 82));
    }
    s->progress = FIPS_HANDSHAKE_MESSAGE1_DONE;
done:
    return rc;
}
static fips_noise_status write_2(fips_handshake_state *s, fips_noise_pattern pattern,
                               uint8_t *out, size_t cap, size_t *len)
{
    fips_noise_status rc = ready(s, pattern, FIPS_HANDSHAKE_RESPONDER, FIPS_HANDSHAKE_MESSAGE1_DONE);
    if (rc != FIPS_NOISE_OK || !s->has_remote_ephemeral) return FIPS_NOISE_WrongState;
    if (cap < FIPS_NOISE_HANDSHAKE_MSG2_SIZE) return FIPS_NOISE_MessageTooLarge;
    uint8_t message[FIPS_NOISE_HANDSHAKE_MSG2_SIZE] = {0};
    TRY(generate_ephemeral(s));
    memcpy(message, s->ephemeral_public, 33);
    fips_symmetric_mix_hash(&s->symmetric, message, 33);
    TRY(mix_dh(s, true, s->remote_ephemeral, false));
    /* FMP v1 repeats msg1's es, NOT Noise IK's se. Wire compatibility. */
    if (pattern == FIPS_NOISE_IK) TRY(mix_dh(s, false, s->remote_ephemeral, false));
    TRY(write_epoch(s, message + 33));
    s->progress = pattern == FIPS_NOISE_IK ? FIPS_HANDSHAKE_COMPLETE : FIPS_HANDSHAKE_MESSAGE2_DONE;
    memcpy(out, message, sizeof(message));
    *len = sizeof(message);
done:
    sodium_memzero(message, sizeof(message));
    return rc;
}
static fips_noise_status read_2(fips_handshake_state *s, fips_noise_pattern pattern,
                              const uint8_t *input, size_t len)
{
    fips_noise_status rc = ready(s, pattern, FIPS_HANDSHAKE_INITIATOR, FIPS_HANDSHAKE_MESSAGE1_DONE);
    if (rc != FIPS_NOISE_OK || !s->has_ephemeral || (pattern == FIPS_NOISE_IK && !s->has_es))
        return FIPS_NOISE_WrongState;
    if (len != FIPS_NOISE_HANDSHAKE_MSG2_SIZE) return FIPS_NOISE_MessageTooShort;
    TRY(read_ephemeral(s, input));
    TRY(mix_dh(s, true, s->remote_ephemeral, false));
    /* Borrow the cache: a failed msg2 with a valid ephemeral costs just ee. */
    if (pattern == FIPS_NOISE_IK)
        TRY(symmetric_status(fips_symmetric_mix_key(&s->symmetric, s->es, 32), false));
    TRY(read_epoch(s, input + 33));
    s->progress = pattern == FIPS_NOISE_IK ? FIPS_HANDSHAKE_COMPLETE : FIPS_HANDSHAKE_MESSAGE2_DONE;
done:
    return rc;
}
fips_noise_status fips_handshake_write_xk_message_3(fips_handshake_state *s,
    uint8_t *out, size_t cap, size_t *len)
{
    fips_noise_status rc = ready(s, FIPS_NOISE_XK, FIPS_HANDSHAKE_INITIATOR, FIPS_HANDSHAKE_MESSAGE2_DONE);
    if (rc != FIPS_NOISE_OK || !s->has_remote_ephemeral) return FIPS_NOISE_WrongState;
    if (cap < FIPS_NOISE_XK_HANDSHAKE_MSG3_SIZE) return FIPS_NOISE_MessageTooLarge;
    uint8_t message[FIPS_NOISE_XK_HANDSHAKE_MSG3_SIZE] = {0};
    TRY(write_static(s, message));
    TRY(mix_dh(s, false, s->remote_ephemeral, false));
    TRY(write_epoch(s, message + 49));
    s->progress = FIPS_HANDSHAKE_COMPLETE;
    memcpy(out, message, sizeof(message));
    *len = sizeof(message);
done:
    sodium_memzero(message, sizeof(message));
    return rc;
}
fips_noise_status fips_handshake_read_xk_message_3(fips_handshake_state *s,
    const uint8_t *input, size_t len)
{
    fips_noise_status rc = ready(s, FIPS_NOISE_XK, FIPS_HANDSHAKE_RESPONDER, FIPS_HANDSHAKE_MESSAGE2_DONE);
    if (rc != FIPS_NOISE_OK || !s->has_ephemeral) return FIPS_NOISE_WrongState;
    if (len != FIPS_NOISE_XK_HANDSHAKE_MSG3_SIZE) return FIPS_NOISE_MessageTooShort;
    TRY(read_static(s, input));
    TRY(mix_dh(s, true, s->remote_static, false));
    TRY(read_epoch(s, input + 49));
    s->progress = FIPS_HANDSHAKE_COMPLETE;
done:
    return rc;
}
#undef TRY
#define MESSAGES(name, pattern, number) \
fips_noise_status fips_handshake_write_##name(fips_handshake_state *s, uint8_t *out, size_t cap, size_t *len) \
{ return write_##number(s, pattern, out, cap, len); } \
fips_noise_status fips_handshake_read_##name(fips_handshake_state *s, const uint8_t *input, size_t len) \
{ return read_##number(s, pattern, input, len); }
MESSAGES(message_1, FIPS_NOISE_IK, 1)
MESSAGES(message_2, FIPS_NOISE_IK, 2)
MESSAGES(xk_message_1, FIPS_NOISE_XK, 1)
MESSAGES(xk_message_2, FIPS_NOISE_XK, 2)
#undef MESSAGES

const uint8_t *fips_handshake_hash(const fips_handshake_state *s) { return s->symmetric.h; }
const char *fips_handshake_progress_name(fips_handshake_progress progress)
{
    switch (progress) {
    case FIPS_HANDSHAKE_INITIAL: return "Initial";
    case FIPS_HANDSHAKE_MESSAGE1_DONE: return "Message1Done";
    case FIPS_HANDSHAKE_MESSAGE2_DONE: return "Message2Done";
    case FIPS_HANDSHAKE_COMPLETE: return "Complete";
    }
    return "Invalid";
}
void fips_handshake_fork(const fips_handshake_state *src, fips_handshake_state *dst)
{
    if (src != dst) memcpy(dst, src, sizeof(*dst));
}
typedef fips_noise_status (*read_fn)(fips_handshake_state *, const uint8_t *, size_t);
static fips_noise_status fork_read(const fips_handshake_state *src, const uint8_t *input,
    size_t len, fips_handshake_state *dst, read_fn read)
{
    if (src == dst) return FIPS_NOISE_WrongState;
    fips_handshake_state copy;
    fips_handshake_fork(src, &copy);
    fips_noise_status rc = read(&copy, input, len);
    if (rc == FIPS_NOISE_OK) memcpy(dst, &copy, sizeof(copy));
    fips_handshake_destroy(&copy);
    return rc;
}
fips_noise_status fips_handshake_fork_msg2(const fips_handshake_state *src,
    const uint8_t *input, size_t len, fips_handshake_state *dst)
{ return fork_read(src, input, len, dst, fips_handshake_read_message_2); }
fips_noise_status fips_handshake_fork_xk2(const fips_handshake_state *src,
    const uint8_t *input, size_t len, fips_handshake_state *dst)
{ return fork_read(src, input, len, dst, fips_handshake_read_xk_message_2); }
fips_noise_status fips_handshake_fork_xk3(const fips_handshake_state *src,
    const uint8_t *input, size_t len, fips_handshake_state *dst)
{ return fork_read(src, input, len, dst, fips_handshake_read_xk_message_3); }
fips_noise_status fips_handshake_finish(fips_handshake_state *s, fips_aead_key *send,
    fips_aead_key *recv, uint8_t final_h[32], uint8_t remote_static[33], uint8_t remote_epoch[8])
{
    if (s->progress != FIPS_HANDSHAKE_COMPLETE) return FIPS_NOISE_HandshakeNotComplete;
    if (!s->has_remote_static || !s->has_remote_epoch || send == recv ||
        (s->role != FIPS_HANDSHAKE_INITIATOR && s->role != FIPS_HANDSHAKE_RESPONDER))
        return FIPS_NOISE_WrongState;
    fips_aead_key k1 = {{0}}, k2 = {{0}};
    fips_noise_status rc = symmetric_status(fips_symmetric_split(&s->symmetric, &k1, &k2), false);
    if (rc == FIPS_NOISE_OK) {
        memcpy(send, s->role == FIPS_HANDSHAKE_INITIATOR ? &k1 : &k2, sizeof(*send));
        memcpy(recv, s->role == FIPS_HANDSHAKE_INITIATOR ? &k2 : &k1, sizeof(*recv));
        memcpy(final_h, s->symmetric.h, 32);
        memcpy(remote_static, s->remote_static, 33);
        memcpy(remote_epoch, s->remote_epoch, 8);
        fips_handshake_destroy(s);
    }
    fips_aead_key_destroy(&k1);
    fips_aead_key_destroy(&k2);
    return rc;
}
void fips_handshake_destroy(fips_handshake_state *s) { sodium_memzero(s, sizeof(*s)); }
