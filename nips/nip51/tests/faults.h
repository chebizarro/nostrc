#ifndef NIP51_TEST_FAULTS_H
#define NIP51_TEST_FAULTS_H
#include <stdlib.h>
#include <string.h>
#include "nostr/nip44/nip44.h"
void *nip51_test_malloc(size_t size);
void *nip51_test_calloc(size_t count, size_t size);
void *nip51_test_realloc(void *ptr, size_t size);
char *nip51_test_strdup(const char *text);
int nip51_test_encrypt(const uint8_t sk[32], const uint8_t pk[32], const uint8_t *plain, size_t len, char **out);
#define malloc nip51_test_malloc
#define calloc nip51_test_calloc
#define realloc nip51_test_realloc
#define strdup nip51_test_strdup
#define nostr_nip44_encrypt_v2 nip51_test_encrypt
#endif
