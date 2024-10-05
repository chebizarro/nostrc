#ifndef NIP44_H
#define NIP44_H

#include <stdint.h>
#include <stdlib.h>

#define NIP44_VERSION 2
#define MIN_PLAINTEXT_SIZE 1
#define MAX_PLAINTEXT_SIZE 65535

typedef struct {
    uint8_t *salt;
    size_t salt_len;
} nip44_encrypt_options_t;

void nip44_generate_conversation_key(const char *pub, const char *sk, uint8_t *key);

char *nip44_encrypt(const char *plaintext, const uint8_t *conversation_key, const nip44_encrypt_options_t *options);

char *nip44_decrypt(const char *ciphertext, const uint8_t *conversation_key);

#endif // NIP44_H
