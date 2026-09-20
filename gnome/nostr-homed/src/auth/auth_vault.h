#ifndef NH_AUTH_VAULT_H
#define NH_AUTH_VAULT_H
#include <stddef.h>
#include <stdint.h>
#include "secure_buf.h"

#define NH_AUTH_VAULT_VERSION 1u
#define NH_AUTH_VAULT_SCRYPT_N 262144u
#define NH_AUTH_VAULT_SCRYPT_R 8u
#define NH_AUTH_VAULT_SCRYPT_P 1u
#define NH_AUTH_VAULT_PASSPHRASE_MIN 12u

typedef enum nh_auth_vault_rc { NH_AUTH_VAULT_OK=0, NH_AUTH_VAULT_INVALID, NH_AUTH_VAULT_UNLOCK_FAILED, NH_AUTH_VAULT_NO_MEMORY, NH_AUTH_VAULT_CRYPTO_ERROR } nh_auth_vault_rc;
typedef struct nh_auth_vault_binding { const char *provider_id,*account_id,*pubkey_hex; uint64_t key_generation; } nh_auth_vault_binding;

nh_auth_vault_rc nh_auth_vault_seal(const uint8_t secret[32],const uint8_t *passphrase,size_t passphrase_len,const nh_auth_vault_binding *binding,uint8_t **blob,size_t *blob_len);
nh_auth_vault_rc nh_auth_vault_open(const uint8_t *blob,size_t blob_len,const uint8_t *passphrase,size_t passphrase_len,const nh_auth_vault_binding *binding,nostr_secure_buf *secret_out);
#endif
