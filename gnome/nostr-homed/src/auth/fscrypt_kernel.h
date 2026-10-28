#ifndef NH_FSCRYPT_KERNEL_H
#define NH_FSCRYPT_KERNEL_H
#include <stdint.h>

#define NH_FSCRYPT_ID_LEN 16
#define NH_FSCRYPT_MASTER_LEN 64
/* Root-only kernel operations; caller serializes all operations per account.
 * They return 0/-1 with errno, except remove/status return the state below. */
typedef enum {
  NH_FSCRYPT_ERROR = -1,
  NH_FSCRYPT_ABSENT = 0,
  NH_FSCRYPT_PRESENT = 1,
  NH_FSCRYPT_BUSY = 2
} nh_fscrypt_key_state;
int nh_fscrypt_identifier(const uint8_t master[64], uint8_t id[16]);
int nh_fscrypt_add(int filesystem_fd, const uint8_t master[64], uint8_t id[16]);
int nh_fscrypt_set_policy(int empty_directory_fd, const uint8_t id[16]);
int nh_fscrypt_verify_policy(int directory_fd, const uint8_t id[16]);
/* 1 any policy (including legacy), 0 plaintext/unsupported, -1 indeterminate. */
int nh_fscrypt_has_policy(int directory_fd);
nh_fscrypt_key_state nh_fscrypt_key_status(int filesystem_fd, const uint8_t id[16]);
nh_fscrypt_key_state nh_fscrypt_remove(int filesystem_fd, const uint8_t id[16]);
/* Same-filesystem capability test with a random throwaway key and a real file
 * roundtrip. Refuses unsupported/misconfigured filesystems, never downgrades. */
int nh_fscrypt_probe(int parent_fd);
#endif
