#include "auth_vault.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
int main(void){
  uint8_t secret[32];memset(secret,7,sizeof secret);const uint8_t pass[]="correct horse battery";
  nh_auth_vault_binding b={"11111111-1111-1111-1111-111111111111","22222222-2222-2222-2222-222222222222","79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",4};
  uint8_t *blob=NULL;size_t n=0;assert(nh_auth_vault_seal(secret,pass,sizeof pass-1,&b,&blob,&n)==NH_AUTH_VAULT_OK);assert(n>32);
  nostr_secure_buf out={0};assert(nh_auth_vault_open(blob,n,pass,sizeof pass-1,&b,&out)==NH_AUTH_VAULT_OK);assert(out.locked&&out.len==32&&!memcmp(out.ptr,secret,32));secure_free(&out);
  const uint8_t wrong[]="wrong passphrase here";assert(nh_auth_vault_open(blob,n,wrong,sizeof wrong-1,&b,&out)==NH_AUTH_VAULT_UNLOCK_FAILED);
  blob[n-1]^=1;assert(nh_auth_vault_open(blob,n,pass,sizeof pass-1,&b,&out)==NH_AUTH_VAULT_UNLOCK_FAILED);blob[n-1]^=1;b.key_generation++;
  assert(nh_auth_vault_open(blob,n,pass,sizeof pass-1,&b,&out)==NH_AUTH_VAULT_UNLOCK_FAILED);free(blob);return 0;
}
