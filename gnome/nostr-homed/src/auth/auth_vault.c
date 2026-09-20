#include "auth_vault.h"
#include "nostr_auth_protocol.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>

#define HDR 12u
#define SALT 32u
#define NONCE 12u
#define TAG 16u
#define CIPHER 32u
#define BLOB_LEN (HDR+SALT+NONCE+TAG+CIPHER)
static const uint8_t magic[4]={'N','A','V','1'};
static void put32(uint8_t *p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static uint32_t get32(const uint8_t*p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static int add_field(uint8_t **p,size_t *left,const char *s){size_t n=s?strlen(s):0;if(n>65535||*left<4+n)return -1;put32(*p,(uint32_t)n);*p+=4;*left-=4;if(n){memcpy(*p,s,n);*p+=n;*left-=n;}return 0;}
static int make_aad(const nh_auth_vault_binding*b,uint8_t **out,size_t *len){
  if(!b||!b->provider_id||!b->account_id||!b->pubkey_hex||strlen(b->pubkey_hex)!=64)return -1;
  size_t n=4+4+strlen(b->provider_id)+4+strlen(b->account_id)+4+64+8;uint8_t *a=malloc(n),*p=a;size_t left=n;if(!a)return -1;
  put32(p,NH_AUTH_VAULT_VERSION);p+=4;left-=4;
  if(add_field(&p,&left,b->provider_id)||add_field(&p,&left,b->account_id)||add_field(&p,&left,b->pubkey_hex)){free(a);return -1;}
  for(int i=7;i>=0;i--)*p++=(uint8_t)(b->key_generation>>(i*8));*out=a;*len=n;return 0;
}
static int derive(const uint8_t*pw,size_t n,const uint8_t salt[SALT],nostr_secure_buf*k){
  *k=secure_alloc(32);if(!k->ptr||!k->locked){secure_free(k);return -1;}
  return EVP_PBE_scrypt((const char*)pw,n,salt,SALT,NH_AUTH_VAULT_SCRYPT_N,NH_AUTH_VAULT_SCRYPT_R,NH_AUTH_VAULT_SCRYPT_P,300u*1024u*1024u,k->ptr,32)==1?0:-1;
}
static int valid_pw(const uint8_t*p,size_t n){return p&&n>=NH_AUTH_VAULT_PASSPHRASE_MIN&&n<=NH_AUTH_SECRET_MAX;}
nh_auth_vault_rc nh_auth_vault_seal(const uint8_t secret[32],const uint8_t*pw,size_t pn,const nh_auth_vault_binding*b,uint8_t**out,size_t*outn){
  if(!secret||!valid_pw(pw,pn)||!out||!outn)return NH_AUTH_VAULT_INVALID;*out=NULL;*outn=0;
  uint8_t *aad=NULL;size_t an=0;if(make_aad(b,&aad,&an))return NH_AUTH_VAULT_INVALID;
  uint8_t *blob=calloc(1,BLOB_LEN);if(!blob){free(aad);return NH_AUTH_VAULT_NO_MEMORY;}memcpy(blob,magic,4);put32(blob+4,NH_AUTH_VAULT_VERSION);put32(blob+8,NH_AUTH_VAULT_SCRYPT_N);
  uint8_t *salt=blob+HDR,*nonce=salt+SALT,*tag=nonce+NONCE,*cipher=tag+TAG;
  nostr_secure_buf key={0};EVP_CIPHER_CTX*c=NULL;int len=0,total=0,ok=0;
  if(RAND_bytes(salt,SALT)!=1||RAND_bytes(nonce,NONCE)!=1||derive(pw,pn,salt,&key))goto done;
  c=EVP_CIPHER_CTX_new();if(!c)goto done;
  if(EVP_EncryptInit_ex(c,EVP_aes_256_gcm(),NULL,NULL,NULL)!=1||EVP_CIPHER_CTX_ctrl(c,EVP_CTRL_GCM_SET_IVLEN,NONCE,NULL)!=1||EVP_EncryptInit_ex(c,NULL,NULL,key.ptr,nonce)!=1||EVP_EncryptUpdate(c,NULL,&len,aad,(int)an)!=1||EVP_EncryptUpdate(c,cipher,&len,secret,32)!=1)goto done;total=len;
  if(EVP_EncryptFinal_ex(c,cipher+total,&len)!=1||EVP_CIPHER_CTX_ctrl(c,EVP_CTRL_GCM_GET_TAG,TAG,tag)!=1)goto done;ok=1;
done: EVP_CIPHER_CTX_free(c);secure_free(&key);if(aad){secure_wipe(aad,an);free(aad);}if(!ok){secure_wipe(blob,BLOB_LEN);free(blob);return NH_AUTH_VAULT_CRYPTO_ERROR;}*out=blob;*outn=BLOB_LEN;return NH_AUTH_VAULT_OK;
}
nh_auth_vault_rc nh_auth_vault_open(const uint8_t*blob,size_t bn,const uint8_t*pw,size_t pn,const nh_auth_vault_binding*b,nostr_secure_buf*out){
  if(out)*out=(nostr_secure_buf){0};if(!blob||bn!=BLOB_LEN||!valid_pw(pw,pn)||!out||memcmp(blob,magic,4)||get32(blob+4)!=NH_AUTH_VAULT_VERSION||get32(blob+8)!=NH_AUTH_VAULT_SCRYPT_N)return NH_AUTH_VAULT_INVALID;
  uint8_t *aad=NULL;size_t an=0;if(make_aad(b,&aad,&an))return NH_AUTH_VAULT_INVALID;
  const uint8_t*salt=blob+HDR,*nonce=salt+SALT,*tag=nonce+NONCE,*cipher=tag+TAG;nostr_secure_buf key={0},plain={0};EVP_CIPHER_CTX*c=NULL;int len=0,total=0,ok=0;
  if(derive(pw,pn,salt,&key))goto done;plain=secure_alloc(32);if(!plain.ptr||!plain.locked)goto done;c=EVP_CIPHER_CTX_new();if(!c)goto done;
  if(EVP_DecryptInit_ex(c,EVP_aes_256_gcm(),NULL,NULL,NULL)!=1||EVP_CIPHER_CTX_ctrl(c,EVP_CTRL_GCM_SET_IVLEN,NONCE,NULL)!=1||EVP_DecryptInit_ex(c,NULL,NULL,key.ptr,nonce)!=1||EVP_DecryptUpdate(c,NULL,&len,aad,(int)an)!=1||EVP_DecryptUpdate(c,plain.ptr,&len,cipher,CIPHER)!=1)goto done;total=len;
  if(EVP_CIPHER_CTX_ctrl(c,EVP_CTRL_GCM_SET_TAG,TAG,(void*)tag)!=1||EVP_DecryptFinal_ex(c,(uint8_t*)plain.ptr+total,&len)!=1)goto done;ok=1;
done:EVP_CIPHER_CTX_free(c);secure_free(&key);if(aad){secure_wipe(aad,an);free(aad);}if(!ok){secure_free(&plain);return NH_AUTH_VAULT_UNLOCK_FAILED;}*out=plain;return NH_AUTH_VAULT_OK;
}
