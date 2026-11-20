#include <fips/identity/local.h>
#include <fips/identity/peer.h>
#include <fips/identity/key.h>
#include <fips/identity/address.h>
#include <fips/identity/node_addr.h>
#include <fips/identity/prefix.h>
#include <fips/identity/encoding.h>
#include <fips/identity/auth.h>
#include "fakes/host.h"
#include <sodium.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)
#define OK(expr) CHECK((expr) == FIPS_IDENTITY_OK)
typedef struct fixture {
    fips_secp *secp;
    fips_identity *id;
    fips_entropy entropy;
    fips_xonly_pubkey key;
    fips_node_addr addr;
    fips_ipv6_address address;
} fixture;
static const char secret_hex[] = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
static const char canonical[] = "npub1lycg5qvjtrp3qjf5f7zl382j9x6nrjz9sdhenvyxq8c3808qxmus6gq266";
static const char *const noncanonical[] = {
    "npub1lycg5qvjtrp3qjf5f7zl382j9x6nrjz9sdhenvyxq8c3808qxmus05sxlc",
    "npub1lycg5qvjtrp3qjf5f7zl382j9x6nrjz9sdhenvyxq8c3808qxmu3875l8g",
    "npub1lycg5qvjtrp3qjf5f7zl382j9x6nrjz9sdhenvyxq8c3808qxmulser6qq"
};
static void spelling(const char *text, char *out, unsigned mode)
{
    size_t i;
    for (i = 0; text[i]; ++i) {
        char c = text[i];
        out[i] = (mode == 1 || (mode == 2 && i%2 == 0) || (mode == 3 && i == 0)) && c >= 'a' && c <= 'z' ? (char)(c-'a'+'A') : c;
    }
    out[i] = '\0';
}
static int make_nsec(fixture *t, char out[64])
{
    uint8_t secret[32]; fips_identity_export_secret(t->id, secret);
    fips_identity_status status = fips_encode_nsec(t->secp, secret, out, 64);
    sodium_memzero(secret, sizeof(secret));
    OK(status); return 0;
}
static int check_auth(fixture *t, unsigned mutation)
{
    fips_fixed_entropy fixed = {7}; fips_entropy e = fips_fixed_entropy_adapter(&fixed);
    fips_auth_challenge c = fips_auth_challenge_generate(e);
    fips_auth_response response;
    OK(fips_identity_sign_challenge(t->id, &c, UINT64_C(1234567890), &response));
    fips_node_addr out = {{0x42}}, before = out;
    if (mutation == 1) c.bytes[0] ^= 1;
    if (mutation == 2) response.timestamp = UINT64_C(9999999999);
    if (mutation == 3) response.signature[32] ^= 1;
    if (mutation == 4) memset(response.pubkey.bytes, 0xff, 32);
    if (mutation == 5) response.pubkey.bytes[0] ^= 1;
    fips_identity_status status = fips_auth_challenge_verify(t->secp, &c, &response, &out);
    if (mutation) {
        CHECK(status == FIPS_IDENTITY_SignatureVerificationFailed);
        CHECK(memcmp(out.bytes, before.bytes, 16) == 0);
    } else { OK(status); CHECK(memcmp(out.bytes, t->addr.bytes, 16) == 0); }
    return 0;
}

static int test_node_addr_from_slice(fixture *t)
{
    (void)t;
    uint8_t b[16] = {0}; fips_node_addr out = {{42}}, before = out;
    CHECK(fips_node_addr_from_slice(b, 8, &out) == FIPS_IDENTITY_InvalidNodeAddrLength);
    CHECK(!memcmp(&out, &before, sizeof(out))); OK(fips_node_addr_from_slice(b, 16, &out)); CHECK(!memcmp(out.bytes, b, 16));
    return 0;
}

static int test_fips_address_validation(fixture *t)
{
    (void)t;
    uint8_t b[16] = {0xfd}; fips_ipv6_address out;
    OK(fips_ipv6_address_from_slice(b, 16, &out)); b[0] = 0xfe;
    CHECK(fips_ipv6_address_from_slice(b, 16, &out) == FIPS_IDENTITY_InvalidAddressPrefix);
    return 0;
}

static int test_decode_npub_invalid_prefix(fixture *t)
{
    (void)t;
    char text[64]; CHECK(make_nsec(t, text) == 0); fips_xonly_pubkey out;
    CHECK(fips_decode_npub(t->secp, text, 63, &out) == FIPS_IDENTITY_InvalidNpubPrefix); sodium_memzero(text, sizeof(text));
    return 0;
}

static int test_decode_npub_invalid_checksum(fixture *t)
{
    (void)t;
    char text[64]; memcpy(text, canonical, 64); text[62] = 'q'; fips_xonly_pubkey out;
    CHECK(fips_decode_npub(t->secp, text, 63, &out) == FIPS_IDENTITY_Bech32Decode);
    return 0;
}

static int test_peer_identity_from_invalid_npub(fixture *t)
{
    (void)t;
    fips_peer_identity peer;
    CHECK(fips_peer_identity_from_npub(t->secp, "npub1invalid", 12, &peer) != FIPS_IDENTITY_OK);
    char nsec[64]; CHECK(make_nsec(t, nsec) == 0);
    CHECK(fips_peer_identity_from_npub(t->secp, nsec, 63, &peer) == FIPS_IDENTITY_InvalidNpubPrefix); sodium_memzero(nsec, 64);
    return 0;
}

static int test_nsec_roundtrip(fixture *t)
{
    (void)t;
    char text[64]; uint8_t bytes[32], expected[32]; CHECK(make_nsec(t, text) == 0);
    CHECK(strlen(text) == 63 && !strncmp(text, "nsec1", 5));
    OK(fips_decode_nsec(t->secp, text, 63, bytes)); fips_identity_export_secret(t->id, expected);
    CHECK(!memcmp(bytes, expected, 32)); sodium_memzero(bytes, 32); sodium_memzero(expected, 32); sodium_memzero(text, 64);
    return 0;
}

static int test_decode_secret_nsec(fixture *t)
{
    (void)t;
    char text[64]; uint8_t bytes[32], expected[32]; CHECK(make_nsec(t, text) == 0);
    OK(fips_decode_secret(t->secp, text, 63, bytes)); fips_identity_export_secret(t->id, expected);
    CHECK(!memcmp(bytes, expected, 32)); sodium_memzero(bytes, 32); sodium_memzero(expected, 32); sodium_memzero(text, 64);
    return 0;
}

static int test_decode_secret_hex(fixture *t)
{
    (void)t;
    uint8_t bytes[32]; OK(fips_decode_secret(t->secp, secret_hex, 64, bytes));
    for (unsigned i = 0; i < 32; ++i) CHECK(bytes[i] == i+1);
    sodium_memzero(bytes, 32);
    return 0;
}

static int test_decode_npub_invalid_length(fixture *t)
{
    (void)t;
    fips_xonly_pubkey key; const char *text = "npub1qqqqqqqqqqqqqqqqqqqqqqqqqqk7h3rf";
    CHECK(fips_decode_npub(t->secp, text, strlen(text), &key) == FIPS_IDENTITY_InvalidNpubLength);
    return 0;
}

static int test_decode_nsec_invalid_length(fixture *t)
{
    (void)t;
    uint8_t key[32]; const char *text = "nsec1qqqqqqqqqqqqqqqqqqqqqqqqqqxacnkl";
    CHECK(fips_decode_nsec(t->secp, text, strlen(text), key) == FIPS_IDENTITY_InvalidNsecLength);
    return 0;
}

static int test_decode_secret_hex_wrong_length(fixture *t)
{
    (void)t;
    uint8_t bytes[32]; CHECK(fips_decode_secret(t->secp, secret_hex, 32, bytes) == FIPS_IDENTITY_InvalidNsecLength);
    return 0;
}

static int test_decode_secret_hex_invalid_chars(fixture *t)
{
    (void)t;
    uint8_t bytes[32]; CHECK(fips_decode_secret(t->secp, "zzzz", 4, bytes) == FIPS_IDENTITY_InvalidHex);
    CHECK(fips_decode_secret(t->secp, "a", 1, bytes) == FIPS_IDENTITY_InvalidHex);
    return 0;
}

static int test_node_addr_display(fixture *t)
{
    (void)t;
    fips_node_addr addr = {{1,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10}};
    char out[33]; OK(fips_node_addr_format(&addr, out, 33)); CHECK(!strcmp(out, "0123456789abcdeffedcba9876543210"));
    return 0;
}

static int test_node_addr_as_slice(fixture *t)
{
    (void)t;
    uint8_t bytes[16]; memset(bytes, 0xaa, 16); fips_node_addr out;
    OK(fips_node_addr_from_slice(bytes, 16, &out)); CHECK(sizeof(out.bytes) == 16 && !memcmp(out.bytes, bytes, 16));
    return 0;
}

static int test_node_addr_as_ref(fixture *t)
{
    (void)t;
    uint8_t bytes[16]; memset(bytes, 0xbb, 16); fips_node_addr out;
    OK(fips_node_addr_from_slice(bytes, 16, &out)); CHECK(!memcmp(out.bytes, bytes, 16));
    return 0;
}

static int test_fips_address_from_slice_success(fixture *t)
{
    (void)t;
    uint8_t bytes[16] = {0xfd,0x42}; fips_ipv6_address out;
    OK(fips_ipv6_address_from_slice(bytes, 16, &out)); CHECK(!memcmp(out.bytes, bytes, 16));
    return 0;
}

static int test_fips_address_from_slice_wrong_length(fixture *t)
{
    (void)t;
    uint8_t bytes[20] = {0xfd}; fips_ipv6_address out = {{42}}, before = out;
    CHECK(fips_ipv6_address_from_slice(bytes, 8, &out) == FIPS_IDENTITY_InvalidAddressLength);
    CHECK(fips_ipv6_address_from_slice(bytes, 20, &out) == FIPS_IDENTITY_InvalidAddressLength);
    CHECK(!memcmp(&out, &before, sizeof(out)));
    return 0;
}

static int test_fips_address_from_slice_wrong_prefix(fixture *t)
{
    (void)t;
    uint8_t bytes[16] = {0xfe}; fips_ipv6_address out = {{42}}, before = out;
    CHECK(fips_ipv6_address_from_slice(bytes, 16, &out) == FIPS_IDENTITY_InvalidAddressPrefix); CHECK(!memcmp(&out, &before, sizeof(out)));
    return 0;
}

static int test_fips_address_display(fixture *t)
{
    (void)t;
    fips_ipv6_address addr = {{0xfd}}; char text[40];
    OK(fips_ipv6_address_format(&addr, text, sizeof(text))); CHECK(!strcmp(text, "fd00::"));
    return 0;
}

static int test_peer_identity_pubkey_full_preserved_parity(fixture *t)
{
    (void)t;
    uint8_t full[33]; fips_identity_pubkey_full(t->id, full); fips_peer_identity peer;
    OK(fips_peer_identity_from_pubkey_full(t->secp, full, &peer)); CHECK(!memcmp(peer.pubkey_full, full, 33));
    full[0] = 3; OK(fips_peer_identity_from_pubkey_full(t->secp, full, &peer)); CHECK(peer.pubkey_full[0] == 3);
    return 0;
}

static int test_seeded_context_signs_and_verifies_like_the_default_one(fixture *t)
{
    (void)t;
    fips_fixed_entropy fixed = {3}; fips_entropy e = fips_fixed_entropy_adapter(&fixed); fips_secp *other;
    OK(fips_identity_init_secp(e, &other)); uint8_t secret[32]; fips_identity_export_secret(t->id, secret); fips_identity *id;
    OK(fips_identity_from_secret_bytes(other, t->entropy, secret, &id)); uint8_t a[64], b[64];
    OK(fips_identity_sign(id, (const uint8_t *)"blind", 5, a)); OK(fips_identity_sign(t->id, (const uint8_t *)"blind", 5, b));
    CHECK(!memcmp(a,b,64)); fips_peer_identity peer; OK(fips_peer_identity_from_pubkey(other, &t->key, &peer));
    OK(fips_peer_identity_verify(other, &peer, (const uint8_t *)"blind", 5, a));
    sodium_memzero(secret,32); fips_identity_destroy(id); fips_identity_destroy_secp(other);
    return 0;
}

static int test_identity_generation(fixture *t)
{
    (void)t;
    fips_identity *id; OK(fips_identity_generate(t->secp, t->entropy, &id));
    fips_node_addr node = fips_identity_node_addr(id); fips_ipv6_address addr = fips_identity_address(id);
    CHECK(sizeof(node.bytes) == 16 && addr.bytes[0] == 0xfd && !memcmp(addr.bytes+1, node.bytes, 15)); fips_identity_destroy(id);
    return 0;
}

static int test_node_addr_from_pubkey_deterministic(fixture *t)
{
    (void)t;
    fips_node_addr a = fips_node_addr_from_pubkey(&t->key), b = fips_node_addr_from_pubkey(&t->key);
    CHECK(!memcmp(a.bytes, b.bytes, 16)); CHECK(!memcmp(a.bytes,t->addr.bytes,16));
    return 0;
}

static int test_fips_address_ipv6_format(fixture *t)
{
    (void)t;
    char text[40]; OK(fips_ipv6_address_format(&t->address, text, sizeof(text))); CHECK(!strncmp(text,"fd",2));
    fips_ipv6_address copy; OK(fips_ipv6_address_from_slice(t->address.bytes,16,&copy)); CHECK(!memcmp(copy.bytes,t->address.bytes,16));
    return 0;
}

static int test_auth_challenge_verify_success(fixture *t)
{
    (void)t;
    CHECK(check_auth(t, 0) == 0);
    return 0;
}

static int test_auth_challenge_verify_wrong_challenge(fixture *t)
{
    (void)t;
    CHECK(check_auth(t, 1) == 0);
    return 0;
}

static int test_auth_challenge_verify_wrong_timestamp(fixture *t)
{
    (void)t;
    CHECK(check_auth(t, 2) == 0);
    return 0;
}

static int test_node_addr_ordering(fixture *t)
{
    (void)t;
    fips_node_addr a = {{0}}, b = {{1}}; CHECK(memcmp(a.bytes,b.bytes,16) < 0); CHECK(!memcmp(a.bytes,a.bytes,16));
    return 0;
}

static int test_identity_from_secret_bytes(fixture *t)
{
    (void)t;
    uint8_t secret[32]; fips_identity_export_secret(t->id, secret); fips_identity *id;
    OK(fips_identity_from_secret_bytes(t->secp,t->entropy,secret,&id)); fips_node_addr addr = fips_identity_node_addr(id);
    fips_ipv6_address ip = fips_identity_address(id); CHECK(!memcmp(addr.bytes,t->addr.bytes,16) && !memcmp(ip.bytes,t->address.bytes,16));
    sodium_memzero(secret,32); fips_identity_destroy(id);
    return 0;
}

static int test_identity_sign(fixture *t)
{
    (void)t;
    uint8_t sig[64]; OK(fips_identity_sign(t->id,(const uint8_t *)"test message",12,sig));
    fips_peer_identity peer; OK(fips_peer_identity_from_pubkey(t->secp,&t->key,&peer));
    OK(fips_peer_identity_verify(t->secp,&peer,(const uint8_t *)"test message",12,sig));
    return 0;
}

static int test_npub_encoding(fixture *t)
{
    (void)t;
    char text[64]; OK(fips_identity_npub(t->id,text,64)); CHECK(strlen(text) == 63 && !strncmp(text,"npub1",5));
    return 0;
}

static int test_npub_roundtrip(fixture *t)
{
    (void)t;
    char text[64]; fips_xonly_pubkey key; OK(fips_identity_npub(t->id,text,64));
    OK(fips_decode_npub(t->secp,text,63,&key)); CHECK(!memcmp(key.bytes,t->key.bytes,32));
    return 0;
}

static int test_npub_known_vector(fixture *t)
{
    (void)t;
    fips_xonly_pubkey key; char text[64]; OK(fips_decode_npub(t->secp,canonical,63,&key));
    OK(fips_encode_npub(&key,text,64)); CHECK(!strcmp(text,canonical));
    return 0;
}

static int test_peer_identity_from_npub(fixture *t)
{
    (void)t;
    char text[64], out[64]; OK(fips_identity_npub(t->id,text,64)); fips_peer_identity p;
    OK(fips_peer_identity_from_npub(t->secp,text,63,&p)); CHECK(!memcmp(p.pubkey.bytes,t->key.bytes,32));
    CHECK(!memcmp(p.node_addr.bytes,t->addr.bytes,16) && !memcmp(p.address.bytes,t->address.bytes,16));
    OK(fips_peer_identity_npub(&p,out,64)); CHECK(!strcmp(out,text));
    return 0;
}

static int test_peer_identity_verify_signature(fixture *t)
{
    (void)t;
    uint8_t sig[64]; fips_peer_identity p; OK(fips_peer_identity_from_pubkey(t->secp,&t->key,&p));
    OK(fips_identity_sign(t->id,(const uint8_t *)"hello world",11,sig));
    OK(fips_peer_identity_verify(t->secp,&p,(const uint8_t *)"hello world",11,sig));
    CHECK(fips_peer_identity_verify(t->secp,&p,(const uint8_t *)"wrong data",10,sig) == FIPS_IDENTITY_SignatureVerificationFailed);
    return 0;
}

static int test_peer_identity_display(fixture *t)
{
    (void)t;
    fips_peer_identity p; char a[64], b[64]; OK(fips_peer_identity_from_pubkey(t->secp,&t->key,&p));
    OK(fips_peer_identity_npub(&p,a,64)); OK(fips_identity_npub(t->id,b,64)); CHECK(!strcmp(a,b));
    return 0;
}

static int test_decode_nsec_invalid_prefix(fixture *t)
{
    (void)t;
    char text[64]; uint8_t secret[32]; OK(fips_identity_npub(t->id,text,64));
    CHECK(fips_decode_nsec(t->secp,text,63,secret) == FIPS_IDENTITY_InvalidNsecPrefix);
    return 0;
}

static int test_identity_from_secret_str_nsec(fixture *t)
{
    (void)t;
    char text[64]; CHECK(make_nsec(t,text) == 0); fips_identity *id;
    OK(fips_identity_from_secret_str(t->secp,t->entropy,text,63,&id)); fips_node_addr addr = fips_identity_node_addr(id);
    CHECK(!memcmp(addr.bytes,t->addr.bytes,16)); fips_identity_destroy(id); sodium_memzero(text,64);
    return 0;
}

static int test_identity_from_secret_str_hex(fixture *t)
{
    (void)t;
    fips_identity *id; OK(fips_identity_from_secret_str(t->secp,t->entropy,secret_hex,64,&id));
    fips_node_addr addr = fips_identity_node_addr(id); CHECK(!memcmp(addr.bytes,t->addr.bytes,16)); fips_identity_destroy(id);
    return 0;
}

static int test_hex_conversion_case1(fixture *t)
{
    (void)t;
    fips_identity *id; OK(fips_identity_from_secret_str(t->secp,t->entropy,secret_hex,64,&id));
    char text[64]; OK(fips_identity_npub(id,text,64)); CHECK(!strncmp(text,"npub1",5)); fips_identity_destroy(id);
    return 0;
}

static int test_hex_conversion_case2(fixture *t)
{
    (void)t;
    const char *hex = "b102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1fb0";
    fips_identity *id; OK(fips_identity_from_secret_str(t->secp,t->entropy,hex,64,&id));
    char text[64]; OK(fips_identity_npub(id,text,64)); CHECK(!strncmp(text,"npub1",5)); fips_identity_destroy(id);
    return 0;
}

static int test_fips_address_into_ipv6(fixture *t)
{
    (void)t;
    fips_ipv6_address copy; OK(fips_ipv6_address_from_slice(t->address.bytes,16,&copy)); CHECK(!memcmp(copy.bytes,t->address.bytes,16));
    return 0;
}

static int test_fips_address_eq_hash(fixture *t)
{
    (void)t;
    fips_ipv6_address addr = fips_ipv6_address_from_node_addr(&t->addr); CHECK(!memcmp(addr.bytes,t->address.bytes,16));
    return 0;
}

static int test_auth_challenge_from_bytes(fixture *t)
{
    (void)t;
    uint8_t bytes[32]; memset(bytes,0x42,32); fips_auth_challenge c = fips_auth_challenge_from_bytes(bytes); CHECK(!memcmp(c.bytes,bytes,32));
    return 0;
}

static int test_peer_identity_from_pubkey_full(fixture *t)
{
    (void)t;
    uint8_t full[33]; fips_identity_pubkey_full(t->id,full); fips_peer_identity p;
    OK(fips_peer_identity_from_pubkey_full(t->secp,full,&p)); CHECK(!memcmp(p.pubkey_full,full,33) && !memcmp(p.pubkey.bytes,t->key.bytes,32));
    CHECK(!memcmp(p.node_addr.bytes,t->addr.bytes,16) && !memcmp(p.address.bytes,t->address.bytes,16));
    return 0;
}

static int test_peer_identity_pubkey_full_even_parity_fallback(fixture *t)
{
    (void)t;
    fips_peer_identity p; OK(fips_peer_identity_from_pubkey(t->secp,&t->key,&p));
    CHECK(p.pubkey_full[0] == 2 && !memcmp(p.pubkey_full+1,t->key.bytes,32));
    return 0;
}

static int test_identity_keypair(fixture *t)
{
    (void)t;
    uint8_t secret[32]; fips_identity_export_secret(t->id,secret); fips_identity *copy;
    OK(fips_identity_from_secret_bytes(t->secp,t->entropy,secret,&copy)); fips_xonly_pubkey key = fips_identity_pubkey(copy);
    CHECK(!memcmp(key.bytes,t->key.bytes,32)); sodium_memzero(secret,32); fips_identity_destroy(copy);
    return 0;
}

static int test_identity_pubkey_full(fixture *t)
{
    (void)t;
    uint8_t full[33]; fips_identity_pubkey_full(t->id,full); CHECK(!memcmp(full+1,t->key.bytes,32));
    return 0;
}

static int test_signatures_draw_their_auxiliary_bytes_from_the_entropy_source(fixture *t)
{
    (void)t;
    fips_fixed_entropy fixed = {2}; fips_entropy e = fips_fixed_entropy_adapter(&fixed);
    uint8_t secret[32]; fips_identity_export_secret(t->id,secret); fips_identity *id;
    OK(fips_identity_from_secret_bytes(t->secp,e,secret,&id)); uint8_t a[64], b[64], again[64];
    OK(fips_identity_sign(t->id,(const uint8_t *)"msg",3,a)); OK(fips_identity_sign(t->id,(const uint8_t *)"msg",3,again));
    OK(fips_identity_sign(id,(const uint8_t *)"msg",3,b)); CHECK(!memcmp(a,again,64) && memcmp(a,b,64));
    fips_auth_challenge c = {{0x22}}; fips_auth_response x,y,z;
    OK(fips_identity_sign_challenge(t->id,&c,123,&x)); OK(fips_identity_sign_challenge(t->id,&c,123,&z));
    OK(fips_identity_sign_challenge(id,&c,123,&y)); CHECK(!memcmp(x.signature,z.signature,64) && memcmp(x.signature,y.signature,64));
    sodium_memzero(secret,32); fips_identity_destroy(id);
    return 0;
}

static int identity_generate_draws_its_secret_from_the_entropy_source(fixture *t)
{
    (void)t;
    fips_fixed_entropy fixed = {1}; fips_entropy e = fips_fixed_entropy_adapter(&fixed); fips_identity *id;
    OK(fips_identity_generate(t->secp,e,&id)); uint8_t bytes[32]; fips_identity_export_secret(id,bytes);
    for (size_t i=0;i<32;++i) CHECK(bytes[i] == 1);
    sodium_memzero(bytes,32); fips_identity_destroy(id);
    return 0;
}

static int auth_challenge_generate_draws_from_the_entropy_source(fixture *t)
{
    (void)t;
    fips_fixed_entropy fixed = {7}; fips_entropy e = fips_fixed_entropy_adapter(&fixed);
    fips_auth_challenge c = fips_auth_challenge_generate(e); for (size_t i=0;i<32;++i) CHECK(c.bytes[i] == 7);
    return 0;
}

static int the_generator_keys_node_address_and_ipv6_address_match_values_computed_outside_the_crate(fixture *t)
{
    (void)t;
    uint8_t secret[32] = {0}; secret[31] = 1; fips_identity *id;
    OK(fips_identity_from_secret_bytes(t->secp,t->entropy,secret,&id)); fips_node_addr addr = fips_identity_node_addr(id);
    fips_ipv6_address ip = fips_identity_address(id); char hex[33], text[40];
    OK(fips_node_addr_format(&addr,hex,33)); CHECK(!strcmp(hex,"132f39a98c31baaddba6525f5d43f295"));
    OK(fips_ipv6_address_format(&ip,text,40)); CHECK(!strcmp(text,"fd13:2f39:a98c:31ba:addb:a652:5f5d:43f2")); fips_identity_destroy(id);
    return 0;
}

static int a_peer_key_round_trips_through_its_xonly_key_and_holds_its_serialized_bytes(fixture *t)
{
    (void)t;
    fips_peer_key p = fips_peer_key_from_xonly(&t->key); fips_xonly_pubkey out;
    OK(fips_peer_key_to_xonly(t->secp,&p,&out)); CHECK(!memcmp(out.bytes,t->key.bytes,32) && !memcmp(p.bytes,t->key.bytes,32));
    fips_peer_key copy = fips_peer_key_from_bytes(t->key.bytes); CHECK(!memcmp(copy.bytes,p.bytes,32));
    return 0;
}

static int a_peer_key_of_bytes_off_the_curve_refuses_conversion_but_still_derives_addresses(fixture *t)
{
    (void)t;
    for (unsigned i=0;i<2;++i) { uint8_t bytes[32]; memset(bytes,i ? 0xff : 0,32);
        fips_peer_key key = fips_peer_key_from_bytes(bytes); fips_xonly_pubkey out;
        CHECK(fips_peer_key_to_xonly(t->secp,&key,&out) == FIPS_IDENTITY_InvalidSecretKey);
        fips_node_addr addr = fips_peer_key_addr(&key); fips_ipv6_address a = fips_peer_key_address(&key), b = fips_ipv6_address_from_node_addr(&addr);
        CHECK(!memcmp(a.bytes,b.bytes,16)); }
    return 0;
}

static int a_peer_key_is_its_32_bytes_and_nothing_more(fixture *t)
{
    (void)t;
    CHECK(sizeof(fips_peer_key) == 32);
    return 0;
}

static int the_generator_keys_node_prefix_matches_the_value_computed_outside_the_crate(fixture *t)
{
    (void)t;
    uint8_t secret[32] = {0}; secret[31]=1; fips_identity *id;
    OK(fips_identity_from_secret_bytes(t->secp,t->entropy,secret,&id)); fips_node_addr addr = fips_identity_node_addr(id);
    fips_ipv6_address ip = fips_identity_address(id); fips_node_prefix a = fips_node_addr_prefix(&addr), b = fips_ipv6_address_prefix(&ip);
    const uint8_t expected[15] = {0x13,0x2f,0x39,0xa9,0x8c,0x31,0xba,0xad,0xdb,0xa6,0x52,0x5f,0x5d,0x43,0xf2};
    CHECK(!memcmp(a.bytes,expected,15) && !memcmp(b.bytes,expected,15)); fips_identity_destroy(id);
    return 0;
}

static int an_ipv6_address_names_the_prefix_of_the_node_address_it_was_derived_from(fixture *t)
{
    (void)t;
    fips_node_prefix a=fips_node_addr_prefix(&t->addr), b=fips_ipv6_address_prefix(&t->address);
    CHECK(!memcmp(a.bytes,b.bytes,15) && !memcmp(a.bytes,t->addr.bytes,15));
    return 0;
}

static int a_prefix_from_octets_takes_bytes_one_to_fifteen_and_ignores_byte_zero(fixture *t)
{
    (void)t;
    uint8_t octets[16]; for (size_t i=0;i<16;++i) octets[i]=(uint8_t)i;
    octets[0]=0xfe; fips_node_prefix a=fips_node_prefix_from_octets(octets); octets[0]=0xfd;
    fips_node_prefix b=fips_node_prefix_from_octets(octets); CHECK(!memcmp(a.bytes,b.bytes,15));
    for (size_t i=0;i<15;++i) CHECK(a.bytes[i] == i+1);
    return 0;
}

static int a_node_address_from_a_prefix_is_the_prefix_followed_by_a_zero_byte(fixture *t)
{
    (void)t;
    fips_node_prefix p=fips_node_addr_prefix(&t->addr); fips_node_addr a=fips_node_addr_from_prefix(p);
    CHECK(!memcmp(a.bytes,t->addr.bytes,15) && a.bytes[15] == 0);
    memset(p.bytes,0xff,15); a=fips_node_addr_from_prefix(p); char text[33]; OK(fips_node_addr_format(&a,text,33));
    CHECK(!strcmp(text,"ffffffffffffffffffffffffffffff00"));
    return 0;
}

static int decode_npub_refuses_a_bech32m_checksum(fixture *t)
{
    (void)t;
    fips_xonly_pubkey out; CHECK(fips_decode_npub(t->secp,noncanonical[0],63,&out) == FIPS_IDENTITY_NonCanonicalNpub);
    return 0;
}

static int decode_npub_refuses_non_zero_padding_bits(fixture *t)
{
    (void)t;
    fips_xonly_pubkey out; for (size_t i=1;i<3;++i) CHECK(fips_decode_npub(t->secp,noncanonical[i],63,&out) == FIPS_IDENTITY_NonCanonicalNpub);
    return 0;
}

static int decode_npub_refuses_a_non_canonical_npub_in_any_case(fixture *t)
{
    (void)t;
    fips_xonly_pubkey out; char text[64]; for (size_t i=0;i<3;++i) for (unsigned mode=1;mode<=2;++mode) {
        spelling(noncanonical[i],text,mode); CHECK(fips_decode_npub(t->secp,text,63,&out) == FIPS_IDENTITY_NonCanonicalNpub); }
    return 0;
}

static int decode_npub_accepts_the_canonical_npub_in_lower_upper_and_mixed_case(fixture *t)
{
    (void)t;
    fips_xonly_pubkey expected, out; OK(fips_decode_npub(t->secp,canonical,63,&expected)); char text[64];
    for (unsigned mode=0;mode<4;++mode) { spelling(canonical,text,mode); OK(fips_decode_npub(t->secp,text,63,&out)); CHECK(!memcmp(expected.bytes,out.bytes,32)); }
    return 0;
}

static int peer_identity_from_a_mixed_case_npub_reports_the_canonical_npub(fixture *t)
{
    (void)t;
    char text[64], out[64]; spelling(canonical,text,2); fips_peer_identity peer;
    OK(fips_peer_identity_from_npub(t->secp,text,63,&peer)); OK(fips_peer_identity_npub(&peer,out,64)); CHECK(!strcmp(out,canonical));
    return 0;
}

static int c_scalar_boundaries_and_transactional_constructors(fixture *t)
{
    (void)t;
    uint8_t bytes[32] = {0}; fips_identity *out = t->id;
    CHECK(fips_identity_from_secret_bytes(t->secp,t->entropy,bytes,&out) == FIPS_IDENTITY_InvalidSecretKey && out == t->id);
    memset(bytes,0xff,32); CHECK(fips_identity_from_secret_bytes(t->secp,t->entropy,bytes,&out) == FIPS_IDENTITY_InvalidSecretKey && out == t->id);
    const uint8_t order[32] = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,0xba,0xae,0xdc,0xe6,0xaf,0x48,0xa0,0x3b,0xbf,0xd2,0x5e,0x8c,0xd0,0x36,0x41,0x41};
    memcpy(bytes,order,32); CHECK(fips_identity_from_secret_bytes(t->secp,t->entropy,bytes,&out) == FIPS_IDENTITY_InvalidSecretKey);
    --bytes[31]; OK(fips_identity_from_secret_bytes(t->secp,t->entropy,bytes,&out)); fips_identity_destroy(out);
    fips_fixed_entropy fixed={0}; fips_entropy e=fips_fixed_entropy_adapter(&fixed); out=t->id;
    CHECK(fips_identity_generate(t->secp,e,&out) == FIPS_IDENTITY_InvalidSecretKey && out == t->id);
    CHECK(fips_identity_from_secret_str(t->secp,t->entropy,"zz",2,&out) == FIPS_IDENTITY_InvalidHex && out == t->id);
    sodium_memzero(bytes,32);
    return 0;
}

static int c_auth_negative_and_timestamp_boundaries(fixture *t)
{
    (void)t;
    CHECK(check_auth(t,3) == 0 && check_auth(t,4) == 0 && check_auth(t,5) == 0);
    fips_auth_challenge c={{0}}; uint64_t stamps[]={0,UINT64_MAX,UINT64_C(0x0102030405060708)};
    for(size_t i=0;i<3;++i) { fips_auth_response response; fips_node_addr addr;
        OK(fips_identity_sign_challenge(t->id,&c,stamps[i],&response)); OK(fips_auth_challenge_verify(t->secp,&c,&response,&addr));
        CHECK(!memcmp(addr.bytes,t->addr.bytes,16)); }
    return 0;
}

static int c_nsec_case_and_canonical_forms(fixture *t)
{
    (void)t;
    char text[64], changed[64]; CHECK(make_nsec(t,text) == 0); uint8_t secret[32], expected[32]; fips_identity_export_secret(t->id,expected);
    for(unsigned mode=0;mode<4;++mode) { spelling(text,changed,mode); OK(fips_decode_nsec(t->secp,changed,63,secret)); CHECK(!memcmp(secret,expected,32));
        OK(fips_decode_secret(t->secp,changed,63,secret)); CHECK(!memcmp(secret,expected,32)); }
    CHECK(fips_decode_nsec(t->secp,"nsec1qypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7rusq5ug50x",63,secret) == FIPS_IDENTITY_NonCanonicalNsec);
    CHECK(fips_decode_nsec(t->secp,"nsec1qypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ruspukvdhk",63,secret) == FIPS_IDENTITY_NonCanonicalNsec);
    CHECK(fips_decode_nsec(t->secp,"nsec1qypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7rus0t3mgs7",63,secret) == FIPS_IDENTITY_NonCanonicalNsec);
    sodium_memzero(text,64); sodium_memzero(changed,64); sodium_memzero(secret,32); sodium_memzero(expected,32);
    return 0;
}

static int c_decoder_malformed_and_unchanged_outputs(fixture *t)
{
    (void)t;
    const char *bad[]={"", "npub", "npub1", "not bech32", "nostr:npub1x", "npub1invalid", "npub1b00000", " npub1", "npub1!00000"};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) { fips_xonly_pubkey key={{42}}, before=key; uint8_t secret[32], expected[32]; memset(secret,42,32); memcpy(expected,secret,32);
        CHECK(fips_decode_npub(t->secp,bad[i],strlen(bad[i]),&key) != FIPS_IDENTITY_OK && !memcmp(key.bytes,before.bytes,32));
        CHECK(fips_decode_nsec(t->secp,bad[i],strlen(bad[i]),secret) != FIPS_IDENTITY_OK && !memcmp(secret,expected,32)); }
    char text[64]; memcpy(text,canonical,64); text[10]='\0'; fips_xonly_pubkey key;
    CHECK(fips_decode_npub(t->secp,text,63,&key) == FIPS_IDENTITY_Bech32Decode);
    memset(text,'x',64); CHECK(fips_decode_npub(t->secp,text,SIZE_MAX,&key) == FIPS_IDENTITY_Bech32Decode);
    uint8_t secret[32];
    CHECK(fips_decode_nsec(t->secp,"nsec1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqwkhnav",63,secret) == FIPS_IDENTITY_InvalidSecretKey);
    CHECK(fips_decode_nsec(t->secp,"nsec1lllllllllllllllllllllllllllllllllllllllllllllllllllsvg5z5m",63,secret) == FIPS_IDENTITY_InvalidSecretKey);
    CHECK(fips_decode_npub(t->secp,"npub1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqzqujme",63,&key) == FIPS_IDENTITY_InvalidSecretKey);
    return 0;
}

static int c_format_capacities_and_short_forms(fixture *t)
{
    (void)t;
    char text[64], before[64]; memset(text,42,64); memcpy(before,text,64);
    CHECK(fips_identity_npub(t->id,text,63) == FIPS_IDENTITY_BufferTooSmall && !memcmp(text,before,64));
    CHECK(fips_node_addr_format(&t->addr,text,32) == FIPS_IDENTITY_BufferTooSmall && !memcmp(text,before,64));
    CHECK(fips_ipv6_address_format(&t->address,text,1) == FIPS_IDENTITY_BufferTooSmall && !memcmp(text,before,64));
    CHECK(fips_node_addr_short_hex(&t->addr,text,11) == FIPS_IDENTITY_BufferTooSmall && !memcmp(text,before,64));
    OK(fips_node_addr_short_hex(&t->addr,text,12)); CHECK(strlen(text) == 11 && !strcmp(text+8,"..."));
    fips_peer_identity p; OK(fips_peer_identity_from_pubkey(t->secp,&t->key,&p));
    OK(fips_peer_identity_short_npub(&p,text,17)); CHECK(strlen(text) == 16 && !strncmp(text,"npub1",5) && !strncmp(text+9,"...",3));
    char full[64]; OK(fips_identity_npub(t->id,full,64)); CHECK(!memcmp(text+12,full+59,4));
    memset(text,42,64); CHECK(fips_peer_identity_short_npub(&p,text,16) == FIPS_IDENTITY_BufferTooSmall && !memcmp(text,before,64));
    uint8_t secret[32]; fips_identity_export_secret(t->id,secret);
    CHECK(fips_encode_nsec(t->secp,secret,text,63) == FIPS_IDENTITY_BufferTooSmall && !memcmp(text,before,64)); sodium_memzero(secret,32);
    return 0;
}

static int c_ipv6_zero_compression(fixture *t)
{
    (void)t;
    const uint16_t inputs[][8]={{0,0,0,0,0,0,0,0},{0xfd00,1,0,0,2,0,0,3},{0xfd00,1,2,3,4,5,0,0},{0xfd00,0,1,2,3,4,5,6},{0,0,1,2,3,4,5,6}};
    const char *expected[]={"::","fd00:1::2:0:0:3","fd00:1:2:3:4:5::","fd00:0:1:2:3:4:5:6","::1:2:3:4:5:6"};
    for(size_t i=0;i<5;++i) { fips_ipv6_address a; for(size_t j=0;j<8;++j) {a.bytes[2*j]=(uint8_t)(inputs[i][j]>>8); a.bytes[2*j+1]=(uint8_t)inputs[i][j];}
        char text[40]; OK(fips_ipv6_address_format(&a,text,40)); CHECK(!strcmp(text,expected[i])); }
    return 0;
}

static int c_error_discriminants(fixture *t)
{
    (void)t;
    CHECK(!strcmp(fips_identity_error_name(FIPS_IDENTITY_InvalidSecretKey),"InvalidSecretKey"));
    CHECK(!strcmp(fips_identity_error_name(FIPS_IDENTITY_Bech32Encode),"Bech32Encode"));
    CHECK(!strcmp(fips_identity_error_name(FIPS_IDENTITY_Bech32Decode),"Bech32Decode"));
    CHECK(!strcmp(fips_identity_error_name(FIPS_IDENTITY_InvalidHex),"InvalidHex"));
    CHECK(!strcmp(fips_identity_error_name((fips_identity_status)999),"Unknown"));
    return 0;
}

int main(void)
{
    fips_fixed_entropy fixed={1}; fixture t;
    t.entropy=fips_fixed_entropy_adapter(&fixed);
    OK(fips_identity_init_secp(t.entropy,&t.secp));
    OK(fips_identity_from_secret_str(t.secp,t.entropy,secret_hex,64,&t.id));
    t.key=fips_identity_pubkey(t.id); t.addr=fips_identity_node_addr(t.id); t.address=fips_identity_address(t.id);
    int failed=0; size_t count=0;
#define RUN(name) do { failed |= name(&t); ++count; } while (0)
    RUN(test_node_addr_from_slice);
    RUN(test_fips_address_validation);
    RUN(test_decode_npub_invalid_prefix);
    RUN(test_decode_npub_invalid_checksum);
    RUN(test_peer_identity_from_invalid_npub);
    RUN(test_nsec_roundtrip);
    RUN(test_decode_secret_nsec);
    RUN(test_decode_secret_hex);
    RUN(test_decode_npub_invalid_length);
    RUN(test_decode_nsec_invalid_length);
    RUN(test_decode_secret_hex_wrong_length);
    RUN(test_decode_secret_hex_invalid_chars);
    RUN(test_node_addr_display);
    RUN(test_node_addr_as_slice);
    RUN(test_node_addr_as_ref);
    RUN(test_fips_address_from_slice_success);
    RUN(test_fips_address_from_slice_wrong_length);
    RUN(test_fips_address_from_slice_wrong_prefix);
    RUN(test_fips_address_display);
    RUN(test_peer_identity_pubkey_full_preserved_parity);
    RUN(test_seeded_context_signs_and_verifies_like_the_default_one);
    RUN(test_identity_generation);
    RUN(test_node_addr_from_pubkey_deterministic);
    RUN(test_fips_address_ipv6_format);
    RUN(test_auth_challenge_verify_success);
    RUN(test_auth_challenge_verify_wrong_challenge);
    RUN(test_auth_challenge_verify_wrong_timestamp);
    RUN(test_node_addr_ordering);
    RUN(test_identity_from_secret_bytes);
    RUN(test_identity_sign);
    RUN(test_npub_encoding);
    RUN(test_npub_roundtrip);
    RUN(test_npub_known_vector);
    RUN(test_peer_identity_from_npub);
    RUN(test_peer_identity_verify_signature);
    RUN(test_peer_identity_display);
    RUN(test_decode_nsec_invalid_prefix);
    RUN(test_identity_from_secret_str_nsec);
    RUN(test_identity_from_secret_str_hex);
    RUN(test_hex_conversion_case1);
    RUN(test_hex_conversion_case2);
    RUN(test_fips_address_into_ipv6);
    RUN(test_fips_address_eq_hash);
    RUN(test_auth_challenge_from_bytes);
    RUN(test_peer_identity_from_pubkey_full);
    RUN(test_peer_identity_pubkey_full_even_parity_fallback);
    RUN(test_identity_keypair);
    RUN(test_identity_pubkey_full);
    RUN(test_signatures_draw_their_auxiliary_bytes_from_the_entropy_source);
    RUN(identity_generate_draws_its_secret_from_the_entropy_source);
    RUN(auth_challenge_generate_draws_from_the_entropy_source);
    RUN(the_generator_keys_node_address_and_ipv6_address_match_values_computed_outside_the_crate);
    RUN(a_peer_key_round_trips_through_its_xonly_key_and_holds_its_serialized_bytes);
    RUN(a_peer_key_of_bytes_off_the_curve_refuses_conversion_but_still_derives_addresses);
    RUN(a_peer_key_is_its_32_bytes_and_nothing_more);
    RUN(the_generator_keys_node_prefix_matches_the_value_computed_outside_the_crate);
    RUN(an_ipv6_address_names_the_prefix_of_the_node_address_it_was_derived_from);
    RUN(a_prefix_from_octets_takes_bytes_one_to_fifteen_and_ignores_byte_zero);
    RUN(a_node_address_from_a_prefix_is_the_prefix_followed_by_a_zero_byte);
    RUN(decode_npub_refuses_a_bech32m_checksum);
    RUN(decode_npub_refuses_non_zero_padding_bits);
    RUN(decode_npub_refuses_a_non_canonical_npub_in_any_case);
    RUN(decode_npub_accepts_the_canonical_npub_in_lower_upper_and_mixed_case);
    RUN(peer_identity_from_a_mixed_case_npub_reports_the_canonical_npub);
    RUN(c_scalar_boundaries_and_transactional_constructors);
    RUN(c_auth_negative_and_timestamp_boundaries);
    RUN(c_nsec_case_and_canonical_forms);
    RUN(c_decoder_malformed_and_unchanged_outputs);
    RUN(c_format_capacities_and_short_forms);
    RUN(c_ipv6_zero_compression);
    RUN(c_error_discriminants);
#undef RUN
    fips_identity_destroy(t.id); fips_identity_destroy_secp(t.secp);
    printf("identity: %zu test cases, %s\n",count,failed ? "FAILED" : "passed");
    return failed;
}
