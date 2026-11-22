#undef NDEBUG
#include <nostr/nip78/nip78.h>
#include <nostr/nip44/nip44.h>
#include "nostr-keys.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* The fake external signer alone owns the secret. The codec receives only its
 * public key and NIP-44 output; use real crypto so the seam is not mock theater. */
static const char *secret = "0000000000000000000000000000000000000000000000000000000000000001";
static int signer_encrypt(const char *plaintext, char **ciphertext) {
    const uint8_t sk[32] = {[31] = 1};
    const uint8_t pk[32] = {
        0x79,0xbe,0x66,0x7e,0xf9,0xdc,0xbb,0xac,0x55,0xa0,0x62,0x95,0xce,0x87,0x0b,0x07,
        0x02,0x9b,0xfc,0xdb,0x2d,0xce,0x28,0xd9,0x59,0xf2,0x81,0x5b,0x16,0xf8,0x17,0x98};
    return nostr_nip44_encrypt_v2(sk, pk, (const uint8_t *)plaintext, strlen(plaintext), ciphertext);
}

int main(void) {
    const char *plaintext = "{\"schema_version\":1,\"hosts\":{}}";
    char *ciphertext = NULL, *author = nostr_key_get_public(secret);
    assert(author && !signer_encrypt(plaintext, &ciphertext));
    NostrEvent *event = nostr_nip78_build_event(author, "metadesk:management:v1", ciphertext, 123);
    assert(event && !nostr_event_get_sig(event));
    assert(!nostr_event_sign(event, secret));
    assert(nostr_event_check_signature(event));
    NostrNip78Document *document = NULL;
    assert(!nostr_nip78_parse_event(event, &document));
    assert(!strcmp(document->content, ciphertext));
    const uint8_t sk[32] = {[31] = 1};
    uint8_t pk[32];
    for (size_t i = 0; i < 32; i++) {
        char pair[3] = {author[2*i], author[2*i+1], 0};
        pk[i] = (uint8_t)strtoul(pair, NULL, 16);
    }
    uint8_t *opened = NULL;
    size_t length = 0;
    assert(!nostr_nip44_decrypt_v2(sk, pk, document->content, &opened, &length));
    assert(length == strlen(plaintext) && !memcmp(opened, plaintext, length));
    free(opened);
    free(author);
    free(ciphertext);
    nostr_nip78_document_free(document);
    nostr_event_free(event);
    return 0;
}
