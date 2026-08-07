#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>

#include <nostr/nip04.h>
#include <nostr-utils.h>
#include <secure_buf.h>

static const char *SENDER_SK =
    "0000000000000000000000000000000000000000000000000000000000000001";
static const char *RECEIVER_SK =
    "0000000000000000000000000000000000000000000000000000000000000002";
static const char *SENDER_PK =
    "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
static const char *RECEIVER_PK =
    "02c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5";

static void expect_uniform(const char *cipher, const nostr_secure_buf *receiver) {
    char *plain = (char *)0x1;
    char *error = (char *)0x1;
    assert(nostr_nip04_decrypt(cipher, SENDER_PK, RECEIVER_SK,
                               &plain, &error) != 0);
    assert(plain == NULL);
    assert(error && strcmp(error, "decrypt failed") == 0);
    free(error);

    plain = (char *)0x1;
    error = (char *)0x1;
    assert(nostr_nip04_decrypt_secure(cipher, SENDER_PK, receiver,
                                      &plain, &error) != 0);
    assert(plain == NULL);
    assert(error && strcmp(error, "decrypt failed") == 0);
    free(error);
}

static char *tamper_last_cipher_byte(const char *content) {
    const char *sep = strstr(content, "?iv=");
    assert(sep);
    const size_t b64_len = (size_t)(sep - content);
    char *b64 = malloc(b64_len + 1);
    assert(b64);
    memcpy(b64, content, b64_len);
    b64[b64_len] = '\0';

    const size_t cap = (b64_len / 4) * 3;
    unsigned char *bin = malloc(cap);
    assert(bin);
    int decoded = EVP_DecodeBlock(bin, (const unsigned char *)b64, (int)b64_len);
    assert(decoded > 0);
    size_t padding = 0;
    if (b64[b64_len - 1] == '=') padding++;
    if (b64[b64_len - 2] == '=') padding++;
    const size_t bin_len = (size_t)decoded - padding;
    assert(bin_len > 0);
    bin[bin_len - 1] ^= 0x80;

    const size_t encoded_len = 4 * ((bin_len + 2) / 3);
    char *encoded = malloc(encoded_len + 1);
    assert(encoded);
    assert(EVP_EncodeBlock((unsigned char *)encoded, bin, (int)bin_len) ==
           (int)encoded_len);
    encoded[encoded_len] = '\0';

    const size_t out_len = encoded_len + strlen(sep) + 1;
    char *out = malloc(out_len);
    assert(out);
    snprintf(out, out_len, "%s%s", encoded, sep);

    free(encoded);
    free(bin);
    free(b64);
    return out;
}

int main(void) {
    nostr_secure_buf receiver = secure_alloc(32);
    nostr_secure_buf sender = secure_alloc(32);
    assert(receiver.ptr && sender.ptr);
    assert(nostr_hex2bin((unsigned char *)receiver.ptr, RECEIVER_SK, 32));
    assert(nostr_hex2bin((unsigned char *)sender.ptr, SENDER_SK, 32));

    expect_uniform("missing-format", &receiver);
    expect_uniform("!!!!?iv=!!!!", &receiver);
    expect_uniform("AAAA?iv=AAAA", &receiver);
    expect_uniform("v=2:!!!!", &receiver);
    expect_uniform("v=2:AAAA", &receiver);

    char *legacy = NULL;
    char *error = NULL;
    assert(nostr_nip04_encrypt_legacy_secure(
               "uniform padding failure", RECEIVER_PK, &sender,
               &legacy, &error) == 0);
    free(error);
    char *tampered = tamper_last_cipher_byte(legacy);
    expect_uniform(tampered, &receiver);
    free(tampered);
    free(legacy);

    /* The old process-global downgrade is ignored by generic encryption. */
    assert(setenv("NIP04_LEGACY_CBC", "1", 1) == 0);
    char *generic = NULL;
    assert(nostr_nip04_encrypt_secure(
               "still aead", RECEIVER_PK, &sender, &generic, &error) == 0);
    assert(generic && strncmp(generic, "v=2:", 4) == 0);
    free(error);
    free(generic);
    unsetenv("NIP04_LEGACY_CBC");

    secure_free(&sender);
    secure_free(&receiver);
    puts("test_nip04_error_uniformity: OK");
    return 0;
}
