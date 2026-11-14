#include "internal.h"
#include <fips/identity/encoding.h>
#include <fips/identity/key.h>

static const char alphabet[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
static unsigned char fold(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a'-'A')) : c;
}
static int value(unsigned char c)
{
    c = fold(c);
    for (int i = 0; i < 32; ++i) if (c == (unsigned char)alphabet[i]) return i;
    return -1;
}
static uint32_t step(uint32_t chk, uint8_t v)
{
    static const uint32_t gen[5] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
    uint32_t top = chk >> 25;
    chk = ((chk & 0x1ffffff) << 5) ^ v;
    for (unsigned i = 0; i < 5; ++i) if ((top >> i) & 1) chk ^= gen[i];
    return chk;
}
static uint32_t hrp_checksum(const char *text, size_t len)
{
    uint32_t chk = 1;
    for (size_t i = 0; i < len; ++i) chk = step(chk, (uint8_t)(fold((unsigned char)text[i]) >> 5));
    chk = step(chk, 0);
    for (size_t i = 0; i < len; ++i) chk = step(chk, fold((unsigned char)text[i]) & 31);
    return chk;
}
static fips_identity_status encode(const char *hrp, const uint8_t bytes[32], char *out, size_t capacity)
{
    if (capacity < FIPS_NIP19_CAPACITY) return FIPS_IDENTITY_BufferTooSmall;
    struct { char text[64]; uint32_t acc; unsigned bits; } tmp = {{0}, 0, 0};
    memcpy(tmp.text, hrp, 4); tmp.text[4] = '1';
    uint32_t chk = hrp_checksum(hrp, 4);
    size_t n = 5;
    for (size_t i = 0; i < 32; ++i) {
        tmp.acc = ((tmp.acc << 8) | bytes[i]) & 0xfff;
        tmp.bits += 8;
        while (tmp.bits >= 5) {
            tmp.bits -= 5;
            uint8_t v = (uint8_t)((tmp.acc >> tmp.bits) & 31);
            tmp.text[n++] = alphabet[v]; chk = step(chk, v);
        }
    }
    if (tmp.bits) {
        uint8_t v = (uint8_t)((tmp.acc << (5-tmp.bits)) & 31);
        tmp.text[n++] = alphabet[v]; chk = step(chk, v);
    }
    for (unsigned i = 0; i < 6; ++i) chk = step(chk, 0);
    chk ^= 1;
    for (unsigned i = 0; i < 6; ++i) tmp.text[n++] = alphabet[(chk >> (25-5*i)) & 31];
    tmp.text[n] = '\0';
    memcpy(out, tmp.text, sizeof(tmp.text));
    sodium_memzero(&tmp, sizeof(tmp));
    return FIPS_IDENTITY_OK;
}

/* Streaming validation bounds both work and memory, even for malformed input.
 * Recognize bech32m only to report upstream's NonCanonicalNpub discriminant. */
static fips_identity_status decode(const fips_secp *secp, const char *text, size_t len, bool secret, uint8_t out[32])
{
    if (len < 8 || len > 1023) return FIPS_IDENTITY_Bech32Decode;
    size_t sep = len;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c < 33 || c > 126) return FIPS_IDENTITY_Bech32Decode;
        if (c == '1') sep = i;
    }
    if (!sep || sep > 83 || sep == len || len-sep-1 < 6) return FIPS_IDENTITY_Bech32Decode;
    uint32_t chk = hrp_checksum(text, sep);
    for (size_t i = sep+1; i < len; ++i) {
        int v = value((unsigned char)text[i]);
        if (v < 0) return FIPS_IDENTITY_Bech32Decode;
        chk = step(chk, (uint8_t)v);
    }
    if (chk != 1 && chk != 0x2bc830a3) return FIPS_IDENTITY_Bech32Decode;
    const char *hrp = secret ? "nsec" : "npub";
    bool right_hrp = sep == 4;
    for (size_t i = 0; right_hrp && i < 4; ++i)
        right_hrp = fold((unsigned char)text[i]) == (unsigned char)hrp[i];
    if (!right_hrp) return secret ? FIPS_IDENTITY_InvalidNsecPrefix : FIPS_IDENTITY_InvalidNpubPrefix;
    size_t groups = len-sep-7;
    if (groups*5/8 != 32)
        return secret ? FIPS_IDENTITY_InvalidNsecLength : FIPS_IDENTITY_InvalidNpubLength;
    struct { uint8_t bytes[32]; uint32_t acc; unsigned bits; } tmp = {{0}, 0, 0};
    size_t n = 0;
    for (size_t i = sep+1; i < len-6; ++i) {
        tmp.acc = ((tmp.acc << 5) | (unsigned)value((unsigned char)text[i])) & 0xfff;
        tmp.bits += 5;
        if (tmp.bits >= 8) { tmp.bits -= 8; tmp.bytes[n++] = (uint8_t)(tmp.acc >> tmp.bits); }
    }
    fips_identity_status status = FIPS_IDENTITY_OK;
    if (secret) {
        if (!secp256k1_ec_seckey_verify(secp->ctx, tmp.bytes)) status = FIPS_IDENTITY_InvalidSecretKey;
    } else {
        secp256k1_xonly_pubkey key;
        if (!secp256k1_xonly_pubkey_parse(secp->ctx, &key, tmp.bytes)) status = FIPS_IDENTITY_InvalidSecretKey;
    }
    if (status == FIPS_IDENTITY_OK && (chk != 1 || groups != 52 || tmp.bits >= 5 ||
        (tmp.acc & ((1u << tmp.bits)-1))))
        status = secret ? FIPS_IDENTITY_NonCanonicalNsec : FIPS_IDENTITY_NonCanonicalNpub;
    if (status == FIPS_IDENTITY_OK) memcpy(out, tmp.bytes, 32);
    sodium_memzero(&tmp, sizeof(tmp));
    return status;
}
fips_identity_status fips_encode_npub(const fips_xonly_pubkey *key, char *out, size_t capacity)
{
    return encode("npub", key->bytes, out, capacity);
}
fips_identity_status fips_decode_npub(const fips_secp *secp, const char *text, size_t len, fips_xonly_pubkey *out)
{
    return decode(secp, text, len, false, out->bytes);
}
fips_identity_status fips_encode_nsec(const fips_secp *secp, const uint8_t secret[32], char *out, size_t capacity)
{
    if (!secp256k1_ec_seckey_verify(secp->ctx, secret)) return FIPS_IDENTITY_InvalidSecretKey;
    return encode("nsec", secret, out, capacity);
}
fips_identity_status fips_decode_nsec(const fips_secp *secp, const char *text, size_t len, uint8_t out[32])
{
    return decode(secp, text, len, true, out);
}
static int hex_value(unsigned char c)
{
    c = fold(c);
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    return -1;
}
fips_identity_status fips_decode_secret(const fips_secp *secp, const char *text, size_t len, uint8_t out[32])
{
    if (len >= 5 && fold((unsigned char)text[0]) == 'n' && fold((unsigned char)text[1]) == 's' &&
        fold((unsigned char)text[2]) == 'e' && fold((unsigned char)text[3]) == 'c' && text[4] == '1')
        return fips_decode_nsec(secp, text, len, out);
    if (len % 2) return FIPS_IDENTITY_InvalidHex;
    /* Match hex::decode's character validation before the key-length check. */
    for (size_t i = 0; i < len; ++i) if (hex_value((unsigned char)text[i]) < 0) return FIPS_IDENTITY_InvalidHex;
    if (len != 64) return FIPS_IDENTITY_InvalidNsecLength;
    uint8_t bytes[32];
    for (size_t i = 0; i < 32; ++i)
        bytes[i] = (uint8_t)(hex_value((unsigned char)text[2*i])*16 + hex_value((unsigned char)text[2*i+1]));
    fips_identity_status status = FIPS_IDENTITY_InvalidSecretKey;
    if (secp256k1_ec_seckey_verify(secp->ctx, bytes)) { memcpy(out, bytes, 32); status = FIPS_IDENTITY_OK; }
    sodium_memzero(bytes, sizeof(bytes));
    return status;
}
